// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Windows OS & Workflow Integrations
// Covers Mark-of-the-Web (Zone.Identifier), Sleep Inhibition, Run-at-Login,
// radium:// URL Scheme, .torrent Association, OLE IDropTarget, and Post-Completion Actions.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "common/types.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace openidm::platform {

class WindowsIntegrations {
public:
    // Mark-of-the-Web (Zone.Identifier ADS)
    static bool KeepsZoneInformation(std::optional<uint32_t> machine_policy, std::optional<uint32_t> user_policy);
    static bool WriteZoneIdentifier(
        const std::filesystem::path& file_path,
        const std::string& url,
        const std::string& referrer = "",
        bool user_option_enabled = true);

    // System Sleep Inhibition while downloads are active
    static void SetDownloadKeepAlive(bool active);

    // Run-at-Login (HKCU\Software\Microsoft\Windows\CurrentVersion\Run)
    static bool SetRunAtLogin(bool enable);
    static bool IsRunAtLoginEnabled();

    // radium:// URL scheme & .torrent file association (HKCU\Software\Classes)
    static bool RegisterUrlScheme(const std::string& scheme = "radium");
    static bool UnregisterUrlScheme(const std::string& scheme = "radium");
    static bool RegisterTorrentAssociation();

    // Parses a radium:// URI (e.g. "radium://add?url=https%3A%2F%2F..." or "radium://wake")
    struct LaunchUri {
        bool valid{false};
        bool wake_only{false};
        std::string url;
    };
    static LaunchUri ParseLaunchUri(const std::string& raw);

    // Post-completion per-task action (Open file, Open folder, Restart system)
    static void ExecutePostCompletionAction(
        PostCompletionAction action,
        const std::filesystem::path& target_path);

#ifdef _WIN32
    // COM OLE IDropTarget supporting UniformResourceLocatorW, CF_UNICODETEXT, and CF_HDROP
    static bool RegisterWindowDropTarget(HWND hwnd, std::function<void(const std::string&)> on_drop);
    static void RevokeWindowDropTarget(HWND hwnd);
#endif
};

} // namespace openidm::platform
