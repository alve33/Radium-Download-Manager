// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — High-Performance Native WinSock2 FTP & FTPS Download Engine
#include "engine_ftp/ftp_client.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <security.h>
#include <schannel.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "secur32.lib")
using SocketType = SOCKET;
#define INVALID_SOCK_FD INVALID_SOCKET
#define CLOSE_SOCK(s) closesocket(s)
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include <openssl/ssl.h>
using SocketType = int;
#define INVALID_SOCK_FD (-1)
#define CLOSE_SOCK(s) ::close(s)
#endif

#include "common/crypto_utils.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"

namespace radium::ftp {

namespace {

struct WinSockInitializer {
    WinSockInitializer() {
#ifdef _WIN32
        WSADATA wsa;
        WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    }
    ~WinSockInitializer() {
#ifdef _WIN32
        WSACleanup();
#endif
    }
};

static WinSockInitializer g_winsock_init;

class AutoSocket {
public:
    AutoSocket(SocketType s = INVALID_SOCK_FD) : m_s(s) {}
    ~AutoSocket() { Close(); }
    void Close() {
        if (m_s != INVALID_SOCK_FD) {
            CLOSE_SOCK(m_s);
            m_s = INVALID_SOCK_FD;
        }
    }
    SocketType Get() const { return m_s; }
    void Reset(SocketType s) { Close(); m_s = s; }
    bool IsValid() const { return m_s != INVALID_SOCK_FD; }

private:
    SocketType m_s{INVALID_SOCK_FD};
};

Result<SocketType> ConnectHost(const std::string& host, uint16_t port, int timeout_ms, bool allow_loopback) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    std::string port_str = std::to_string(port);
    struct addrinfo* result = nullptr;
    int rc = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        return Result<SocketType>::Err("ERR_FTP_DNS", "Failed to resolve FTP host: " + host);
    }

    AutoSocket sock;
    for (struct addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
        if (!allow_loopback) {
            if (ptr->ai_family == AF_INET) {
                struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(ptr->ai_addr);
                uint32_t ip = ntohl(sin->sin_addr.s_addr);
                if (brisk::HttpClient::IsPrivateOrLoopbackIp(ip)) {
                    continue;
                }
            } else if (ptr->ai_family == AF_INET6) {
                struct sockaddr_in6* sin6 = reinterpret_cast<struct sockaddr_in6*>(ptr->ai_addr);
                if (brisk::HttpClient::IsPrivateOrLoopbackIpv6(reinterpret_cast<const uint8_t*>(&sin6->sin6_addr))) {
                    continue;
                }
            }
        }

        SocketType s = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
        if (s == INVALID_SOCK_FD) continue;

#ifdef _WIN32
        DWORD tv = static_cast<DWORD>(timeout_ms);
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

        if (connect(s, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0) {
            sock.Reset(s);
            break;
        } else {
            CLOSE_SOCK(s);
        }
    }
    freeaddrinfo(result);

    if (!sock.IsValid()) {
        return Result<SocketType>::Err("ERR_FTP_CONNECT", "Failed to connect to FTP host " + host + ":" + std::to_string(port));
    }
    SocketType out = sock.Get();
    sock.Reset(INVALID_SOCK_FD);
    return Result<SocketType>::Ok(out);
}

Result<SocketType> ConnectWithProxy(
    const std::string& host,
    uint16_t port,
    int timeout_ms,
    bool allow_loopback,
    const FtpProxyOptions& proxy) {
    if (proxy.mode == "direct" || proxy.host.empty() || proxy.port == 0) {
        return ConnectHost(host, port, timeout_ms, allow_loopback);
    }
    auto pconn = ConnectHost(proxy.host, proxy.port, timeout_ms, true);
    if (!pconn.ok()) return pconn;
    AutoSocket sock(pconn.value.value());

    if (proxy.mode == "socks5") {
        bool has_auth = !proxy.username.empty();
        std::vector<uint8_t> greet = has_auth ? std::vector<uint8_t>{0x05, 0x02, 0x00, 0x02}
                                              : std::vector<uint8_t>{0x05, 0x01, 0x00};
        if (send(sock.Get(), reinterpret_cast<const char*>(greet.data()), static_cast<int>(greet.size()), 0) !=
            static_cast<int>(greet.size())) {
            return Result<SocketType>::Err("ERR_FTP_PROXY", "SOCKS5 greeting send failed");
        }
        uint8_t meth[2]{};
        if (recv(sock.Get(), reinterpret_cast<char*>(meth), 2, 0) != 2 || meth[0] != 0x05 || meth[1] == 0xFF) {
            return Result<SocketType>::Err("ERR_FTP_PROXY", "SOCKS5 auth negotiation failed");
        }
        if (meth[1] == 0x02) {
            std::vector<uint8_t> auth;
            auth.push_back(0x01);
            auth.push_back(static_cast<uint8_t>(std::min<size_t>(255, proxy.username.size())));
            auth.insert(auth.end(), proxy.username.begin(), proxy.username.begin() + auth.back());
            uint8_t plen = static_cast<uint8_t>(std::min<size_t>(255, proxy.password.size()));
            auth.push_back(plen);
            auth.insert(auth.end(), proxy.password.begin(), proxy.password.begin() + plen);
            if (send(sock.Get(), reinterpret_cast<const char*>(auth.data()), static_cast<int>(auth.size()), 0) !=
                static_cast<int>(auth.size())) {
                return Result<SocketType>::Err("ERR_FTP_PROXY", "SOCKS5 auth send failed");
            }
            uint8_t aresp[2]{};
            if (recv(sock.Get(), reinterpret_cast<char*>(aresp), 2, 0) != 2 || aresp[1] != 0x00) {
                return Result<SocketType>::Err("ERR_FTP_PROXY", "SOCKS5 username/password rejected");
            }
        }
        if (host.size() > 255) return Result<SocketType>::Err("ERR_FTP_PROXY", "Host too long for SOCKS5");
        std::vector<uint8_t> req = {0x05, 0x01, 0x00, 0x03, static_cast<uint8_t>(host.size())};
        req.insert(req.end(), host.begin(), host.end());
        req.push_back(static_cast<uint8_t>((port >> 8) & 0xFF));
        req.push_back(static_cast<uint8_t>(port & 0xFF));
        if (send(sock.Get(), reinterpret_cast<const char*>(req.data()), static_cast<int>(req.size()), 0) !=
            static_cast<int>(req.size())) {
            return Result<SocketType>::Err("ERR_FTP_PROXY", "SOCKS5 CONNECT send failed");
        }
        uint8_t cresp[10]{};
        if (recv(sock.Get(), reinterpret_cast<char*>(cresp), 4, 0) != 4 || cresp[1] != 0x00) {
            return Result<SocketType>::Err("ERR_FTP_PROXY", "SOCKS5 CONNECT failed");
        }
        size_t skip = (cresp[3] == 0x01) ? 6 : (cresp[3] == 0x04 ? 18 : 0);
        if (cresp[3] == 0x03) {
            uint8_t dlen = 0;
            if (recv(sock.Get(), reinterpret_cast<char*>(&dlen), 1, 0) != 1) return Result<SocketType>::Err("ERR_FTP_PROXY", "SOCKS5 truncated");
            skip = static_cast<size_t>(dlen) + 2;
        }
        if (skip > 0) {
            std::vector<char> dummy(skip);
            recv(sock.Get(), dummy.data(), static_cast<int>(skip), 0);
        }
        SocketType out = sock.Get();
        sock.Reset(INVALID_SOCK_FD);
        return Result<SocketType>::Ok(out);
    }

    if (proxy.mode == "http") {
        std::ostringstream req;
        req << "CONNECT " << host << ":" << port << " HTTP/1.1\r\nHost: " << host << ":" << port << "\r\n";
        if (!proxy.username.empty()) {
            req << "Proxy-Authorization: Basic " << CryptoUtils::Base64Encode(proxy.username + ":" + proxy.password) << "\r\n";
        }
        req << "\r\n";
        std::string rstr = req.str();
        if (send(sock.Get(), rstr.data(), static_cast<int>(rstr.size()), 0) != static_cast<int>(rstr.size())) {
            return Result<SocketType>::Err("ERR_FTP_PROXY", "HTTP CONNECT send failed");
        }
        std::string resp;
        char ch = 0;
        while (resp.size() < 4096 && recv(sock.Get(), &ch, 1, 0) == 1) {
            resp.push_back(ch);
            if (resp.size() >= 4 && resp.compare(resp.size() - 4, 4, "\r\n\r\n") == 0) break;
        }
        if (resp.find(" 200 ") == std::string::npos) {
            return Result<SocketType>::Err("ERR_FTP_PROXY", "HTTP CONNECT tunnel rejected");
        }
        SocketType out = sock.Get();
        sock.Reset(INVALID_SOCK_FD);
        return Result<SocketType>::Ok(out);
    }

    return Result<SocketType>::Err("ERR_FTP_PROXY", "Unsupported FTP proxy mode: " + proxy.mode);
}

class FtpStream {
public:
    explicit FtpStream(SocketType s) : m_sock(s) {}
    ~FtpStream() { Close(); }

    SocketType Socket() const { return m_sock.Get(); }

    void Close() {
#ifdef _WIN32
        if (m_has_ctxt) { DeleteSecurityContext(&m_ctxt); m_has_ctxt = false; }
        if (m_has_cred) { FreeCredentialsHandle(&m_cred); m_has_cred = false; }
#else
        if (m_ssl) { SSL_shutdown(m_ssl); SSL_free(m_ssl); m_ssl = nullptr; }
        if (m_ctx) { SSL_CTX_free(m_ctx); m_ctx = nullptr; }
#endif
        m_tls = false;
        m_sock.Close();
    }

    bool UpgradeTls(const std::string& host, bool allow_self_signed) {
#ifdef _WIN32
        SCHANNEL_CRED sc{};
        sc.dwVersion = SCHANNEL_CRED_VERSION;
        sc.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
        if (allow_self_signed) {
            sc.dwFlags |= SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_IGNORE_NO_REVOCATION_CHECK;
        } else {
            sc.dwFlags |= SCH_CRED_AUTO_CRED_VALIDATION;
        }
        TimeStamp ts{};
        if (AcquireCredentialsHandleW(nullptr, const_cast<LPWSTR>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND,
                                      nullptr, &sc, nullptr, nullptr, &m_cred, &ts) != SEC_E_OK) {
            return false;
        }
        m_has_cred = true;

        std::wstring whost(host.begin(), host.end());
        DWORD flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT | ISC_REQ_CONFIDENTIALITY |
                      ISC_RET_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM;
        if (allow_self_signed) flags |= ISC_REQ_MANUAL_CRED_VALIDATION;
        DWORD out_flags = 0;
        SecBuffer out_buf{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out_buf};
        SECURITY_STATUS st = InitializeSecurityContextW(
            &m_cred, nullptr, const_cast<LPWSTR>(whost.c_str()), flags, 0, 0, nullptr, 0,
            &m_ctxt, &out_desc, &out_flags, &ts);
        if (st != SEC_I_CONTINUE_NEEDED && st != SEC_E_OK) return false;
        m_has_ctxt = true;
        if (out_buf.cbBuffer > 0 && out_buf.pvBuffer) {
            int sent = send(m_sock.Get(), static_cast<const char*>(out_buf.pvBuffer), static_cast<int>(out_buf.cbBuffer), 0);
            FreeContextBuffer(out_buf.pvBuffer);
            if (sent <= 0) return false;
        }

        std::vector<char> in_acc;
        while (st == SEC_I_CONTINUE_NEEDED || st == SEC_E_INCOMPLETE_MESSAGE) {
            char tmp[8192];
            int n = recv(m_sock.Get(), tmp, sizeof(tmp), 0);
            if (n <= 0) return false;
            in_acc.insert(in_acc.end(), tmp, tmp + n);
            SecBuffer in_bufs[2] = {
                {static_cast<unsigned long>(in_acc.size()), SECBUFFER_TOKEN, in_acc.data()},
                {0, SECBUFFER_EMPTY, nullptr}
            };
            SecBufferDesc in_desc{SECBUFFER_VERSION, 2, in_bufs};
            SecBuffer ob{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc od{SECBUFFER_VERSION, 1, &ob};
            st = InitializeSecurityContextW(
                &m_cred, &m_ctxt, const_cast<LPWSTR>(whost.c_str()), flags, 0, 0, &in_desc, 0,
                &m_ctxt, &od, &out_flags, &ts);
            if (st == SEC_E_INCOMPLETE_MESSAGE) continue;
            if (ob.cbBuffer > 0 && ob.pvBuffer) {
                send(m_sock.Get(), static_cast<const char*>(ob.pvBuffer), static_cast<int>(ob.cbBuffer), 0);
                FreeContextBuffer(ob.pvBuffer);
            }
            if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) return false;
            if (in_bufs[1].BufferType == SECBUFFER_EXTRA && in_bufs[1].cbBuffer > 0) {
                size_t extra = in_bufs[1].cbBuffer;
                std::memmove(in_acc.data(), in_acc.data() + in_acc.size() - extra, extra);
                in_acc.resize(extra);
            } else {
                in_acc.clear();
            }
        }
        QueryContextAttributesW(&m_ctxt, SECPKG_ATTR_STREAM_SIZES, &m_sizes);
        m_enc_extra.assign(in_acc.begin(), in_acc.end());
        m_tls = true;
        return true;
#else
        m_ctx = SSL_CTX_new(TLS_client_method());
        if (!m_ctx) return false;
        if (!allow_self_signed) SSL_CTX_set_default_verify_paths(m_ctx);
        m_ssl = SSL_new(m_ctx);
        if (!m_ssl) return false;
        SSL_set_tlsext_host_name(m_ssl, host.c_str());
        SSL_set_fd(m_ssl, m_sock.Get());
        if (SSL_connect(m_ssl) != 1) return false;
        m_tls = true;
        return true;
#endif
    }

    int Send(const char* data, size_t len) {
        if (!m_tls) {
            return send(m_sock.Get(), data, static_cast<int>(len), 0);
        }
#ifdef _WIN32
        size_t sent_total = 0;
        while (sent_total < len) {
            size_t chunk = std::min<size_t>(len - sent_total, m_sizes.cbMaximumMessage);
            std::vector<uint8_t> buf(m_sizes.cbHeader + chunk + m_sizes.cbTrailer);
            std::memcpy(buf.data() + m_sizes.cbHeader, data + sent_total, chunk);
            SecBuffer sbufs[4] = {
                {m_sizes.cbHeader, SECBUFFER_STREAM_HEADER, buf.data()},
                {static_cast<unsigned long>(chunk), SECBUFFER_DATA, buf.data() + m_sizes.cbHeader},
                {m_sizes.cbTrailer, SECBUFFER_STREAM_TRAILER, buf.data() + m_sizes.cbHeader + chunk},
                {0, SECBUFFER_EMPTY, nullptr}
            };
            SecBufferDesc sdesc{SECBUFFER_VERSION, 4, sbufs};
            if (EncryptMessage(&m_ctxt, 0, &sdesc, 0) != SEC_E_OK) return -1;
            size_t wire_len = sbufs[0].cbBuffer + sbufs[1].cbBuffer + sbufs[2].cbBuffer;
            if (send(m_sock.Get(), reinterpret_cast<const char*>(buf.data()), static_cast<int>(wire_len), 0) <= 0) return -1;
            sent_total += chunk;
        }
        return static_cast<int>(sent_total);
#else
        return SSL_write(m_ssl, data, static_cast<int>(len));
#endif
    }

    int Recv(char* out, size_t cap) {
        if (!m_tls) {
            return recv(m_sock.Get(), out, static_cast<int>(cap), 0);
        }
#ifdef _WIN32
        if (!m_plain_buf.empty()) {
            size_t take = std::min(cap, m_plain_buf.size());
            std::memcpy(out, m_plain_buf.data(), take);
            m_plain_buf.erase(m_plain_buf.begin(), m_plain_buf.begin() + static_cast<ptrdiff_t>(take));
            return static_cast<int>(take);
        }
        while (true) {
            if (!m_enc_extra.empty()) {
                SecBuffer bufs[4] = {
                    {static_cast<unsigned long>(m_enc_extra.size()), SECBUFFER_DATA, m_enc_extra.data()},
                    {0, SECBUFFER_EMPTY, nullptr},
                    {0, SECBUFFER_EMPTY, nullptr},
                    {0, SECBUFFER_EMPTY, nullptr}
                };
                SecBufferDesc desc{SECBUFFER_VERSION, 4, bufs};
                SECURITY_STATUS st = DecryptMessage(&m_ctxt, &desc, 0, nullptr);
                if (st == SEC_E_OK) {
                    std::vector<uint8_t> next_extra;
                    for (int i = 0; i < 4; ++i) {
                        if (bufs[i].BufferType == SECBUFFER_DATA && bufs[i].cbBuffer > 0) {
                            const auto* p = static_cast<const uint8_t*>(bufs[i].pvBuffer);
                            m_plain_buf.insert(m_plain_buf.end(), p, p + bufs[i].cbBuffer);
                        } else if (bufs[i].BufferType == SECBUFFER_EXTRA && bufs[i].cbBuffer > 0) {
                            const auto* p = static_cast<const uint8_t*>(bufs[i].pvBuffer);
                            next_extra.assign(p, p + bufs[i].cbBuffer);
                        }
                    }
                    m_enc_extra.swap(next_extra);
                    if (!m_plain_buf.empty()) {
                        size_t take = std::min(cap, m_plain_buf.size());
                        std::memcpy(out, m_plain_buf.data(), take);
                        m_plain_buf.erase(m_plain_buf.begin(), m_plain_buf.begin() + static_cast<ptrdiff_t>(take));
                        return static_cast<int>(take);
                    }
                } else if (st == SEC_I_CONTEXT_EXPIRED) {
                    return 0;
                } else if (st != SEC_E_INCOMPLETE_MESSAGE) {
                    return -1;
                }
            }
            char tmp[16384];
            int n = recv(m_sock.Get(), tmp, sizeof(tmp), 0);
            if (n <= 0) return n;
            m_enc_extra.insert(m_enc_extra.end(), reinterpret_cast<uint8_t*>(tmp), reinterpret_cast<uint8_t*>(tmp) + n);
        }
#else
        return SSL_read(m_ssl, out, static_cast<int>(cap));
#endif
    }

private:
    AutoSocket m_sock;
    bool m_tls{false};
#ifdef _WIN32
    CredHandle m_cred{};
    CtxtHandle m_ctxt{};
    bool m_has_cred{false};
    bool m_has_ctxt{false};
    SecPkgContext_StreamSizes m_sizes{};
    std::vector<uint8_t> m_enc_extra;
    std::vector<uint8_t> m_plain_buf;
#else
    SSL_CTX* m_ctx{nullptr};
    SSL* m_ssl{nullptr};
#endif
};

Result<std::pair<int, std::string>> ReadFtpResponse(FtpStream& s) {
    std::string full_response;
    char ch;
    std::string current_line;
    int status_code = 0;

    while (true) {
        current_line.clear();
        while (true) {
            int n = s.Recv(&ch, 1);
            if (n <= 0) {
                return Result<std::pair<int, std::string>>::Err("ERR_FTP_DISCONNECTED", "Connection closed while reading FTP response");
            }
            if (ch == '\r') continue;
            if (ch == '\n') break;
            current_line.push_back(ch);
        }

        full_response += current_line + "\n";
        if (current_line.size() >= 3 &&
            std::isdigit(static_cast<unsigned char>(current_line[0])) &&
            std::isdigit(static_cast<unsigned char>(current_line[1])) &&
            std::isdigit(static_cast<unsigned char>(current_line[2]))) {
            int code = std::atoi(current_line.substr(0, 3).c_str());
            if (current_line.size() == 3 || current_line[3] == ' ') {
                status_code = code;
                break;
            }
        }
    }
    return Result<std::pair<int, std::string>>::Ok({status_code, full_response});
}

Result<std::pair<int, std::string>> SendFtpCommand(FtpStream& s, const std::string& cmd) {
    for (char c : cmd) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return Result<std::pair<int, std::string>>::Err("ERR_FTP_COMMAND_INJECTION", "CRLF or null character in FTP command");
        }
    }
    std::string wire = cmd + "\r\n";
    size_t total_sent = 0;
    while (total_sent < wire.size()) {
        int sent = s.Send(wire.data() + total_sent, wire.size() - total_sent);
        if (sent <= 0) {
            return Result<std::pair<int, std::string>>::Err("ERR_FTP_SEND", "Failed to send FTP command: " + cmd);
        }
        total_sent += static_cast<size_t>(sent);
    }
    return ReadFtpResponse(s);
}

} // namespace

Result<FtpUrl> FtpUrl::Parse(std::string_view url_str) {
    for (char c : url_str) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return Result<FtpUrl>::Err("ERR_INVALID_FTP_URL", "CRLF or null character in FTP URL");
        }
    }
    FtpUrl out;
    std::string s(url_str);

    for (char c : url_str) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return Result<FtpUrl>::Err("ERR_FTP_INVALID_URL", "CRLF or null injection detected in FTP URL");
        }
    }

    std::string prefix_ftp = "ftp://";
    std::string prefix_ftps = "ftps://";
    size_t offset = 0;
    if (s.compare(0, prefix_ftp.size(), prefix_ftp) == 0) {
        out.scheme = "ftp";
        out.port = 21;
        offset = prefix_ftp.size();
    } else if (s.compare(0, prefix_ftps.size(), prefix_ftps) == 0) {
        out.scheme = "ftps";
        out.port = 990;
        offset = prefix_ftps.size();
    } else {
        return Result<FtpUrl>::Err("ERR_FTP_INVALID_URL", "URL must begin with ftp:// or ftps://");
    }

    size_t path_pos = s.find('/', offset);
    std::string authority;
    if (path_pos == std::string::npos) {
        authority = s.substr(offset);
        out.remote_path = "/";
    } else {
        authority = s.substr(offset, path_pos - offset);
        out.remote_path = s.substr(path_pos);
    }

    // User / Password in authority: user:pass@host
    size_t at_pos = authority.rfind('@');
    if (at_pos != std::string::npos) {
        std::string userinfo = authority.substr(0, at_pos);
        authority = authority.substr(at_pos + 1);
        size_t colon = userinfo.find(':');
        if (colon != std::string::npos) {
            out.user = PathSanitizer::UrlDecode(userinfo.substr(0, colon));
            out.password = PathSanitizer::UrlDecode(userinfo.substr(colon + 1));
        } else {
            out.user = PathSanitizer::UrlDecode(userinfo);
        }
        if (out.user.find('\r') != std::string::npos || out.user.find('\n') != std::string::npos || out.user.find('\0') != std::string::npos ||
            out.password.find('\r') != std::string::npos || out.password.find('\n') != std::string::npos || out.password.find('\0') != std::string::npos) {
            return Result<FtpUrl>::Err("ERR_FTP_INVALID_URL", "CRLF or null injection detected in FTP userinfo");
        }
        out.username = out.user;
    }

    // Host & Port
    if (!authority.empty() && authority.front() == '[') {
        size_t closing = authority.find(']');
        if (closing != std::string::npos) {
            out.host = authority.substr(1, closing - 1);
            if (closing + 1 < authority.size() && authority[closing + 1] == ':') {
                out.port = static_cast<uint16_t>(std::atoi(authority.substr(closing + 2).c_str()));
            }
        }
    } else {
        size_t colon = authority.find(':');
        if (colon != std::string::npos) {
            out.host = authority.substr(0, colon);
            out.port = static_cast<uint16_t>(std::atoi(authority.substr(colon + 1).c_str()));
        } else {
            out.host = authority;
        }
    }

    if (out.host.empty()) {
        return Result<FtpUrl>::Err("ERR_INVALID_FTP_HOST", "Missing FTP host in URL");
    }

    // Extract filename
    size_t last_slash = out.remote_path.rfind('/');
    if (last_slash != std::string::npos && last_slash + 1 < out.remote_path.size()) {
        out.file_name = PathSanitizer::SanitizeFileName(out.remote_path.substr(last_slash + 1));
    }
    if (out.file_name.empty()) {
        out.file_name = "ftp_download.bin";
    }

    return Result<FtpUrl>::Ok(std::move(out));
}

Result<uint64_t> FtpClient::ProbeFileSize(
    const FtpUrl& url,
    int timeout_ms,
    bool allow_loopback,
    const FtpProxyOptions& proxy_opts) {

    auto conn_res = ConnectWithProxy(url.host, url.port, timeout_ms, allow_loopback, proxy_opts);
    if (!conn_res.ok()) return Result<uint64_t>::Err(conn_res.error->code, conn_res.error->message);
    FtpStream ctrl(conn_res.value.value());

    const bool implicit_tls = (url.scheme == "ftps");
    if (implicit_tls && !ctrl.UpgradeTls(url.host, allow_loopback)) {
        return Result<uint64_t>::Err("ERR_FTPS_TLS", "Implicit FTPS TLS handshake failed");
    }

    auto greeting = ReadFtpResponse(ctrl);
    if (!greeting.ok() || greeting.value->first != 220) {
        return Result<uint64_t>::Err("ERR_FTP_GREETING", "Server greeting failed");
    }

    if (!implicit_tls && proxy_opts.try_explicit_tls) {
        auto auth_res = SendFtpCommand(ctrl, "AUTH TLS");
        if (auth_res.ok() && auth_res.value->first == 234) {
            if (!ctrl.UpgradeTls(url.host, allow_loopback)) {
                return Result<uint64_t>::Err("ERR_FTPS_TLS", "Explicit FTPS TLS handshake failed");
            }
        }
    }

    auto user_res = SendFtpCommand(ctrl, "USER " + url.user);
    if (!user_res.ok()) return Result<uint64_t>::Err(user_res.error->code, user_res.error->message);
    if (user_res.value->first == 331) {
        auto pass_res = SendFtpCommand(ctrl, "PASS " + url.password);
        if (!pass_res.ok() || pass_res.value->first != 230) {
            return Result<uint64_t>::Err("ERR_FTP_AUTH", "FTP Authentication failed for user: " + url.user);
        }
    } else if (user_res.value->first != 230) {
        return Result<uint64_t>::Err("ERR_FTP_AUTH", "FTP USER rejected");
    }

    // Switch to binary mode (TYPE I)
    auto type_res = SendFtpCommand(ctrl, "TYPE I");
    if (!type_res.ok() || type_res.value->first != 200) {
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_FTP_TYPE_FAILED", "Failed to negotiate binary transfer mode (TYPE I)");
    }

    // SIZE command (RFC 3659)
    auto size_res = SendFtpCommand(ctrl, "SIZE " + url.remote_path);
    uint64_t file_size = 0;
    if (size_res.ok() && size_res.value->first == 213) {
        std::stringstream ss(size_res.value->second);
        int code = 0;
        uint64_t bytes = 0;
        if (ss >> code >> bytes) {
            file_size = bytes;
        }
    }

    SendFtpCommand(ctrl, "QUIT");
    return Result<uint64_t>::Ok(file_size);
}

Result<uint64_t> FtpClient::DownloadFile(
    const FtpUrl& url,
    const std::filesystem::path& target_part_path,
    uint64_t resume_offset,
    std::atomic<bool>& pause_requested,
    std::atomic<bool>& cancel_requested,
    std::atomic<uint64_t>& task_speed_limit_bps,
    FtpProgressCallback on_progress,
    std::shared_ptr<brisk::TokenBucketLimiter> global_limiter,
    std::atomic<uint64_t>* global_speed_limit_bps,
    int timeout_ms,
    bool allow_loopback,
    const FtpProxyOptions& proxy_opts) {

    auto conn_res = ConnectWithProxy(url.host, url.port, timeout_ms, allow_loopback, proxy_opts);
    if (!conn_res.ok()) return Result<uint64_t>::Err(conn_res.error->code, conn_res.error->message);
    FtpStream ctrl(conn_res.value.value());

    bool use_tls = (url.scheme == "ftps");
    if (use_tls && !ctrl.UpgradeTls(url.host, allow_loopback)) {
        return Result<uint64_t>::Err("ERR_FTPS_TLS", "Implicit FTPS TLS handshake failed");
    }

    auto greeting = ReadFtpResponse(ctrl);
    if (!greeting.ok() || greeting.value->first != 220) {
        return Result<uint64_t>::Err("ERR_FTP_GREETING", "Server greeting failed");
    }

    if (!use_tls && proxy_opts.try_explicit_tls) {
        auto auth_res = SendFtpCommand(ctrl, "AUTH TLS");
        if (auth_res.ok() && auth_res.value->first == 234) {
            if (!ctrl.UpgradeTls(url.host, allow_loopback)) {
                return Result<uint64_t>::Err("ERR_FTPS_TLS", "Explicit FTPS TLS handshake failed");
            }
            use_tls = true;
        }
    }

    auto user_res = SendFtpCommand(ctrl, "USER " + url.user);
    if (!user_res.ok()) return Result<uint64_t>::Err(user_res.error->code, user_res.error->message);
    if (user_res.value->first == 331) {
        auto pass_res = SendFtpCommand(ctrl, "PASS " + url.password);
        if (!pass_res.ok() || pass_res.value->first != 230) {
            return Result<uint64_t>::Err("ERR_FTP_AUTH", "FTP Authentication failed for user: " + url.user);
        }
    } else if (user_res.value->first != 230) {
        return Result<uint64_t>::Err("ERR_FTP_AUTH", "FTP USER rejected");
    }

    if (use_tls) {
        SendFtpCommand(ctrl, "PBSZ 0");
        auto prot_res = SendFtpCommand(ctrl, "PROT P");
        if (!prot_res.ok() || prot_res.value->first != 200) {
            SendFtpCommand(ctrl, "QUIT");
            return Result<uint64_t>::Err("ERR_FTPS_PROT", "FTPS PROT P data protection negotiation failed");
        }
    }

    auto type_dl_res = SendFtpCommand(ctrl, "TYPE I");
    if (!type_dl_res.ok() || type_dl_res.value->first != 200) {
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_FTP_TYPE_FAILED", "Failed to negotiate binary transfer mode (TYPE I)");
    }

    // Get Total Size
    uint64_t total_size = 0;
    auto size_res = SendFtpCommand(ctrl, "SIZE " + url.remote_path);
    if (size_res.ok() && size_res.value->first == 213) {
        std::stringstream ss(size_res.value->second);
        int code = 0;
        uint64_t bytes = 0;
        if (ss >> code >> bytes) total_size = bytes;
    }

    // Passive Mode Negotiation: Try EPSV (RFC 2428) first, fallback to PASV (RFC 959)
    std::string data_host = url.host;
    uint16_t data_port = 0;

    auto epsv_res = SendFtpCommand(ctrl, "EPSV");
    if (epsv_res.ok() && epsv_res.value->first == 229) {
        const std::string& epsv_str = epsv_res.value->second;
        size_t p1 = epsv_str.find("|||");
        if (p1 != std::string::npos) {
            size_t p2 = epsv_str.find('|', p1 + 3);
            if (p2 != std::string::npos) {
                data_port = static_cast<uint16_t>(std::atoi(epsv_str.substr(p1 + 3, p2 - p1 - 3).c_str()));
            }
        }
    }

    if (data_port == 0) {
        // Fallback to PASV
        auto pasv_res = SendFtpCommand(ctrl, "PASV");
        if (!pasv_res.ok() || pasv_res.value->first != 227) {
            return Result<uint64_t>::Err("ERR_FTP_PASV", "PASV and EPSV commands failed");
        }

        const std::string& pasv_str = pasv_res.value->second;
        size_t open_paren = pasv_str.find('(');
        size_t close_paren = pasv_str.find(')', open_paren);
        if (open_paren == std::string::npos || close_paren == std::string::npos) {
            return Result<uint64_t>::Err("ERR_FTP_PASV", "Invalid PASV response format");
        }
        std::string ip_port_str = pasv_str.substr(open_paren + 1, close_paren - open_paren - 1);
        int h1, h2, h3, h4, p1, p2;
        if (sscanf(ip_port_str.c_str(), "%d,%d,%d,%d,%d,%d", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
            return Result<uint64_t>::Err("ERR_FTP_PASV", "Failed to parse PASV host/port");
        }

        data_port = static_cast<uint16_t>((p1 << 8) | p2);
        // Eliminate PASV third-party bounce SSRF by always pinning data host to control host
        data_host = url.host;
    }

    // Connect Data Socket
    auto data_conn = ConnectWithProxy(data_host, data_port, timeout_ms, allow_loopback, proxy_opts);
    if (!data_conn.ok()) {
        return Result<uint64_t>::Err("ERR_FTP_DATA_CONNECT", "Failed to connect to FTP data port " + data_host + ":" + std::to_string(data_port));
    }
    FtpStream data_sock(data_conn.value.value());

    // Validate target_part_path on disk before attempting resume
    std::error_code ec;
    if (resume_offset > 0) {
        if (!std::filesystem::exists(target_part_path, ec)) {
            RADIUM_LOG_WARN("FtpClient", "Part file missing on disk; resetting resume offset to 0");
            resume_offset = 0;
        } else {
            uint64_t actual_sz = std::filesystem::file_size(target_part_path, ec);
            if (actual_sz < resume_offset) {
                RADIUM_LOG_WARN("FtpClient", "Part file size (" + std::to_string(actual_sz) +
                    ") is smaller than resume offset (" + std::to_string(resume_offset) + "); resetting to 0");
                resume_offset = 0;
            } else if (actual_sz > resume_offset) {
                // Truncate partial file down to resume_offset to eliminate stale trailing data
                std::filesystem::resize_file(target_part_path, resume_offset, ec);
            }
        }
    }

    // REST offset validation if resuming (RFC 3659)
    bool resume_accepted = false;
    if (resume_offset > 0) {
        auto rest_res = SendFtpCommand(ctrl, "REST " + std::to_string(resume_offset));
        if (rest_res.ok() && rest_res.value->first == 350) {
            resume_accepted = true;
        } else {
            RADIUM_LOG_WARN("FtpClient", "FTP server rejected REST " + std::to_string(resume_offset) + "; restarting transfer from zero");
            resume_offset = 0;
            resume_accepted = false;
            if (std::filesystem::exists(target_part_path, ec)) {
                std::filesystem::resize_file(target_part_path, 0, ec);
            }
        }
    }

    // RETR
    auto retr_res = SendFtpCommand(ctrl, "RETR " + url.remote_path);
    if (!retr_res.ok() || (retr_res.value->first != 150 && retr_res.value->first != 125)) {
        data_sock.Close();
        SendFtpCommand(ctrl, "ABOR");
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_FTP_RETR", "RETR failed: " + (retr_res.ok() ? retr_res.value->second : "error"));
    }
    if (use_tls && !data_sock.UpgradeTls(url.host, allow_loopback)) {
        data_sock.Close();
        SendFtpCommand(ctrl, "ABOR");
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_FTPS_DATA_TLS", "FTPS data channel TLS handshake failed");
    }

    std::ios_base::openmode mode = std::ios::binary | std::ios::out;
    if (resume_accepted && resume_offset > 0 && std::filesystem::exists(target_part_path, ec)) {
        mode |= std::ios::in; // Open existing partial file
    } else {
        mode |= std::ios::trunc; // Clean truncation: never append to stale content
    }

    std::fstream ofs(target_part_path, mode);
    if (!ofs.is_open()) {
        data_sock.Close();
        SendFtpCommand(ctrl, "ABOR");
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_STORAGE_OPEN", "Failed to open part file: " + target_part_path.string());
    }
    if (resume_accepted && resume_offset > 0) {
        ofs.seekp(resume_offset, std::ios::beg);
    }

    uint64_t total_written = resume_offset;
    std::vector<char> buf(65536);
    auto start_time = std::chrono::steady_clock::now();
    uint64_t last_bytes = resume_offset;
    auto last_calc = start_time;
    auto task_limiter = std::make_shared<brisk::TokenBucketLimiter>();

    while (!pause_requested.load() && !cancel_requested.load()) {
        int n = data_sock.Recv(buf.data(), buf.size());
        if (n < 0) {
            ofs.close();
            data_sock.Close();
            SendFtpCommand(ctrl, "ABOR");
            SendFtpCommand(ctrl, "QUIT");
            return Result<uint64_t>::Err("ERR_FTP_TRANSFER", "FTP transfer error on data connection");
        }
        if (n == 0) {
            break; // EOF
        }

        // Dual token bucket rate limiting (per-task cap and aggregate global cap)
        if (task_speed_limit_bps.load(std::memory_order_relaxed) > 0) {
            task_limiter->Consume(static_cast<size_t>(n), task_speed_limit_bps, cancel_requested);
        }
        if (global_limiter && global_speed_limit_bps && global_speed_limit_bps->load(std::memory_order_relaxed) > 0) {
            global_limiter->Consume(static_cast<size_t>(n), *global_speed_limit_bps, cancel_requested);
        }

        ofs.write(buf.data(), n);
        if (!ofs.good()) {
            ofs.close();
            data_sock.Close();
            SendFtpCommand(ctrl, "ABOR");
            SendFtpCommand(ctrl, "QUIT");
            return Result<uint64_t>::Err("ERR_DISK_FULL", "Failed to write FTP data to disk");
        }
        total_written += static_cast<uint64_t>(n);

        auto now = std::chrono::steady_clock::now();
        auto diff_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_calc).count();
        if (diff_ms >= 250 && on_progress) {
            uint64_t speed = ((total_written - last_bytes) * 1000ULL) / std::max<int64_t>(1, diff_ms);
            last_bytes = total_written;
            last_calc = now;

            FtpProgress prog;
            prog.downloaded_bytes = total_written;
            prog.total_bytes = total_size;
            prog.speed_bps = speed;
            prog.status_detail = "Downloading via FTP";
            on_progress(prog);
        }
    }

    ofs.flush();
    if (!ofs.good()) {
        ofs.close();
        data_sock.Close();
        SendFtpCommand(ctrl, "ABOR");
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_DISK_FULL", "Failed to flush FTP data to disk");
    }
    ofs.close();
    data_sock.Close();

    if (cancel_requested.load()) {
        std::filesystem::remove(target_part_path, ec);
        SendFtpCommand(ctrl, "ABOR");
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_CANCELED", "FTP download canceled");
    }
    if (pause_requested.load()) {
        SendFtpCommand(ctrl, "QUIT");
        return Result<uint64_t>::Err("ERR_PAUSED", "FTP download paused by user");
    }

    auto comp_res = ReadFtpResponse(ctrl);
    SendFtpCommand(ctrl, "QUIT");
    if (!comp_res.ok() || (comp_res.value->first != 226 && comp_res.value->first != 250)) {
        return Result<uint64_t>::Err("ERR_FTP_TRANSFER", "FTP transfer did not complete cleanly: " + (comp_res.ok() ? comp_res.value->second : comp_res.error->message));
    }

    if (total_size > 0 && total_written != total_size) {
        return Result<uint64_t>::Err("ERR_FTP_SIZE_MISMATCH",
            "FTP downloaded size mismatch: expected " + std::to_string(total_size) +
            " bytes, got " + std::to_string(total_written));
    }

    return Result<uint64_t>::Ok(total_written);
}

} // namespace radium::ftp
