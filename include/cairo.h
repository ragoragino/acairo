#include <functional>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <condition_variable>

namespace cairo {

    struct DeadlineExceededError : public std::exception {
        const char * what () const noexcept {
            return "Deadline exceeded.";
        }
    };

    struct TCPStreamConfiguration {
        std::chrono::seconds read_timeout;
        std::chrono::seconds write_timeout;
    };

    class TCPStream {
        public:
            TCPStream(const TCPStreamConfiguration& config, int fd) 
                : m_fd(fd)
                , m_config(config) {}

            std::vector<char> read(size_t number_of_bytes);

            void write(std::vector<char>&& buffer);
        private:
            const int m_fd;
            const TCPStreamConfiguration m_config;
    };

     struct TCPListenerConfiguration {
        TCPStreamConfiguration stream_config;
        size_t max_number_of_fds;
        int max_number_of_queued_conns;
    };

    class TCPListener {
        public:
            TCPListener(const TCPListenerConfiguration& config) : m_config(config) {}

            void bind(const std::string& address);

            TCPStream accept(std::chrono::seconds timeout);

            void shutdown(std::chrono::seconds timeout);

        private:
            void start_epoll_listener();

            int m_events_count;
            std::mutex m_events_mutex;
            std::condition_variable m_events_cv;

            std::thread m_epoll_thread;
            std::mutex m_epoll_thread_mutex;

            std::atomic_bool m_stopped;
            
            int m_listener_sockfd = 0;
            const TCPListenerConfiguration m_config;
    };

    class Awaitable {
        public:
            Awaitable(std::function<void()>&& func) : m_func(func) {}

        private:
            const std::function<void()> m_func;
    };

    struct ExecutorConfiguration {
        std::uint8_t number_of_worker_threads;
    };

     class Task {
        public:
            Task(std::function<void()>&& f) 
                : m_f(f) {}

            void operator()(){
                m_f();
            }

            void set_exception(std::exception_ptr e) {
                m_exception = e;
            }

        private:
            std::exception_ptr m_exception;
            std::function<void()> m_f;
    };

    class Executor {
        public:
            Executor(const ExecutorConfiguration& config) 
                : m_threadPool(config.number_of_worker_threads)
                , m_config(config) {
                    for(std::uint8_t i = 0; i != m_config.number_of_worker_threads; i++) {
                        m_threadPool[i] = std::thread(&Executor::start_thread, this);
                    }
            }

            void spawn(Task&& task);

            void stop();

        private:
            void start_thread();
            
            std::vector<std::thread> m_threadPool;

            std::atomic_bool m_stopped;
            const std::chrono::milliseconds m_work_waiting_timeout = std::chrono::milliseconds(100);

            std::queue<Task> m_work_queue;
            std::mutex m_work_queue_mutex;
            std::condition_variable m_work_queue_cv;

            const ExecutorConfiguration m_config;
    };
}