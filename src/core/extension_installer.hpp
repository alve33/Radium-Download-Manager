// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Shared Browser Extension Downloader, Validator & Installer
#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace radium::core {

struct ExtensionFetchResponse {
    int status_code{0};
    std::map<std::string, std::string> headers;
    std::string body;
    std::string error_message;
};

using ExtensionFetchFn = std::function<ExtensionFetchResponse(
    const std::string& url,
    const std::map<std::string, std::string>& headers)>;

using ExtensionProgressFn = std::function<void(int percent, const std::string& status)>;

struct ExtensionInstallResult {
    bool success{false};
    bool rate_limited{false};
    bool already_up_to_date{false};
    std::string version;
    std::string commit_sha;
    std::string package_sha256;
    std::filesystem::path install_dir;
    std::vector<std::string> installed_files;
    std::string error_code;
    std::string error_message;
};

struct InstalledExtensionInfo {
    bool installed{false};
    std::string version;
    std::string commit_sha;
    std::filesystem::path install_dir;
    bool manifest_valid{false};
    bool all_required_files_present{false};
};

struct NativeMessagingStatus {
    bool host_executable_found{false};
    std::filesystem::path host_executable_path;
    bool registry_configured{false};
    bool manifest_valid{false};
    std::string summary;
};

class ExtensionInstaller {
public:
    static std::filesystem::path DefaultInstallDir(const std::filesystem::path& app_data_dir);

    // Validates a relative path inside the extension directory (blocks traversal, backslashes, reserved names).
    static bool IsSafeRelativeExtensionPath(std::string_view rel_path);

    // Validates manifest.json and required files inside a candidate extension directory.
    static bool ValidateExtensionDirectory(
        const std::filesystem::path& dir,
        std::string* out_version = nullptr,
        std::string* out_error = nullptr);

    // Inspects an already-installed extension directory on disk.
    static InstalledExtensionInfo InspectInstalledExtension(
        const std::filesystem::path& install_dir,
        std::string_view recorded_commit_sha = {});

    // Downloads the latest extension files from alve33/Radium-Download-Manager on GitHub,
    // stages and validates them, and atomically replaces install_dir without leaving partial files.
    static ExtensionInstallResult DownloadAndInstall(
        const std::filesystem::path& app_data_dir,
        const ExtensionProgressFn& on_progress = {},
        const ExtensionFetchFn& fetch_fn = {});

    // Verifies (and on Windows, if radium-nmh.exe exists and HKCU is missing/stale, repairs)
    // the per-user Native Messaging Host configuration for Chrome/Edge/Brave/Firefox.
    static NativeMessagingStatus VerifyOrRepairNativeMessaging(
        const std::filesystem::path& exe_dir,
        const std::filesystem::path& app_data_dir);

    static const std::vector<std::string>& RequiredFiles();
    static ExtensionFetchFn DefaultHttpFetch();
};

} // namespace radium::core
