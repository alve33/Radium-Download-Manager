// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Shared Browser Extension Downloader, Validator & Installer
#include "core/extension_installer.hpp"

#include "archive/zip_preview.hpp"
#include "brisk_engine/http_client.hpp"
#include "common/central_config.hpp"
#include "common/crypto_utils.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/version.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <set>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace radium::core {

namespace {

constexpr std::string_view kReleaseExtensionZipUrl =
    "https://github.com/alve33/Radium-Download-Manager/releases/latest/download/radium-extension.zip";
constexpr std::string_view kMainBranchZipUrl =
    "https://github.com/alve33/Radium-Download-Manager/archive/refs/heads/main.zip";

std::string ToLowerAscii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::optional<std::string> FindHeaderCaseInsensitive(
    const std::map<std::string, std::string>& headers,
    std::string_view target_lower) {
    for (const auto& [k, v] : headers) {
        if (ToLowerAscii(k) == target_lower) {
            return v;
        }
    }
    return std::nullopt;
}

bool IsReservedDosName(std::string_view segment) {
    std::string base = ToLowerAscii(segment);
    auto dot = base.find('.');
    if (dot != std::string::npos) {
        base = base.substr(0, dot);
    }
    if (base == "con" || base == "prn" || base == "aux" || base == "nul") return true;
    if (base.size() == 4 && (base.rfind("com", 0) == 0 || base.rfind("lpt", 0) == 0) &&
        base[3] >= '1' && base[3] <= '9') {
        return true;
    }
    return false;
}

bool IsTrustedGithubDownloadUrl(std::string_view url) {
    return url.rfind("https://github.com/alve33/Radium-Download-Manager/releases/latest/download/", 0) == 0 ||
           url.rfind("https://github.com/alve33/Radium-Download-Manager/releases/download/", 0) == 0 ||
           url.rfind("https://github.com/alve33/Radium-Download-Manager/archive/refs/heads/", 0) == 0 ||
           url.rfind("https://codeload.github.com/alve33/Radium-Download-Manager/", 0) == 0 ||
           url.rfind("https://objects.githubusercontent.com/", 0) == 0;
}

bool IsRateLimitedResponse(const ExtensionFetchResponse& resp) {
    if (resp.status_code == 429) return true;
    if (resp.status_code == 403) {
        for (const auto& [k, v] : resp.headers) {
            if (ToLowerAscii(k) == "x-ratelimit-remaining" && v == "0") {
                return true;
            }
        }
        if (ToLowerAscii(resp.body).find("rate limit") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::string ReadFileUtf8(const std::filesystem::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

bool WriteFileBinary(const std::filesystem::path& p, const std::string& data) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    if (ec) return false;
    std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    ofs.flush();
    return ofs.good();
}

#ifdef _WIN32
std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring w(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

std::string WideToUtf8(std::wstring_view w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string s(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), needed, nullptr, nullptr);
    return s;
}

std::optional<std::string> ReadHkcuDefaultString(const wchar_t* subkey) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
        return std::nullopt;
    }
    DWORD type = 0;
    DWORD cbData = 0;
    LONG rc = RegQueryValueExW(hKey, nullptr, nullptr, &type, nullptr, &cbData);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || cbData < sizeof(wchar_t)) {
        RegCloseKey(hKey);
        return std::nullopt;
    }
    std::wstring buf(cbData / sizeof(wchar_t), L'\0');
    rc = RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &cbData);
    RegCloseKey(hKey);
    if (rc != ERROR_SUCCESS) return std::nullopt;
    while (!buf.empty() && buf.back() == L'\0') buf.pop_back();
    return WideToUtf8(buf);
}

bool WriteHkcuDefaultString(const wchar_t* subkey, const std::wstring& value) {
    HKEY hKey = nullptr;
    DWORD disp = 0;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, REG_OPTION_NON_VOLATILE,
                        KEY_WRITE, nullptr, &hKey, &disp) != ERROR_SUCCESS) {
        return false;
    }
    const DWORD cbData = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    LONG rc = RegSetValueExW(hKey, nullptr, 0, REG_SZ,
                             reinterpret_cast<const BYTE*>(value.c_str()), cbData);
    RegCloseKey(hKey);
    return rc == ERROR_SUCCESS;
}
#endif

} // namespace

const std::vector<std::string>& ExtensionInstaller::RequiredFiles() {
    static const std::vector<std::string> kRequired = {
        "manifest.json",
        "manifest.firefox.json",
        "popup.html",
        "catch-script/catch.js",
        "css/popup.css",
        "css/tokens.css",
        "icons/icon-16.png",
        "icons/icon-32.png",
        "icons/icon-48.png",
        "icons/icon-128.png",
        "js/background.js",
        "js/categories.generated.js",
        "js/content-script.js",
        "js/function.js",
        "js/init.js",
        "js/popup.js"
    };
    return kRequired;
}

std::filesystem::path ExtensionInstaller::DefaultInstallDir(const std::filesystem::path& app_data_dir) {
    return app_data_dir / "extension";
}

bool ExtensionInstaller::IsSafeRelativeExtensionPath(std::string_view rel_path) {
    if (rel_path.empty() || rel_path.size() > 240) return false;
    if (rel_path.front() == '/' || rel_path.back() == '/') return false;
    if (rel_path.find('\\') != std::string_view::npos) return false;
    if (rel_path.find(':') != std::string_view::npos) return false;
    for (unsigned char c : rel_path) {
        if (c < 0x20 || c == 0x7F) return false;
    }

    size_t start = 0;
    while (start < rel_path.size()) {
        size_t slash = rel_path.find('/', start);
        std::string_view seg = (slash == std::string_view::npos)
            ? rel_path.substr(start)
            : rel_path.substr(start, slash - start);
        if (seg.empty() || seg == "." || seg == "..") return false;
        if (seg.back() == '.' || seg.back() == ' ') return false;
        if (IsReservedDosName(seg)) return false;
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }
    return true;
}

bool ExtensionInstaller::ValidateExtensionDirectory(
    const std::filesystem::path& dir,
    std::string* out_version,
    std::string* out_error) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) {
        if (out_error) *out_error = "Extension directory does not exist.";
        return false;
    }

    for (const auto& rel : RequiredFiles()) {
        if (!IsSafeRelativeExtensionPath(rel)) {
            if (out_error) *out_error = "Unsafe relative path in required file list.";
            return false;
        }
        const auto p = dir / std::filesystem::path(rel);
        if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
            if (out_error) *out_error = "Missing required extension file: " + rel;
            return false;
        }
        const auto sz = std::filesystem::file_size(p, ec);
        if (ec || sz == 0) {
            if (out_error) *out_error = "Extension file is empty or unreadable: " + rel;
            return false;
        }
    }

    // Validate manifest.json
    const std::string manifest_raw = ReadFileUtf8(dir / "manifest.json");
    if (manifest_raw.empty()) {
        if (out_error) *out_error = "Failed to read manifest.json.";
        return false;
    }

    try {
        auto manifest = radium::json::Parse(manifest_raw);
        if (!manifest.is_object()) {
            if (out_error) *out_error = "manifest.json is not a valid JSON object.";
            return false;
        }
        if (manifest["manifest_version"].as_int64(0) != 3) {
            if (out_error) *out_error = "manifest.json must declare manifest_version 3.";
            return false;
        }
        std::string name = manifest["name"].as_string("");
        std::string version = manifest["version"].as_string("");
        if (name.empty() || version.empty()) {
            if (out_error) *out_error = "manifest.json is missing name or version.";
            return false;
        }

        // Verify nativeMessaging permission
        bool has_native_messaging = false;
        if (manifest.contains("permissions") && manifest["permissions"].is_array()) {
            for (const auto& perm : manifest["permissions"].as_array()) {
                if (perm.as_string("") == "nativeMessaging") {
                    has_native_messaging = true;
                    break;
                }
            }
        }
        if (!has_native_messaging) {
            if (out_error) *out_error = "manifest.json does not declare nativeMessaging permission.";
            return false;
        }

        // Verify background service worker and action popup exist
        std::string sw = manifest["background"]["service_worker"].as_string("");
        std::string popup = manifest["action"]["default_popup"].as_string("");
        if (!IsSafeRelativeExtensionPath(sw) || !std::filesystem::exists(dir / sw, ec)) {
            if (out_error) *out_error = "manifest.json references missing background service_worker: " + sw;
            return false;
        }
        if (!IsSafeRelativeExtensionPath(popup) || !std::filesystem::exists(dir / popup, ec)) {
            if (out_error) *out_error = "manifest.json references missing action default_popup: " + popup;
            return false;
        }

        // Validate manifest.firefox.json as well
        const std::string ff_raw = ReadFileUtf8(dir / "manifest.firefox.json");
        auto ff = radium::json::Parse(ff_raw);
        if (!ff.is_object() || ff["manifest_version"].as_int64(0) != 3) {
            if (out_error) *out_error = "manifest.firefox.json is invalid.";
            return false;
        }

        if (out_version) *out_version = version;
        return true;
    } catch (const std::exception& ex) {
        if (out_error) *out_error = std::string("Invalid manifest JSON: ") + ex.what();
        return false;
    } catch (...) {
        if (out_error) *out_error = "Invalid manifest JSON.";
        return false;
    }
}

InstalledExtensionInfo ExtensionInstaller::InspectInstalledExtension(
    const std::filesystem::path& install_dir,
    std::string_view recorded_commit_sha) {
    InstalledExtensionInfo info;
    info.install_dir = install_dir;
    info.commit_sha = std::string(recorded_commit_sha);

    std::error_code ec;
    if (!std::filesystem::exists(install_dir, ec) || !std::filesystem::is_directory(install_dir, ec)) {
        return info;
    }

    std::string version;
    std::string err;
    if (ValidateExtensionDirectory(install_dir, &version, &err)) {
        info.installed = true;
        info.manifest_valid = true;
        info.all_required_files_present = true;
        info.version = version;
    }
    return info;
}

ExtensionFetchFn ExtensionInstaller::DefaultHttpFetch() {
    return [](const std::string& url, const std::map<std::string, std::string>& extra_headers) -> ExtensionFetchResponse {
        ExtensionFetchResponse out;
        if (!IsTrustedGithubDownloadUrl(url)) {
            out.error_message = "Blocked untrusted download URL: " + url;
            return out;
        }

        std::map<std::string, std::string> headers = extra_headers;
        if (headers.find("User-Agent") == headers.end()) {
            headers["User-Agent"] = std::string(radium::config::NetworkPoliciesConfig::kUserAgent);
        }

        std::atomic<bool> cancel{false};
        auto res = radium::brisk::HttpClient::PerformGet(
            url,
            headers,
            std::nullopt,
            std::nullopt,
            30000,
            false,
            cancel,
            [&out](const radium::brisk::HttpResponseMeta& meta) {
                out.status_code = meta.status_code;
                out.headers = meta.raw_headers;
                return radium::brisk::HeaderAction::kContinue;
            },
            [&out](const uint8_t* data, size_t len) {
                constexpr size_t kMaxPackageBytes = 64ULL * 1024ULL * 1024ULL;
                if (out.body.size() + len > kMaxPackageBytes) return false;
                out.body.append(reinterpret_cast<const char*>(data), len);
                return true;
            });

        if (!res.ok() && out.status_code == 0) {
            out.error_message = res.error->message;
        }
        return out;
    };
}

ExtensionInstallResult ExtensionInstaller::DownloadAndInstall(
    const std::filesystem::path& app_data_dir,
    const ExtensionProgressFn& on_progress,
    const ExtensionFetchFn& fetch_fn) {
    ExtensionInstallResult result;
    const auto install_dir = DefaultInstallDir(app_data_dir);
    result.install_dir = install_dir;

    ExtensionFetchFn fetcher = fetch_fn ? fetch_fn : DefaultHttpFetch();
    const auto report = [&on_progress](int pct, const std::string& msg) {
        if (on_progress) on_progress(pct, msg);
    };

    // 1. Inspect existing installation and cached package metadata
    std::string existing_version;
    const bool existing_valid = ValidateExtensionDirectory(install_dir, &existing_version);
    const auto cache_path = app_data_dir / "extension_cache.json";
    std::string cached_etag;
    std::string cached_last_mod;
    std::string cached_sha256;
    std::string cached_source_url;
    if (existing_valid) {
        const std::string cache_raw = ReadFileUtf8(cache_path);
        if (!cache_raw.empty()) {
            try {
                auto cj = radium::json::Parse(cache_raw);
                if (cj.is_object()) {
                    cached_etag = cj["etag"].as_string("");
                    cached_last_mod = cj["lastModified"].as_string("");
                    cached_sha256 = cj["sha256"].as_string("");
                    cached_source_url = cj["sourceUrl"].as_string("");
                }
            } catch (...) {}
        }
    }

    // 2. Prefer downloading prepackaged extension ZIP directly from official GitHub Release
    //    (zero api.github.com calls, immune to the unauthenticated REST API rate limit).
    std::map<std::string, std::string> req_headers = {
        {"Accept", "application/zip, application/octet-stream"},
        {"User-Agent", std::string(radium::config::NetworkPoliciesConfig::kUserAgent)}
    };
    if (existing_valid && cached_source_url == kReleaseExtensionZipUrl) {
        if (!cached_etag.empty()) req_headers["If-None-Match"] = cached_etag;
        if (!cached_last_mod.empty()) req_headers["If-Modified-Since"] = cached_last_mod;
    }

    report(10, "Downloading extension package from GitHub Release...");
    std::string used_url(kReleaseExtensionZipUrl);
    ExtensionFetchResponse resp = fetcher(used_url, req_headers);

    if (resp.status_code == 304 && existing_valid) {
        report(100, "Browser extension is already up to date (v" + existing_version + ").");
        result.success = true;
        result.already_up_to_date = true;
        result.version = existing_version;
        result.package_sha256 = cached_sha256;
        result.installed_files = RequiredFiles();
        return result;
    }

    // 3. If the release asset is missing (HTTP 404), fall back once to the main branch archive ZIP
    //    (also zero api.github.com calls). Never loop or retry rate limits automatically.
    if (resp.status_code == 404) {
        report(20, "Release asset not found; downloading extension archive from main branch...");
        used_url = std::string(kMainBranchZipUrl);
        resp = fetcher(used_url, {
            {"Accept", "application/zip, application/octet-stream"},
            {"User-Agent", std::string(radium::config::NetworkPoliciesConfig::kUserAgent)}
        });
    }

    if (IsRateLimitedResponse(resp)) {
        result.rate_limited = true;
        result.error_code = "ERR_GITHUB_RATE_LIMIT";
        result.error_message = "GitHub rate limit exceeded (HTTP " + std::to_string(resp.status_code) +
                               "). Please wait a few minutes and click Retry Download.";
        return result;
    }

    if (resp.status_code == 403) {
        result.error_code = "ERR_GITHUB_FORBIDDEN";
        result.error_message = "GitHub denied access to the extension package (HTTP 403). Please try again later.";
        return result;
    }

    if (resp.status_code == 404) {
        result.error_code = "ERR_EXTENSION_ASSET_NOT_FOUND";
        result.error_message = "Extension package was not found on GitHub (HTTP 404).";
        return result;
    }

    if (resp.status_code != 200 || resp.body.empty()) {
        result.error_code = "ERR_GITHUB_NETWORK";
        result.error_message = resp.error_message.empty()
            ? ("Failed to download extension package (HTTP " + std::to_string(resp.status_code) + ").")
            : ("Failed to download extension package: " + resp.error_message);
        return result;
    }

    const std::string zip_sha256 = radium::CryptoUtils::ComputeBufferSha256Hex(resp.body);
    const std::string resp_etag = FindHeaderCaseInsensitive(resp.headers, "etag").value_or("");
    const std::string resp_last_mod = FindHeaderCaseInsensitive(resp.headers, "last-modified").value_or("");

    // 4. Skip redundant re-extraction if identical package SHA-256 is already installed & valid
    if (existing_valid && !zip_sha256.empty() && zip_sha256 == cached_sha256) {
        radium::json::Value cj = radium::json::Object{};
        cj["version"] = existing_version;
        cj["sha256"] = zip_sha256;
        cj["etag"] = resp_etag.empty() ? cached_etag : resp_etag;
        cj["lastModified"] = resp_last_mod.empty() ? cached_last_mod : resp_last_mod;
        cj["sourceUrl"] = used_url;
        WriteFileBinary(cache_path, cj.dump());

        report(100, "Browser extension is already up to date (v" + existing_version + ").");
        result.success = true;
        result.already_up_to_date = true;
        result.version = existing_version;
        result.package_sha256 = zip_sha256;
        result.installed_files = RequiredFiles();
        return result;
    }

    // 5. Write ZIP to isolated temp file and extract into staging directory
    report(55, "Verifying and extracting extension package...");
    const auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto tmp_dir = app_data_dir / "tmp";
    const auto tmp_zip = tmp_dir / ("ext_pkg_" + std::to_string(now_ticks) + ".zip");
    const auto extract_dir = tmp_dir / ("ext_stage_" + std::to_string(now_ticks));

    const auto cleanup_tmp = [&tmp_zip, &extract_dir]() {
        std::error_code clean_ec;
        std::filesystem::remove(tmp_zip, clean_ec);
        std::filesystem::remove_all(extract_dir, clean_ec);
    };

    if (!WriteFileBinary(tmp_zip, resp.body)) {
        cleanup_tmp();
        result.error_code = "ERR_WRITE_FILE";
        result.error_message = "Failed to write downloaded extension package to temporary storage.";
        return result;
    }

    auto extract_res = radium::archive::ArchiveExtractor::ExtractArchive(
        tmp_zip,
        extract_dir,
        radium::archive::SubfolderPolicy::kNever,
        64ULL * 1024ULL * 1024ULL,
        500);
    std::error_code rm_zip_ec;
    std::filesystem::remove(tmp_zip, rm_zip_ec);

    if (!extract_res.ok()) {
        cleanup_tmp();
        result.error_code = extract_res.error->code.empty() ? "ERR_ARCHIVE_EXTRACT_FAILED" : extract_res.error->code;
        result.error_message = "Failed to extract extension package: " + extract_res.error->message;
        return result;
    }

    // 6. Locate extension root inside extracted directory (direct root, extension/, or <repo>-main/extension/)
    std::error_code ec;
    std::filesystem::path candidate_root;
    if (std::filesystem::is_regular_file(extract_dir / "manifest.json", ec)) {
        candidate_root = extract_dir;
    } else if (std::filesystem::is_regular_file(extract_dir / "extension" / "manifest.json", ec)) {
        candidate_root = extract_dir / "extension";
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(extract_dir, ec)) {
            if (!entry.is_directory(ec)) continue;
            auto nested = entry.path() / "extension";
            if (std::filesystem::is_regular_file(nested / "manifest.json", ec)) {
                candidate_root = nested;
                break;
            }
        }
    }

    if (candidate_root.empty()) {
        cleanup_tmp();
        result.error_code = "ERR_MISSING_EXTENSION_FILES";
        result.error_message = "Downloaded archive does not contain a valid Radium extension manifest.";
        return result;
    }

    // 7. Verify all extracted relative paths are safe and collect installed file list
    std::vector<std::string> rel_files;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(candidate_root, ec)) {
        if (ec) {
            cleanup_tmp();
            result.error_code = "ERR_STAGE_DIR";
            result.error_message = "Failed to enumerate extracted extension files.";
            return result;
        }
        if (entry.is_symlink(ec)) {
            cleanup_tmp();
            result.error_code = "ERR_UNSAFE_PATH";
            result.error_message = "Rejected symbolic link in extension archive.";
            return result;
        }
        if (!entry.is_regular_file(ec)) continue;
        auto rel = entry.path().lexically_relative(candidate_root).generic_string();
        if (!IsSafeRelativeExtensionPath(rel)) {
            cleanup_tmp();
            result.error_code = "ERR_UNSAFE_PATH";
            result.error_message = "Rejected unsafe file path in extension archive: " + rel;
            return result;
        }
        rel_files.push_back(std::move(rel));
    }

    // 8. Validate staged extension directory before touching any existing installation
    report(85, "Validating extension manifest and resources...");
    std::string ext_version;
    std::string val_err;
    if (!ValidateExtensionDirectory(candidate_root, &ext_version, &val_err)) {
        cleanup_tmp();
        result.error_code = "ERR_INVALID_EXTENSION";
        result.error_message = val_err;
        return result;
    }

    // 9. Atomic replacement of install_dir
    report(95, "Installing extension files...");
    std::filesystem::create_directories(install_dir.parent_path(), ec);
    if (ec) {
        cleanup_tmp();
        result.error_code = "ERR_INSTALL_DIR";
        result.error_message = "Failed to create installation parent directory: " + ec.message();
        return result;
    }

    const auto backup_dir = app_data_dir / "extension.bak";
    std::filesystem::remove_all(backup_dir, ec);
    bool had_existing = std::filesystem::exists(install_dir, ec);
    if (had_existing) {
        std::filesystem::rename(install_dir, backup_dir, ec);
        if (ec) {
            cleanup_tmp();
            result.error_code = "ERR_REPLACE_EXTENSION";
            result.error_message = "Could not move previous extension aside (close any file locks and retry): " + ec.message();
            return result;
        }
    }

    std::filesystem::rename(candidate_root, install_dir, ec);
    if (ec) {
        std::error_code copy_ec;
        std::filesystem::create_directories(install_dir, copy_ec);
        std::filesystem::copy(
            candidate_root, install_dir,
            std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing,
            copy_ec);
        if (copy_ec) {
            std::filesystem::remove_all(install_dir, copy_ec);
            if (had_existing) {
                std::error_code restore_ec;
                std::filesystem::rename(backup_dir, install_dir, restore_ec);
            }
            cleanup_tmp();
            result.error_code = "ERR_REPLACE_EXTENSION";
            result.error_message = "Failed to finalize extension installation: " + copy_ec.message();
            return result;
        }
    }

    if (had_existing) {
        std::filesystem::remove_all(backup_dir, ec);
    }
    cleanup_tmp();

    // 10. Persist cache metadata for future updates
    radium::json::Value cj = radium::json::Object{};
    cj["version"] = ext_version;
    cj["sha256"] = zip_sha256;
    cj["etag"] = resp_etag;
    cj["lastModified"] = resp_last_mod;
    cj["sourceUrl"] = used_url;
    WriteFileBinary(cache_path, cj.dump());

    report(100, "Extension downloaded and verified.");
    result.success = true;
    result.version = ext_version;
    result.package_sha256 = zip_sha256;
    result.installed_files = std::move(rel_files);
    RADIUM_LOG_INFO("EXT", "Installed Radium browser extension v" + ext_version + " (sha256=" + zip_sha256 + ") to " + install_dir.string());
    return result;
}

NativeMessagingStatus ExtensionInstaller::VerifyOrRepairNativeMessaging(
    const std::filesystem::path& exe_dir,
    const std::filesystem::path& app_data_dir) {
    NativeMessagingStatus status;
    std::error_code ec;

#ifdef _WIN32
    const auto host_exe = exe_dir / "radium-nmh.exe";
#else
    const auto host_exe = exe_dir / "radium-nmh";
#endif
    status.host_executable_path = host_exe;
    status.host_executable_found = std::filesystem::exists(host_exe, ec);

    if (!status.host_executable_found) {
        status.summary = "Native Messaging Host executable (radium-nmh.exe) was not found in " + exe_dir.string();
        return status;
    }

#ifdef _WIN32
    const wchar_t* chrome_key = L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.radium.downloadmanager";
    const wchar_t* edge_key = L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.radium.downloadmanager";
    const wchar_t* brave_key = L"Software\\BraveSoftware\\Brave-Browser\\NativeMessagingHosts\\com.radium.downloadmanager";
    const wchar_t* firefox_key = L"Software\\Mozilla\\NativeMessagingHosts\\com.radium.downloadmanager";

    auto check_manifest_points_to_existing_host = [](const std::optional<std::string>& reg_path, bool is_firefox) -> bool {
        if (!reg_path || reg_path->empty()) return false;
        std::error_code fec;
        std::filesystem::path mp(*reg_path);
        if (!std::filesystem::exists(mp, fec)) return false;
        std::string raw = ReadFileUtf8(mp);
        if (raw.empty()) return false;
        try {
            auto j = radium::json::Parse(raw);
            if (j["name"].as_string("") != "com.radium.downloadmanager") return false;
            std::string host_path = j["path"].as_string("");
            if (host_path.empty() || !std::filesystem::exists(host_path, fec)) return false;
            if (is_firefox) {
                return j.contains("allowed_extensions") && j["allowed_extensions"].is_array();
            }
            return j.contains("allowed_origins") && j["allowed_origins"].is_array();
        } catch (...) {
            return false;
        }
    };

    const bool chrome_ok = check_manifest_points_to_existing_host(ReadHkcuDefaultString(chrome_key), false);
    const bool edge_ok = check_manifest_points_to_existing_host(ReadHkcuDefaultString(edge_key), false);
    const bool brave_ok = check_manifest_points_to_existing_host(ReadHkcuDefaultString(brave_key), false);
    const bool firefox_ok = check_manifest_points_to_existing_host(ReadHkcuDefaultString(firefox_key), true);

    if (chrome_ok && edge_ok && brave_ok && firefox_ok) {
        status.registry_configured = true;
        status.manifest_valid = true;
        status.summary = "Native Messaging Host configured (com.radium.downloadmanager -> " + host_exe.string() + ").";
        return status;
    }

    // Per-user non-admin repair/registration in %LOCALAPPDATA%\Radium Download Manager\manifests
    const auto manifest_dir = app_data_dir / "manifests";
    const auto chromium_manifest_path = manifest_dir / "com.radium.downloadmanager.chromium.json";
    const auto firefox_manifest_path = manifest_dir / "com.radium.downloadmanager.firefox.json";

    radium::json::Value chromium_json = radium::json::Object{};
    chromium_json["name"] = "com.radium.downloadmanager";
    chromium_json["description"] = "Radium Download Manager Native Messaging Host";
    chromium_json["path"] = host_exe.string();
    chromium_json["type"] = "stdio";
    chromium_json["allowed_origins"] = radium::json::Array{
        std::string("chrome-extension://mdgmlcnpodoehidlplplfegngnjnilap/"),
        std::string("chrome-extension://chbojfoombcanhhpoclailollapkglma/")
    };

    radium::json::Value firefox_json = radium::json::Object{};
    firefox_json["name"] = "com.radium.downloadmanager";
    firefox_json["description"] = "Radium Download Manager Native Messaging Host";
    firefox_json["path"] = host_exe.string();
    firefox_json["type"] = "stdio";
    firefox_json["allowed_extensions"] = radium::json::Array{
        std::string("radium-extension@radiumdownloadmanager.org"),
        std::string("flux-extension@fluxdownloadmanager.org")
    };

    const bool wrote_chromium = WriteFileBinary(chromium_manifest_path, chromium_json.dump());
    const bool wrote_firefox = WriteFileBinary(firefox_manifest_path, firefox_json.dump());

    if (wrote_chromium && wrote_firefox) {
        const std::wstring w_chromium = chromium_manifest_path.wstring();
        const std::wstring w_firefox = firefox_manifest_path.wstring();
        bool reg_ok = WriteHkcuDefaultString(chrome_key, w_chromium) &&
                      WriteHkcuDefaultString(edge_key, w_chromium) &&
                      WriteHkcuDefaultString(brave_key, w_chromium) &&
                      WriteHkcuDefaultString(firefox_key, w_firefox);
        status.registry_configured = reg_ok;
        status.manifest_valid = true;
        status.summary = reg_ok
            ? ("Native Messaging Host registered (com.radium.downloadmanager -> " + host_exe.string() + ").")
            : "Native Messaging Host manifests written, but HKCU registry update failed.";
    } else {
        status.summary = "Failed to write Native Messaging Host manifests to " + manifest_dir.string();
    }
#else
    status.registry_configured = true;
    status.manifest_valid = true;
    status.summary = "Native Messaging Host binary found at " + host_exe.string();
#endif
    return status;
}

} // namespace radium::core
