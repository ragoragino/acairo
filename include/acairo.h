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
#include <unordered_map>
#include <coroutine>

// SocketEventKey's hash template specialization must be defined before 
// its first usage.
namespace acairo {
    enum class EVENT_TYPE : uint8_t {
        IN,
        OUT,
    };

    struct SocketEventKey {
        int fd;
        EVENT_TYPE event_type;

        bool operator==(const SocketEventKey &other) const { 
            return (fd == other.fd && event_type == other.event_type);
        }
    };
}

namespace std {
    template <> 
    struct hash<acairo::SocketEventKey>
    {
        std::size_t operator()(const acairo::SocketEventKey& k) const {
            static auto hasher = std::hash<int>();
            return hasher(k.fd);
        }
    };
}

namespace acairo {
    // Create an error message containing stringified errno
    std::string error_with_errno(const std::string& message);

    std::ostream& operator<<(std::ostream& os, const EVENT_TYPE& event_type);

    struct TCPListenerStoppedError : public std::exception {
        const char * what () const noexcept {
            return "TCPListener was stopped.";
        }
    };

    struct SchedulerConfiguration {
        std::uint8_t number_of_worker_threads;
    };

    class Scheduler {
        public:
            class WorkUnit {
                public:
                    explicit WorkUnit(const std::function<void()>& work_unit) 
                    : m_id(WorkUnit::generate_id())
                    , m_work_unit(work_unit) {}

                    explicit WorkUnit(std::function<void()>&& work_unit) 
                    : m_id(WorkUnit::generate_id())
                    , m_work_unit(std::move(work_unit)) {}

                    uint64_t get_id() {
                        return m_id;
                    }

                    void set_exception(std::exception_ptr exception_ptr){
                        m_exception_ptr = exception_ptr;
                    }

                    void operator()(){
                        m_work_unit();
                    }

                private:
                    static uint64_t generate_id(){
                        static std::random_device rd;  
                        static std::mt19937 gen(rd()); 
                        static std::uniform_int_distribution<uint64_t> distrib(0, std::numeric_limits<uint64_t>::max());

                        return distrib(gen);
                    }

                    const uint64_t m_id;

                    std::function<void()> m_work_unit;
                    std::exception_ptr m_exception_ptr;
            };

            Scheduler(const SchedulerConfiguration& config) 
                : m_threadPool(config.number_of_worker_threads)
                , m_stopped(false)
                , m_config(config) {
                    for(std::uint8_t i = 0; i != m_config.number_of_worker_threads; i++) {
                        m_threadPool[i] = std::thread(&Scheduler::start_worker, this);
                    }
            }

            void spawn(WorkUnit&& handler);

            void stop();

        private:
            void start_worker();

            std::optional<WorkUnit> get_new_work_unit();
            
            std::vector<std::thread> m_threadPool;

            std::atomic_bool m_stopped;
            const std::chrono::milliseconds m_work_waiting_timeout = std::chrono::milliseconds(100);

            std::queue<WorkUnit> m_work_queue;
            std::mutex m_work_queue_mutex;
            std::condition_variable m_work_queue_cv;

            const SchedulerConfiguration m_config;
    };

    struct ExecutorConfiguration {
        SchedulerConfiguration scheduler_config;
        int max_number_of_fds;
    };

    class Executor {
        public:
            Executor(const ExecutorConfiguration& config);

            template<typename Callable>
            void spawn(Callable&& callable) {
                auto work_unit = [callable = std::move(callable)](){
                    auto task = callable();
                    // TODO: task.detach();
                };

                m_scheduler->spawn(Scheduler::WorkUnit(std::move(work_unit)));
            }

            template<typename Callable>
            void register_event_handler(int fd, EVENT_TYPE event_type, Callable&& callable);

            void stop() {
                m_stopped = true;

                if (m_epoll_thread.joinable()) {
                    m_epoll_thread.join();
                }
            }

        private:
            void run_epoll_listener();

            static int get_epoll_event_type(EVENT_TYPE event_type);

            std::unique_ptr<Scheduler> m_scheduler;

            std::atomic_bool m_stopped;
            ExecutorConfiguration m_config;

            int m_epoll_fd;
            std::thread m_epoll_thread; 

            std::mutex m_coroutines_map_mutex;
            std::unordered_map<SocketEventKey, std::vector<Scheduler::WorkUnit>> m_coroutines_map;
    };

    class Future {
        public:
            Future(std::shared_ptr<Executor> executor, int fd_in, EVENT_TYPE event_in) 
            : m_executor(executor), m_fd(fd_in), m_event_type(event_in) {}

            std::shared_ptr<Executor> get_executor() const noexcept {
                return m_executor;
            }

            EVENT_TYPE get_event_type() const noexcept {
                return m_event_type;
            }

            int get_fd() const noexcept {
                return m_fd;
            }

            virtual ~Future() = default;

        protected:
            const std::shared_ptr<Executor> m_executor;

            const int m_fd;
            const EVENT_TYPE m_event_type; 
    };

    class ReadFuture : public Future {
        public:
            ReadFuture(std::shared_ptr<Executor> executor, int fd_in) 
            : Future(executor, fd_in, EVENT_TYPE::IN) {}
    };

    class WriteFuture : public Future {
         public:
            WriteFuture(std::shared_ptr<Executor> executor, int fd_in) 
            : Future(executor, fd_in, EVENT_TYPE::OUT) {}
    };

    // Awaits Future object
    template<typename FutureType>
    class FutureAwaiter {
        public:
            FutureAwaiter(FutureType&& future_awaitable)
                : m_future_awaitable(std::move(future_awaitable)) {}
            
            bool await_ready() const noexcept { 
                return false; 
            }

            void await_suspend(std::coroutine_handle<> handle) const noexcept {
                auto continuationHandle = [handle]() mutable {
                    handle.resume();
                };

                auto executor = m_future_awaitable.get_executor();

                int fd = m_future_awaitable.get_fd();
                EVENT_TYPE event_type = m_future_awaitable.get_event_type();

                // https://lewissbaker.github.io/2017/11/17/understanding-operator-co-await
                /* 
                So within the await_suspend() method, once it’s possible for the coroutine to be resumed concurrently 
                on another thread, you need to make sure that you avoid accessing this or the coroutine’s .promise() 
                object because both could already be destroyed. In general, the only things that are safe to access 
                after the operation is started and the coroutine is scheduled for resumption are local variables within await_suspend().
                */
                executor->register_event_handler(fd, event_type, std::move(continuationHandle));
            }

            void await_resume() const noexcept {}

        private:
            FutureType m_future_awaitable;
    };

    // Forward declaration
    template<typename T>
    class Promise;

    // Awaits Task object
    template<typename T>
    class TaskAwaiter {
        public:
            TaskAwaiter(std::coroutine_handle<Promise<T>> handle)
                : m_handle(handle) {}
            
            bool await_ready() const noexcept { 
                return false; 
            }

            void await_suspend(std::coroutine_handle<> handle) const noexcept {
                m_handle.promise().set_continuation(handle);
            }

            T await_resume() const noexcept {
                return m_handle.promise().get_value();
            }

        private:
            std::coroutine_handle<Promise<T>> m_handle;
    };

    // TODO: Implement detaching
    template<typename T>
    struct Task {
        public:
            using promise_type = Promise<T>;

            Task(std::coroutine_handle<promise_type> handle) 
                : m_handle(handle) { }

            Task(Task&) = delete;
            
            Task(Task&& other) noexcept 
            : m_handle{other.m_handle} { 
                other.m_handle = {}; 
            }


            bool done() const {
                return m_handle.done();
            }
            
            TaskAwaiter<T> operator co_await() noexcept {
                return TaskAwaiter<T> { m_handle };
            }

            ~Task() { 
                m_handle.destroy(); 
            }

        private:
            std::coroutine_handle<promise_type> m_handle;
    };

   // Waits during final_suspend of the promise for the resumption of continuations
    template<typename T>
    struct ContinuationAwaiter {
        ContinuationAwaiter() = default;
        
        bool await_ready() const noexcept { 
            return false; 
        }

        void await_suspend(std::coroutine_handle<Promise<T>> handle) const noexcept {
            auto continuation = handle.promise().get_continuation();
            if (continuation && !continuation.done()) {
                continuation.resume();
            }
        }

        void await_resume() const noexcept {}
    };

    // https://devblogs.microsoft.com/oldnewthing/20210330-00/?p=105019
    /*
    It is illegal to have both return_value and return_void in a promise type, even if one of them is removed by SFINAE.
    The reason is that in order for the compiler to perform substitution in order to determine which methods are callable, 
    it needs to know what was passed to all of the co_return statements, so it can try substituting them into return_value‘s 
    parameter and see which ones succeed. But it hasn’t started compiling the coroutine function body yet, so it doesn’t 
    know what to try to substitute for value.
    */
   template<typename T>
   class PromiseBase {
       public:
            PromiseBase() = default;

            std::suspend_never initial_suspend() { return {}; }

            // Resume a coroutine that have been chained after the source coroutine
            ContinuationAwaiter<T> final_suspend() noexcept { 
                return ContinuationAwaiter<T>{}; 
            }

            // https://lewissbaker.github.io/2018/09/05/understanding-the-promise-type
            /*
            Alternatively, the implementation could immediately rethrow the exception
            by executing a throw; statement. For example see folly::Optional.
            However, doing so will (likely - see below) cause the the coroutine frame to be 
            immediately destroyed and for the exception to propagate out to the caller/resumer. 
            This could cause problems for some abstractions that assume/require the call
            to coroutine_handle::resume() to be noexcept, so you should generally only use this
            approach when you have full control over who/what calls resume().
            */
            void unhandled_exception() {
                auto exceptionPtr = std::current_exception();
                if (exceptionPtr) {
                    //TODO: 
                } 
            }

            // Not thread-safe. This should be always running synchronously with get_continuation.
            void set_continuation(std::coroutine_handle<> continuation) noexcept {
                m_continuation = continuation;
            }

            std::coroutine_handle<> get_continuation() const noexcept {
                return m_continuation;
            }

            // By await_transform we transform awaitables - one signature is used to obtain just awaiters holding
            // instances of classes deriving from Future interface. The other signature is used for all other
            // objects that need to define co_await operators to obtain Awaiters (e.g. like the Task class).
            template <typename FutureType>
            typename std::enable_if<std::is_base_of<Future, FutureType>::value, FutureAwaiter<FutureType>>::type
            await_transform(FutureType&& future) {
                return FutureAwaiter<FutureType>(std::move(future));
            }

            template <typename Awaitable>
            typename std::enable_if<!std::is_base_of<Future, Awaitable>::value, Awaitable>::type
            await_transform(Awaitable&& awaitable) {
                return std::forward<Awaitable>(awaitable);
            }

        private:
            std::coroutine_handle<> m_continuation;
   };

    template<typename T>
    class Promise : public PromiseBase<T> {
        public:
            Promise() = default;

            Task<T> get_return_object() { 
                return Task<T>(std::coroutine_handle<Promise<T>>::from_promise(*this)); 
            }

            void return_value(T&& value) noexcept {
                m_value = std::move(value);
            }

            T get_value() const noexcept {
                return m_value;
            }

        private:
            T m_value;
    };

    template<>
    class Promise<void> : public PromiseBase<void> {
        public:
            Promise<void>() = default;

            Task<void> get_return_object() { 
                return Task<void>(std::coroutine_handle<Promise<void>>::from_promise(*this)); 
            }

            void return_void() noexcept {}

            // get_value is not used, but needs to exist due to the Promise duality problem 
            // (see comment on PromiseBase)
            void get_value() noexcept {} 
    };

    struct TCPStreamConfiguration {
        std::chrono::seconds read_timeout;
        std::chrono::seconds write_timeout;
    };

    class TCPStream {
        public:
            TCPStream(const TCPStreamConfiguration& config, int fd, std::shared_ptr<Executor> executor) 
                : m_fd(fd)
                , m_config(config)
                , m_executor(executor) {}

            acairo::Task<std::vector<char>> read(size_t number_of_bytes);

            acairo::Task<void> write(std::vector<char>&& buffer);

            ~TCPStream();

        private:
            const int m_fd;
            const TCPStreamConfiguration m_config;
            std::shared_ptr<Executor> m_executor;
    };

    struct TCPListenerConfiguration {
        TCPStreamConfiguration stream_config;
        size_t max_number_of_fds;
        int max_number_of_queued_conns;
    };

    class TCPListener {
        public:
            TCPListener(const TCPListenerConfiguration& config, std::shared_ptr<Executor> executor) 
                : m_stopped(false)
                , m_config(config)
                , m_executor(executor) {}

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
            std::shared_ptr<Executor> m_executor;
    };
}

