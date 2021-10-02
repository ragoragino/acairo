#pragma once

#include <memory>
#include <coroutine>

#include "common.hpp"
#include "executor.hpp"

namespace acairo {
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
                : m_future_awaitable(std::forward<FutureType>(future_awaitable)) {}
            
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

                try {
                    executor->register_event_handler(fd, event_type, std::move(continuation_handle));
                } catch (const std::exception& e) {
                    auto l = logger::Logger();
                    LOG(l, logger::error) << "Registering callback for fd [" << fd << "] and event [" 
                        << event_type << "] failed: " << e.what() << ".";
                }
            }

            void await_resume() const {
                auto executor = m_future_awaitable.get_executor();

                int fd = m_future_awaitable.get_fd();
                EVENT_TYPE event_type = m_future_awaitable.get_event_type();

                if (executor->is_event_handler_cancelled(fd, event_type)) {
                    throw CancelledError("Coroutine got cancelled by an Executor.");
                }
            }

        private:
            FutureType m_future_awaitable;
    };
}  // namespace acairo