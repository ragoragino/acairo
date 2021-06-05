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

// Use (void) to silent unused warnings.
#define assertm(exp, msg) assert(((void)msg, exp))

#include "cairo.h"

namespace cairo {
    std::pair<std::string, int> split_address(const std::string& full_address) {
        std::string portStr = "80";
        std::string address = full_address;
        
        auto const pos = full_address.find_last_of(':');
        if (pos != std::string::npos) {
            portStr = full_address.substr(pos+1);
            address = full_address.substr(0, pos);
        }

        int port = std::atoi(portStr.c_str());
        
        return { address, port };
    }

    void make_socket_non_blocking(int fd) {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags == -1) {
            throw std::runtime_error("Unable to get fd's flags.\n");
        }

        if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            throw std::runtime_error("Unable to set O_NONBLOCK on the socket.\n");
        }
    }

    std::vector<char> TCPStream::read(size_t number_of_bytes) {
        std::vector<char> result(number_of_bytes, 0);

        int remaining_buffer_size = number_of_bytes;
        char* current_buffer_ptr = result.data();

        while(remaining_buffer_size > 0){
            const int number_of_bytes_written = ::read(m_fd, (void*)current_buffer_ptr, remaining_buffer_size);
            if (number_of_bytes_written < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }

                std::stringstream ss{};
                ss << "Unable to read from the socket: " << strerror(errno) << ".\n";
                throw std::runtime_error(ss.str());
            }

            remaining_buffer_size -= number_of_bytes_written;
            current_buffer_ptr += number_of_bytes_written;
        }
        
        return result;
    }

    void TCPStream::write(std::vector<char>&& buffer) {
        int remaining_buffer_size = buffer.size();
        char* current_buffer_ptr = buffer.data();

        while(remaining_buffer_size > 0){
            const int number_of_bytes_written = ::write(m_fd, (void*)current_buffer_ptr, remaining_buffer_size);
            if (number_of_bytes_written < 0) {
                if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                    continue;
                }

                std::stringstream ss{};
                ss << "Unable to write to the socket: " << strerror(errno) << ".\n";
                throw std::runtime_error(ss.str());
            }

            remaining_buffer_size -= number_of_bytes_written;
            current_buffer_ptr += number_of_bytes_written;
        }
        
        return;
    }

    void TCPListener::bind(const std::string& address) {
        auto [ip_address, port] = split_address(address);

        int sockfd = socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) {
            std::stringstream ss{};
            ss << "Unable to add create socket: " << strerror(errno) << ".\n";
            throw std::runtime_error(ss.str());
        }

        // Avoid spurious EADDRINUSE (previous instance of this server might have died)
        int opt = 1;
        if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
            std::stringstream ss{};
            ss << "Unable to set EADDRINUSE on a socket: " << strerror(errno) << ".\n";
            throw std::runtime_error(ss.str());
        }

        struct sockaddr_in serv_addr;
        memset(&serv_addr, 0, sizeof(serv_addr));
        serv_addr.sin_family = AF_INET;
        inet_aton(ip_address.c_str(), &serv_addr.sin_addr);
        serv_addr.sin_port = htons(port);

        if (::bind(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
            std::stringstream ss{};
            ss << "Unable to bind socket to the address " << address << ": " << strerror(errno) << ".\n";
            throw std::runtime_error(ss.str());
        }

        if (listen(sockfd, m_config.max_number_of_queued_conns) < 0) {
            std::stringstream ss{};
            ss << "Unable to mark the socket as a listening socket: " << strerror(errno) << ".\n";
            throw std::runtime_error(ss.str());
        }

        make_socket_non_blocking(sockfd);

        m_listener_sockfd = sockfd;
    }

    void TCPListener::start_epoll_listener() {
        using namespace std::chrono_literals;

        // Setup epoll
        auto epoll_fd = epoll_create1(0);
        if (epoll_fd < 0) {
            std::stringstream ss{};
            ss << "Unable to create a fd with epoll: " << strerror(errno) << ".\n";;
            throw std::runtime_error(ss.str());
        }

        struct epoll_event accept_event;
        accept_event.events = EPOLLIN;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, m_listener_sockfd, &accept_event) < 0) {
            std::stringstream ss{};
            ss << "Unable to add listener socket to the epoll interest list: " << strerror(errno) << ".\n";
            throw std::runtime_error(ss.str());
        }

        struct epoll_event* events = (struct epoll_event*)calloc(m_config.max_number_of_fds, 
            sizeof(struct epoll_event));
        if (events == NULL) {
            throw std::runtime_error("Unable to allocate memory for epoll_events");
        }

        std::unique_lock<std::mutex> events_lock(m_events_mutex, std::defer_lock);
        while (!m_stopped) {
            // Check how many events can be accepted. If no events, 
            // then wait for the consumers to accept some of the waiting connections.
            int events_buffer_size;
            while (true) {
                events_lock.lock();
                events_buffer_size = m_config.max_number_of_fds - m_events_count;
                events_lock.unlock();

                assertm(events_buffer_size >= 0, "Space available for events is negative.");

                if (events_buffer_size != 0) {
                    break;
                }
                
                std::this_thread::sleep_for(100ms);
            }   

            // Get new events from epoll
            int count_of_ready_fds = epoll_wait(epoll_fd, events, events_buffer_size, 100);
            if (count_of_ready_fds < 0) {
                std::stringstream ss{};
                ss << "Waititing for epoll_events failed: " << strerror(errno) << ".\n";
                throw std::runtime_error(ss.str());
            }

            // Check how many new connections are waiting to be accepted
            int count_of_valid_fs = 0;
            for (int i = 0; i < count_of_ready_fds; i++) {
                if (events[i].events & EPOLLERR) {
                    // TODO
                    std::stringstream ss{};
                    ss << "epoll_wait returned EPOLLERR: " << strerror(errno) << ".\n";
                    throw std::runtime_error(ss.str());
                }

                count_of_valid_fs++;
            }

            // Notify waiting threads
            events_lock.lock();
            m_events_count += count_of_valid_fs;
            events_lock.unlock();

            m_events_cv.notify_all();
        }
    }

    TCPStream TCPListener::accept(std::chrono::seconds timeout) {
        if (m_listener_sockfd <= 0) {
            std::runtime_error("Listening socket was not initialized.");
        }

        // Lazily initialize thread waiting for epoll events
        if (auto lock = std::unique_lock<std::mutex>(m_epoll_thread_mutex); !m_epoll_thread.joinable()) {
            this->start_epoll_listener();
        }

        // Wait for a new event, if there is no available
        if(auto lock = std::unique_lock<std::mutex>(m_events_mutex); m_events_count <= 0 ){
            assertm(m_events_count == 0, "Count of events is negative");

            if (!m_events_cv.wait_for(lock, timeout, [this]{ return m_events_count > 0; })) {
                throw DeadlineExceededError();
            }

            m_events_count--;
        }
        
        // Accept new connection
        struct sockaddr_in peer_addr;
        socklen_t peer_addr_len = sizeof(peer_addr);
        int newsockfd = ::accept(m_listener_sockfd, (struct sockaddr*)&peer_addr, &peer_addr_len);
        if (newsockfd < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // This can happen due to the nonblocking socket mode; in this
                // case don't do anything, but print a notice (since these events
                // are extremely rare and interesting to observe...)                       
                std::cout << "Accepting new connection failed due to EAGAIN or EWOULDBLOCK.\n";
            } else {
                std::stringstream ss{};
                ss << "Accepting new connection failed: " << strerror(errno) << ".\n";
                throw std::runtime_error(ss.str());
            }

            // TODO: What to return here? Probably loop over.
        }

        // TODO: Add later
        // set_non_blocking_on_socket(newsockfd);
        return TCPStream(m_config.stream_config, newsockfd);   
    }

    void TCPListener::shutdown(std::chrono::seconds) {
        // TODO: Honor timeout if possible
        if (auto lock = std::unique_lock<std::mutex>(m_epoll_thread_mutex); !m_epoll_thread.joinable()) {
            m_stopped = true;
            m_epoll_thread.join();
        }
    }

   
    void Executor::spawn(Task&&) {
       // TODO     
    }

    void Executor::stop() {
        // TODO
    }

    void Executor::start_thread() {
        std::unique_lock<std::mutex> lock(m_work_queue_mutex, std::defer_lock);
        while (!m_stopped) {
            lock.lock();
            if (m_work_queue.size() == 0) {
                bool timed_out = m_work_queue_cv.wait_for(lock, m_work_waiting_timeout, [this]{ return m_work_queue.size() > 0; });
                if (timed_out) {
                    continue;
                }
            }

            auto work_unit = m_work_queue.back();
            m_work_queue.pop();
            lock.unlock();

            try {
                work_unit();
            } catch(std::exception) {
                work_unit.set_exception(std::current_exception());
            }
        }
    }
}
