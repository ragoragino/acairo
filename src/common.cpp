#include <sstream>
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <sys/types.h>
#include <fcntl.h>

#include "common.hpp"

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

    detail::Types::ID generate_id() {
        static thread_local std::mt19937 gen; 
        std::uniform_int_distribution<detail::Types::ID> distrib(0, std::numeric_limits<detail::Types::ID>::max());
        return distrib(gen);
    }
}  // namespace acairo