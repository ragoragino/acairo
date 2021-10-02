#pragma once

#include <cstdint>
#include <functional>
#include <thread>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <optional>
#include <vector>

#include "common.hpp"

namespace acairo {
    struct SchedulerConfiguration {
        std::uint8_t number_of_worker_threads;
    };

    class Scheduler {
        public:
            class WorkUnit {
                public:
                    using id_type = detail::Types::ID;

                    explicit WorkUnit(const std::function<void()>& work_unit) 
                    : m_id(generate_id())
                    , m_work_unit(work_unit) {}

                    explicit WorkUnit(std::function<void()>&& work_unit) 
                    : m_id(generate_id())
                    , m_work_unit(std::move(work_unit)) {}

                    id_type get_id() const noexcept {
                        return m_id;
                    }

                    void operator()() {
                        m_work_unit();
                    }

                private:
                    const detail::Types::ID m_id;

                    std::function<void()> m_work_unit;
            };

            Scheduler(const SchedulerConfiguration& config) 
                : m_threadPool(config.number_of_worker_threads)
                , m_stopped(false)
                , m_paused(false)
                , m_notify_on_worker_waiting(false)
                , m_waiting_workers_count(0)
                , m_config(config)
                , m_l(logger::Logger().WithPair("Component", "Scheduler")) {
                    for (std::uint8_t i = 0; i != m_config.number_of_worker_threads; i++) {
                        m_threadPool[i] = std::thread(&Scheduler::run_worker, this);
                    }
            }

            void spawn(WorkUnit&& handler);

            void pause();

            void restart();

            void stop(bool drain);

        private:
            void run_worker() noexcept;

            std::optional<WorkUnit> get_new_work_unit();
            
            std::vector<std::thread> m_threadPool;

            bool m_stopped;
            bool m_paused;
            std::condition_variable m_paused_cv;
            bool m_notify_on_worker_waiting;
            int m_waiting_workers_count;

            std::queue<WorkUnit> m_work_queue;
            std::mutex m_work_queue_mutex;
            std::condition_variable m_work_queue_cv;

            const SchedulerConfiguration m_config;

            const detail::Types::Logger m_l;
    };
}  // namespace acairo