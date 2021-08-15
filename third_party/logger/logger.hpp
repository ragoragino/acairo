#pragma once

#include <string>
#include <unordered_map>
#include <sstream>
#include <mutex>
#include <memory>
#include <iostream>
#include <iomanip>
#include <iostream>
#include <chrono>

namespace logger {

    enum SeverityLevel { trace, debug, info, warn, error, critical };

    std::ostream& operator<<(std::ostream& stream, const SeverityLevel& level) noexcept;

    struct Configuration {
        Configuration() : log_level(info) {}

        explicit Configuration(const std::string& level);

        SeverityLevel log_level;
    };

    Configuration GetGlobalConfiguration() noexcept;

    // Must be called only once and before any Logger constructors.
    void InitializeGlobalLogger(const Configuration& config);

    namespace detail {
        // Used internally to fetch the global mutex protecting standard time functions.
        std::mutex& GetGlobalTimeMutex();
    } // namespace detail
    
    // TODO: Use C++ 20 syncstream here instead of custom lock, when it will be properly supported.
    class LogStream {
        public:
            LogStream(bool noop, std::string&& metadata) noexcept
                : m_noop(noop)
                , m_metadata(std::move(metadata)) {}

            template<typename T>
            LogStream& operator<<(T&& elem) noexcept {
                if (m_noop) {
                    return *this;
                }

                m_message << std::forward<T>(elem);

                return *this;
            }

            ~LogStream() noexcept;

        private:
            const bool m_noop = false;

            std::stringstream m_message;

            const std::string m_metadata;
    };

    class BasicFormatter {
        public:
            BasicFormatter() noexcept = default;

            template<typename T>
            std::string FormatPair(std::string&& key, T&& value) const {
                std::stringstream ss;
                ss << "(" << std::move(key) << ": " << std::forward<T>(value) << ")";  

                return ss.str();
            }

            std::string FormatTime(std::string&& key) {
                std::stringstream ss;

                auto now = std::chrono::system_clock::now();
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

                std::lock_guard<std::mutex> lock(detail::GetGlobalTimeMutex());
                auto time = std::time(nullptr);
                ss << "(" << key << ": " << std::put_time(std::gmtime(&time), "%Y-%m-%d %T") << "." 
                    << std::setfill('0') << std::setw(3) << ms.count() << ')';

                return ss.str();
            }
    };

    template<typename Formatter = BasicFormatter>
    struct Logger {
        public:
            Logger() noexcept
                : m_config(GetGlobalConfiguration())
                , m_formatter(std::make_shared<BasicFormatter>()) {}

            explicit Logger(const Configuration& config) noexcept
                : m_config(config)
                , m_formatter(std::make_shared<BasicFormatter>()) {}

            Logger(const Configuration& config, std::shared_ptr<Formatter> formatter) noexcept
                : m_config(config)
                , m_formatter(formatter) {}

            Logger(const Configuration& config, std::shared_ptr<Formatter> formatter, 
                std::string&& metadata) noexcept
                : m_config(config)
                , m_formatter(formatter)
                , m_metadata(std::move(metadata)) {}

            Logger(const Logger& l) = default;

            LogStream Log(SeverityLevel severity) const noexcept {
                std::string metadata = "";

                try {
                    if (severity < m_config.log_level) {
                        return LogStream(true, std::move(metadata));
                    }

                    metadata = m_formatter->FormatPair("Severity", severity) +
                        " " +
                        m_formatter->FormatTime("Time") +
                        " " + 
                        m_metadata;
                } catch (const std::exception& e) {
                    std::cout << "Unable to construct a LogStream: " << e.what() << ".";
                    return LogStream(true, std::move(metadata));
                }
                
                return LogStream(false, std::move(metadata));
            }
            
            template<typename T>
            Logger WithPair(std::string&& key, T&& value) const {
                std::string formatted_pair = m_formatter->FormatPair(std::move(key), std::forward<T>(value));

                std::string delimiter = "";
                if (!m_metadata.empty()) {
                    delimiter = " ";
                }

                std::string metadata = m_metadata + delimiter + formatted_pair;

                return Logger(m_config, m_formatter, std::move(metadata));
            }

        private:
            const Configuration m_config;
            std::shared_ptr<Formatter> m_formatter;

            std::string m_metadata;
    };

#define LOG(logger, severity) logger.Log(severity)
}  // namespace logger