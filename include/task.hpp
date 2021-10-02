#pragma once

#include <coroutine>
#include <memory>
#include <atomic>

#include "common.hpp"

namespace acairo {
    // Forward declarations
    template<typename T>
    class Promise;

    class FinalTaskPromise;

    // Awaits Task object
    template<typename T>
    class TaskAwaiter {
        public:
            TaskAwaiter(std::coroutine_handle<Promise<T>> handle)
                : m_handle(handle) {}
            
            bool await_ready() const noexcept { 
                return m_handle.done();
            }

            template<typename ContinuationPromiseType>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<ContinuationPromiseType> handle) const noexcept {
                m_handle.promise().set_continuation(handle);
                return m_handle;
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
                if (m_handle) {
                    m_handle.destroy(); 
                }
            }

        private:               
            std::coroutine_handle<promise_type> m_handle;
            id_type m_id;
    };

    // Coroutine state is a helper struct that gathers information 
    // about the state of the coroutine
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
}  // namespace acairo