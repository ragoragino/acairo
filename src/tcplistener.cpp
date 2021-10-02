#include "tcplistener.hpp"

#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "promise.hpp"

namespace acairo {
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
}  // namespace acairo