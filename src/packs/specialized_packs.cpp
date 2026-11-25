// SPDX-License-Identifier: GPL-3.0-or-later
#include "packs/specialized_packs.hpp"
#include "brisk_engine/http_client.hpp"
#include "common/json_mini.hpp"
#include "common/path_sanitizer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <regex>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <winhttp.h>
#include <bcrypt.h>
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#else
#include <openssl/evp.h>
#endif

namespace radium::packs {

namespace {

static std::string ToLowerStr(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string UrlEncodeForm(const std::string& s) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(static_cast<char>(c));
        } else if (c == ' ') {
            out.push_back('+');
        } else {
            out.push_back('%');
            out.push_back(kHex[(c >> 4) & 0x0F]);
            out.push_back(kHex[c & 0x0F]);
        }
    }
    return out;
}

static std::optional<std::vector<uint8_t>> Base64Decode(const std::string& input) {
    static const int kDec[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,62,-1,63,52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,63,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    std::vector<uint8_t> out;
    int val = 0, valb = -8;
    for (unsigned char c : input) {
        if (std::isspace(c) || c == '=') continue;
        int d = kDec[c];
        if (d == -1) return std::nullopt;
        val = (val << 6) + d;
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<uint8_t>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

static bool Aes128CbcDecrypt(
    const std::vector<uint8_t>& key,
    const std::vector<uint8_t>& iv,
    const std::vector<uint8_t>& cipher,
    std::vector<uint8_t>& plain) {
    if (key.size() != 16 || iv.size() != 16 || cipher.empty() || (cipher.size() % 16) != 0) {
        return false;
    }
#ifdef _WIN32
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_KEY_HANDLE hKey = nullptr;
    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return false;
    if (BCryptSetProperty(hAlg, BCRYPT_CHAINING_MODE,
                          reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
                          sizeof(BCRYPT_CHAIN_MODE_CBC), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    if (BCryptGenerateSymmetricKey(hAlg, &hKey, nullptr, 0,
                                   const_cast<PUCHAR>(key.data()), static_cast<ULONG>(key.size()), 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }
    std::vector<uint8_t> iv_copy = iv;
    plain.resize(cipher.size());
    ULONG out_len = 0;
    NTSTATUS st = BCryptDecrypt(hKey, const_cast<PUCHAR>(cipher.data()), static_cast<ULONG>(cipher.size()),
                                nullptr, iv_copy.data(), static_cast<ULONG>(iv_copy.size()),
                                plain.data(), static_cast<ULONG>(plain.size()), &out_len, BCRYPT_BLOCK_PADDING);
    BCryptDestroyKey(hKey);
    BCryptCloseAlgorithmProvider(hAlg, 0);
    if (st != 0) return false;
    plain.resize(out_len);
    return true;
#else
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return false;
    plain.resize(cipher.size() + 16);
    int len1 = 0, len2 = 0;
    bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, key.data(), iv.data()) == 1 &&
              EVP_DecryptUpdate(ctx, plain.data(), &len1, cipher.data(), static_cast<int>(cipher.size())) == 1 &&
              EVP_DecryptFinal_ex(ctx, plain.data() + len1, &len2) == 1;
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return false;
    plain.resize(static_cast<size_t>(len1 + len2));
    return true;
#endif
}

static Result<std::string> HttpPostFormSimple(
    const std::string& url,
    const std::string& bearer_token,
    const std::string& form_body) {
#ifdef _WIN32
    auto pu = brisk::HttpClient::ParseUrl(url);
    if (!pu.ok() || !pu.value) return Result<std::string>::Err("ERR_URL", "Invalid URL");

    auto to_wide = [](const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
        std::wstring w(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
        return w;
    };

    HINTERNET hSession = WinHttpOpen(L"RadiumDownloadManager/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return Result<std::string>::Err("ERR_NET", "WinHttpOpen failed");
    HINTERNET hConnect = WinHttpConnect(hSession, to_wide(pu.value->host).c_str(), pu.value->port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return Result<std::string>::Err("ERR_NET", "WinHttpConnect failed");
    }
    DWORD flags = (pu.value->scheme == "https") ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"POST", to_wide(pu.value->path_and_query).c_str(),
                                        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return Result<std::string>::Err("ERR_NET", "WinHttpOpenRequest failed");
    }
    std::wstring hdrs = L"Content-Type: application/x-www-form-urlencoded\r\n";
    if (!bearer_token.empty()) {
        hdrs += L"Authorization: Bearer " + to_wide(bearer_token) + L"\r\n";
    }
    BOOL ok = WinHttpSendRequest(hReq, hdrs.c_str(), static_cast<DWORD>(hdrs.size()),
                                 const_cast<char*>(form_body.data()), static_cast<DWORD>(form_body.size()),
                                 static_cast<DWORD>(form_body.size()), 0);
    if (ok) ok = WinHttpReceiveResponse(hReq, nullptr);
    std::string resp_body;
    if (ok) {
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            std::string chunk(avail, '\0');
            DWORD read = 0;
            if (WinHttpReadData(hReq, chunk.data(), avail, &read) && read > 0) {
                chunk.resize(read);
                resp_body += chunk;
            } else {
                break;
            }
            if (resp_body.size() > 4 * 1024 * 1024) break;
        }
    }
    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    if (!ok) return Result<std::string>::Err("ERR_NET", "HTTP POST failed");
    return Result<std::string>::Ok(std::move(resp_body));
#else
    (void)url;
    (void)bearer_token;
    (void)form_body;
    return Result<std::string>::Err("ERR_UNSUPPORTED", "HTTP POST requires WinHTTP on Windows");
#endif
}

} // namespace

// ============================================================================
// DebridManager
// ============================================================================

std::string DebridManager::ServiceName(DebridServiceKind kind) {
    switch (kind) {
        case DebridServiceKind::kRealDebrid: return "Real-Debrid";
        case DebridServiceKind::kAllDebrid:  return "AllDebrid";
        case DebridServiceKind::kPremiumize: return "Premiumize";
        case DebridServiceKind::kTorBox:     return "TorBox";
        case DebridServiceKind::kMegaDebrid: return "Mega-Debrid";
    }
    return "Real-Debrid";
}

std::string DebridManager::TokenPageUrl(DebridServiceKind kind) {
    switch (kind) {
        case DebridServiceKind::kRealDebrid: return "https://real-debrid.com/apitoken";
        case DebridServiceKind::kAllDebrid:  return "https://alldebrid.com/apikeys";
        case DebridServiceKind::kPremiumize: return "https://www.premiumize.me/account";
        case DebridServiceKind::kTorBox:     return "https://torbox.app/settings";
        case DebridServiceKind::kMegaDebrid: return "https://www.mega-debrid.eu/index.php?page=profil";
    }
    return "";
}

std::string DebridManager::ApiBaseUrl(DebridServiceKind kind) {
    switch (kind) {
        case DebridServiceKind::kRealDebrid: return "https://api.real-debrid.com/rest/1.0";
        case DebridServiceKind::kAllDebrid:  return "https://api.alldebrid.com/v4";
        case DebridServiceKind::kPremiumize: return "https://www.premiumize.me/api";
        case DebridServiceKind::kTorBox:     return "https://api.torbox.app/v1/api";
        case DebridServiceKind::kMegaDebrid: return "https://www.mega-debrid.eu/api.php";
    }
    return "";
}

bool DebridManager::SupportsMagnets(DebridServiceKind kind) {
    return kind != DebridServiceKind::kMegaDebrid;
}

Result<DebridUnlockedLink> DebridManager::UnlockLink(
    DebridServiceKind kind,
    const std::string& api_token,
    const std::string& host_url,
    std::atomic<bool>& cancel_flag) {
    if (api_token.empty()) {
        return Result<DebridUnlockedLink>::Err("ERR_DEBRID_AUTH", "Missing API token for " + ServiceName(kind));
    }
    if (cancel_flag.load()) {
        return Result<DebridUnlockedLink>::Err("ERR_CANCELED", "Canceled");
    }
    std::string endpoint;
    std::string bearer;
    std::string form = "link=" + UrlEncodeForm(host_url);
    switch (kind) {
        case DebridServiceKind::kRealDebrid:
            endpoint = ApiBaseUrl(kind) + "/unrestrict/link";
            bearer = api_token;
            break;
        case DebridServiceKind::kAllDebrid:
            endpoint = ApiBaseUrl(kind) + "/link/unlock";
            bearer = api_token;
            break;
        case DebridServiceKind::kPremiumize:
            endpoint = ApiBaseUrl(kind) + "/transfer/directdl";
            form = "apikey=" + UrlEncodeForm(api_token) + "&src=" + UrlEncodeForm(host_url);
            break;
        case DebridServiceKind::kTorBox:
            endpoint = ApiBaseUrl(kind) + "/webdl/createwebdownload";
            bearer = api_token;
            break;
        case DebridServiceKind::kMegaDebrid:
            endpoint = ApiBaseUrl(kind) + "?action=getLink&token=" + UrlEncodeForm(api_token);
            break;
    }
    auto post_res = HttpPostFormSimple(endpoint, bearer, form);
    if (!post_res.ok()) return Result<DebridUnlockedLink>::Err(post_res.error->code, post_res.error->message);
    auto root = json::Parse(*post_res.value);
    if (!root.is_object()) return Result<DebridUnlockedLink>::Err("ERR_DEBRID_PARSE", "Invalid JSON response from " + ServiceName(kind));

    DebridUnlockedLink out;
    if (root["download"].is_string()) out.direct_url = root["download"].as_string();
    else if (root["data"].is_object() && root["data"]["link"].is_string()) out.direct_url = root["data"]["link"].as_string();
    else if (root["debridLink"].is_string()) out.direct_url = root["debridLink"].as_string();
    else if (root["location"].is_string()) out.direct_url = root["location"].as_string();

    if (root["filename"].is_string()) out.file_name = root["filename"].as_string();
    if (root["filesize"].is_number()) out.file_size_bytes = static_cast<uint64_t>(root["filesize"].as_int64(0));
    if (out.direct_url.empty()) {
        return Result<DebridUnlockedLink>::Err("ERR_DEBRID_UNLOCK", "Debrid provider did not return a direct URL");
    }
    return Result<DebridUnlockedLink>::Ok(std::move(out));
}

Result<std::string> DebridManager::AddMagnet(
    DebridServiceKind kind,
    const std::string& api_token,
    const std::string& magnet_url,
    std::atomic<bool>& cancel_flag) {
    if (!SupportsMagnets(kind)) {
        return Result<std::string>::Err("ERR_DEBRID_UNSUPPORTED", ServiceName(kind) + " does not support magnet resolution");
    }
    if (api_token.empty()) {
        return Result<std::string>::Err("ERR_DEBRID_AUTH", "Missing API token for " + ServiceName(kind));
    }
    if (cancel_flag.load()) {
        return Result<std::string>::Err("ERR_CANCELED", "Canceled");
    }
    std::string endpoint;
    std::string bearer = api_token;
    std::string form = "magnet=" + UrlEncodeForm(magnet_url);
    switch (kind) {
        case DebridServiceKind::kRealDebrid: endpoint = ApiBaseUrl(kind) + "/torrents/addMagnet"; break;
        case DebridServiceKind::kAllDebrid:  endpoint = ApiBaseUrl(kind) + "/magnet/upload"; form = "magnets[]=" + UrlEncodeForm(magnet_url); break;
        case DebridServiceKind::kPremiumize: endpoint = ApiBaseUrl(kind) + "/transfer/create"; bearer.clear(); form = "apikey=" + UrlEncodeForm(api_token) + "&src=" + UrlEncodeForm(magnet_url); break;
        case DebridServiceKind::kTorBox:     endpoint = ApiBaseUrl(kind) + "/torrents/createtorrent"; break;
        case DebridServiceKind::kMegaDebrid: break;
    }
    auto post_res = HttpPostFormSimple(endpoint, bearer, form);
    if (!post_res.ok()) return Result<std::string>::Err(post_res.error->code, post_res.error->message);
    return Result<std::string>::Ok(*post_res.value);
}

// ============================================================================
// Cnl2Server (Click'n'Load 2)
// ============================================================================

std::optional<std::vector<uint8_t>> Cnl2Server::ExtractKeyFromJk(const std::string& jk_js) {
    auto ret_pos = jk_js.find("return");
    if (ret_pos == std::string::npos) return std::nullopt;
    size_t i = ret_pos + 6;
    while (i < jk_js.size() && std::isspace(static_cast<unsigned char>(jk_js[i]))) ++i;
    if (i >= jk_js.size() || (jk_js[i] != '\'' && jk_js[i] != '"')) return std::nullopt;
    char quote = jk_js[i++];
    auto endq = jk_js.find(quote, i);
    if (endq == std::string::npos) return std::nullopt;
    std::string hex = jk_js.substr(i, endq - i);
    if (hex.size() != 32) return std::nullopt;
    std::vector<uint8_t> key(16, 0);
    for (size_t k = 0; k < 16; ++k) {
        auto hex_nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
            if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
            return -1;
        };
        int hi = hex_nibble(hex[2 * k]);
        int lo = hex_nibble(hex[2 * k + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        key[k] = static_cast<uint8_t>((hi << 4) | lo);
    }
    return key;
}

Result<std::vector<std::string>> Cnl2Server::DecryptCrypted2(
    const std::string& crypted_b64,
    const std::string& jk_js) {
    auto key = ExtractKeyFromJk(jk_js);
    if (!key) {
        return Result<std::vector<std::string>>::Err("ERR_CNL2_KEY", "Could not extract 128-bit hex key from jk function");
    }
    auto cipher = Base64Decode(crypted_b64);
    if (!cipher || cipher->empty()) {
        return Result<std::vector<std::string>>::Err("ERR_CNL2_BASE64", "Invalid Base64 crypted payload");
    }
    std::vector<uint8_t> plain;
    if (!Aes128CbcDecrypt(*key, *key, *cipher, plain)) {
        return Result<std::vector<std::string>>::Err("ERR_CNL2_DECRYPT", "AES-128-CBC decryption failed");
    }
    std::string text(reinterpret_cast<const char*>(plain.data()), plain.size());
    std::vector<std::string> links;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line) && links.size() < kMaxLinks) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0' || std::isspace(static_cast<unsigned char>(line.back())))) {
            line.pop_back();
        }
        size_t a = line.find_first_not_of(" \t");
        if (a != std::string::npos) {
            links.push_back(line.substr(a));
        }
    }
    return Result<std::vector<std::string>>::Ok(std::move(links));
}

Cnl2Server::~Cnl2Server() {
    Stop();
}

bool Cnl2Server::Start(OnPackageCallback cb, uint16_t port) {
#ifdef _WIN32
    if (m_running.load()) return true;
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (ls == INVALID_SOCKET) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    if (bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(ls, 8) != 0) {
        closesocket(ls);
        return false;
    }
    m_listen_sock = static_cast<uintptr_t>(ls);
    m_running.store(true);
    m_thread = std::thread([this, cb = std::move(cb), ls]() {
        auto parse_form = [](const std::string& body) {
            std::map<std::string, std::string> out;
            std::istringstream iss(body);
            std::string kv;
            while (std::getline(iss, kv, '&')) {
                auto eq = kv.find('=');
                if (eq == std::string::npos) continue;
                std::string k = PathSanitizer::UrlDecode(kv.substr(0, eq));
                std::string v = kv.substr(eq + 1);
                for (char& ch : v) if (ch == '+') ch = ' ';
                out[k] = PathSanitizer::UrlDecode(v);
            }
            return out;
        };

        while (m_running.load()) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(ls, &fds);
            timeval tv{0, 100000};
            if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
            SOCKET c = accept(ls, nullptr, nullptr);
            if (c == INVALID_SOCKET) continue;
            std::string req;
            char buf[4096];
            int n = recv(c, buf, sizeof(buf), 0);
            if (n > 0) req.append(buf, static_cast<size_t>(n));
            auto hdr_end = req.find("\r\n\r\n");
            bool accepted = false;
            if (hdr_end != std::string::npos && req.size() <= kMaxBodyBytes + 8192) {
                std::string hdrs = req.substr(0, hdr_end);
                std::string body = req.substr(hdr_end + 4);
                auto form = parse_form(body);
                Cnl2Package pkg;
                pkg.name = form["package"];
                if (hdrs.rfind("POST /flash/addcrypted2", 0) == 0) {
                    auto dec = DecryptCrypted2(form["crypted"], form["jk"]);
                    if (dec.ok() && dec.value) {
                        pkg.links = std::move(*dec.value);
                        accepted = !pkg.links.empty() && (!cb || cb(pkg));
                    }
                } else if (hdrs.rfind("POST /flash/add", 0) == 0) {
                    std::istringstream lss(form["urls"]);
                    std::string u;
                    while (std::getline(lss, u) && pkg.links.size() < kMaxLinks) {
                        if (!u.empty() && u.back() == '\r') u.pop_back();
                        if (!u.empty()) pkg.links.push_back(u);
                    }
                    accepted = !pkg.links.empty() && (!cb || cb(pkg));
                }
            }
            const char* resp = accepted
                ? "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nsuccess"
                : "HTTP/1.1 400 Bad Request\r\nContent-Length: 6\r\nConnection: close\r\n\r\nfailed";
            send(c, resp, static_cast<int>(std::strlen(resp)), 0);
            closesocket(c);
        }
    });
    return true;
#else
    (void)cb;
    (void)port;
    return false;
#endif
}

void Cnl2Server::Stop() {
#ifdef _WIN32
    m_running.store(false);
    if (m_listen_sock != static_cast<uintptr_t>(-1)) {
        closesocket(static_cast<SOCKET>(m_listen_sock));
        m_listen_sock = static_cast<uintptr_t>(-1);
    }
    if (m_thread.joinable()) m_thread.join();
#endif
}

// ============================================================================
// SpecializedUrlPacks
// ============================================================================

Result<std::string> SpecializedUrlPacks::DecodeWrappedLink(const std::string& wrapped_url) {
    auto sep = wrapped_url.find("://");
    if (sep == std::string::npos) {
        return Result<std::string>::Err("ERR_WRAPPED_LINK", "Missing scheme in wrapped URL");
    }
    std::string scheme = ToLowerStr(wrapped_url.substr(0, sep));
    std::string payload = wrapped_url.substr(sep + 3);
    auto amp = payload.find('&');
    if (amp != std::string::npos) payload = payload.substr(0, amp);
    while (!payload.empty() && payload.back() == '/') payload.pop_back();

    auto decoded_bytes = Base64Decode(payload);
    if (!decoded_bytes) {
        return Result<std::string>::Err("ERR_WRAPPED_LINK", "Invalid Base64 in wrapped URL");
    }
    std::string text(reinterpret_cast<const char*>(decoded_bytes->data()), decoded_bytes->size());
    if (scheme == "thunder") {
        if (text.size() < 4 || text.rfind("AA", 0) != 0 || text.substr(text.size() - 2) != "ZZ") {
            return Result<std::string>::Err("ERR_WRAPPED_LINK", "Invalid thunder:// AA...ZZ framing");
        }
        text = text.substr(2, text.size() - 4);
    } else if (scheme == "flashget") {
        constexpr const char* kFg = "[FLASHGET]";
        constexpr const char* kFgEnd = "[/FLASHGET]";
        if (text.rfind(kFg, 0) == 0) text = text.substr(std::strlen(kFg));
        if (text.size() >= std::strlen(kFgEnd) && text.substr(text.size() - std::strlen(kFgEnd)) == kFgEnd) {
            text.resize(text.size() - std::strlen(kFgEnd));
        } else if (text.size() >= std::strlen(kFg) && text.substr(text.size() - std::strlen(kFg)) == kFg) {
            text.resize(text.size() - std::strlen(kFg));
        }
    } else if (scheme != "qqdl") {
        return Result<std::string>::Err("ERR_WRAPPED_LINK", "Unsupported wrapped link scheme: " + scheme);
    }

    std::string low = ToLowerStr(text);
    if (low.rfind("thunder://", 0) == 0 || low.rfind("flashget://", 0) == 0 || low.rfind("qqdl://", 0) == 0) {
        return Result<std::string>::Err("ERR_WRAPPED_LINK", "Nested wrapped links are not allowed");
    }
    if (low.rfind("http://", 0) != 0 && low.rfind("https://", 0) != 0 &&
        low.rfind("ftp://", 0) != 0 && low.rfind("ftps://", 0) != 0 && low.rfind("magnet:?", 0) != 0) {
        return Result<std::string>::Err("ERR_WRAPPED_LINK", "Decoded wrapped link is not a valid target URL");
    }
    return Result<std::string>::Ok(std::move(text));
}

std::vector<std::string> SpecializedUrlPacks::BuildGitHubMirrorCandidates(const std::string& url) {
    std::vector<std::string> out;
    out.push_back(url);
    std::string low = ToLowerStr(url);
    if (low.find("github.com/") != std::string::npos ||
        low.find("raw.githubusercontent.com/") != std::string::npos ||
        low.find("gist.githubusercontent.com/") != std::string::npos) {
        out.push_back("https://gh-proxy.com/" + url);
        out.push_back("https://mirror.ghproxy.com/" + url);
    }
    return out;
}

std::string SpecializedUrlPacks::ResolveFastestGitHubMirror(
    const std::string& url,
    std::atomic<bool>& cancel_flag) {
    auto candidates = BuildGitHubMirrorCandidates(url);
    for (const auto& c : candidates) {
        if (cancel_flag.load()) break;
        auto res = brisk::HttpClient::PerformGet(
            c, {}, 0, 0, 4000, false, cancel_flag,
            [](const brisk::HttpResponseMeta& m) {
                return (m.status_code >= 200 && m.status_code < 400)
                    ? brisk::HeaderAction::kStopSuccess : brisk::HeaderAction::kAbort;
            },
            [](const uint8_t*, size_t) { return true; });
        if (res.ok()) return c;
    }
    return url;
}

std::optional<std::string> SpecializedUrlPacks::ParseArxivId(const std::string& url) {
    static const std::regex kArxivRe(
        R"(arxiv\.org/(?:abs|pdf|src|e-print|html)/([0-9]{4}\.[0-9]{4,5}(?:v\d+)?|[a-z\-]+(?:\.[A-Z]{2})?/[0-9]{7}(?:v\d+)?))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(url, m, kArxivRe)) {
        return m[1].str();
    }
    return std::nullopt;
}

std::optional<ArxivDownloadBundle> SpecializedUrlPacks::BuildArxivDownloads(const std::string& url) {
    auto id = ParseArxivId(url);
    if (!id) return std::nullopt;
    ArxivDownloadBundle b;
    b.arxiv_id = *id;
    b.pdf_url = "https://arxiv.org/pdf/" + *id + ".pdf";
    b.latex_source_url = "https://arxiv.org/e-print/" + *id;
    b.bibtex_url = "https://arxiv.org/bibtex/" + *id;
    return b;
}

std::optional<HuggingFaceTarget> SpecializedUrlPacks::ParseHuggingFaceUrl(const std::string& url) {
    static const std::regex kHfRe(
        R"(https?://(?:huggingface\.co|hf-mirror\.com)/([^/\s]+)/([^/\s]+)/(?:blob|resolve)/([^/\s]+)/(.+))",
        std::regex::icase);
    std::smatch m;
    if (std::regex_search(url, m, kHfRe)) {
        HuggingFaceTarget t;
        t.owner = m[1].str();
        t.repo = m[2].str();
        t.revision = m[3].str();
        t.file_path = m[4].str();
        auto q = t.file_path.find('?');
        if (q != std::string::npos) t.file_path.resize(q);
        return t;
    }
    return std::nullopt;
}

std::string SpecializedUrlPacks::BuildHuggingFaceFileUrl(
    const HuggingFaceTarget& target,
    const std::string& commit_sha,
    bool use_mirror) {
    const std::string host = use_mirror ? "https://hf-mirror.com" : "https://huggingface.co";
    const std::string rev = commit_sha.empty() ? target.revision : commit_sha;
    return host + "/" + target.owner + "/" + target.repo + "/resolve/" + rev + "/" + target.file_path;
}

Result<Ed2kLinkInfo> SpecializedUrlPacks::ParseEd2kLink(const std::string& ed2k_url) {
    static const std::regex kEd2kRe(R"(^ed2k://\|file\|([^|]+)\|(\d+)\|([0-9a-fA-F]{32})\|)", std::regex::icase);
    std::smatch m;
    if (!std::regex_search(ed2k_url, m, kEd2kRe)) {
        return Result<Ed2kLinkInfo>::Err("ERR_ED2K_INVALID", "Invalid eD2k file link format");
    }
    Ed2kLinkInfo info;
    info.file_name = PathSanitizer::SanitizeFileName(PathSanitizer::UrlDecode(m[1].str()));
    try {
        info.file_size = std::stoull(m[2].str());
    } catch (...) {
        return Result<Ed2kLinkInfo>::Err("ERR_ED2K_SIZE", "Invalid eD2k file size");
    }
    info.md4_hash = ToLowerStr(m[3].str());
    return Result<Ed2kLinkInfo>::Ok(std::move(info));
}

} // namespace radium::packs
