#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <csignal>

#include <sstream>
#include <iostream>
#include <optional>

#include "acairo.h"

namespace acairo {
    std::string error_with_errno(const std::string& message) noexcept {
        try {
            std::stringstream ss{};
            ss << message << ": " << strerror(errno) << ".";
            return ss.str();
        } catch (const std::exception& e) {
            std::cout << "Failure while error_with_errno: " << e.what() << ".";
        }
        
        return "";
    }

    std::ostream& operator<<(std::ostream& os, const EVENT_TYPE& event_type) {
        switch (event_type) {
            case EVENT_TYPE::IN:
                os << "IN";
                break;
            case EVENT_TYPE::OUT:
                os << "OUT";
                break;
            default:
                os << uint8_t(event_type);
        }

        return os;
    }

    // Retry syscalls on EINTR
    template<typename F, typename... Args>
    int retry_sys_call(F&& f, Args&&... args) noexcept {
        while (true) {
            int result = f(std::forward<Args>(args)...);
            if (result < 0 && errno == EINTR) {
                auto l = logger::Logger();
                LOG(l, logger::debug) << "Retrying syscall on EINTR.";
                continue;
            } else {
                return result;
            }
        }
    }

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

    // Split listening address to an IP address and port 
    // (default port being 80 and address 127.0.0.1)
    std::pair<std::string, int> split_address(const std::string& full_address) {
        int defaulPort = 80;

        if (full_address.empty()) {
            return { "127.0.0.1", defaulPort };
        }

        int port = defaulPort;
        std::string address = full_address;
        
        auto const pos = full_address.find_last_of(':');
        if (pos != std::string::npos) {
            std::string portStr = full_address.substr(pos+1);
            port = std::atoi(portStr.c_str());

            address = full_address.substr(0, pos);
        }
        
        return { address, port };
    }

    // Set nonblocking flag on the socket 
    void make_socket_non_blocking(int fd) {
        int flags = retry_sys_call(fcntl, fd, F_GETFL, 0);
        if (flags == -1) {
            throw std::runtime_error(error_with_errno("Unable to get fd's flags"));
        }

        if (retry_sys_call(fcntl, fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error(error_with_errno("Unable to set O_NONBLOCK on the fd."));
        }
    }

    void Scheduler::spawn(Scheduler::WorkUnit&& work_unit) {
        {
            std::lock_guard<std::mutex> lock(m_work_queue_mutex);
            m_work_queue.push(std::move(work_unit));
        }

        m_work_queue_cv.notify_one();
    }

    void Scheduler::stop() {
        m_stopped = true;
        m_work_queue_cv.notify_all();

        for (auto& worker : m_threadPool) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void Scheduler::run_worker() noexcept {
        while (!m_stopped) {
            WorkUnit::id_type work_id{};

            try {
                auto work_unit = this->get_new_work_unit();

                if (work_unit) {
                    work_id = work_unit->get_id();
                    
                    work_unit->operator()();
                    LOG(m_l, logger::debug) << "Successfully finished work unit: " << work_id;
                }
            } catch (const std::exception& e) {
                LOG(m_l, logger::error) << "Work unit [" << work_id << "] failed with an error: " << e.what() << ".";
            }
        }
    }

    std::optional<Scheduler::WorkUnit> Scheduler::get_new_work_unit() {
        std::unique_lock<std::mutex> lock(m_work_queue_mutex);

        if (m_work_queue.size() == 0) {
            m_work_queue_cv.wait(lock, [this]{ return m_work_queue.size() > 0 || m_stopped; });
            if (m_stopped) {
                return {};
            }
        }

        auto work_unit = m_work_queue.front();
        m_work_queue.pop();

        return work_unit;
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

    template<typename Callable>
    void Executor::register_event_handler(int fd, EVENT_TYPE event_type, Callable&& callable) {
        auto socket_event_key = SocketEventKey{
            fd: fd,
            event_type: event_type,
        };

        LOG(m_l, logger::debug) << "Registering callback for fd [" << fd << "] and event [" << event_type << "].";

        std::lock_guard<std::mutex> lock(m_coroutines_map_mutex);
        m_coroutines_map.try_emplace(socket_event_key, m_scheduler);
        m_coroutines_map[socket_event_key].set_callback(Scheduler::WorkUnit(std::forward<Callable>(callable)));
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

            std::lock_guard<std::mutex> lock(m_coroutines_map_mutex);
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
        std::lock_guard<std::mutex> lock(m_coroutines_map_mutex);

        auto socket_event_key = SocketEventKey{
            fd: fd,
            event_type: event_type,
        };

        if (m_coroutines_map.find(socket_event_key) != m_coroutines_map.end()) { 
            if (m_coroutines_map[socket_event_key].has_callback()) {
                LOG(m_l, logger::warn) << socket_event_key.event_type << " event handlers for fd" << 
                    socket_event_key.fd << " active while deregistering fd from coroutines map.";
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
            // Try destryoing the underlying coroutine handle, as scheduler is stopped
            // and it is guaranteed no work unit can be running. Therefore it should
            // be safe to destroy the coroutine.
            task.destroy();
            return;
        }
    
        if (coroutine_state->exception_ptr != nullptr) {
            std::rethrow_exception(coroutine_state->exception_ptr);
        }
    }

    FinalTask spawn_final_task(std::function<Task<void>()>&& awaitable) {
        co_await awaitable();
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

    acairo::Task<std::vector<char>> TCPStream::read(size_t number_of_bytes) {
        std::vector<char> result(number_of_bytes, 0);

        int remaining_buffer_size = number_of_bytes;
        char* current_buffer_ptr = result.data();

        while(remaining_buffer_size > 0){
            const int number_of_bytes_written = retry_sys_call(::read, m_fd, (void*)current_buffer_ptr, remaining_buffer_size);
            if (number_of_bytes_written < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    co_await ReadFuture(m_executor, m_fd);
                    continue;
                }

                throw std::runtime_error(error_with_errno("Unable to read from the socket"));
            }

            remaining_buffer_size -= number_of_bytes_written;
            current_buffer_ptr += number_of_bytes_written;
        }
        
        co_return result;
    }

    acairo::Task<void> TCPStream::write(std::vector<char>&& buffer) {
        int remaining_buffer_size = buffer.size();
        char* current_buffer_ptr = buffer.data();

        while(remaining_buffer_size > 0){
            const int number_of_bytes_written = retry_sys_call(::write, m_fd, (void*)current_buffer_ptr, remaining_buffer_size);
            if (number_of_bytes_written < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    co_await WriteFuture(m_executor, m_fd);
                    continue;
                }

                throw std::runtime_error(error_with_errno("Unable to write to the socket"));
            }

            remaining_buffer_size -= number_of_bytes_written;
            current_buffer_ptr += number_of_bytes_written;
        }
    }

    // Firstly, deregister fd from epoll and then close it
    // https://idea.popcount.org/2017-03-20-epoll-is-fundamentally-broken-22/
    TCPStream::~TCPStream() noexcept {
        LOG(m_l, logger::debug) << "Destructing fd.";

        // Deregistering a fd can throw, so in case it throws we just log the error and continue
        // as there is not much we can do about it and we want the program to continue
        try {
            m_executor->deregister_event_handler(m_fd, EVENT_TYPE::IN);
            m_executor->deregister_event_handler(m_fd, EVENT_TYPE::OUT);

            m_executor->deregister_fd(m_fd);
        } catch(const std::exception& e){
            LOG(m_l, logger::error) << "Unable to deregister fd: " << e.what() << ".";
        }

        // close won't block on non-blocking sockets
        if (retry_sys_call(::close, m_fd) < 0) {
            LOG(m_l, logger::error) << error_with_errno("Unable to close file descriptor");
        }
    }

    void TCPListener::bind(const std::string& address) {
        auto [ip_address, port] = split_address(address);

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            throw std::runtime_error(error_with_errno("Unable to add create socket"));
        }

        // Avoid spurious EADDRINUSE (previous instance of this server might have died)
        int opt = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            throw std::runtime_error(error_with_errno("Unable to set EADDRINUSE on a socket"));
        }

        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        inet_aton(ip_address.c_str(), &serv_addr.sin_addr);
        serv_addr.sin_port = htons(port);

        if (::bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            throw std::runtime_error(error_with_errno("Unable to bind socket to the address"));
        }

        m_listener_sockfd = sockfd;
    }

    void TCPListener::listen() const {
        if (m_listener_sockfd <= 0) {
            throw std::runtime_error("Listening socket was not bint to an endpoint.");
        }

        if (::listen(m_listener_sockfd, m_config.max_number_of_queued_conns) < 0) {
            throw std::runtime_error(error_with_errno("Unable to mark the socket as a listening socket"));
        }

        make_socket_non_blocking(m_listener_sockfd);

        m_executor->register_fd(m_listener_sockfd);        
    }

    Task<std::shared_ptr<TCPStream>> TCPListener::accept() const {
        if (m_listener_sockfd <= 0) {
            throw std::runtime_error("Listening socket was not initialized.");
        }

        int accepted_conn_fd = -1;
        while (accepted_conn_fd < 0) {
            struct sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);
            accepted_conn_fd = retry_sys_call(::accept, m_listener_sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
             if (accepted_conn_fd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    co_await AcceptFuture(m_executor, m_listener_sockfd);
                    continue;
                }

                // Even though m_stopped might be set, but the fd hasn't been closed yet and the error
                // received was something else, we throw TCPListenerStoppedError as the acceptor 
                // is expected to be stopped nonetheless.
                if (m_stopped) {
                    throw TCPListenerStoppedError();
                } else {
                    throw std::runtime_error(error_with_errno("Unable to accept new connection"));
                }
            }

            LOG(m_l, logger::debug) << "Accepted new connection: " << accepted_conn_fd << ".";
        }
        
        make_socket_non_blocking(accepted_conn_fd);
        
        co_return std::make_shared<TCPStream>(m_config.stream_config, accepted_conn_fd, m_executor);   
    }

    void TCPListener::stop() noexcept {
        LOG(m_l, logger::debug) << "Destructing listener fd.";

        m_stopped = true;

        // Deregistering a fd can throw, so in case it throws we just log the error and continue
        // as there is not much we can do about it and we want the program to continue
        try {
            m_executor->deregister_event_handler(m_listener_sockfd, EVENT_TYPE::IN);
            m_executor->deregister_event_handler(m_listener_sockfd, EVENT_TYPE::OUT);

            m_executor->deregister_fd(m_listener_sockfd);
        } catch(const std::exception& e){
            LOG(m_l, logger::error) << "Unable to deregister listener fd: " << e.what() << ".";
        }

        // close shouldn't block on non-blocking sockets
        if (retry_sys_call(::close, m_listener_sockfd) < 0) {
            LOG(m_l, logger::error) << error_with_errno("Unable to close file descriptor");
        }
    }

    TCPListener::~TCPListener() noexcept {
        if (!m_stopped) {
            stop();
        }
    }
}