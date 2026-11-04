// SPDX-License-Identifier: Apache-2.0
// OpenIDM — Admin-Only Download Startup Log Dialog
// Displays millisecond-level lifecycle timeline, network milestones, and performance reports.
#pragma once

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include "core/download_controller.hpp"

namespace openidm::ui {

/**
 * Displays the Admin-Only Download Startup Log & Performance Report modal dialog.
 * If user is not an administrator, access is denied.
 */
void ShowStartupLogDialogWin32(HWND hwnd_parent, DownloadController& controller);

} // namespace openidm::ui
#endif
