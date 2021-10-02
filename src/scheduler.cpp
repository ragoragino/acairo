#include "scheduler.hpp"

namespace acairo {
    void Scheduler::spawn(Scheduler::WorkUnit&& work_unit) {
        {
            std::lock_guard<std::mutex> lock(m_work_queue_mutex);
            m_work_queue.push(std::move(work_unit));
        }

        m_work_queue_cv.notify_one();
    }

    void Scheduler::pause() {
        std::unique_lock<std::mutex> lock(m_work_queue_mutex);
        m_paused = true;
        m_notify_on_worker_waiting = true;
        m_paused_cv.wait(lock, [this](){
            return m_waiting_workers_count == m_config.number_of_worker_threads;
        });
        m_notify_on_worker_waiting = false;
    }

    void Scheduler::restart(){
        {
            std::lock_guard<std::mutex> lock(m_work_queue_mutex);
            m_paused = false;
        }

        m_work_queue_cv.notify_all();
    }

    void Scheduler::stop(bool drain) {
        {
            std::unique_lock<std::mutex> lock(m_work_queue_mutex);
            if (drain && m_work_queue.size() != 0) {
                m_notify_on_worker_waiting = true;
                m_paused_cv.wait(lock, [this](){
                    return m_work_queue.size() == 0;
                });
                m_notify_on_worker_waiting = false;
            }

            m_stopped = true;
        }

        m_work_queue_cv.notify_all();

        for (auto& worker : m_threadPool) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void Scheduler::run_worker() noexcept {
        while (!m_stopped) {
            WorkUnit::id_type work_id{};

            try {
                auto work_unit = this->get_new_work_unit();

                if (work_unit) {
                    work_id = work_unit->get_id();
                    
                    work_unit->operator()();
                    LOG(m_l, logger::debug) << "Successfully finished work unit: " << work_id;
                }
            } catch (const CancelledError& e) {
                LOG(m_l, logger::debug) << "Work unit [" << work_id << "] got cancelled: " << e.what();
            } catch (const std::exception& e) {
                LOG(m_l, logger::error) << "Work unit [" << work_id << "] failed with an error: " << e.what();
            }
        }
    }

    std::optional<Scheduler::WorkUnit> Scheduler::get_new_work_unit() {
        std::unique_lock<std::mutex> lock(m_work_queue_mutex);

        if (m_stopped) {
            return {};
        }

        if (m_work_queue.size() == 0 || m_paused) {
            m_waiting_workers_count++;
            if (m_notify_on_worker_waiting) {
                m_paused_cv.notify_one();
            }

            m_work_queue_cv.wait(lock, [this] { 
                bool can_continue = m_paused ? false : m_work_queue.size() != 0;
                return can_continue || m_stopped; 
            });
            m_waiting_workers_count--;

            if (m_stopped) {
                return {};
            }
        }

        auto work_unit = m_work_queue.front();
        m_work_queue.pop();

        return work_unit;
    }
}  // namespace acairo