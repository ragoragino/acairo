#include "logger.hpp"

#include <iomanip>
#include <iostream>
#include <mutex>

/*
There is no serious logging library in the whole fucking C++ 
that would provide features as adding attributes, multi-threaded safety,
reasonable performance, and be written in sensible C++.
Therefore, we need to roll our own mini-library here.
*/

namespace logger {

    SeverityLevel SeverityLevelFromString(const std::string& level) {
        if (level == "trace") {
            return trace;
        } else if (level == "debug") {
            return debug;
        } else if (level == "info") {
            return info;
        } else if (level == "warn") {
            return warn;
        } else if (level == "error") {
            return error;
        } else if (level == "critical") {
            return critical;
        } else {
            throw std::runtime_error("Unrecognized logger level: " + level + ".");
        }
    }

    Configuration::Configuration(std::string level) { 
        log_level = SeverityLevelFromString(level); 
    };

    std::ostream& operator<<(std::ostream& stream, SeverityLevel level) {
        static const char* strings[] = {"trace", "debug", "info", "warn", "error", "critical"};

        if (static_cast<size_t>(level) < sizeof(strings) / sizeof(*strings)) {
            stream << strings[level];
        } else {
            stream << static_cast<size_t>(level);
        }

        return stream;
    }

    void InitializeLogger(const Configuration& configuration) {
        m_global_config = configuration;
    }

    LogStream::~LogStream() {
        if (m_noop) {
            return;
        }

        std::string delimiter = "";
        if (!m_metadata.empty()) {
            delimiter = ": ";
        }

        std::lock_guard<std::mutex> lock(stdout_mutex);
        std::cout << m_metadata << delimiter << m_message.str() << "\n";
    }

}  // namespace logger
