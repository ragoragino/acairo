#include "tcpstream.hpp"

#include <unistd.h>

#include "promise.hpp"

namespace acairo {
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
}  // namespace acairo