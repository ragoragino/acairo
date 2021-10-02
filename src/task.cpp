#include "task.hpp"

#include <functional>

#include "promise.hpp"

namespace acairo {
    FinalTask spawn_final_task(std::function<Task<void>()>&& awaitable) {
        co_await awaitable();
    }

    FinalTask::FinalTask(std::coroutine_handle<promise_type> handle)
        : m_handle(handle) 
        , m_waiter(std::make_shared<SynchronizedWaiter>())
        , m_state(std::make_shared<CoroutineState>())
        , m_id(generate_id()) {
            m_handle.promise().on_finish_callback(
                [waiter = this->m_waiter, state = this->m_state](std::exception_ptr e){
                    {
                        std::unique_lock<std::mutex> lock(waiter->m);      
                        state->exception_ptr = e;
                        state->done = true;
                    }

                    waiter->cv.notify_one();
                }
            );

            m_handle.promise().set_id(m_id);
        }
}  // namespace acairo