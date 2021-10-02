#pragma once 

#include <memory>

#include "common.hpp"
#include "task.hpp"
#include "executor.hpp"

namespace acairo {
    struct TCPStreamConfiguration {};

    class TCPStream {
        public:
            TCPStream(const TCPStreamConfiguration& config, int fd, std::shared_ptr<Executor> executor) 
                : m_fd(fd)
                , m_config(config)
                , m_executor(executor)
                , m_l(logger::Logger().WithPair("Component", "TCPStream").WithPair("fd", fd)) {
                    m_executor->register_fd(fd);
                }

            // TODO: An improvement could be to allow setting read timeouts
            acairo::Task<std::vector<char>> read(size_t number_of_bytes);

            // TODO: An improvement could be to allow setting write timeouts
            acairo::Task<void> write(std::vector<char>&& buffer);

            ~TCPStream() noexcept;

        private:
            const int m_fd;
            const TCPStreamConfiguration m_config;
            std::shared_ptr<Executor> m_executor;

            const detail::Types::Logger m_l;
    };
}  // namespace acairo