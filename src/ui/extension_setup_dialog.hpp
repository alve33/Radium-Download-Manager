// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Browser Extension Setup & Folder Instructions Dialogs (Win32)
#pragma once

#include <filesystem>
#include <string>

#include "core/download_controller.hpp"
#include "core/extension_installer.hpp"
#include "ui/design_system.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace radium::ui {

#ifdef _WIN32
// Shows the shared Browser Extension Setup / Download dialog.
// When is_first_launch is true:
//   - Immediately persists that first-launch onboarding was displayed.
//   - Displays "Set up your browser extension" with "Download Extension" and "Later".
// When is_first_launch is false (triggered from Settings -> Browser Integration):
//   - Immediately begins downloading/updating the extension from GitHub with live progress.
// On successful download, transitions to ShowExtensionInstalledDialogWin32.
void ShowExtensionSetupDialogWin32(
    HWND hwnd_parent,
    DownloadController& controller,
    const ThemePalette& palette,
    bool is_first_launch);

// Shows the post-download Extension Folder & Browser Installation Instructions dialog
// with Open Folder, Copy Path, and instructions for supported browsers (Chromium & Firefox).
void ShowExtensionInstalledDialogWin32(
    HWND hwnd_parent,
    const std::filesystem::path& install_dir,
    const std::string& version,
    const radium::core::NativeMessagingStatus& nmh_status,
    const ThemePalette& palette);
#endif

} // namespace radium::ui
