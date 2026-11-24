// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace openidm::test {

struct LabFaults {
    bool tarpit_headers{false};
    uint32_t range_latency_ms{0};
    uint32_t max_conns{0};
    bool etag_change_on_range{false};
    uint64_t drop_after_bytes{0};
};

class LabServer {
public:
    explicit LabServer(std::string payload, LabFaults faults = {})
        : m_payload(std::move(payload)), m_faults(faults) {}

    ~LabServer() { Stop(); }

    bool Start() {
#ifdef _WIN32
        WSADATA wsa{};
        WSAStartup(MAKEWORD(2, 2), &wsa);
        m_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (m_sock == INVALID_SOCKET) return false;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(m_sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(m_sock, 16) != 0) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
            return false;
        }
        int len = sizeof(addr);
        getsockname(m_sock, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);
        m_stop.store(false);
        m_thread = std::thread([this]() { ServeLoop(); });
        return true;
#else
        return false;
#endif
    }

    void Stop() {
#ifdef _WIN32
        m_stop.store(true);
        if (m_sock != INVALID_SOCKET) {
            closesocket(m_sock);
            m_sock = INVALID_SOCKET;
        }
        if (m_thread.joinable()) m_thread.join();
#endif
    }

    int Port() const { return m_port; }
    std::string BaseUrl() const { return "http://127.0.0.1:" + std::to_string(m_port); }
    uint32_t RequestCount() const { return m_requests.load(); }

private:
    void ServeLoop() {
#ifdef _WIN32
        while (!m_stop.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(m_sock, &fds);
            timeval tv{0, 50000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(m_sock, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::thread([this, c]() {
                uint32_t cur = ++m_active;
                ++m_requests;
                char buf[4096];
                int n = recv(c, buf, sizeof(buf) - 1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string req(buf);
                    if (m_faults.tarpit_headers) {
                        while (!m_stop.load()) {
                            std::this_thread::sleep_for(std::chrono::milliseconds(20));
                        }
                        --m_active;
                        closesocket(c);
                        return;
                    }
                    if (m_faults.max_conns > 0 && cur > m_faults.max_conns) {
                        const char* r503 = "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
                        send(c, r503, static_cast<int>(std::strlen(r503)), 0);
                        --m_active;
                        closesocket(c);
                        return;
                    }
                    bool has_range = (req.find("Range:") != std::string::npos);
                    if (has_range && m_faults.range_latency_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(m_faults.range_latency_ms));
                    }
                    std::string etag = (has_range && m_faults.etag_change_on_range) ? "\"v2\"" : "\"v1\"";
                    std::string slice = m_payload;
                    if (m_faults.drop_after_bytes > 0 && slice.size() > m_faults.drop_after_bytes) {
                        slice.resize(static_cast<size_t>(m_faults.drop_after_bytes));
                    }
                    std::string hdr =
                        "HTTP/1.1 200 OK\r\nETag: " + etag +
                        "\r\nAccept-Ranges: bytes\r\nContent-Length: " + std::to_string(m_payload.size()) +
                        "\r\nConnection: close\r\n\r\n";
                    send(c, hdr.data(), static_cast<int>(hdr.size()), 0);
                    send(c, slice.data(), static_cast<int>(slice.size()), 0);
                }
                --m_active;
                closesocket(c);
            }).detach();
        }
#endif
    }

    std::string m_payload;
    LabFaults m_faults;
    int m_port{0};
    std::atomic<bool> m_stop{false};
    std::atomic<uint32_t> m_active{0};
    std::atomic<uint32_t> m_requests{0};
    std::thread m_thread;
#ifdef _WIN32
    SOCKET m_sock{INVALID_SOCKET};
#endif
};

// SlowDisk backpressure model (MDM slowdisk.rs port)
class SlowDiskModel {
public:
    SlowDiskModel(uint64_t rate_bps, uint64_t cache_bytes)
        : m_rate_bps(rate_bps), m_cache_bytes(cache_bytes) {}

    bool RecordWrite(uint64_t bytes) {
        m_backlog_bytes += bytes;
        if (m_backlog_bytes > m_cache_bytes) {
            ++m_writes_waited;
            uint64_t excess = m_backlog_bytes - m_cache_bytes;
            if (m_rate_bps > 0) {
                uint64_t wait_ms = (excess * 1000ULL) / m_rate_bps;
                m_total_wait_ms += wait_ms;
                m_backlog_bytes = m_cache_bytes;
            }
            return true; // Backpressure triggered
        }
        return false;
    }

    uint64_t WritesWaited() const { return m_writes_waited; }
    uint64_t TotalWaitMs() const { return m_total_wait_ms; }

private:
    uint64_t m_rate_bps{0};
    uint64_t m_cache_bytes{0};
    uint64_t m_backlog_bytes{0};
    uint64_t m_writes_waited{0};
    uint64_t m_total_wait_ms{0};
};

} // namespace openidm::test
