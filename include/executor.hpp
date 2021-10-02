#pragma once

#include <memory>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <thread>
#include <sys/epoll.h>

#include "common.hpp"
#include "scheduler.hpp"
#include "task.hpp"

namespace acairo {
    struct SocketEventKey {
        int fd;
        EVENT_TYPE event_type;

        bool operator==(const SocketEventKey &other) const { 
            return (fd == other.fd && event_type == other.event_type);
        }
    };
}  // namespace acairo

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
}  // namespace std

namespace acairo {
    // SocketEventCallback is a helper class to store information about epoll events and related callbacks.
     // It isn't thread-safe.
    class SocketEventCallback {
        public:
            SocketEventCallback() = default;

            explicit SocketEventCallback(std::shared_ptr<Scheduler> scheduler) 
                : m_scheduler(scheduler) {};

            // Either schedule the previously saved work unit or 
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

    // Spawns the final task in the chain of coroutines.
    // Each coroutine passed by the user should be co_await-ed
    // to obtain FinalTask. FinalTask handles proper destruction 
    // of such objects.
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
            void spawn(std::function<Task<void>()>&& awaitable);

            template<typename Callable>
            void register_event_handler(int fd, EVENT_TYPE event_type, Callable&& callable) {
                auto socket_event_key = SocketEventKey{
                    fd: fd,
                    event_type: event_type,
                };

                LOG(m_l, logger::debug) << "Registering callback for fd [" << fd << "] and event [" << event_type << "].";

                std::unique_lock<std::shared_mutex> lock(m_coroutines_map_mutex);
                m_coroutines_map.try_emplace(socket_event_key, m_scheduler);
                m_coroutines_map[socket_event_key].set_callback(Scheduler::WorkUnit(std::forward<Callable>(callable)));
            }

            void register_fd(int fd) const;

            void deregister_event_handler(int fd, EVENT_TYPE event_type);

            void deregister_fd(int fd) const;

            bool is_event_handler_cancelled(int fd, EVENT_TYPE event_type);

            void stop() {
                m_stopping = true;

                if (m_epoll_thread.joinable()) {
                    m_epoll_thread.join();
                }

                // We pause the scheduler to stop all the tasks
                // from executing, as executing them would mean
                // that they might still be producing further tasks.
                // We then schedule all the remaining tasks from the map of
                // waiting coroutines. We know that all of them 
                // will raise an exception as we are in the process
                // of stopping. That way we can clean all the non-completed coroutines
                // and avoid any memory leaks.
                m_scheduler->pause();

                for (auto& [key, c] : m_coroutines_map) {
                    c.schedule();
                }

                m_scheduler->restart();

                m_scheduler->stop(true);

                // TODO: Is having two vars necessary?
                m_stopped = true;

                // Notify if there is someone synchronously waiting for the final coroutine (via sync_wait)
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

            std::shared_mutex m_coroutines_map_mutex;
            std::unordered_map<SocketEventKey, SocketEventCallback> m_coroutines_map;
            
            std::mutex m_sync_waiter_mutex;
            std::shared_ptr<SynchronizedWaiter> m_sync_waiter;

            const detail::Types::Logger m_l;
    };
}  // namespace acairo