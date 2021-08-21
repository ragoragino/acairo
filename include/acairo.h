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
#include <cassert>
#include <sys/epoll.h>

#include "logger.hpp"

// Use (void) cast to silent unused warnings.
#define assertm(exp, msg) assert(((void)msg, exp))

namespace acairo {
    // We define all types commonly used at the namespace level, so they
    // don't need to be defined in each separate class and we can easily 
    // change them when needed.
    namespace detail {
        struct Types {
            using Logger = logger::Logger<>;
            using ID = uint64_t;
        };
    }  // namespace detail

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

// SocketEventKey's hash template specialization must be defined before 
// its first usage.
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
    std::string error_with_errno(const std::string& message) noexcept;

    std::ostream& operator<<(std::ostream& os, const EVENT_TYPE& event_type);

    struct TCPListenerStoppedError : public std::exception {
        const char * what () const noexcept {
            return "TCPListener was stopped.";
        }
    };

    static detail::Types::ID generate_id() {
        static thread_local std::mt19937 gen; 
        std::uniform_int_distribution<detail::Types::ID> distrib(0, std::numeric_limits<detail::Types::ID>::max());
        return distrib(gen);
    }

    struct SchedulerConfiguration {
        std::uint8_t number_of_worker_threads;
    };

    // TODO: Maybe drain tasks at the end before returning?
    // Some of the coroutines might potentially leak if we won't destroy them properly.
    class Scheduler {
        public:
            class WorkUnit {
                public:
                    using id_type = detail::Types::ID;

                    explicit WorkUnit(const std::function<void()>& work_unit) 
                    : m_id(generate_id())
                    , m_work_unit(work_unit) {}

                    explicit WorkUnit(std::function<void()>&& work_unit) 
                    : m_id(generate_id())
                    , m_work_unit(std::move(work_unit)) {}

                    id_type get_id() const {
                        return m_id;
                    }

                    void operator()() {
                        m_work_unit();
                    }

                private:
                    const detail::Types::ID m_id;

                    std::function<void()> m_work_unit;
            };

            Scheduler(const SchedulerConfiguration& config) 
                : m_threadPool(config.number_of_worker_threads)
                , m_stopped(false)
                , m_config(config)
                , m_l(logger::Logger().WithPair("Component", "Scheduler")) {
                    for (std::uint8_t i = 0; i != m_config.number_of_worker_threads; i++) {
                        m_threadPool[i] = std::thread(&Scheduler::run_worker, this);
                    }
            }

            void spawn(WorkUnit&& handler);

            void stop();

        private:
            void run_worker() noexcept;

            std::optional<WorkUnit> get_new_work_unit();
            
            std::vector<std::thread> m_threadPool;

            std::atomic_bool m_stopped;

            std::queue<WorkUnit> m_work_queue;
            std::mutex m_work_queue_mutex;
            std::condition_variable m_work_queue_cv;

            const SchedulerConfiguration m_config;

            const detail::Types::Logger m_l;
    };

     // SocketEventCallback is a helper class to store information about epoll event and related callbacks.
     // It isn't thread-safe.
    class SocketEventCallback {
        public:
            SocketEventCallback() = default;

            explicit SocketEventCallback(std::shared_ptr<Scheduler> scheduler) 
                : m_scheduler(scheduler) {};

            // Either schedule the previously aved work unit or 
            // mark that an unhandled event occured.
            void schedule() {
                if (!m_scheduler) {
                    return;
                }

                if (m_work_unit) {
                    m_scheduler->spawn(std::move(*m_work_unit));
                    m_work_unit.reset();
                } else {
                    m_unhandled_event = true;
                }
            }

            // Either save the work unit, or in case an epoll event has already
            // happened before, schedule the work unit.
            void set_callback(Scheduler::WorkUnit&& work_unit) {
                if (!m_scheduler) {
                    return;
                }

                if (m_work_unit) {
                    throw std::runtime_error("Work unit already registered for the socket event!");
                }

                if (m_unhandled_event) {
                    m_scheduler->spawn(std::move(work_unit));
                    m_unhandled_event = false;
                } else {
                    m_work_unit.emplace(std::move(work_unit));
                }
            }

            bool has_callback() const noexcept {
                return m_work_unit.has_value();
            }

        private:
            std::shared_ptr<Scheduler> m_scheduler;

            bool m_unhandled_event;
            std::optional<Scheduler::WorkUnit> m_work_unit;
    };

    // Forward declarations
    template<typename T>
    struct Task;

    struct FinalTask;

    // SynchronizedWaiter is a helper struct that can used to lock 
    // object that need thread-synchronization and notify different handlers 
    // waiting for events related to the locked object.
    struct SynchronizedWaiter {
        std::mutex m;
        std::condition_variable cv;
    };

    // Spawns the final task in the chain of coroutines.
    // Each coroutine passed by the user should be co_await-ed
    // to obtain FinalTask. FinalTask handles proper destruction 
    // of such objects.
    //
    // In the beginning, I pondered whether such setting a flag from the Task
    // (like here: https://gitlab.com/deus_ex_machina399/coroutine-async/-/blob/master/lib/include/async/Task.h#L124)
    // wouldn't be sufficent, but it seems to me not to be free from race conditions. 
    // That is why I have decided to create another coroutine wrapper
    // that will be used exclusively for handling the last coroutines in the chain of coroutines.
    FinalTask spawn_final_task(std::function<Task<void>()>&& awaitable);

    struct ExecutorConfiguration {
        SchedulerConfiguration scheduler_config;
        int max_number_of_fds;
    };

    class Executor {
        public:
            Executor(const ExecutorConfiguration& config);

            // enable_if_t enforces that users pass only rvalue awaitables.
            // Lvalues could potentially cause object lifetime issues.
            // TODO: Enforce that awaitable is awaitable - maybe un-template it?
            template<typename Awaitable,
                typename = typename std::enable_if_t<!std::is_lvalue_reference<Awaitable>::value, Awaitable>>
            void spawn(Awaitable&& awaitable) {
                auto work_unit = [awaitable = std::move(awaitable)](){
                    auto task = spawn_final_task(std::move(awaitable));
                };

                m_scheduler->spawn(Scheduler::WorkUnit(std::move(work_unit)));
            }

            template<typename Callable>
            void register_event_handler(int fd, EVENT_TYPE event_type, Callable&& callable);

            void register_fd(int fd) const;

            void deregister_event_handler(int fd, EVENT_TYPE event_type);

            void deregister_fd(int fd) const;

            void stop() {
                m_stopping = true;

                if (m_epoll_thread.joinable()) {
                    m_epoll_thread.join();
                }

                m_scheduler->stop();

                m_stopped = true;

                // Notify if there is someone synchronously waiting for the coroutine (via sync_wait)
                std::lock_guard<std::mutex> lock(m_sync_waiter_mutex);
                if (m_sync_waiter) {
                    m_sync_waiter->cv.notify_one();
                }
            }

            // Must be called at most once!
            void sync_wait(std::function<Task<void>()>&& handler);

            ~Executor();

        private:
            void run_epoll_listener();

            static int get_epoll_event_type(EVENT_TYPE event_type);

            std::shared_ptr<Scheduler> m_scheduler;

            ExecutorConfiguration m_config;

            std::atomic_bool m_stopped, m_stopping;

            int m_epoll_fd;
            std::thread m_epoll_thread; 

            std::mutex m_coroutines_map_mutex;
            std::unordered_map<SocketEventKey, SocketEventCallback> m_coroutines_map;
            
            std::mutex m_sync_waiter_mutex;
            std::shared_ptr<SynchronizedWaiter> m_sync_waiter;

            const detail::Types::Logger m_l;
    };

    class Future {
        public:
            Future(std::shared_ptr<Executor> executor, int fd_in, EVENT_TYPE event_in) 
            : m_executor(executor), m_fd(fd_in), m_event_type(event_in) { }

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
            ReadFuture(std::shared_ptr<Executor> executor, int fd) 
            : Future(executor, fd, EVENT_TYPE::IN) {}
    };

    class WriteFuture : public Future {
         public:
            WriteFuture(std::shared_ptr<Executor> executor, int fd) 
            : Future(executor, fd, EVENT_TYPE::OUT) {}
    };

    class AcceptFuture : public Future {
         public:
            AcceptFuture(std::shared_ptr<Executor> executor, int fd) 
            : Future(executor, fd, EVENT_TYPE::IN) {}
    };

    // Awaits Future object. Use enable_if_t to force that only classes derived from the Future class
    // can be a template parameter of this class. 
    template<typename FutureType, 
        typename = typename std::enable_if_t<std::is_base_of<Future, FutureType>::value, FutureType>>
    class FutureAwaiter {
        public:
            FutureAwaiter(FutureType&& future_awaitable)
                : m_future_awaitable(std::move(future_awaitable)) {}
            
            bool await_ready() const noexcept { 
                return false; 
            }

            /*
            https://en.cppreference.com/w/cpp/language/coroutines

            Note that because the coroutine is fully suspended before entering awaiter.await_suspend(), 
            that function is free to transfer the coroutine handle across threads, with no additional
            synchronization. For example, it can put it inside a callback, scheduled to run on a threadpool 
            when async I/O operation completes. In that case, since the current coroutine may have been 
            resumed and thus executed the awaiter object's destructor, all concurrently as await_suspend() 
            continues its execution on the current thread, await_suspend() should treat *this as destroyed 
            and not access it after the handle was published to other threads. 
            */
            void await_suspend(std::coroutine_handle<> handle) const noexcept {
                auto continuation_handle = [handle]() mutable {
                    handle.resume();
                };

                auto executor = m_future_awaitable.get_executor();

                int fd = m_future_awaitable.get_fd();
                EVENT_TYPE event_type = m_future_awaitable.get_event_type();

                executor->register_event_handler(fd, event_type, std::move(continuation_handle));
            }

            void await_resume() const noexcept {}

        private:
            FutureType m_future_awaitable;
    };

    // Forward declarations
    template<typename T>
    class Promise;

    class FinalTaskPromise;

    // Awaits Task object
    // TaskAwaiter must be always resumed before the awaited coroutine gets destroyed.
    // Otherwise, the handle will become dangling.
    template<typename T>
    class TaskAwaiter {
        public:
            TaskAwaiter(std::coroutine_handle<Promise<T>> handle)
                : m_handle(handle) {}
            
            bool await_ready() const noexcept { 
                return m_handle.done();
            }

            template<typename F>
            void await_suspend(std::coroutine_handle<F> handle) const noexcept {
                m_handle.promise().set_continuation(handle);
            }

            T await_resume() const {
                return m_handle.promise().get_return_value();
            }

        private:
            std::coroutine_handle<Promise<T>> m_handle;
    };

    template<typename T>
    struct Task {
        public:
            using promise_type = Promise<T>;

            using id_type = detail::Types::ID;

            Task(std::coroutine_handle<promise_type> handle)
                : m_handle(handle) 
                , m_id(generate_id()) {
                    auto l = logger::Logger();
                    LOG(l, logger::info) << "Task(): " << m_id;

                    m_handle.promise().set_id(m_id);
                }

            Task(Task&) = delete;
            
            Task(Task&& other) noexcept 
            : m_handle{other.m_handle} 
            , m_id(other.m_id) { 
                other.m_handle = {}; 
                other.m_id = 0;
            }

            TaskAwaiter<T> operator co_await() noexcept {
                return TaskAwaiter<T> { m_handle };
            }

            id_type get_id() const noexcept {
                return m_id;
            }

            bool done() const noexcept {
                return m_handle.done();
            }

            ~Task() { 
                auto l = logger::Logger();
                LOG(l, logger::info) << "~Task: " << m_id << ".";

                if (m_handle) {
                    m_handle.destroy(); 
                }
            }

        private:               
            std::coroutine_handle<promise_type> m_handle;
            id_type m_id;
    };

    // Coroutine state is a helper struct that gathers information 
    // about the state of the coroutien
    struct CoroutineState {
        std::exception_ptr exception_ptr;
        std::atomic_bool done;
    };

    struct FinalTask {
        public:
            using promise_type = FinalTaskPromise;

            using id_type = detail::Types::ID;

            FinalTask(std::coroutine_handle<promise_type> handle);

            FinalTask(FinalTask&) = delete;
            
            FinalTask(FinalTask&& other) noexcept 
            : m_handle{other.m_handle} 
            , m_waiter(std::move(other.m_waiter)) 
            , m_id(other.m_id) { 
                other.m_handle = {}; 
                other.m_id = 0;
            }

            std::shared_ptr<SynchronizedWaiter> get_coroutine_waiter() const noexcept {
                return m_waiter;
            }

            std::shared_ptr<CoroutineState> get_coroutine_state() const noexcept {
                return m_state;
            }

            id_type get_id() const noexcept {
                return m_id;
            }

            bool done() const noexcept {
                return m_handle.done();
            }

            void destroy() {
                if (m_handle) {
                    m_handle.destroy();
                }
            }

            ~FinalTask() {}

        private:                
            std::coroutine_handle<promise_type> m_handle;
            std::shared_ptr<SynchronizedWaiter> m_waiter;
            std::shared_ptr<CoroutineState> m_state;
            id_type m_id;
    };

   // ContinuationAwaiter waits during final_suspend of the promise for the resumption of continuations.
   // Allows to be always ready in case it is tied with the last promise in a chain of promises.
    template<typename T>
    class ContinuationAwaiter {
        public:
            ContinuationAwaiter() noexcept {};
            
            bool await_ready() const noexcept { 
                return false; 
            }

            template<typename PromiseType,
                typename = typename std::enable_if_t<!std::is_same<PromiseType, FinalTaskPromise>::value, PromiseType>>
            void await_suspend(std::coroutine_handle<PromiseType> handle) const noexcept {
                try {
                    handle.promise().on_finish();
                } catch (const std::exception& e) {
                    auto l = logger::Logger();
                    LOG(l, logger::error) << "Exception thrown when calling on_finish during "  
                        "await_suspend in ContinuationAwaiter: " << e.what() << ".";
                }

                auto continuation = handle.promise().get_continuation();
                if (continuation && !continuation.done()) {
                    continuation.resume();
                }               
            }

            // Awaiting when the promise type is FinalTaskPromise. We don't resume
            // any continuations here as this is the final one. We also return false
            // to finish the coroutine.
            template<typename PromiseType,
                typename = typename std::enable_if_t<std::is_same<PromiseType, FinalTaskPromise>::value, PromiseType>>
            bool await_suspend(std::coroutine_handle<PromiseType> handle) const noexcept {
                try {
                    handle.promise().on_finish();
                } catch (const std::exception& e) {
                    auto l = logger::Logger();
                    LOG(l, logger::error) << "Exception thrown when calling on_finish during "  
                        "await_suspend in ContinuationAwaiter: " << e.what() << ".";
                }

                return false;
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
            using id_type = detail::Types::ID;

            PromiseBase() {
                auto l = logger::Logger();
                LOG(l, logger::info) << "PromiseBase is constructed.";
            }

            std::suspend_never initial_suspend() { 
                auto l = logger::Logger();
                LOG(l, logger::info) << "initial_suspend: " << m_id;
                return {};
            }

            ContinuationAwaiter<T> final_suspend() noexcept {              
                return ContinuationAwaiter<T>(); 
            }

            void on_finish_callback(std::function<void()>&& on_finish) noexcept {
                m_on_finish = std::move(on_finish);
            }

            void on_finish()  {
                if (m_on_finish) {
                    m_on_finish();
                }
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
                m_eptr = std::current_exception();

                try {
                    std::rethrow_exception(m_eptr);
                } catch (const std::exception& e) {
                    auto l = logger::Logger();
                    LOG(l, logger::error) << "Promise [" << m_id << "] failed with an error: " << e.what() << ".";
                }
            }

            std::exception_ptr get_exception() const noexcept {
                return m_eptr;
            }

            void set_continuation(std::coroutine_handle<> continuation) noexcept {
                m_continuation = continuation;
            }

            std::coroutine_handle<> get_continuation() const noexcept {
                return m_continuation;
            }

            void set_id(unsigned long long id) {
                m_id = id;
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

            virtual ~PromiseBase() {
                auto l = logger::Logger();
                LOG(l, logger::info) << "~PromiseBase: " << m_id;
            };

        private:
            std::coroutine_handle<> m_continuation;

            std::function<void()> m_on_finish;
            std::exception_ptr m_eptr;

            id_type m_id;
   };

    class FinalTaskPromise : public PromiseBase<void> {
        public:
            FinalTaskPromise() = default;

            FinalTask get_return_object() { 
                return FinalTask(std::coroutine_handle<FinalTaskPromise>::from_promise(*this)); 
            }

            void return_void() noexcept {}
    };

    template<typename T>
    class Promise : public PromiseBase<T> {
        public:
            Promise() = default;

            Task<T> get_return_object() { 
                return Task<T>(std::coroutine_handle<Promise<T>>::from_promise(*this)); 
            }

            void return_value(T&& value) noexcept {
                m_value = std::forward<T>(value);
            }

            T get_return_value() const {
                if (auto eptr = PromiseBase<T>::get_exception(); eptr) {
                    std::rethrow_exception(eptr);
                }

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
            void get_return_value() {
                 if (auto eptr = PromiseBase<void>::get_exception(); eptr) {
                    std::rethrow_exception(eptr);
                }
            } 
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
                , m_executor(executor)
                , m_l(logger::Logger().WithPair("Component", "TCPStream").WithPair("fd", fd)) {
                    m_executor->register_fd(fd);
                }

            acairo::Task<std::vector<char>> read(size_t number_of_bytes);

            acairo::Task<void> write(std::vector<char>&& buffer);

            ~TCPStream() noexcept;

        private:
            const int m_fd;
            const TCPStreamConfiguration m_config;
            std::shared_ptr<Executor> m_executor;

            const detail::Types::Logger m_l;
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
                , m_executor(executor) 
                , m_l(logger::Logger().WithPair("Component", "TCPListener")) {}

            void bind(const std::string& address);
            
            void listen() const;

            Task<std::shared_ptr<TCPStream>> accept() const;

            void stop() noexcept;

            ~TCPListener();

        private:
            std::atomic_bool m_stopped;

            struct epoll_event m_accept_event;

            int m_listener_sockfd = -1;
            int m_epoll_fd = -1;

            const TCPListenerConfiguration m_config;
            std::shared_ptr<Executor> m_executor;

            const detail::Types::Logger m_l;
    };
}

