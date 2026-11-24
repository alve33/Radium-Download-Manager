// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Shared Browser Extension Downloader, Validator & Installer
#include "core/extension_installer.hpp"

#include "brisk_engine/http_client.hpp"
#include "common/central_config.hpp"
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

constexpr std::string_view kGithubBranchesMainUrl =
    "https://api.github.com/repos/alve33/Radium-Download-Manager/branches/main";
constexpr std::string_view kGithubTreePrefixUrl =
    "https://api.github.com/repos/alve33/Radium-Download-Manager/git/trees/";
constexpr std::string_view kGithubRawPrefixUrl =
    "https://raw.githubusercontent.com/alve33/Radium-Download-Manager/";

std::string ToLowerAscii(std::string_view sv) {
    std::string out(sv);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
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

bool IsValidHexSha40(std::string_view sha) {
    if (sha.size() != 40) return false;
    for (char c : sha) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

bool IsTrustedGithubApiOrRawUrl(std::string_view url) {
    return url.rfind("https://api.github.com/repos/alve33/Radium-Download-Manager/", 0) == 0 ||
           url.rfind("https://raw.githubusercontent.com/alve33/Radium-Download-Manager/", 0) == 0;
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
        auto manifest = openidm::json::Parse(manifest_raw);
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
        auto ff = openidm::json::Parse(ff_raw);
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
        if (!IsTrustedGithubApiOrRawUrl(url)) {
            out.error_message = "Blocked untrusted download URL: " + url;
            return out;
        }

        std::map<std::string, std::string> headers = extra_headers;
        if (headers.find("User-Agent") == headers.end()) {
            headers["User-Agent"] = std::string(radium::config::NetworkPoliciesConfig::kUserAgent);
        }

        std::atomic<bool> cancel{false};
        auto res = openidm::brisk::HttpClient::PerformGet(
            url,
            headers,
            std::nullopt,
            std::nullopt,
            20000,
            false,
            cancel,
            [&out](const openidm::brisk::HttpResponseMeta& meta) {
                out.status_code = meta.status_code;
                out.headers = meta.raw_headers;
                return openidm::brisk::HeaderAction::kContinue;
            },
            [&out](const uint8_t* data, size_t len) {
                constexpr size_t kMaxFileBytes = 16 * 1024 * 1024;
                if (out.body.size() + len > kMaxFileBytes) return false;
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

    const std::map<std::string, std::string> api_headers = {
        {"Accept", "application/vnd.github+json"},
        {"User-Agent", std::string(radium::config::NetworkPoliciesConfig::kUserAgent)}
    };

    // 1. Check latest GitHub release first; if no standalone extension zip asset is published,
    //    proceed to fetch the pinned commit SHA from main and download extension/ files.
    report(5, "Checking latest GitHub release...");
    const auto rel_resp = fetcher(
        std::string(radium::config::UpdateAndReleaseConfig::kApiLatestReleaseUrl),
        api_headers);
    if (IsRateLimitedResponse(rel_resp)) {
        result.rate_limited = true;
        result.error_code = "ERR_GITHUB_RATE_LIMIT";
        result.error_message = "GitHub API rate limit exceeded. Please wait a few minutes and try again.";
        return result;
    }

    // 2. Resolve consistent commit SHA from main branch so all files come from one exact revision.
    report(12, "Resolving repository revision from GitHub...");
    const auto branch_resp = fetcher(std::string(kGithubBranchesMainUrl), api_headers);
    if (IsRateLimitedResponse(branch_resp)) {
        result.rate_limited = true;
        result.error_code = "ERR_GITHUB_RATE_LIMIT";
        result.error_message = "GitHub API rate limit exceeded while resolving branch commit.";
        return result;
    }
    if (branch_resp.status_code != 200 || branch_resp.body.empty()) {
        result.error_code = "ERR_GITHUB_NETWORK";
        result.error_message = branch_resp.error_message.empty()
            ? ("Failed to query GitHub branch metadata (HTTP " + std::to_string(branch_resp.status_code) + ").")
            : branch_resp.error_message;
        return result;
    }

    std::string commit_sha;
    try {
        auto branch_json = openidm::json::Parse(branch_resp.body);
        commit_sha = branch_json["commit"]["sha"].as_string("");
    } catch (...) {
        commit_sha.clear();
    }
    if (!IsValidHexSha40(commit_sha)) {
        result.error_code = "ERR_GITHUB_INVALID_METADATA";
        result.error_message = "GitHub returned an invalid commit SHA for the main branch.";
        return result;
    }
    result.commit_sha = commit_sha;

    // 3. Fetch recursive tree at that exact commit SHA.
    report(20, "Listing extension files from GitHub...");
    const std::string tree_url = std::string(kGithubTreePrefixUrl) + commit_sha + "?recursive=1";
    const auto tree_resp = fetcher(tree_url, api_headers);
    if (IsRateLimitedResponse(tree_resp)) {
        result.rate_limited = true;
        result.error_code = "ERR_GITHUB_RATE_LIMIT";
        result.error_message = "GitHub API rate limit exceeded while listing extension files.";
        return result;
    }
    if (tree_resp.status_code != 200 || tree_resp.body.empty()) {
        result.error_code = "ERR_GITHUB_TREE";
        result.error_message = tree_resp.error_message.empty()
            ? ("Failed to list repository files (HTTP " + std::to_string(tree_resp.status_code) + ").")
            : tree_resp.error_message;
        return result;
    }

    std::vector<std::string> rel_files;
    try {
        auto tree_json = openidm::json::Parse(tree_resp.body);
        if (!tree_json.contains("tree") || !tree_json["tree"].is_array()) {
            result.error_code = "ERR_GITHUB_TREE";
            result.error_message = "GitHub tree response is missing the 'tree' array.";
            return result;
        }
        for (const auto& entry : tree_json["tree"].as_array()) {
            if (entry["type"].as_string("") != "blob") continue;
            std::string path = entry["path"].as_string("");
            constexpr std::string_view kExtPrefix = "extension/";
            if (path.rfind(kExtPrefix, 0) != 0) continue;
            std::string rel = path.substr(kExtPrefix.size());
            if (!IsSafeRelativeExtensionPath(rel)) {
                result.error_code = "ERR_UNSAFE_PATH";
                result.error_message = "Rejected unsafe file path in repository tree: " + rel;
                return result;
            }
            rel_files.push_back(std::move(rel));
        }
    } catch (...) {
        result.error_code = "ERR_GITHUB_TREE";
        result.error_message = "Failed to parse GitHub repository tree JSON.";
        return result;
    }

    // Ensure all required files are present in the remote tree before downloading.
    std::set<std::string> remote_set(rel_files.begin(), rel_files.end());
    for (const auto& req : RequiredFiles()) {
        if (remote_set.find(req) == remote_set.end()) {
            result.error_code = "ERR_MISSING_EXTENSION_FILES";
            result.error_message = "GitHub repository is missing required extension file: " + req;
            return result;
        }
    }

    // 4. Create isolated staging directory inside app_data_dir/tmp
    const auto now_ticks = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto stage_dir = app_data_dir / "tmp" / ("ext_stage_" + std::to_string(now_ticks));
    std::error_code ec;
    std::filesystem::remove_all(stage_dir, ec);
    std::filesystem::create_directories(stage_dir, ec);
    if (ec) {
        result.error_code = "ERR_STAGE_DIR";
        result.error_message = "Unable to create temporary staging directory: " + ec.message();
        return result;
    }

    const auto cleanup_stage = [&stage_dir]() {
        std::error_code clean_ec;
        std::filesystem::remove_all(stage_dir, clean_ec);
    };

    const auto canon_stage = std::filesystem::weakly_canonical(stage_dir, ec);
    if (ec) {
        cleanup_stage();
        result.error_code = "ERR_STAGE_DIR";
        result.error_message = "Unable to resolve staging directory path.";
        return result;
    }

    // 5. Download each file from raw.githubusercontent.com/<commit_sha>/extension/<rel>
    const size_t total_files = rel_files.size();
    for (size_t i = 0; i < total_files; ++i) {
        const std::string& rel = rel_files[i];
        const int pct = 25 + static_cast<int>(((i + 1) * 60) / std::max<size_t>(1, total_files));
        report(pct, "Downloading " + rel + " (" + std::to_string(i + 1) + "/" + std::to_string(total_files) + ")...");

        const std::string raw_url = std::string(kGithubRawPrefixUrl) + commit_sha + "/extension/" + rel;
        const auto file_resp = fetcher(raw_url, {{"User-Agent", std::string(radium::config::NetworkPoliciesConfig::kUserAgent)}});
        if (IsRateLimitedResponse(file_resp)) {
            cleanup_stage();
            result.rate_limited = true;
            result.error_code = "ERR_GITHUB_RATE_LIMIT";
            result.error_message = "GitHub rate limit exceeded while downloading " + rel + ".";
            return result;
        }
        if (file_resp.status_code != 200 || file_resp.body.empty()) {
            cleanup_stage();
            result.error_code = "ERR_DOWNLOAD_FILE";
            result.error_message = file_resp.error_message.empty()
                ? ("Failed to download " + rel + " (HTTP " + std::to_string(file_resp.status_code) + ").")
                : ("Failed to download " + rel + ": " + file_resp.error_message);
            return result;
        }

        const auto target_path = stage_dir / std::filesystem::path(rel);
        const auto canon_target = std::filesystem::weakly_canonical(target_path, ec);
        if (ec) {
            cleanup_stage();
            result.error_code = "ERR_UNSAFE_PATH";
            result.error_message = "Failed to canonicalize staged file path: " + rel;
            return result;
        }
        const auto rel_check = canon_target.lexically_relative(canon_stage);
        if (rel_check.empty() || rel_check.is_absolute() || *rel_check.begin() == "..") {
            cleanup_stage();
            result.error_code = "ERR_UNSAFE_PATH";
            result.error_message = "Path traversal detected for file: " + rel;
            return result;
        }

        if (!WriteFileBinary(target_path, file_resp.body)) {
            cleanup_stage();
            result.error_code = "ERR_WRITE_FILE";
            result.error_message = "Failed to write staged file: " + rel;
            return result;
        }
    }

    // 6. Validate staged extension directory before touching any existing installation
    report(90, "Validating extension manifest and resources...");
    std::string ext_version;
    std::string val_err;
    if (!ValidateExtensionDirectory(stage_dir, &ext_version, &val_err)) {
        cleanup_stage();
        result.error_code = "ERR_INVALID_EXTENSION";
        result.error_message = val_err;
        return result;
    }

    // 7. Atomic replacement of install_dir
    report(95, "Installing extension files...");
    std::filesystem::create_directories(install_dir.parent_path(), ec);
    if (ec) {
        cleanup_stage();
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
            cleanup_stage();
            result.error_code = "ERR_REPLACE_EXTENSION";
            result.error_message = "Could not move previous extension aside (close any file locks and retry): " + ec.message();
            return result;
        }
    }

    std::filesystem::rename(stage_dir, install_dir, ec);
    if (ec) {
        // Restore backup so we never leave the user without a working extension
        if (had_existing) {
            std::error_code restore_ec;
            std::filesystem::rename(backup_dir, install_dir, restore_ec);
        }
        cleanup_stage();
        result.error_code = "ERR_REPLACE_EXTENSION";
        result.error_message = "Failed to finalize extension installation: " + ec.message();
        return result;
    }

    if (had_existing) {
        std::filesystem::remove_all(backup_dir, ec);
    }

    report(100, "Extension downloaded and verified.");
    result.success = true;
    result.version = ext_version;
    result.installed_files = std::move(rel_files);
    OPENIDM_LOG_INFO("EXT", "Installed Radium browser extension v" + ext_version + " (" + commit_sha + ") to " + install_dir.string());
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
            auto j = openidm::json::Parse(raw);
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

    openidm::json::Value chromium_json = openidm::json::Object{};
    chromium_json["name"] = "com.radium.downloadmanager";
    chromium_json["description"] = "Radium Download Manager Native Messaging Host";
    chromium_json["path"] = host_exe.string();
    chromium_json["type"] = "stdio";
    chromium_json["allowed_origins"] = openidm::json::Array{
        std::string("chrome-extension://mdgmlcnpodoehidlplplfegngnjnilap/"),
        std::string("chrome-extension://chbojfoombcanhhpoclailollapkglma/")
    };

    openidm::json::Value firefox_json = openidm::json::Object{};
    firefox_json["name"] = "com.radium.downloadmanager";
    firefox_json["description"] = "Radium Download Manager Native Messaging Host";
    firefox_json["path"] = host_exe.string();
    firefox_json["type"] = "stdio";
    firefox_json["allowed_extensions"] = openidm::json::Array{
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
