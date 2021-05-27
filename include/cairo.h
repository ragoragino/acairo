#include <functional>
#include <vector>
#include <string>
#include <chrono>
#include <cstdint>

namespace cairo {
    struct TCPStreamConfiguration {
        std::chrono::seconds read_timeout;
        std::chrono::seconds write_timeout;
    };

    class TCPStream {
        public:
            TCPStream(const TCPStreamConfiguration& config) : m_config(config) {}

            std::vector<char> read(size_t number_of_bytes);

            void write(std::vector<char>&& buffer);
        private:
            const TCPStreamConfiguration m_config;
    };

     struct TCPListenerConfiguration {
        TCPStreamConfiguration stream_config;
    };

    class TCPListener {
        public:
            TCPListener(const TCPListenerConfiguration& config) : m_config(config) {}

            void bind(const std::string& address);

            TCPStream accept();

            void shutdown(std::chrono::seconds timeout);

        private:
            const TCPListenerConfiguration m_config;
    };

    class Awaitable {
        public:
            Awaitable(std::function<void()>&& func) : m_func(func) {}

        private:
            const std::function<void()> m_func;
    };

    struct ExecutorConfiguration {
        std::uint8_t number_of_worker_threads;
    };

    class Executor {
        public:
            Executor(const ExecutorConfiguration& config) : m_config(config) {}

            void spawn(Awaitable&& awaitable);

            void stop();

        private:
            const ExecutorConfiguration m_config;
    };
}