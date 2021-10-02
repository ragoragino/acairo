#pragma once 

#include <memory>

#include "common.hpp"
#include "tcpstream.hpp"
#include "task.hpp"

namespace acairo {
    struct TCPListenerConfiguration {
        TCPStreamConfiguration stream_config;
        int max_number_of_queued_conns;
    };

    class TCPListener {
        public:
            TCPListener(const TCPListenerConfiguration& config, std::shared_ptr<Executor> executor) 
                : m_stopped(false)
                , m_config(config)
                , m_executor(executor) 
                , m_l(logger::Logger().WithPair("Component", "TCPListener")) {}

            void bind(const std::string& address);
            
            void listen() const;

            Task<std::shared_ptr<TCPStream>> accept() const;

            void stop() noexcept;

            ~TCPListener();

        private:
            std::atomic_bool m_stopped;

            struct epoll_event m_accept_event;

            int m_listener_sockfd = -1;
            int m_epoll_fd = -1;

            const TCPListenerConfiguration m_config;
            std::shared_ptr<Executor> m_executor;

            const detail::Types::Logger m_l;
    };
}  // namespace acairo