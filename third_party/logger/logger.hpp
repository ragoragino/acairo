#pragma once

#include <string>
#include <unordered_map>
#include <sstream>
#include <mutex>
#include <memory>
#include <iostream>

namespace logger {

    enum SeverityLevel { trace, debug, info, warn, error, critical };

    struct Configuration {
        Configuration() : log_level(debug) {}

        Configuration(std::string level);

        SeverityLevel log_level;
    };

    // Mutex protecting stdout
    static std::mutex stdout_mutex;

    // Global log config
    // Not protected by lock!
    // TODO: why doesn't this work? 
    static Configuration m_global_config;

    // TODO: Use syncstream here
    class LogStream {
        public:
            LogStream(bool noop, std::string&& metadata) 
                : m_noop(noop)
                , m_metadata(std::move(metadata)) {}

            template<typename T>
            LogStream& operator<<(T&& elem) {
                if (m_noop) {
                    return *this;
                }

                m_message << std::forward<T>(elem);

                return *this;
            }

            ~LogStream();

        private:
            const bool m_noop = false;

            std::stringstream m_message;

            std::string m_metadata;
    };

    class BasicFormatter {
        public:
            BasicFormatter() = default;

            template<typename T>
            std::string FormatPair(std::string&& key, T&& value) const {
                std::stringstream ss;
                ss << "(" << key << ": " << value << ")";  

                return ss.str();
            }
    };

    template<typename Formatter = BasicFormatter>
    struct Logger {
        public:
            Logger()
                : m_config(m_global_config)
                , m_formatter(std::make_shared<BasicFormatter>()) {
                    // auto config_ptr = &m_global_config;

                    std::cout << this << "\n";
                }

            explicit Logger(const Configuration& config) 
                : m_config(config)
                , m_formatter(std::make_shared<BasicFormatter>()) {
                }

            Logger(const Configuration& config, std::shared_ptr<Formatter> formatter) 
                : m_config(config)
                , m_formatter(formatter) {
                }

            Logger(const Configuration& config, std::shared_ptr<Formatter> formatter, 
                std::string&& metadata) 
                : m_config(config)
                , m_formatter(formatter)
                , m_metadata(std::move(metadata)) {}

            Logger(const Logger& l) = default;

            LogStream Log(SeverityLevel severity) const {
                if (severity < m_config.log_level) {
                    return LogStream(true, "");
                }

                std::string metadata = m_metadata + " " + m_formatter->FormatPair("Severity", severity);

                return LogStream(false, std::move(metadata));
            }
            
            template<typename T>
            Logger WithPair(std::string&& key, T&& value) {
                std::string formatted_pair = m_formatter->FormatPair(std::move(key), std::move(value));

                std::string metadata = m_metadata + " " + formatted_pair;

                return Logger(m_config, m_formatter, std::move(metadata));
            }

        private:
            const Configuration m_config;
            std::shared_ptr<Formatter> m_formatter;

            std::string m_metadata;
    };

    // Must be called only once and before any Logger constructors.
    void InitializeLogger(const Configuration& config);

#define LOG(logger, severity) logger.Log(severity)
}  // namespace logger