#include <cstring>
#include <cstdlib>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <sstream>
#include <iostream>
#include <cassert>
#include <optional>

// Use (void) to silent unused warnings.
#define assertm(exp, msg) assert(((void)msg, exp))

#include "acairo.h"

namespace acairo {
    std::string error_with_errno(const std::string& message){
        std::stringstream ss{};
        ss << message << ": " << strerror(errno) << ".";
        return ss.str();
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
    int retry_sys_call(F&& f, Args&&... args) {
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
        auto l = logger::Logger();

        int error = 0;
        socklen_t errlen = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen) == 0) {
            LOG(l, logger::error) << error_with_errno("Error occured on a socket: ");
        } else {
            LOG(l, logger::error) << "Error occured on a socket, but unable to get socket error.";
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
            throw std::runtime_error(error_with_errno("Unable to set O_NONBLOCK on the socket."));
        }
    }

    void Scheduler::spawn(Scheduler::WorkUnit&& work_unit) {
        {
            std::lock_guard<std::mutex> lock(m_work_queue_mutex);
            m_work_queue.push(std::move(work_unit));
        }

        m_work_queue_cv.notify_one();
    };

    void Scheduler::stop() {
        m_stopped = true;
        m_work_queue_cv.notify_all();

        for (auto& worker : m_threadPool) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void Scheduler::start_worker() {
        while (!m_stopped) {
            auto work_unit = this->get_new_work_unit();

            try {
                if (work_unit) {
                    work_unit->operator()();
                    LOG(m_l, logger::debug) << "Successfully finished work unit: " << work_unit->get_id();
                }
            } catch(const std::exception& e) {
                LOG(m_l, logger::error) << "Work unit [" << work_unit->get_id() << "] failed with an error: " << e.what();
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
    : m_scheduler(std::make_unique<Scheduler>(config.scheduler_config))
    , m_config(config)
    , m_l(logger::Logger().WithPair("Component", "Executor")) {
        m_epoll_fd = epoll_create1(0);
        if (m_epoll_fd < 0) {
            throw std::runtime_error(error_with_errno("Unable to create a fd with epoll"));
        }

        m_epoll_thread = std::thread(&Executor::run_epoll_listener, this);
    }

    template<typename Callable>
    void Executor::register_event_handler(int fd, EVENT_TYPE event_type, Callable&& callable) {
        auto socket_event_key = SocketEventKey{
            fd: fd,
            event_type: event_type,
        };

        LOG(m_l, logger::debug) << "Registering callback for fd [" << fd << "] and event [" << event_type << "].";

        std::lock_guard<std::mutex> lock(m_coroutines_map_mutex);
        
        // If there is a coroutine waiting for the fd, it must have been already registered with epoll.
        // Otherwise, register it.
        if (m_coroutines_map.find(socket_event_key) == m_coroutines_map.end()) {  
            LOG(m_l, logger::debug) << "Adding fd [" << fd << "] and event [" << event_type << "] to the epoll interest list.";

            // We need an edge-triggered notifications as we will be invoking handlers based on incoming events 
            struct epoll_event accept_event;
            accept_event.events = get_epoll_event_type(socket_event_key.event_type) | EPOLLET;
            accept_event.data.fd = socket_event_key.fd;
            if (retry_sys_call(epoll_ctl, m_epoll_fd, EPOLL_CTL_ADD, socket_event_key.fd, &accept_event) < 0) {
                throw std::runtime_error(error_with_errno("Unable to add new socket to the epoll interest list"));
            }
        }

        m_coroutines_map[socket_event_key].push_back(Scheduler::WorkUnit(std::forward<Callable>(callable)));
    }

    void Executor::run_epoll_listener() {
        struct epoll_event* events = (struct epoll_event*)calloc(m_config.max_number_of_fds, 
            sizeof(struct epoll_event));
        if (events == NULL) {
            throw std::runtime_error("Unable to allocate memory for epoll_events");
        }

        auto schedule_ready_tasks = [this](const SocketEventKey& event_key){
            auto& tasks = m_coroutines_map[event_key];
            for (auto it = tasks.begin(); it != tasks.end(); it++) {
                m_scheduler->spawn(Scheduler::WorkUnit(std::move(*it)));
            }

            tasks.clear();
        };

        while (!m_stopped) {
            // TODO: Make epoll_wait waiting time configurable
            int count_of_ready_fds = retry_sys_call(epoll_wait, m_epoll_fd, events, m_config.max_number_of_fds, 10);
            if (count_of_ready_fds < 0) {
                throw std::runtime_error(error_with_errno("Waititing for epoll_events failed"));
            }

            std::lock_guard<std::mutex> lock(m_coroutines_map_mutex);
            for (int i = 0; i < count_of_ready_fds; i++) {
                int fd = events[i].data.fd;

                // In case of a socket error, let the handlers finish their work
                if (events[i].events & EPOLLERR) {
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
                if (events[i].events & EPOLLIN) {
                    LOG(m_l, logger::debug) << "Adding handler for a fd " << fd << " and event_type " 
                        << EVENT_TYPE::IN << " to the scheduler's queue.";

                    SocketEventKey event_key{fd, EVENT_TYPE::IN};
                    schedule_ready_tasks(event_key);
                }

                if (events[i].events & EPOLLOUT) {
                    LOG(m_l, logger::debug) << "Adding handler for a fd " << fd << " and event_type " 
                        << EVENT_TYPE::OUT << " to the scheduler's queue.";

                    SocketEventKey event_key{fd, EVENT_TYPE::OUT};
                    schedule_ready_tasks(event_key);
                }
            }
        }
    }

    void Executor::deregister_fd(int fd){
        bool fd_was_registered = false;

        // Firstly remove all the registered handlers associated with the fd
        {
            std::lock_guard<std::mutex> lock(m_coroutines_map_mutex);

            auto socket_event_key = SocketEventKey{
                fd: fd,
                event_type: EVENT_TYPE::IN,
            };

            if (m_coroutines_map.find(socket_event_key) != m_coroutines_map.end()) { 
                fd_was_registered = true;

                if (m_coroutines_map[socket_event_key].size() != 0) {
                    LOG(m_l, logger::warn) << "In event handlers active while deregistering fd from epoll.";
                }

                m_coroutines_map.erase(socket_event_key);
            }

            socket_event_key.event_type = EVENT_TYPE::OUT;
            if (m_coroutines_map.find(socket_event_key) != m_coroutines_map.end()) { 
                fd_was_registered = true;

                if (m_coroutines_map[socket_event_key].size() != 0) {
                    LOG(m_l, logger::warn) << "Out event handlers active while deregistering fd from epoll.";
                }

                m_coroutines_map.erase(socket_event_key);
            };
        }
        
        // We remove the fd from the epoll interest list if it was added there before
        if (fd_was_registered) {
            LOG(m_l, logger::debug) << "Removing fd " << fd << " from the epoll interest list.";

            // In kernel versions before 2.6.9, the EPOLL_CTL_DEL operation required a non-null pointer in event, even though this argument
            // is ignored. Since Linux 2.6.9, event can be specified as NULL when using EPOLL_CTL_DEL.
            if (retry_sys_call(epoll_ctl, m_epoll_fd, EPOLL_CTL_DEL, fd, (struct epoll_event *)nullptr) < 0) {
                throw std::runtime_error(error_with_errno("Unable to deregister fd from the epoll interest list"));
            }
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

    acairo::Task<std::vector<char>> TCPStream::read(size_t number_of_bytes) {
        using namespace std::chrono_literals;

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
        using namespace std::chrono_literals;

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

    TCPStream::~TCPStream() {
        LOG(m_l, logger::debug) << "Destructing fd.";

        // Deregistering a fd can throw, so in case it throws we just log the error and continue
        // as there is not much we can do about it and we (probably?) want the program to continue
        try {
            m_executor->deregister_fd(m_fd);
        } catch(const std::exception& e){
            LOG(m_l, logger::error) << "Unable to deregister fd: " << e.what() << ".";
        }

        // close shouldn't block on non-blocking sockets
        int result = retry_sys_call(::close, m_fd);
        if (result < 0) {
            LOG(m_l, logger::error) << "Unable to close file descriptor: " << strerror(errno) << ".";
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

        if (listen(sockfd, m_config.max_number_of_queued_conns) < 0) {
            throw std::runtime_error(error_with_errno("Unable to mark the socket as a listening socket"));
        }

        make_socket_non_blocking(sockfd);

        m_listener_sockfd = sockfd;
    }

    void TCPListener::run_epoll_listener() {
        using namespace std::chrono_literals;

        auto epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            throw std::runtime_error(error_with_errno("Unable to create a fd with epoll"));
        }

        struct epoll_event accept_event;
        accept_event.events = EPOLLIN;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, m_listener_sockfd, &accept_event) < 0) {
            throw std::runtime_error(error_with_errno("Unable to add listener socket to the epoll interest list"));
        }

        struct epoll_event* events = (struct epoll_event*)calloc(m_config.max_number_of_fds, 
            sizeof(struct epoll_event));
        if (events == NULL) {
            throw std::runtime_error("Unable to allocate memory for epoll_events");
        }

        while (!m_stopped) {    
            // In case consumer threads arer slow, wait for them until they
            // process at least some of the accepted connections
            bool consumers_throttled = false;
            {
                std::lock_guard<std::mutex> lock(m_accepted_conns_mutex); 
                assertm(m_config.max_number_of_fds >= m_accepted_conns.size(), "Limit of accepted connections was exceeded.");
                consumers_throttled = m_config.max_number_of_fds == m_accepted_conns.size();
            }
            
            if (consumers_throttled) {
                // TODO: Make this configurable
                std::this_thread::sleep_for(50ms); 
                continue;
            }

            int count_of_ready_fds = retry_sys_call(epoll_wait, epoll_fd, events, m_config.max_number_of_fds, 100);
            if (count_of_ready_fds < 0) {
                throw std::runtime_error(error_with_errno("Waititing for epoll_events failed"));
            }

            int processed_conns_count = process_waiting_connections(events, count_of_ready_fds);
            if (processed_conns_count == 0) {
                continue;
            }

            LOG(m_l, logger::debug) << "Number of accepted connections pushed to the buffer: " << processed_conns_count << ".";

            m_accepted_conns_cv.notify_all();
        }
    }

    int TCPListener::process_waiting_connections(struct epoll_event* events, int waiting_conns_count) {
        std::lock_guard<std::mutex> lock(m_accepted_conns_mutex);

        size_t original_conns_count = m_accepted_conns.size();

        for (int i = 0; i < waiting_conns_count; i++) {
            if (events[i].events & EPOLLERR) {
                log_socket_error(events[i].data.fd);
                continue;
            }

            if (m_accepted_conns.size() == m_config.max_number_of_fds) {
                return m_accepted_conns.size() - original_conns_count;
            }
            
            struct sockaddr_in peer_addr;
            socklen_t peer_addr_len = sizeof(peer_addr);
            int newsockfd = retry_sys_call(::accept, m_listener_sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
            if (newsockfd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    LOG(m_l, logger::debug) << "EAGAIN or EWOULDBLOCK after accepting a new connection. Going to wait for new connection.";
                } else {
                    throw std::runtime_error("Accepting new connection failed");
                }
            }

            LOG(m_l, logger::debug) << "Accepted new connection: " << newsockfd << ".";

            m_accepted_conns.push(newsockfd);
        }

        return m_accepted_conns.size() - original_conns_count;
    }

    std::shared_ptr<TCPStream> TCPListener::accept() {
        if (m_listener_sockfd <= 0) {
            throw std::runtime_error("Listening socket was not initialized.");
        }

        // Lazily initialize thread waiting for epoll events
        if (auto lock = std::lock_guard<std::mutex>(m_epoll_thread_mutex); !m_epoll_thread.joinable()) {
            m_epoll_thread = std::thread(&TCPListener::run_epoll_listener, this);
        }
        
        // Take new connection from the queue of accepted connections
        std::unique_lock<std::mutex> lock(m_accepted_conns_mutex);
        if (m_accepted_conns.size() == 0) {
            m_accepted_conns_cv.wait(lock, [this]{ return m_accepted_conns.size() > 0 || m_stopped; });
            if (m_stopped) {
                throw TCPListenerStoppedError();
            }
        }

        int accepted_conn_fd = m_accepted_conns.front();
        m_accepted_conns.pop();
        
        make_socket_non_blocking(accepted_conn_fd);
        
        return std::make_shared<TCPStream>(m_config.stream_config, accepted_conn_fd, m_executor);   
    }

    void TCPListener::shutdown() {
        m_stopped = true;
        m_accepted_conns_cv.notify_all();

        std::lock_guard<std::mutex> lock(m_epoll_thread_mutex); 
        if (m_epoll_thread.joinable()) {
            m_epoll_thread.join();
        }
    }
}