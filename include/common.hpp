#pragma once

#include <string>
#include <random>
#include <condition_variable>

#include "logger.hpp"

// Use (void) cast to silent unused warnings.
#define assertm(exp, msg) assert(((void)msg, exp))

namespace acairo {
    // We define all types commonly used at the namespace level, so they
    // don't need to be defined in each separate class and we can easily 
    // change them when needed.
    namespace detail {
        struct Types {
            using Logger = logger::Logger<>;
            using ID = uint64_t;
        };
    }  // namespace detail

    enum class EVENT_TYPE : uint8_t {
        IN,
        OUT,
    };

    std::ostream& operator<<(std::ostream& os, const EVENT_TYPE& event_type);

    // Create an error message containing stringified errno
    std::string error_with_errno(const std::string& message) noexcept;

    struct TCPListenerStoppedError : public std::exception {
        const char * what () const noexcept {
            return "TCPListener was stopped.";
        }
    };

    struct CancelledError : public std::exception {
        public:
            CancelledError(const std::string& msg) 
            : m_what(msg) {}

            const char * what () const noexcept {
                return m_what.c_str();
            }

        private:
            std::string m_what;
    };

    detail::Types::ID generate_id();

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

    // SynchronizedWaiter is a helper struct that can used to lock 
    // object that need thread synchronization and notify different handlers 
    // waiting for events related to the locked object.
    struct SynchronizedWaiter {
        std::mutex m;
        std::condition_variable cv;
    };
}  // namespace acairo