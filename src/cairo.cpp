#include "cairo.h"

namespace cairo {
    std::vector<char> TCPStream::read(size_t number_of_bytes) {
        // TODO
        return {};
    }

    void TCPStream::write(std::vector<char>&& buffer) {
        // TODO
        return;
    }

    void TCPListener::bind(const std::string& address) {
        // TODO
    }

    TCPStream TCPListener::accept() {
        // TODO
        return TCPStream(m_config.stream_config);
    }

    void TCPListener::shutdown(std::chrono::seconds timeout) {
        // TODO
    }

    void Executor::spawn(Awaitable&& awaitable) {
        // TODO
    }

    void Executor::stop() {
        // TODO
    }
}
