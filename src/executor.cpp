#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "executor.hpp"
#include "task.hpp"

namespace acairo {
    void log_socket_error(int fd) {
        auto l = logger::Logger().WithPair("fd", fd);

        int error = 0;
        socklen_t errlen = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen) == 0) {
            LOG(l, logger::error) << "Error occured on a socket:" << error << ".";
        } else {
            LOG(l, logger::error) << error_with_errno("Error occured on a socket, but unable to get it.");
        }
    }

    Executor::Executor(const ExecutorConfiguration& config) 
    : m_scheduler(std::make_shared<Scheduler>(config.scheduler_config))
    , m_config(config)
    , m_l(logger::Logger().WithPair("Component", "Executor")) {
        m_epoll_fd = epoll_create1(0);
        if (m_epoll_fd < 0) {
            throw std::runtime_error(error_with_errno("Unable to create epoll instance"));
        }

        m_epoll_thread = std::thread(&Executor::run_epoll_listener, this);
    }

    void Executor::spawn(std::function<Task<void>()>&& awaitable) {
        auto work_unit = [awaitable = std::move(awaitable)]() mutable {
            auto task = spawn_final_task(std::move(awaitable));
        };

        m_scheduler->spawn(Scheduler::WorkUnit(std::move(work_unit)));
    }

    // register_fd must be called once for a given fd
    void Executor::register_fd(int fd) const {
        LOG(m_l, logger::debug) << "Registering fd [" << fd << "] to the epoll interest list.";

        // We need edge-triggered notifications as we will be invoking handlers based on incoming events.
        // We start listening on EPOLLIN and EPOLLOUT at the same time, although the user might only need one
        // direction. However, by using edge-triggered notifications, we shouldn't worsen our perf.
        struct epoll_event accept_event;
        accept_event.events = EPOLLIN | EPOLLOUT | EPOLLET;
        accept_event.data.fd = fd;
        if (retry_sys_call(epoll_ctl, m_epoll_fd, EPOLL_CTL_ADD, fd, &accept_event) < 0) {
            throw std::runtime_error(error_with_errno("Unable to add new socket to the epoll interest list"));
        }
    }

    // TODO: What to do if this throws?
    void Executor::run_epoll_listener() {
        auto events = std::make_unique<struct epoll_event[]>(m_config.max_number_of_fds);

        auto schedule_ready_tasks = [this](const SocketEventKey& event_key){
            // Try emplacing an event key if one is not in the map. That way we can call 
            // a handler that will be registered later.
            auto [it, ok] = m_coroutines_map.try_emplace(event_key, m_scheduler);
            it->second.schedule();  
        };

        while (!m_stopping) {
            const int count_of_ready_fds = retry_sys_call(epoll_wait, m_epoll_fd, events.get(), m_config.max_number_of_fds, 10); 
            if (count_of_ready_fds < 0) {
                throw std::runtime_error(error_with_errno("Waititing for epoll_events failed"));
            }

            std::unique_lock<std::shared_mutex> lock(m_coroutines_map_mutex);
            for (int i = 0; i < count_of_ready_fds; i++) {
                const struct epoll_event& event = events.get()[i];
                const int fd = event.data.fd;

                // In case of a socket error, let the handlers finish their work
                if (event.events & EPOLLERR) {
                    log_socket_error(fd);

                    SocketEventKey event_key_in{fd, EVENT_TYPE::IN};
                    schedule_ready_tasks(event_key_in);

                    SocketEventKey event_key_out{fd, EVENT_TYPE::OUT};
                    schedule_ready_tasks(event_key_out);

                    continue;
                }
            
                // EPOLLIN and EPOLLOUT should be also called when the peer closed that end of the socket.
                // Therefore, it doesn't seem that we need to handle any special events connected
                // with unexpected peer socket shutdown here.
                if (event.events & EPOLLIN) {
                    LOG(m_l, logger::debug) << "Adding handler for a fd " << fd << " and event_type " 
                        << EVENT_TYPE::IN << " to the scheduler's queue.";

                    SocketEventKey event_key{fd, EVENT_TYPE::IN};
                    schedule_ready_tasks(event_key);
                }

                if (event.events & EPOLLOUT) {
                    LOG(m_l, logger::debug) << "Adding handler for a fd " << fd << " and event_type " 
                        << EVENT_TYPE::OUT << " to the scheduler's queue.";

                    SocketEventKey event_key{fd, EVENT_TYPE::OUT};
                    schedule_ready_tasks(event_key);
                }
            }
        }
    }

    void Executor::deregister_event_handler(int fd, EVENT_TYPE event_type) {
        std::unique_lock<std::shared_mutex> lock(m_coroutines_map_mutex);

        auto socket_event_key = SocketEventKey{
            fd: fd,
            event_type: event_type,
        };

        if (m_coroutines_map.find(socket_event_key) != m_coroutines_map.end()) { 
            if (m_coroutines_map[socket_event_key].has_callback()) {
                LOG(m_l, logger::warn) << socket_event_key.event_type << " event handlers for fd" << 
                    socket_event_key.fd << " active while deregistering fd from coroutines map.";
            
                // We schedule the coroutine here to fully destroy it.
                // The coroutine, upon awaking, will ask the executor 
                // if it is cancelled. The executor won't find the 
                // coroutine in its map and therefore will consider it as 
                // cancelled. The coroutine then can destroy itself
                // by propagating CancelledError exception.
                m_coroutines_map[socket_event_key].schedule();
            }

            m_coroutines_map.erase(socket_event_key);
        }
    }

    // We remove the fd from the epoll interest list
    void Executor::deregister_fd(int fd) const {
        LOG(m_l, logger::debug) << "Removing fd " << fd << " from the epoll interest list.";

        // In kernel versions before 2.6.9, the EPOLL_CTL_DEL operation required a non-null pointer in event, even though this argument
        // is ignored. Since Linux 2.6.9, event can be specified as NULL when using EPOLL_CTL_DEL.
        if (retry_sys_call(epoll_ctl, m_epoll_fd, EPOLL_CTL_DEL, fd, (struct epoll_event *)nullptr) < 0) {
            throw std::runtime_error(error_with_errno("Unable to deregister fd from the epoll interest list"));
        }
    }

    bool Executor::is_event_handler_cancelled(int fd, EVENT_TYPE event_type) {
        // If Executor is in stopping mode, we consider all coroutines
        // to be cancelled.
        if (m_stopping.load()) {
            return true;
        }

        // For cancellation of a particular coroutine, we just check 
        // whether the event handler was deregistered from the map of coroutines.
        std::shared_lock lock(m_coroutines_map_mutex);
        auto socket_event_key = SocketEventKey{
            fd: fd,
            event_type: event_type,
        };
        return m_coroutines_map.find(socket_event_key) == m_coroutines_map.end();
    }

    int Executor::get_epoll_event_type(EVENT_TYPE event_type) {
        switch (event_type) {
            case EVENT_TYPE::IN:
                return EPOLLIN;
            case EVENT_TYPE::OUT:
                return EPOLLOUT;
            default:
                std::stringstream ss;
                ss << "Unrecognized waiting event type: " << event_type << ".";
                throw std::runtime_error(ss.str());
        }
    }

    // We just wait until the promise calls final_suspend (or throws) as we know there 
    // is no continuation after the promise returns (i.e. the coroutine
    // will get destroyed after the work unit finished).
    // We also handle the case when the coroutine is suspended and the executor is getting stopped.
    void Executor::sync_wait(std::function<Task<void>()>&& handler) {
        auto task = spawn_final_task(std::move(handler));

        auto sync_waiter = task.get_coroutine_waiter();
        auto coroutine_state = task.get_coroutine_state();

        // Set m_sync_waiter that will be used to signal stopping of the executor
        {
            std::lock_guard<std::mutex> lock(m_sync_waiter_mutex);
            m_sync_waiter = sync_waiter;
        }
       
        std::unique_lock<std::mutex> lock(sync_waiter->m);
        if (!coroutine_state->done) {
            sync_waiter->cv.wait(lock, [coroutine_state, this]{ return coroutine_state->done.load() || m_stopped; });
        }

        if (m_stopped) {
            return;
        }
    
        if (coroutine_state->exception_ptr != nullptr) {
            // We check whether the exception thrown is CancelledError.
            // This would suggest that the handler got cancelled either
            // by getting deregistered from the Executor, or by getting
            // scheduled when the Executor was stopping. 
            // These types of errors are internal states and therefore
            // we catch them here as they don't signal any error conditions
            // to the user.
            try {
                std::rethrow_exception(coroutine_state->exception_ptr);
            } catch (const CancelledError& e) {
                auto l = logger::Logger();
                LOG(l, logger::debug) << "FinalTask got cancelled in sync_wait: " << e.what();
            } catch (const std::exception& e) {
                throw;
            }
        }
    }

    Executor::~Executor() {
        if (!m_stopped) {
            stop();
        }

        if (m_epoll_fd >= 0) {
            if (retry_sys_call(::close, m_epoll_fd) < 0) {
                LOG(m_l, logger::error) << error_with_errno("Unable to close listening epoll fd");
            }
        }
    }
}  // namespace acairo