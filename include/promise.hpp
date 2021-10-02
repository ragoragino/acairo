#pragma once

#include <coroutine>
#include <functional>

#include "common.hpp"
#include "future.hpp"
#include "task.hpp"

namespace acairo {
    // ContinuationAwaiter waits during final_suspend of the promise for the resumption of continuations.
    template<typename T>
    class ContinuationAwaiter {
        public:
            ContinuationAwaiter() noexcept {};
            
            bool await_ready() const noexcept { 
                return false; 
            }

            template<typename PromiseType,
                typename = typename std::enable_if_t<!std::is_same<PromiseType, FinalTaskPromise>::value, PromiseType>>
            std::coroutine_handle<> await_suspend(std::coroutine_handle<PromiseType> handle) const noexcept {
                try {
                    handle.promise().on_finish();
                } catch (const std::exception& e) {
                    auto l = logger::Logger();
                    LOG(l, logger::error) << "Exception thrown when calling on_finish during "  
                        "await_suspend in ContinuationAwaiter: " << e.what() << ".";
                }

                return handle.promise().get_continuation();
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

            PromiseBase() = default;

            ContinuationAwaiter<T> final_suspend() const noexcept {              
                return ContinuationAwaiter<T>(); 
            }

            void on_finish_callback(std::function<void(std::exception_ptr e)>&& on_finish) noexcept {
                m_on_finish = std::move(on_finish);
            }

            void on_finish()  {
                if (m_on_finish) {
                    m_on_finish(m_eptr);
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
            void unhandled_exception() noexcept {
                m_eptr = std::current_exception();

                try {
                    std::rethrow_exception(m_eptr);
                } catch (const CancelledError& e) {
                    // CancelledError is an internal state signaling that a coroutine
                    // is in the process of destruction. Therefore, we don't need to
                    // log it as an error. 
                    auto l = logger::Logger();
                    LOG(l, logger::info) << "Promise [" << m_id << "] got cancelled: " << e.what();
                } catch (const std::exception& e) {
                    auto l = logger::Logger();
                    LOG(l, logger::error) << "Promise [" << m_id << "] failed with an error: " << e.what();
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

            void set_id(id_type id) noexcept {
                m_id = id;
            }

            id_type get_id() const noexcept {
                return m_id;
            }   

            // By await_transform we transform awaitables - one signature is used to obtain just awaiters holding
            // instances of classes deriving from Future interface. The other signature is used for all other
            // objects that need to define co_await operators to obtain Awaiters (e.g. like the Task class).
            template <typename FutureType>
            typename std::enable_if<std::is_base_of<Future, FutureType>::value, FutureAwaiter<FutureType>>::type
            await_transform(FutureType&& future) {
                return FutureAwaiter<FutureType>(std::forward<FutureType>(future));
            }

            template <typename Awaitable>
            typename std::enable_if<!std::is_base_of<Future, Awaitable>::value, Awaitable>::type
            await_transform(Awaitable&& awaitable) {
                return std::forward<Awaitable>(awaitable);
            }

            virtual ~PromiseBase() = default;

        private:
            std::coroutine_handle<> m_continuation;

            std::function<void(std::exception_ptr)> m_on_finish;
            std::exception_ptr m_eptr;

            id_type m_id;
   };

    class FinalTaskPromise : public PromiseBase<void> {
        public:
            FinalTaskPromise() = default;

            std::suspend_never initial_suspend() const noexcept { 
                return {};
            }

            FinalTask get_return_object() { 
                return FinalTask(std::coroutine_handle<FinalTaskPromise>::from_promise(*this)); 
            }

            void return_void() const noexcept {}
    };

    template<typename T>
    class Promise : public PromiseBase<T> {
        public:
            Promise() = default;

            std::suspend_always initial_suspend() const noexcept { 
                return {};
            }

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

            std::suspend_always initial_suspend() const noexcept { 
                return {};
            }

            Task<void> get_return_object() { 
                return Task<void>(std::coroutine_handle<Promise<void>>::from_promise(*this)); 
            }

            void return_void() const noexcept {}

            void get_return_value() const {
                 if (auto eptr = PromiseBase<void>::get_exception(); eptr) {
                    std::rethrow_exception(eptr);
                }
            } 
    };
}  // namespace acairo