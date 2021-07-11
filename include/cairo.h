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
#include <optional>
#include <random>
#include <limits>
#include <memory>

namespace cairo {
    struct TCPListenerStoppedError : public std::exception {
        const char * what () const noexcept {
            return "TCPListener was stopped.";
        }
    };

    class Task {           
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

            ~TCPStream();

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
            TCPListener(const TCPListenerConfiguration& config) 
                : m_stopped(false)
                , m_config(config) {}

            void bind(const std::string& address);

            std::shared_ptr<TCPStream> accept();

            void shutdown();

        private:
            void run_epoll_listener();

            int process_waiting_connections(struct epoll_event* events, int waiting_conns_count);

            std::queue<int> m_accepted_conns;
            std::mutex m_accepted_conns_mutex;
            std::condition_variable m_accepted_conns_cv;

            std::thread m_epoll_thread;
            std::mutex m_epoll_thread_mutex;

            std::atomic_bool m_stopped;
            
            int m_listener_sockfd = 0;
            const TCPListenerConfiguration m_config;
    };

    struct ExecutorConfiguration {
        std::uint8_t number_of_worker_threads;
    };

    class Handler {
        public:
            using id_type = unsigned long long;

            Handler(std::function<void()>&& f) 
                : m_id(generate_id())
                , m_f(std::move(f)) {}

            void operator()(){
                m_f();
            }

            void set_exception(std::exception_ptr e) {
                m_exception = e;
            }

            id_type get_id() const {
                return m_id;
            }

        private:
            id_type generate_id() {
                static std::random_device rd;
                static std::mt19937 gen(rd());
                static std::uniform_int_distribution<id_type> dis(std::numeric_limits<id_type>::min(), 
                    std::numeric_limits<id_type>::max());
                return dis(gen);
            }

            id_type m_id;
            std::exception_ptr m_exception;
            std::function<void()> m_f;
    };

    class Executor {
        public:
            Executor(const ExecutorConfiguration& config) 
                : m_threadPool(config.number_of_worker_threads)
                , m_stopped(false)
                , m_config(config) {
                    for(std::uint8_t i = 0; i != m_config.number_of_worker_threads; i++) {
                        m_threadPool[i] = std::thread(&Executor::start_worker, this);
                    }
            }

            void spawn(Handler&& handler);

            void stop();

        private:
            void start_worker();

            std::optional<Handler> get_new_work_unit();
            
            std::vector<std::thread> m_threadPool;

            std::atomic_bool m_stopped;
            const std::chrono::milliseconds m_work_waiting_timeout = std::chrono::milliseconds(100);

            std::queue<Handler> m_work_queue;
            std::mutex m_work_queue_mutex;
            std::condition_variable m_work_queue_cv;

            const ExecutorConfiguration m_config;
    };
}