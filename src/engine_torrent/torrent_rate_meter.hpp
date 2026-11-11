// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <optional>

namespace openidm::torrent {

// Converts a monotonic cumulative payload counter into a sampled byte rate.
// Calls from concurrent peer workers are serialized; samples closer than 500 ms
// are coalesced, and counter rollback reseeds the baseline without a spike.
class TorrentRateMeter {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kMinimumSampleInterval = std::chrono::milliseconds(500);

    uint64_t Update(uint64_t cumulative_bytes, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_previous_time) {
            m_previous_bytes = cumulative_bytes;
            m_previous_time = now;
            m_rate_bps = 0;
            return m_rate_bps;
        }
        if (cumulative_bytes < m_previous_bytes || now < *m_previous_time) {
            m_previous_bytes = cumulative_bytes;
            m_previous_time = now;
            m_rate_bps = 0;
            return m_rate_bps;
        }

        const auto elapsed = now - *m_previous_time;
        if (elapsed < kMinimumSampleInterval) return m_rate_bps;

        const uint64_t delta = cumulative_bytes - m_previous_bytes;
        const long double seconds = std::chrono::duration<long double>(elapsed).count();
        const long double rate = seconds > 0 ? static_cast<long double>(delta) / seconds : 0;
        m_rate_bps = rate >= static_cast<long double>(std::numeric_limits<uint64_t>::max())
            ? std::numeric_limits<uint64_t>::max()
            : static_cast<uint64_t>(rate);
        m_previous_bytes = cumulative_bytes;
        m_previous_time = now;
        return m_rate_bps;
    }

    uint64_t Current() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_rate_bps;
    }

    void Reset(uint64_t cumulative_bytes = 0, Clock::time_point now = Clock::now()) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_previous_bytes = cumulative_bytes;
        m_previous_time = now;
        m_rate_bps = 0;
    }

private:
    mutable std::mutex m_mutex;
    uint64_t m_previous_bytes{0};
    std::optional<Clock::time_point> m_previous_time;
    uint64_t m_rate_bps{0};
};

} // namespace openidm::torrent
