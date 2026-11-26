// SPDX-License-Identifier: Apache-2.0
#include "ui/win32_main_window.hpp"
#include "ui/localization.hpp"
#include "ui/startup_log_dialog.hpp"
#include "ui/extension_setup_dialog.hpp"
#include "security/admin_check.hpp"
#include "common/startup_diagnostic.hpp"

#include "brisk_engine/http_client.hpp"
#include "common/central_config.hpp"
#include "common/crypto_utils.hpp"
#include "common/json_mini.hpp"
#include "common/logger.hpp"
#include "common/path_sanitizer.hpp"
#include "common/semver.hpp"
#include "common/version.hpp"
#include "core/extension_installer.hpp"
#include "core/update_manager.hpp"
#include "platform/windows_integrations.hpp"
#include "storage/import_export.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winreg.h>
#include <uiautomation.h>
#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "advapi32.lib")

#endif

namespace radium::ui {

namespace {

#ifdef _WIN32
UINT g_msgTaskbarButtonCreated = 0;
HICON CreateRadiumAppIcon();

bool ShowAddTorrentDialogWin32(
    HWND hwnd_parent,
    DesktopView& view,
    DownloadController& controller,
    torrent::TorrentInfo& info,
    const std::string& torrent_file_path = "",
    const std::string& initial_save_dir = "",
    bool start_immediately = true,
    std::string* selected_save_dir = nullptr,
    bool* selected_start_immediately = nullptr,
    std::string* selected_category = nullptr);

std::wstring Utf8ToW(std::string_view sv) {
    if (sv.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), len);
    return out;
}

std::string WToUtf8(std::wstring_view wv) {
    if (wv.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wv.data(), static_cast<int>(wv.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::string DestinationFolderForDisplay(std::string_view file_path) {
    const auto path = PathSanitizer::Utf8ToPath(file_path);
    const auto folder = path.parent_path();
    return folder.empty() ? std::string(file_path) : PathSanitizer::PathToUtf8(folder);
}

bool IsWindowsAppThemeLight() {
    HKEY key = nullptr;
    DWORD value = 0;
    DWORD size = sizeof(value);
    DWORD type = 0;
    const LONG opened = RegOpenKeyExW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_QUERY_VALUE, &key);
    if (opened != ERROR_SUCCESS) return false;
    const LONG queried = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &size);
    RegCloseKey(key);
    return queried == ERROR_SUCCESS && type == REG_DWORD && value != 0;
}

HMODULE GetUxThemeModule() {
    static HMODULE module = LoadLibraryW(L"uxtheme.dll");
    return module;
}

using SetPreferredAppModeFn = int(WINAPI*)(int mode);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND hwnd, BOOL allow);

SetPreferredAppModeFn GetSetPreferredAppModeFn() {
    static auto fn = reinterpret_cast<SetPreferredAppModeFn>(
        GetProcAddress(GetUxThemeModule(), MAKEINTRESOURCEA(135)));
    return fn;
}

AllowDarkModeForWindowFn GetAllowDarkModeForWindowFn() {
    static auto fn = reinterpret_cast<AllowDarkModeForWindowFn>(
        GetProcAddress(GetUxThemeModule(), MAKEINTRESOURCEA(133)));
    return fn;
}

void ApplyNativeEditTheme(HWND hwnd, bool use_dark) {
    (void)use_dark;
    using SetWindowThemeFn = HRESULT(WINAPI*)(HWND, LPCWSTR, LPCWSTR);
    static auto set_window_theme = reinterpret_cast<SetWindowThemeFn>(
        GetProcAddress(GetUxThemeModule(), "SetWindowTheme"));
    if (set_window_theme) {
        set_window_theme(hwnd, L"", L"");
    }
    InvalidateRect(hwnd, nullptr, TRUE);
    UpdateWindow(hwnd);
}

void SetWindowRightToLeft(HWND hwnd, bool /*right_to_left*/) {
    SetProcessDefaultLayout(0);
    if (!hwnd || !IsWindow(hwnd)) return;
    LONG_PTR ex_style = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    const LONG_PTR rtl_flags = static_cast<LONG_PTR>(WS_EX_LAYOUTRTL) |
                               static_cast<LONG_PTR>(WS_EX_RTLREADING) |
                               static_cast<LONG_PTR>(WS_EX_RIGHT);
    if (ex_style & rtl_flags) {
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex_style & ~rtl_flags);
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

BOOL CALLBACK ApplyChildRightToLeft(HWND child, LPARAM rtl_param) {
    SetWindowRightToLeft(child, rtl_param != FALSE);
    return TRUE;
}

void ApplyWin32DarkModePreference(HWND hwnd, bool use_dark) {
    if (auto set_app_mode = GetSetPreferredAppModeFn()) {
        // PreferredAppMode: ForceDark = 2, ForceLight = 3. Radium paints its
        // own surfaces, while this keeps native child controls in that mode.
        set_app_mode(use_dark ? 2 : 3);
    }
    if (auto allow_window_dark = GetAllowDarkModeForWindowFn()) {
        allow_window_dark(hwnd, use_dark ? TRUE : FALSE);
    }
}

BOOL CALLBACK ApplyChildDarkModePreference(HWND child, LPARAM use_dark_param) {
    const bool use_dark = use_dark_param != FALSE;
    if (auto allow_window_dark = GetAllowDarkModeForWindowFn()) {
        allow_window_dark(child, use_dark ? TRUE : FALSE);
    }
    wchar_t class_name[32]{};
    if (GetClassNameW(child, class_name, static_cast<int>(std::size(class_name))) > 0 &&
        _wcsicmp(class_name, L"Edit") == 0) {
        ApplyNativeEditTheme(child, use_dark);
    }
    return TRUE;
}

void ApplyNativeWindowTheme(HWND hwnd, const ThemePalette& palette) {
    BOOL use_dark = palette.mode != ThemeMode::kLight;
    const bool right_to_left = GetLocalizationManager().IsRightToLeft();
    SetWindowRightToLeft(hwnd, right_to_left);
    EnumChildWindows(hwnd, ApplyChildRightToLeft, right_to_left ? TRUE : FALSE);
    ApplyWin32DarkModePreference(hwnd, use_dark != FALSE);
    EnumChildWindows(hwnd, ApplyChildDarkModePreference, static_cast<LPARAM>(use_dark));
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &use_dark, sizeof(use_dark));
    COLORREF caption = palette.app_background.ToWin32ColorRef();
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    COLORREF text = palette.text_primary.ToWin32ColorRef();
    DwmSetWindowAttribute(hwnd, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof(text));
    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}
#else
bool IsWindowsAppThemeLight() { return false; }
#endif

std::string ToLower(std::string_view sv) {
    std::string s(sv);
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

inline std::string ToLowerStr(std::string_view sv) {
    return ToLower(sv);
}

std::string FormatTimestampUtc(int64_t epoch_ms) {
    if (epoch_ms <= 0) return "—";
    std::time_t tt = static_cast<std::time_t>(epoch_ms / 1000);
    std::tm tm_buf{};
#ifdef _WIN32
    localtime_s(&tm_buf, &tt);
#else
    localtime_r(&tt, &tm_buf);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm_buf, "%b %d, %I:%M %p");
    std::string out = oss.str();
    // Strip leading zero on hour ("Nov 14, 01:52 PM" -> "Nov 14, 1:52 PM")
    auto comma_pos = out.find(", 0");
    if (comma_pos != std::string::npos) {
        out.erase(comma_pos + 2, 1);
    }
    return out;
}

std::string StripLeadingOrphanCombiningMarks(std::string_view utf8) {
#ifdef _WIN32
    if (utf8.empty()) return std::string(utf8);
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wlen <= 0) return std::string(utf8);
    std::wstring w(static_cast<size_t>(wlen), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), w.data(), wlen);
    size_t pos = 0;
    while (pos < w.size()) {
        wchar_t ch = w[pos];
        if (ch == 0xFEFF || (ch >= 0x200B && ch <= 0x200F) ||
            (ch >= 0x202A && ch <= 0x202E) || (ch >= 0x2066 && ch <= 0x2069)) {
            ++pos;
            continue;
        }
        WORD c3 = 0;
        if (GetStringTypeW(CT_CTYPE3, &ch, 1, &c3) &&
            (c3 & (C3_NONSPACING | C3_DIACRITIC | C3_VOWELMARK)) != 0) {
            ++pos;
            continue;
        }
        break;
    }
    if (pos == 0) return std::string(utf8);
    std::wstring trimmed = w.substr(pos);
    if (trimmed.empty()) return std::string(utf8);
    int ulen = WideCharToMultiByte(CP_UTF8, 0, trimmed.data(), static_cast<int>(trimmed.size()), nullptr, 0, nullptr, nullptr);
    if (ulen <= 0) return std::string(utf8);
    std::string res(static_cast<size_t>(ulen), '\0');
    WideCharToMultiByte(CP_UTF8, 0, trimmed.data(), static_cast<int>(trimmed.size()), res.data(), ulen, nullptr, nullptr);
    return res;
#else
    return std::string(utf8);
#endif
}

std::string FormatConflictPolicyLabel(ConflictPolicy p) {
    switch (p) {
        case ConflictPolicy::kPrompt:      return "Ask When File Exists";
        case ConflictPolicy::kAutoRename:   return "Auto-Rename (file (1).ext)";
        case ConflictPolicy::kOverwrite:    return "Overwrite Existing File";
        case ConflictPolicy::kFailIfExists: return "Skip / Fail if File Exists";
    }
    return "Auto-Rename (file (1).ext)";
}

} // namespace

DesktopView::DesktopView(DownloadController& controller, ipc::IpcServer* ipc_server)
    : m_controller(controller),
      m_ipc_server(ipc_server),
      m_metrics(DesignSystem::ComputeMetrics(96)),
      m_palette(DesignSystem::GetRedesignPalette()) {
    auto settings = m_controller.GetSettings();
    GetLocalizationManager().SetLocale(settings.language);
    m_draft_settings = settings;
    m_anim.reduced_motion = settings.reduced_motion;
    m_anim.window_visible = !settings.start_minimized;
    m_anim.window_minimized = settings.start_minimized;
    SetTheme(settings.theme, IsWindowsAppThemeLight());
    LoadColumnWidthsFromSettings();
}

void DesktopView::SetWindowDimensions(int width, int height, uint32_t dpi) {
    ClearHoverState();
    m_metrics = DesignSystem::ComputeMetrics(dpi);
    m_window_width = std::max(width, 100);
    m_window_height = std::max(height, 100);
}

void DesktopView::SetWindowVisibility(bool visible, bool minimized) {
    m_anim.window_visible = visible;
    m_anim.window_minimized = minimized;
}

void DesktopView::TriggerActionPulse(const std::string& task_id, const std::string& action) {
    if (!m_anim.IsActive()) return;
    m_anim.icon_pulses[task_id] = {GetTickCount64(), action};
}

TaskbarProgressInfo DesktopView::ComputeTaskbarProgress() const {
    TaskbarProgressInfo info;
    auto tasks = m_controller.ListTasks();
    if (tasks.empty()) {
        info.state = TaskbarProgressState::kNoProgress;
        return info;
    }

    size_t active_count = 0;
    size_t paused_count = 0;
    size_t failed_count = 0;
    uint64_t active_completed = 0;
    uint64_t active_total = 0;
    size_t active_with_total = 0;

    uint64_t paused_completed = 0;
    uint64_t paused_total = 0;

    for (const auto& t : tasks) {
        if (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || 
            t.state == TaskState::kMuxing || t.state == TaskState::kVerifying || 
            t.state == TaskState::kFinalizing) {
            active_count++;
            if (t.total_bytes > 0) {
                active_completed += t.downloaded_bytes;
                active_total += t.total_bytes;
                active_with_total++;
            }
        } else if (t.state == TaskState::kPaused) {
            paused_count++;
            if (t.total_bytes > 0) {
                paused_completed += t.downloaded_bytes;
                paused_total += t.total_bytes;
            }
        } else if (t.state == TaskState::kFailed) {
            failed_count++;
        }
    }

    if (active_count > 0) {
        if (active_with_total == 0 || active_total == 0) {
            info.state = TaskbarProgressState::kIndeterminate;
        } else {
            info.state = TaskbarProgressState::kNormal;
            info.completed_bytes = active_completed;
            info.total_bytes = active_total;
            info.progress_fraction = static_cast<double>(active_completed) / static_cast<double>(active_total);
        }
    } else {
        // When there are no active downloads (kDownloading, kProbing, kMuxing, kVerifying, kFinalizing),
        // clear taskbar progress completely using TBPF_NOPROGRESS.
        // Historical completed, failed, canceled, and paused downloads in the task list must NEVER keep
        // a stale progress indicator on the Windows taskbar.
        info.state = TaskbarProgressState::kNoProgress;
    }

    return info;
}

std::vector<std::string> DesktopView::GetVisibleColumns() const {
    if (IsCompactColumnsMode()) {
        return {"Filename", "Status", "Progress", "Size", "Speed", "Actions"};
    }
    return {"Filename", "Type", "Status", "Progress", "Size", "Speed", "ETA", "Actions"};
}

WindowLayoutGeometry DesktopView::ComputeLayoutGeometry(size_t scroll_offset) const {
    WindowLayoutGeometry g;
    const auto& m = m_metrics;
    int w = m_window_width;
    int h = m_window_height;

    const bool compact_sidebar = w < DesignSystem::ScaleForDpi(1050, m.dpi);
    const bool compact_toolbar = w < DesignSystem::ScaleForDpi(1050, m.dpi);
    const bool hide_added = w < DesignSystem::ScaleForDpi(1080, m.dpi);
    int sidebar_w = compact_sidebar
        ? DesignSystem::ScaleForDpi(152, m.dpi)
        : m.sidebar_width;
    int content_x = sidebar_w;
    int bar_h = m.toolbar_height;
    g.toolbar_rect = {content_x, 0, w, bar_h};
    g.sidebar_rect = {0, 0, sidebar_w, h};

    int add_btn_w = DesignSystem::ScaleForDpi(compact_toolbar ? 134 : 136, m.dpi);
    int add_btn_x = content_x + DesignSystem::ScaleForDpi(compact_toolbar ? 10 : 16, m.dpi);
    g.add_download_button_rect = {add_btn_x, (bar_h - m.button_height) / 2, add_btn_x + add_btn_w,
        (bar_h + m.button_height) / 2};

    int sort_w = DesignSystem::ScaleForDpi(compact_toolbar ? 46 : 76, m.dpi);
    int sort_h = m.button_height;
    int sort_y = (bar_h - sort_h) / 2;
    int sort_x = g.add_download_button_rect.right + DesignSystem::ScaleForDpi(compact_toolbar ? 6 : 12, m.dpi);
    g.sort_dropdown_rect = {sort_x, sort_y, sort_x + sort_w, sort_y + sort_h};

    int filt_w = DesignSystem::ScaleForDpi(compact_toolbar ? 70 : 122, m.dpi);
    int filt_x = g.sort_dropdown_rect.right + DesignSystem::ScaleForDpi(compact_toolbar ? 5 : 8, m.dpi);
    g.filter_dropdown_rect = {filt_x, sort_y, filt_x + filt_w, sort_y + sort_h};

    int btn_h = m.button_height;
    int by = (bar_h - btn_h) / 2;

    const int kSegBtnW = compact_toolbar ? 28 : 32;
    const int kStartAllButtonWidth = compact_toolbar ? 54 : 94;
    const int kPauseAllButtonWidth = compact_toolbar ? 54 : 98;
    const int kClearHistoryButtonWidth = compact_toolbar ? 62 : 114;
    const int kToolbarActionGap = compact_toolbar ? 5 : 8;
    const int kSegmentedClusterWidth = kSegBtnW * 3 + (compact_toolbar ? 8 : 16);

    int theme_button_w = DesignSystem::ScaleForDpi(34, m.dpi);
    g.theme_button_rect = {w - theme_button_w - DesignSystem::ScaleForDpi(compact_toolbar ? 10 : 16, m.dpi), by,
        w - DesignSystem::ScaleForDpi(compact_toolbar ? 10 : 16, m.dpi), by + btn_h};

    int tools_button_w = DesignSystem::ScaleForDpi(compact_toolbar ? 34 : 68, m.dpi);
    int tools_gap = DesignSystem::ScaleForDpi(compact_toolbar ? 6 : 8, m.dpi);
    g.tools_button_rect = {g.theme_button_rect.left - tools_gap - tools_button_w, by,
        g.theme_button_rect.left - tools_gap, by + btn_h};

    int settings_btn_x = g.tools_button_rect.left;
    g.settings_button_rect = {0, 0, 0, 0};
    g.search_sliders_rect = {0, 0, 0, 0};

    int search_max_w = DesignSystem::ScaleForDpi(220, m.dpi);
    int search_min_w = DesignSystem::ScaleForDpi(compact_toolbar ? 90 : 120, m.dpi);
    int essential_actions_left = g.filter_dropdown_rect.right + DesignSystem::ScaleForDpi(compact_toolbar ? 6 : 12, m.dpi);
    int essential_actions_w = DesignSystem::ScaleForDpi(
        kSegmentedClusterWidth +
        kStartAllButtonWidth + kToolbarActionGap +
        kPauseAllButtonWidth + kToolbarActionGap + kClearHistoryButtonWidth + 12, m.dpi);
    int avail_for_search = settings_btn_x - DesignSystem::ScaleForDpi(12, m.dpi) - essential_actions_left - essential_actions_w;
    int search_w = std::clamp(avail_for_search, search_min_w, search_max_w);
    int search_x = settings_btn_x - search_w - DesignSystem::ScaleForDpi(compact_toolbar ? 6 : 10, m.dpi);
    g.search_box_rect = {search_x, by, search_x + search_w, by + btn_h};

    auto tb = ComputeToolbarState();
    struct BtnDef { ToolbarActionId id; const char* label; bool enabled; bool primary; int base_w; };
    const BtnDef defs[] = {
        {ToolbarActionId::kAddUrl,       "+ Add Download", tb.can_add_url,       true,  add_btn_w},
        {ToolbarActionId::kStart,        "Start",          true,                 false, kSegBtnW},
        {ToolbarActionId::kPause,        "Pause",          true,                 false, kSegBtnW},
        {ToolbarActionId::kRemove,       "Remove",         true,                 false, kSegBtnW},
        {ToolbarActionId::kStartAll,     "Start All",      tb.can_start_all,     false, kStartAllButtonWidth},
        {ToolbarActionId::kPauseAll,     "Pause All",      tb.can_pause_all,     false, kPauseAllButtonWidth},
        {ToolbarActionId::kClearHistory, "Clear History",  tb.can_clear_history, false, kClearHistoryButtonWidth},
        {ToolbarActionId::kResume,       "Resume",         tb.can_resume,        false, 0},
        {ToolbarActionId::kRetry,        "Retry",          tb.can_retry,         false, 0},
        {ToolbarActionId::kSettings,     "Settings",       tb.can_open_settings, false, 32}
    };
    int act_x = essential_actions_left;
    int max_act_x = search_x - DesignSystem::ScaleForDpi(compact_toolbar ? 6 : 12, m.dpi);
    for (size_t i = 0; i < std::size(defs); ++i) {
        const auto& d = defs[i];
        RectI brc{0, 0, 0, 0};
        if (i == 0) {
            brc = g.add_download_button_rect;
        } else if (i + 1 == std::size(defs) || d.base_w <= 0) {
            brc = {0, 0, 0, 0};
        } else if (d.enabled) {
            int bw = DesignSystem::ScaleForDpi(d.base_w, m.dpi);
            if (act_x + bw <= max_act_x) {
                brc = {act_x, by, act_x + bw, by + btn_h};
                if (d.id == ToolbarActionId::kStart || d.id == ToolbarActionId::kPause) {
                    act_x += bw; // No gap inside segmented control
                } else if (d.id == ToolbarActionId::kRemove) {
                    act_x += bw + DesignSystem::ScaleForDpi(compact_toolbar ? 8 : 16, m.dpi); // Gap after segmented group
                } else {
                    act_x += bw + DesignSystem::ScaleForDpi(kToolbarActionGap, m.dpi);
                }
            }
        }
        g.toolbar_buttons.push_back({d.id, GetLocalizationManager().Translate(d.label), d.enabled, d.primary, brc});
    }

    auto counts = ComputeCategoryCounts();
    const std::pair<SidebarCategory, size_t> cats[] = {
        {SidebarCategory::kAllDownloads, counts.all_downloads},
        {SidebarCategory::kInProgress, counts.in_progress},
        {SidebarCategory::kCompleted, counts.completed},
        {SidebarCategory::kPaused, counts.paused}
    };
    int nav_top = DesignSystem::ScaleForDpi(44, m.dpi);
    int nav_h = DesignSystem::ScaleForDpi(compact_sidebar ? 32 : 34, m.dpi);
    int nav_gap = DesignSystem::ScaleForDpi(4, m.dpi);
    int nav_margin = DesignSystem::ScaleForDpi(compact_sidebar ? 6 : 10, m.dpi);
    for (size_t i = 0; i < 4; ++i) {
        const auto& [cat, cnt] = cats[i];
        int item_y = nav_top + static_cast<int>(i) * (nav_h + nav_gap);
        RectI crc{nav_margin, item_y, sidebar_w - nav_margin, item_y + nav_h};
        std::string item_lbl = (compact_sidebar && cat == SidebarCategory::kAllDownloads)
            ? GetLocalizationManager().Translate("All Items")
            : CategoryToLabel(cat);
        g.sidebar_items.push_back({cat, item_lbl, cnt, cat == m_selected_category, crc});
    }
    const int sidebar_action_gap = DesignSystem::ScaleForDpi(4, m.dpi);
    const int sidebar_action_height = DesignSystem::ScaleForDpi(compact_sidebar ? 32 : 34, m.dpi);
    const int footer_bottom = h - DesignSystem::ScaleForDpi(12, m.dpi);
    g.sidebar_settings_rect = {nav_margin, footer_bottom - sidebar_action_height, sidebar_w - nav_margin, footer_bottom};
    g.sidebar_language_rect = {
        nav_margin,
        g.sidebar_settings_rect.top - sidebar_action_gap - sidebar_action_height,
        sidebar_w - nav_margin,
        g.sidebar_settings_rect.top - sidebar_action_gap
    };

    int hdr_h = m.table_header_height;
    int det_h = m_selected_task_id.empty() ? 0 : std::min(m.details_panel_height, std::max(DesignSystem::ScaleForDpi(104, m.dpi), h / 4));
    const int panel_inset = DesignSystem::ScaleForDpi(16, m.dpi);
    const int table_top = bar_h;
    const int table_bottom = h - det_h;
    const int table_left = content_x + panel_inset;
    const int table_right = w - panel_inset;
    g.table_header_rect = {content_x, bar_h, w, bar_h + hdr_h};
    g.table_body_rect = {content_x, bar_h + hdr_h, w, table_bottom};
    const int master_chk_size = DesignSystem::ScaleForDpi(16, m.dpi);
    g.select_all_checkbox_rect = {table_left + DesignSystem::ScaleForDpi(10, m.dpi),
        table_top + (hdr_h - master_chk_size) / 2,
        table_left + DesignSystem::ScaleForDpi(10, m.dpi) + master_chk_size,
        table_top + (hdr_h + master_chk_size) / 2};

    g.details_panel_rect = {content_x, h - det_h, w, h};
    g.status_bar_rect = {0, h, w, h};
    g.status_toggle_off_rect = {0, 0, 0, 0};

    int col_chk_w = DesignSystem::ScaleForDpi(36, m.dpi);
    int def_name_w = DesignSystem::ScaleForDpi(320, m.dpi);
    int def_size_w = DesignSystem::ScaleForDpi(compact_toolbar ? 96 : 150, m.dpi);
    int def_stat_w = DesignSystem::ScaleForDpi(compact_toolbar ? 122 : 150, m.dpi);
    int def_dl_w = DesignSystem::ScaleForDpi(compact_toolbar ? 72 : 90, m.dpi);
    int def_ul_w = DesignSystem::ScaleForDpi(compact_toolbar ? 64 : 80, m.dpi);
    int def_added_w = DesignSystem::ScaleForDpi(145, m.dpi);

    if (m_column_widths.size() < 6) {
        m_column_widths = {def_name_w, def_size_w, def_stat_w, def_dl_w, def_ul_w, def_added_w};
    } else {
        if (m_column_widths[0] < 120 || m_column_widths[0] > 2000) m_column_widths[0] = def_name_w;
        if (m_column_widths[1] < 140 || m_column_widths[1] > 2000) m_column_widths[1] = def_size_w;
        if (m_column_widths[2] < 140 || m_column_widths[2] > 2000) m_column_widths[2] = def_stat_w;
        if (m_column_widths[3] < 40 || m_column_widths[3] > 2000) m_column_widths[3] = def_dl_w;
        if (m_column_widths[4] < 40 || m_column_widths[4] > 2000) m_column_widths[4] = def_ul_w;
        if (m_column_widths[5] < 135 || m_column_widths[5] > 2000) m_column_widths[5] = def_added_w;
    }

    int col_size_w = (m_column_widths.size() > 1 && m_column_widths[1] >= def_size_w)
        ? m_column_widths[1]
        : def_size_w;
    int col_stat_w = (m_column_widths.size() > 2 && m_column_widths[2] >= def_stat_w)
        ? m_column_widths[2]
        : def_stat_w;
    int col_dl_w   = (m_column_widths.size() > 3 && m_column_widths[3] > 40 && m_column_widths[3] != def_dl_w)
        ? m_column_widths[3]
        : def_dl_w;
    int col_ul_w   = (m_column_widths.size() > 4 && m_column_widths[4] > 40 && m_column_widths[4] != def_ul_w)
        ? m_column_widths[4]
        : def_ul_w;
    int col_added_w= (m_column_widths.size() > 5 && m_column_widths[5] >= def_added_w)
        ? m_column_widths[5]
        : def_added_w;

    int total_avail = std::max(1, table_right - table_left);

    // Responsive column display (Design Vision Part 2.1):
    // Hide Added first (< 1080px), keep Download and Upload speeds visible in compact windows.
    if (hide_added) {
        col_added_w = 0;
    }

    int min_name_w = DesignSystem::ScaleForDpi(260, m.dpi);
    int fixed_non_name_w = col_size_w + col_stat_w + col_dl_w + col_ul_w + col_added_w;

    // If metadata columns exceed the budget needed to keep Name at least min_name_w,
    // proportionally shrink columns down to their safe minimums while keeping Download visible
    int overflow = fixed_non_name_w + min_name_w - total_avail;
    if (overflow > 0) {
        auto shrink = [&overflow](int& width, int min_w) {
            int reduce = std::min(overflow, std::max(0, width - min_w));
            width -= reduce;
            overflow -= reduce;
        };
        shrink(col_added_w, 0);
        shrink(col_ul_w, 0);
        shrink(col_dl_w, DesignSystem::ScaleForDpi(86, m.dpi));
        shrink(col_size_w, DesignSystem::ScaleForDpi(120, m.dpi));
        shrink(col_stat_w, DesignSystem::ScaleForDpi(135, m.dpi));
        fixed_non_name_w = col_size_w + col_stat_w + col_dl_w + col_ul_w + col_added_w;
    }

    int col_name_total_w = std::max(min_name_w, total_avail - fixed_non_name_w);
    if (m_resizing_col_index == 0 && m_column_widths.size() > 0 && m_column_widths[0] >= min_name_w) {
        col_name_total_w = m_column_widths[0];
    } else if (m_column_widths.size() > 0 && m_column_widths[0] != def_name_w && m_column_widths[0] >= min_name_w) {
        col_name_total_w = m_column_widths[0];
    }

    int cx = table_left;
    g.table_columns.push_back({SortColumn::kFileName, GetLocalizationManager().Translate("Name"), {cx, table_top, cx + col_name_total_w, table_top + hdr_h}});
    cx += col_name_total_w;

    g.table_columns.push_back({SortColumn::kSize, GetLocalizationManager().Translate("Size"), {cx, table_top, cx + col_size_w, table_top + hdr_h}});
    cx += col_size_w;

    g.table_columns.push_back({SortColumn::kStatus, GetLocalizationManager().Translate("Status"), {cx, table_top, cx + col_stat_w, table_top + hdr_h}});
    cx += col_stat_w;

    int dl_right = (col_ul_w == 0 && col_added_w == 0) ? table_right : (cx + col_dl_w);
    g.table_columns.push_back({SortColumn::kSpeed, GetLocalizationManager().Translate("Download"), {cx, table_top, dl_right, table_top + hdr_h}});
    cx = dl_right;

    int ul_right = (col_added_w == 0) ? table_right : (cx + col_ul_w);
    g.table_columns.push_back({SortColumn::kDownloadType, GetLocalizationManager().Translate("Upload"), {cx, table_top, ul_right, table_top + hdr_h}});
    cx = ul_right;

    int added_right = (col_added_w == 0) ? cx : table_right;
    g.table_columns.push_back({SortColumn::kCreatedAt, GetLocalizationManager().Translate("Added"), {cx, table_top, added_right, table_top + hdr_h}});
    cx = added_right;

    g.table_columns.push_back({SortColumn::kProgress, GetLocalizationManager().Translate("Progress"), {cx, table_top, cx, table_top + hdr_h}});
    g.table_columns.push_back({SortColumn::kEta, GetLocalizationManager().Translate("ETA"), {cx, table_top, cx, table_top + hdr_h}});

    const int body_h = std::max(1, g.table_body_rect.height());
    int row_h = m.table_row_height;
    const int base_row_count = body_h / row_h;
    const int next_row_height = body_h / (base_row_count + 1);
    // Keep rows spacious while using a comfortable 48px minimum to reveal one more item.
    if (base_row_count > 0 && next_row_height >= DesignSystem::ScaleForDpi(48, m.dpi)) {
        row_h = next_row_height;
    }
    g.task_row_height = row_h;
    size_t max_rows = static_cast<size_t>(std::max(1, body_h / row_h));
    auto visible_rows = BuildVisibleTaskRows(scroll_offset, max_rows);
    int ry = g.table_body_rect.top;
    int dl_col_left = (g.table_columns[3].rect.width() > 0) ? g.table_columns[3].rect.left : g.table_columns[2].rect.right;

    for (const auto& r : visible_rows) {
        if (ry + row_h > g.table_body_rect.bottom) break;
        RectI row_rc{table_left, ry, table_right, ry + row_h};
        RectI fn_rc{table_left + col_chk_w, ry, table_left + 12 + col_name_total_w, ry + row_h};
        RectI prog_rc{table_left + col_chk_w + 4, ry + row_h - 14, table_left + col_chk_w + std::min(160, std::max(50, col_name_total_w - col_chk_w - 70)), ry + row_h - 11};
        RectI act_rc{dl_col_left, ry + (row_h - 22) / 2, dl_col_left + 22, ry + (row_h + 22) / 2};
        int chk_sz = DesignSystem::ScaleForDpi(16, m.dpi);
        RectI chk_rc{table_left + DesignSystem::ScaleForDpi(10, m.dpi), ry + (row_h - chk_sz) / 2, table_left + DesignSystem::ScaleForDpi(10, m.dpi) + chk_sz, ry + (row_h + chk_sz) / 2};
        g.task_rows.push_back({r.task_id, row_rc, fn_rc, prog_rc, act_rc, chk_rc, act_rc});
        ry += row_h;
    }

    if (det_h > 0) {
        const auto state = ComputeToolbarState();
        std::vector<std::pair<DetailsActionId, std::pair<std::string, bool>>> actions;
        if (state.can_open_folder) actions.push_back({DetailsActionId::kOpenFolder, {GetLocalizationManager().Translate("Open Folder"), false}});
        if (state.can_open_file) actions.push_back({DetailsActionId::kOpenFile, {GetLocalizationManager().Translate("Open File"), false}});
        if (state.can_pause) actions.push_back({DetailsActionId::kPause, {GetLocalizationManager().Translate("Pause"), true}});
        if (state.can_resume) actions.push_back({DetailsActionId::kResume, {GetLocalizationManager().Translate("Resume"), true}});
        if (state.can_cancel) actions.push_back({DetailsActionId::kCancel, {GetLocalizationManager().Translate("Cancel"), false}});
        if (!state.can_pause && !state.can_resume && !state.can_cancel && (state.can_open_file || state.can_open_folder))
            actions.push_back({DetailsActionId::kRemoveHistory, {GetLocalizationManager().Translate("Remove from History"), false}});
        int action_h = DesignSystem::ScaleForDpi(32, m.dpi);
        int gap = DesignSystem::ScaleForDpi(8, m.dpi);
        int x = w - DesignSystem::ScaleForDpi(16, m.dpi);
        int y = g.details_panel_rect.top + (det_h - action_h) / 2;
        const bool compact_inspector = w < DesignSystem::ScaleForDpi(1050, m.dpi);
        for (auto it = actions.rbegin(); it != actions.rend(); ++it) {
            int btn_dp = 112;
            if (it->first == DetailsActionId::kRemoveHistory) {
                btn_dp = 32;
            } else if (compact_inspector) {
                if (it->first == DetailsActionId::kOpenFolder || it->first == DetailsActionId::kCancel) {
                    btn_dp = 32;
                } else if (it->first == DetailsActionId::kOpenFile) {
                    btn_dp = 86;
                } else {
                    btn_dp = 76;
                }
            }
            const int button_w = DesignSystem::ScaleForDpi(btn_dp, m.dpi);
            x -= button_w;
            g.details_action_buttons.push_back({it->first, it->second.first, it->second.second,
                {x, y, x + button_w, y + action_h}});
            x -= gap;
        }
        std::reverse(g.details_action_buttons.begin(), g.details_action_buttons.end());
    }

    return g;
}

HitTestResult DesktopView::HitTestPoint(int x, int y, size_t scroll_offset) const {
    if (m_settings_center_open) {
        auto sv = BuildSettingsCenterView();
        if (sv.apply_button_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsApplyButton, 0, "", "Apply and save modified settings"};
        }
        if (sv.cancel_button_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsCancelButton, 0, "", "Discard unsaved changes and close Settings (Esc)"};
        }
        if (sv.restore_defaults_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsRestoreDefaultsButton, 0, "", "Restore all settings to factory defaults"};
        }
        for (size_t i = 0; i < sv.categories.size(); ++i) {
            if (sv.categories[i].rect.Contains(x, y)) {
                return {HitTargetType::kSettingsCategoryTab, static_cast<int>(i), "", sv.categories[i].label};
            }
        }
        if (sv.content_rect.Contains(x, y)) {
            for (size_t i = 0; i < sv.visible_items.size(); ++i) {
                if (sv.visible_items[i].control_rect.Contains(x, y) || sv.visible_items[i].row_rect.Contains(x, y)) {
                    return {HitTargetType::kSettingsItemControl, static_cast<int>(i), sv.visible_items[i].key, sv.visible_items[i].description};
                }
            }
        }
        if (sv.modal_rect.Contains(x, y)) {
            return {HitTargetType::kSettingsModalBackground, 0, "", ""};
        }
        return {HitTargetType::kSettingsBackdrop, 0, "", "Click outside to close settings"};
    }

    auto geom = ComputeLayoutGeometry(scroll_offset);
    if (geom.theme_button_rect.Contains(x, y)) return {HitTargetType::kThemeToggleButton, 0, "", "Toggle light and dark appearance"};
    if (geom.tools_button_rect.Contains(x, y)) return {HitTargetType::kToolsMenuButton, 0, "", "Tools & Global Actions"};
    if (geom.sidebar_language_rect.Contains(x, y)) {
        return {HitTargetType::kSidebarLanguageButton, 0, "", "Language"};
    }
    if (geom.sidebar_settings_rect.Contains(x, y)) {
        return {HitTargetType::kSidebarSettingsButton, 0, "", "Settings"};
    }
    for (size_t i = 0; i < geom.details_action_buttons.size(); ++i) {
        if (geom.details_action_buttons[i].rect.Contains(x, y))
            return {HitTargetType::kDetailsActionButton, static_cast<int>(i), "", geom.details_action_buttons[i].label};
    }
    if (geom.search_sliders_rect.Contains(x, y)) {
        return {HitTargetType::kSearchSlidersButton, 0, "", "Settings Center (Ctrl+,)"};
    }
    if (geom.search_box_rect.Contains(x, y)) {
        return {HitTargetType::kSearchBox, 0, "", "Search downloads (Ctrl+K)"};
    }
    if (geom.add_download_button_rect.Contains(x, y)) {
        return {HitTargetType::kToolbarButton, 0, "", "+ Add Download (Ctrl+N)"};
    }
    if (geom.select_all_checkbox_rect.Contains(x, y)) {
        return {HitTargetType::kSelectAllCheckbox, 0, "", "Select all / Deselect all"};
    }
    if (geom.sort_dropdown_rect.Contains(x, y)) {
        return {HitTargetType::kSortDropdown, 0, "", "Sort downloads"};
    }
    for (size_t i = 0; i < geom.sidebar_items.size(); ++i) {
        if (geom.sidebar_items[i].rect.width() > 0 && geom.sidebar_items[i].rect.height() > 0 && geom.sidebar_items[i].rect.Contains(x, y)) {
            return {HitTargetType::kSidebarCategory, static_cast<int>(i), "", geom.sidebar_items[i].label};
        }
    }
    if (geom.filter_dropdown_rect.Contains(x, y)) {
        return {HitTargetType::kFilterDropdown, 0, "", "Filter by category"};
    }
    if (geom.status_toggle_off_rect.Contains(x, y)) {
        return {HitTargetType::kStatusToggleOff, 0, "", "Speed limiter toggle"};
    }
    for (size_t i = 0; i < geom.toolbar_buttons.size(); ++i) {
        if (geom.toolbar_buttons[i].rect.Contains(x, y)) {
            return {HitTargetType::kToolbarButton, static_cast<int>(i), "", geom.toolbar_buttons[i].label};
        }
    }
    if (geom.toolbar_rect.Contains(x, y)) {
        return {HitTargetType::kNone, 0, "", ""};
    }
    if (geom.table_header_rect.Contains(x, y)) {
        for (size_t i = 0; i < geom.table_columns.size(); ++i) {
            if (geom.table_columns[i].rect.width() > 0 && geom.table_columns[i].rect.Contains(x, y)) {
                return {HitTargetType::kTableHeaderColumn, static_cast<int>(i), "", geom.table_columns[i].title};
            }
        }
        return {HitTargetType::kNone, 0, "", ""};
    }
    if (geom.status_bar_rect.Contains(x, y)) {
        return {HitTargetType::kNone, 0, "", ""};
    }
    if (geom.details_panel_rect.height() > 0 && geom.details_panel_rect.Contains(x, y)) {
        return {HitTargetType::kNone, 0, "", ""};
    }
    std::vector<FormattedTaskRow> rows;
    bool rows_built = false;
    for (size_t i = 0; i < geom.task_rows.size(); ++i) {
        if (geom.task_rows[i].checkbox_rect.Contains(x, y)) {
            return {HitTargetType::kRowCheckbox, static_cast<int>(i), geom.task_rows[i].task_id, "Select download"};
        }
        if (geom.task_rows[i].action_button_rect.Contains(x, y) || geom.task_rows[i].action_icon_rect.Contains(x, y)) {
            if (!rows_built) {
                rows = BuildVisibleTaskRows(scroll_offset, geom.task_rows.size());
                rows_built = true;
            }
            std::string act_lbl = (i < rows.size()) ? rows[i].primary_action_label : "Action";
 return {HitTargetType::kTaskRowActionButton, static_cast<int>(i), geom.task_rows[i].task_id, act_lbl};
 }
 if (geom.task_rows[i].row_rect.Contains(x, y)) {
 if (!rows_built) {
 rows = BuildVisibleTaskRows(scroll_offset, geom.task_rows.size());
 rows_built = true;
 }
 std::string tip;
 if (i < rows.size()) {
 if (geom.task_rows[i].filename_rect.Contains(x, y)) {
 tip = rows[i].full_filename_tooltip;
 } else if (!rows[i].full_status_tooltip.empty()) {
 tip = rows[i].full_status_tooltip;
 } else {
 tip = rows[i].full_filename_tooltip;
 }
 }
 return {HitTargetType::kTaskRow, static_cast<int>(i), geom.task_rows[i].task_id, tip};
 }
 }
 return {};
}

bool DesktopView::HandleMouseClick(int x, int y, bool double_click, size_t scroll_offset) {
    auto hit = HitTestPoint(x, y, scroll_offset);

    if (m_settings_center_open) {
        auto sv = BuildSettingsCenterView();
        switch (hit.type) {
            case HitTargetType::kSettingsCategoryTab: {
                if (hit.index >= 0 && static_cast<size_t>(hit.index) < sv.categories.size()) {
                    SelectSettingsCategory(sv.categories[static_cast<size_t>(hit.index)].category);
                    return true;
                }
                return false;
            }
            case HitTargetType::kSettingsItemControl: {
                if (!hit.task_id.empty()) {
                    CycleOrToggleDraftSetting(hit.task_id);
                    return true;
                }
                return false;
            }
            case HitTargetType::kSettingsApplyButton: {
                auto r = ApplySettingsChanges();
                if (r.ok()) {
                    CloseSettingsCenter(false);
#ifdef _WIN32
                    if (m_hwnd) ApplyNativeWindowTheme(m_hwnd, m_palette);
#endif
                }
                return r.ok();
            }
            case HitTargetType::kSettingsCancelButton: {
                if (HasUnsavedSettingsChanges()) {
#ifdef _WIN32
                    if (m_hwnd) {
                        int res = MessageBoxW(m_hwnd, L"You have unsaved changes. Discard changes and close settings?", L"Discard Changes", MB_YESNO | MB_ICONQUESTION);
                        if (res != IDYES) return true;
                    }
#endif
                }
                CloseSettingsCenter(true);
                return true;
            }
            case HitTargetType::kSettingsRestoreDefaultsButton: {
                auto restored = ActionRestoreDefaultSettings();
#ifdef _WIN32
                if (restored.ok() && m_hwnd) ApplyNativeWindowTheme(m_hwnd, m_palette);
#endif
                return true;
            }
            case HitTargetType::kSettingsBackdrop: {
                if (HasUnsavedSettingsChanges()) {
#ifdef _WIN32
                    if (m_hwnd) {
                        int res = MessageBoxW(m_hwnd, L"You have unsaved changes. Discard changes and close settings?", L"Discard Changes", MB_YESNO | MB_ICONQUESTION);
                        if (res != IDYES) return true;
                    }
#endif
                }
                CloseSettingsCenter(true);
                return true;
            }
            case HitTargetType::kSettingsModalBackground: {
                return true;
            }
            default:
                return true;
        }
    }

    auto geom = ComputeLayoutGeometry(scroll_offset);
    switch (hit.type) {
        case HitTargetType::kSidebarSettingsButton:
            OpenSettingsCenter(SettingsCategory::kGeneral);
            return true;
        case HitTargetType::kDetailsActionButton: {
            if (hit.index < 0 || static_cast<size_t>(hit.index) >= geom.details_action_buttons.size()) return false;
            switch (geom.details_action_buttons[static_cast<size_t>(hit.index)].action_id) {
                case DetailsActionId::kOpenFolder: return ActionOpenDestinationFolder(false).ok();
                case DetailsActionId::kOpenFile: return ActionOpenCompletedFile(false).ok();
                case DetailsActionId::kPause: return ActionPauseSelected().ok();
                case DetailsActionId::kResume: return ActionResumeSelected().ok();
                case DetailsActionId::kCancel: return ActionCancelSelected().ok();
                case DetailsActionId::kRemoveHistory: return ActionRemoveSelectedFromList().ok();
            }
            return false;
        }
        case HitTargetType::kSelectAllCheckbox: {
            ToggleSelectAll();
            return true;
        }
        case HitTargetType::kSortDropdown: {
            CycleSortDropdown();
            return true;
        }
        case HitTargetType::kFilterDropdown: {
            CycleFilterDropdown();
            return true;
        }
        case HitTargetType::kSearchSlidersButton: {
            OpenSettingsCenter(SettingsCategory::kGeneral);
            return true;
        }
        case HitTargetType::kStatusToggleOff: {
            ToggleSpeedLimiter();
            return true;
        }
        case HitTargetType::kRowCheckbox: {
            ToggleTaskChecked(hit.task_id);
            SelectTask(hit.task_id);
            return true;
        }
        case HitTargetType::kToolbarButton: {
            if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.toolbar_buttons.size()) {
                const auto& btn = geom.toolbar_buttons[static_cast<size_t>(hit.index)];
                if (!btn.enabled) return false;
                TriggerToolbarAction(btn.action_id);
                return true;
            }
            return false;
        }
        case HitTargetType::kSidebarCategory: {
            if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.sidebar_items.size()) {
                SelectCategory(geom.sidebar_items[static_cast<size_t>(hit.index)].category);
                return true;
            }
            return false;
        }
        case HitTargetType::kTableHeaderColumn: {
            if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.table_columns.size()) {
                ToggleSortColumn(geom.table_columns[static_cast<size_t>(hit.index)].sort_column);
                return true;
            }
            return false;
        }
        case HitTargetType::kTaskRowActionButton: {
            SelectTask(hit.task_id);
            TriggerRowPrimaryAction(hit.task_id);
            return true;
        }
        case HitTargetType::kTaskRow: {
            SelectTask(hit.task_id);
            if (double_click) {
                ActionOpenCompletedFile(false);
            }
            return true;
        }
        default:
            return false;
    }
}

HitTestResult DesktopView::HandleMouseMove(int x, int y, size_t scroll_offset) {
    auto hit = HitTestPoint(x, y, scroll_offset);

    int new_toolbar_idx = (hit.type == HitTargetType::kToolbarButton) ? hit.index : -1;
    if (new_toolbar_idx != m_hovered_toolbar_index) {
        m_hovered_toolbar_index = new_toolbar_idx;
        if (new_toolbar_idx >= 0) {
            m_anim.hovered_toolbar_idx = new_toolbar_idx;
            m_anim.toolbar_hover_start_ms = GetTickCount64();
        } else {
            m_anim.hovered_toolbar_idx = -1;
            m_anim.toolbar_hover_start_ms = 0;
        }
    }

    int new_row_idx = (hit.type == HitTargetType::kTaskRow || hit.type == HitTargetType::kTaskRowActionButton) ? hit.index : -1;
    if (new_row_idx != m_hovered_row_index) {
        m_hovered_row_index = new_row_idx;
        if (new_row_idx >= 0) {
            m_anim.hovered_row_idx = new_row_idx;
            m_anim.row_hover_start_ms = GetTickCount64();
        } else {
            m_anim.hovered_row_idx = -1;
            m_anim.row_hover_start_ms = 0;
        }
    }

    m_hovered_sidebar_index = (hit.type == HitTargetType::kSidebarCategory) ? hit.index : -1;
    m_hovered_details_btn_index = (hit.type == HitTargetType::kSidebarSettingsButton) ? -2 :
        (hit.type == HitTargetType::kSidebarLanguageButton ? -3 :
        (hit.type == HitTargetType::kDetailsActionButton ? hit.index : -1));
    m_active_tooltip = hit.tooltip_text;
    return hit;
}

void DesktopView::ClearHoverState() {
    m_hovered_toolbar_index = -1;
    m_hovered_sidebar_index = -1;
    m_hovered_details_btn_index = -1;
    m_hovered_row_index = -1;
    m_anim.hovered_toolbar_idx = -1;
    m_anim.toolbar_hover_start_ms = 0;
    m_anim.hovered_row_idx = -1;
    m_anim.row_hover_start_ms = 0;
    m_active_tooltip.clear();
}

RectI DesktopView::GetRowRect(size_t row_index) const {
    const auto geometry = ComputeLayoutGeometry(m_scroll_offset);
    int row_h = geometry.task_row_height;
    int content_x = geometry.sidebar_rect.right;
    int bar_h = geometry.table_header_rect.top;
    int hdr_h = geometry.table_header_rect.height();
    int ry = bar_h + hdr_h + static_cast<int>(row_index) * row_h;
    return RectI{content_x, ry, m_window_width, ry + row_h};
}

void DesktopView::SetTheme(std::string_view theme_name, bool os_prefers_light) {
    if (theme_name == "light" || theme_name == "system" || theme_name == "dark" || theme_name == "coral" || theme_name == "coral_dark" || theme_name == "redesign" || theme_name == "classic_blue") {
        m_theme_name = std::string(theme_name);
    } else {
        m_theme_name = "coral_dark";
    }
    m_palette = DesignSystem::ResolvePalette(m_theme_name, os_prefers_light);
}

std::string DesktopView::FormatBytes(uint64_t bytes) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(1);
    if (bytes >= 1073741824ULL) {
        oss << (static_cast<double>(bytes) / 1073741824.0) << " GiB";
    } else if (bytes >= 1048576ULL) {
        oss << (static_cast<double>(bytes) / 1048576.0) << " MiB";
    } else if (bytes >= 1024ULL) {
        oss << (static_cast<double>(bytes) / 1024.0) << " KiB";
    } else {
        oss << bytes << " B";
    }
    return oss.str();
}

std::string DesktopView::FormatSpeed(uint64_t bps) {
    if (bps == 0) return "0 B/s";
    return FormatBytes(bps) + "/s";
}

std::string DesktopView::FormatBandwidthLimit(uint64_t bps) {
    if (bps == 0) return "Unlimited";
    return FormatBytes(bps) + "/s";
}

std::string DesktopView::FormatEta(uint64_t remaining_bytes, uint64_t speed_bps) {
    if (speed_bps == 0 || remaining_bytes == 0) return "—";
    uint64_t secs = remaining_bytes / speed_bps;
    if (secs > 359999ULL) return ">99h";
    uint64_t hrs = secs / 3600;
    uint64_t mins = (secs % 3600) / 60;
    uint64_t rem_s = secs % 60;
    char buf[32];
    if (hrs > 0) {
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
                      static_cast<unsigned long long>(hrs),
                      static_cast<unsigned long long>(mins),
                      static_cast<unsigned long long>(rem_s));
    } else {
        std::snprintf(buf, sizeof(buf), "%02llu:%02llu",
                      static_cast<unsigned long long>(mins),
                      static_cast<unsigned long long>(rem_s));
    }
    return std::string(buf);
}

std::string DesktopView::TruncateFilenameGracefully(std::string_view filename, size_t max_chars) {
#ifdef _WIN32
    std::wstring wstr = Utf8ToW(filename);
    if (wstr.size() <= max_chars || max_chars < 12) {
        return std::string(filename);
    }
    size_t tail_len = std::min<size_t>(10, (max_chars - 3) / 3);
    size_t head_len = max_chars - 3 - tail_len;
    std::wstring truncated = wstr.substr(0, head_len) + L"..." + wstr.substr(wstr.size() - tail_len);
    return WToUtf8(truncated);
#else
    if (filename.size() <= max_chars || max_chars < 12) {
        return std::string(filename);
    }
    size_t tail_len = std::min<size_t>(10, (max_chars - 3) / 3);
    size_t head_len = max_chars - 3 - tail_len;
    return std::string(filename.substr(0, head_len)) + "..." +
           std::string(filename.substr(filename.size() - tail_len));
#endif
}

std::string DesktopView::FormatFriendlyTime(int64_t epoch_ms) {
    if (epoch_ms <= 0) return "—";
    std::time_t item_t = static_cast<std::time_t>(epoch_ms / 1000);
    std::time_t now_t = std::time(nullptr);
    std::tm item_tm{};
    std::tm now_tm{};
#ifdef _WIN32
    localtime_s(&item_tm, &item_t);
    localtime_s(&now_tm, &now_t);
#else
    localtime_r(&item_t, &item_tm);
    localtime_r(&now_t, &now_tm);
#endif
    int hour12 = item_tm.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char* ampm = (item_tm.tm_hour >= 12) ? "PM" : "AM";
    char time_str[32];
    std::snprintf(time_str, sizeof(time_str), "%d:%02d %s", hour12, item_tm.tm_min, ampm);

    if (item_tm.tm_year == now_tm.tm_year && item_tm.tm_yday == now_tm.tm_yday) {
        return "Today " + std::string(time_str);
    }
    if (item_tm.tm_year == now_tm.tm_year && item_tm.tm_yday == now_tm.tm_yday - 1) {
        return "Yesterday " + std::string(time_str);
    }
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int m_idx = std::clamp(item_tm.tm_mon, 0, 11);
    char date_str[32];
    if (item_tm.tm_year == now_tm.tm_year) {
        std::snprintf(date_str, sizeof(date_str), "%s %d", months[m_idx], item_tm.tm_mday);
    } else {
        std::snprintf(date_str, sizeof(date_str), "%s %d, %d", months[m_idx], item_tm.tm_mday, 1900 + item_tm.tm_year);
    }
    return std::string(date_str);
}

std::string DesktopView::FormatSizePretty(uint64_t bytes) {
    if (bytes == 0) return "0 B";
    char buf[32];
    double b = static_cast<double>(bytes);
    if (bytes >= 1073741824ULL) {
        double gib = b / 1073741824.0;
        std::snprintf(buf, sizeof(buf), "%.1f GB", gib);
    } else if (bytes >= 1048576ULL) {
        double mib = b / 1048576.0;
        if (mib >= 10.0) {
            std::snprintf(buf, sizeof(buf), (mib == static_cast<int>(mib)) ? "%.0f MB" : "%.1f MB", mib);
        } else {
            std::snprintf(buf, sizeof(buf), "%.2f MB", mib);
        }
    } else if (bytes >= 1024ULL) {
        double kib = b / 1024.0;
        std::snprintf(buf, sizeof(buf), (kib >= 100.0) ? "%.0f KB" : "%.1f KB", kib);
    } else {
        std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
    }
    return std::string(buf);
}

std::string DesktopView::FormatSpeedKbps(uint64_t bps) {
    return FormatSpeed(bps);
}

void DesktopView::ToggleSelectAll() {
    auto visible = GetFilteredAndSortedTasks();
    if (visible.empty()) {
        m_checked_task_ids.clear();
        m_select_all_checked = false;
        return;
    }
    bool all_checked = true;
    for (const auto& t : visible) {
        if (m_checked_task_ids.find(t.task_id) == m_checked_task_ids.end()) {
            all_checked = false;
            break;
        }
    }
    if (all_checked) {
        m_checked_task_ids.clear();
        m_select_all_checked = false;
    } else {
        for (const auto& t : visible) {
            m_checked_task_ids.insert(t.task_id);
        }
        m_select_all_checked = true;
    }
}

void DesktopView::ToggleTaskChecked(const std::string& task_id) {
    if (m_checked_task_ids.count(task_id)) {
        m_checked_task_ids.erase(task_id);
    } else {
        m_checked_task_ids.insert(task_id);
    }
    auto visible = GetFilteredAndSortedTasks();
    m_select_all_checked = (!visible.empty() && m_checked_task_ids.size() >= visible.size());
}

void DesktopView::SetTaskChecked(const std::string& task_id, bool checked) {
    if (checked) {
        m_checked_task_ids.insert(task_id);
    } else {
        m_checked_task_ids.erase(task_id);
    }
    auto visible = GetFilteredAndSortedTasks();
    m_select_all_checked = (!visible.empty() && m_checked_task_ids.size() >= visible.size());
}

void DesktopView::CycleSortDropdown() {
    switch (m_sort_column) {
        case SortColumn::kCreatedAt:
            SetSort(SortColumn::kSize, false);
            break;
        case SortColumn::kSize:
            SetSort(SortColumn::kStatus, true);
            break;
        case SortColumn::kStatus:
            SetSort(SortColumn::kSpeed, false);
            break;
        case SortColumn::kSpeed:
            SetSort(SortColumn::kFileName, true);
            break;
        case SortColumn::kFileName:
        default:
            SetSort(SortColumn::kCreatedAt, false);
            break;
    }
}

void DesktopView::CycleFilterDropdown() {
    switch (m_selected_category) {
        case SidebarCategory::kAllDownloads:
            SelectCategory(SidebarCategory::kInProgress);
            break;
        case SidebarCategory::kInProgress:
            SelectCategory(SidebarCategory::kQueued);
            break;
        case SidebarCategory::kQueued:
            SelectCategory(SidebarCategory::kCompleted);
            break;
        case SidebarCategory::kCompleted:
            SelectCategory(SidebarCategory::kFailed);
            break;
        case SidebarCategory::kFailed:
            SelectCategory(SidebarCategory::kVideoDownloads);
            break;
        case SidebarCategory::kVideoDownloads:
            SelectCategory(SidebarCategory::kScheduled);
            break;
        case SidebarCategory::kScheduled:
            SelectCategory(SidebarCategory::kPaused);
            break;
        case SidebarCategory::kPaused:
        default:
            SelectCategory(SidebarCategory::kAllDownloads);
            break;
    }
}

void DesktopView::SelectCategory(SidebarCategory category) {
    ClearHoverState();
    if (m_selected_category != category) {
        m_anim.prev_category_idx = static_cast<int>(m_selected_category);
        m_anim.current_category_idx = static_cast<int>(category);
        m_anim.category_transition_start_ms = GetTickCount64();
    }
    m_selected_category = category;
    auto visible = GetFilteredAndSortedTasks();
    bool still_visible = false;
    for (const auto& t : visible) {
        if (t.task_id == m_selected_task_id) {
            still_visible = true;
            break;
        }
    }
    if (!still_visible) {
        m_selected_task_id = visible.empty() ? "" : visible.front().task_id;
    }
    m_last_feedback_message = "Filtered category: " + CategoryToLabel(category);
}

void DesktopView::SetSidebarVisible(bool visible) {
    if (m_sidebar_visible != visible) {
        m_sidebar_visible = visible;
        ClearHoverState();
    }
}

void DesktopView::ToggleSidebarVisible() {
    SetSidebarVisible(!m_sidebar_visible);
}

std::string DesktopView::CategoryToLabel(SidebarCategory category) {
    switch (category) {
        case SidebarCategory::kAllDownloads:   return GetLocalizationManager().Translate("All Downloads");
        case SidebarCategory::kInProgress:     return GetLocalizationManager().Translate("In Progress");
        case SidebarCategory::kQueued:         return GetLocalizationManager().Translate("Queued");
        case SidebarCategory::kCompleted:      return GetLocalizationManager().Translate("Completed");
        case SidebarCategory::kFailed:         return GetLocalizationManager().Translate("Failed");
        case SidebarCategory::kVideoDownloads: return GetLocalizationManager().Translate("Video Downloads");
        case SidebarCategory::kScheduled:      return GetLocalizationManager().Translate("Scheduled");
        case SidebarCategory::kPaused:         return GetLocalizationManager().Translate("Paused");
    }
    return GetLocalizationManager().Translate("All Downloads");
}

bool DesktopView::MatchesCategory(const TaskModel& t, SidebarCategory cat) const {
    switch (cat) {
        case SidebarCategory::kAllDownloads:
            return true;
        case SidebarCategory::kInProgress:
            return t.state == TaskState::kProbing || t.state == TaskState::kDownloading ||
                   t.state == TaskState::kMuxing || t.state == TaskState::kVerifying ||
                   t.state == TaskState::kFinalizing;
        case SidebarCategory::kQueued:
            return t.state == TaskState::kQueued || (t.state == TaskState::kPaused && !t.scheduled);
        case SidebarCategory::kPaused:
            return t.state == TaskState::kPaused && !t.scheduled;
        case SidebarCategory::kCompleted:
            return t.state == TaskState::kCompleted;
        case SidebarCategory::kFailed:
            return t.state == TaskState::kFailed || t.state == TaskState::kCanceled;
        case SidebarCategory::kVideoDownloads:
            return t.task_type == TaskType::kStreamHls || t.task_type == TaskType::kStreamDash ||
                   t.task_type == TaskType::kStreamMss || t.category == "Video";
        case SidebarCategory::kScheduled:
            return t.scheduled;
    }
    return true;
}

bool DesktopView::MatchesSearch(const TaskModel& t) const {
    if (m_search_query.empty()) return true;
    std::string q = ToLower(m_search_query);
    return ToLower(t.file_name).find(q) != std::string::npos ||
           ToLower(t.url).find(q) != std::string::npos ||
           ToLower(t.page_url).find(q) != std::string::npos ||
           ToLower(t.category).find(q) != std::string::npos;
}

CategoryCounts DesktopView::ComputeCategoryCounts() const {
    CategoryCounts counts{};
    auto tasks = m_controller.ListTasks();
    for (const auto& t : tasks) {
        ++counts.all_downloads;
        if (MatchesCategory(t, SidebarCategory::kInProgress)) ++counts.in_progress;
        if (MatchesCategory(t, SidebarCategory::kQueued)) ++counts.queued;
        if (MatchesCategory(t, SidebarCategory::kPaused)) ++counts.paused;
        if (MatchesCategory(t, SidebarCategory::kCompleted)) ++counts.completed;
        if (MatchesCategory(t, SidebarCategory::kFailed)) ++counts.failed;
        if (MatchesCategory(t, SidebarCategory::kVideoDownloads)) ++counts.video_downloads;
        if (MatchesCategory(t, SidebarCategory::kScheduled)) ++counts.scheduled;
    }
    return counts;
}

void DesktopView::SetSearchQuery(std::string query) {
    ClearHoverState();
    m_search_query = std::move(query);
}

void DesktopView::SetSort(SortColumn column, bool ascending) {
    ClearHoverState();
    m_sort_column = column;
    m_sort_ascending = ascending;
}

void DesktopView::ToggleSortColumn(SortColumn column) {
    if (m_sort_column == column) {
        m_sort_ascending = !m_sort_ascending;
    } else {
        m_sort_column = column;
        m_sort_ascending = true;
    }
}

void DesktopView::SelectTask(const std::string& task_id) {
    m_selected_task_id = task_id;
}

void DesktopView::SelectNextRow() {
    auto list = GetFilteredAndSortedTasks();
    if (list.empty()) {
        m_selected_task_id.clear();
        return;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].task_id == m_selected_task_id) {
            size_t next = std::min(i + 1, list.size() - 1);
            m_selected_task_id = list[next].task_id;
            return;
        }
    }
    m_selected_task_id = list.front().task_id;
}

void DesktopView::SelectPrevRow() {
    auto list = GetFilteredAndSortedTasks();
    if (list.empty()) {
        m_selected_task_id.clear();
        return;
    }
    for (size_t i = 0; i < list.size(); ++i) {
        if (list[i].task_id == m_selected_task_id) {
            size_t prev = (i > 0) ? (i - 1) : 0;
            m_selected_task_id = list[prev].task_id;
            return;
        }
    }
    m_selected_task_id = list.front().task_id;
}

void DesktopView::SelectFirstRow() {
    auto list = GetFilteredAndSortedTasks();
    m_selected_task_id = list.empty() ? "" : list.front().task_id;
}

void DesktopView::SelectLastRow() {
    auto list = GetFilteredAndSortedTasks();
    m_selected_task_id = list.empty() ? "" : list.back().task_id;
}

void DesktopView::ScrollBy(int delta_rows) {
    ClearHoverState();
    auto filtered = GetFilteredAndSortedTasks();
    const auto geometry = ComputeLayoutGeometry(0);
    size_t visible_count = static_cast<size_t>(std::max(1,
        geometry.table_body_rect.height() / std::max(1, geometry.task_row_height)));

    if (filtered.size() <= visible_count) {
        m_scroll_offset = 0;
        return;
    }
    size_t max_scroll = filtered.size() - visible_count;
    int cur = static_cast<int>(m_scroll_offset) + delta_rows;
    if (cur < 0) cur = 0;
    if (cur > static_cast<int>(max_scroll)) cur = static_cast<int>(max_scroll);
    m_scroll_offset = static_cast<size_t>(cur);
}

bool DesktopView::ScrollTaskIntoView(const std::string& task_id, bool select_row) {
    if (task_id.empty()) return false;
    auto filtered = GetFilteredAndSortedTasks();
    if (filtered.empty()) return false;

    // Check if task exists in currently filtered/sorted list
    int found_idx = -1;
    for (size_t i = 0; i < filtered.size(); ++i) {
        if (filtered[i].task_id == task_id) {
            found_idx = static_cast<int>(i);
            break;
        }
    }

    // If not visible under current filter/search, switch category to AllDownloads and clear search
    if (found_idx < 0) {
        auto task_opt = m_controller.GetTask(task_id);
        if (!task_opt.has_value()) return false;
        m_selected_category = SidebarCategory::kAllDownloads;
        m_search_query.clear();
        filtered = GetFilteredAndSortedTasks();
        for (size_t i = 0; i < filtered.size(); ++i) {
            if (filtered[i].task_id == task_id) {
                found_idx = static_cast<int>(i);
                break;
            }
        }
        if (found_idx < 0) return false;
    }

    if (select_row) {
        m_selected_task_id = task_id;
    }

    const auto geometry = ComputeLayoutGeometry(0);
    size_t visible_count = static_cast<size_t>(std::max(1,
        geometry.table_body_rect.height() / std::max(1, geometry.task_row_height)));

    if (filtered.size() <= visible_count) {
        m_scroll_offset = 0;
    } else {
        size_t max_scroll = filtered.size() - visible_count;
        size_t target_idx = static_cast<size_t>(found_idx);
        if (target_idx < m_scroll_offset) {
            m_scroll_offset = target_idx;
        } else if (target_idx >= m_scroll_offset + visible_count) {
            m_scroll_offset = target_idx - visible_count + 1;
        }
        if (m_scroll_offset > max_scroll) {
            m_scroll_offset = max_scroll;
        }
    }
    return true;
}

bool DesktopView::CheckAndAutoScrollNewOrStartedDownloads() {
    auto all_tasks = m_controller.ListTasks();

    if (!m_known_tasks_initialized) {
        m_known_tasks_initialized = true;
        for (const auto& t : all_tasks) {
            m_known_task_states[t.task_id] = t.state;
        }
        return false;
    }

    std::string task_to_focus;

    // Check for newly added downloads
    for (const auto& t : all_tasks) {
        auto it = m_known_task_states.find(t.task_id);
        if (it == m_known_task_states.end()) {
            // Brand new download task detected!
            task_to_focus = t.task_id;
            break;
        }
    }

    // Refresh known state cache
    m_known_task_states.clear();
    for (const auto& t : all_tasks) {
        m_known_task_states[t.task_id] = t.state;
    }

    if (!task_to_focus.empty()) {
        return ScrollTaskIntoView(task_to_focus, true);
    }

    return false;
}

int DesktopView::GetColumnWidth(size_t col_idx) const {
    if (col_idx < m_column_widths.size()) {
        return m_column_widths[col_idx];
    }
    return 100;
}

void DesktopView::SetColumnWidth(size_t col_idx, int width) {
    if (col_idx < m_column_widths.size()) {
        m_column_widths[col_idx] = std::max(50, width);
    }
}

void DesktopView::StartColumnResize(int col_idx, int mouse_x) {
    if (col_idx >= 0 && col_idx < static_cast<int>(m_column_widths.size())) {
        auto geom = ComputeLayoutGeometry(0);
        m_resizing_col_index = col_idx;
        m_resize_start_x = mouse_x;
        if (col_idx < static_cast<int>(geom.table_columns.size())) {
            m_resize_orig_width = geom.table_columns[static_cast<size_t>(col_idx)].rect.width();
        } else {
            m_resize_orig_width = m_column_widths[static_cast<size_t>(col_idx)];
        }
    }
}

void DesktopView::UpdateColumnResize(int mouse_x) {
    if (m_resizing_col_index >= 0 && m_resizing_col_index < static_cast<int>(m_column_widths.size())) {
        int delta = mouse_x - m_resize_start_x;
        int min_w = 60;
        int max_w = 1200;
        if (m_resizing_col_index == 0) { // Name
            min_w = 140;
            max_w = 1600;
        } else if (m_resizing_col_index == 1) { // Size
            min_w = 60;
            max_w = 350;
        } else if (m_resizing_col_index == 2) { // Status
            min_w = 90;
            max_w = 450;
        } else if (m_resizing_col_index == 3) { // Download
            min_w = 75;
            max_w = 350;
        } else if (m_resizing_col_index == 4) { // Upload
            min_w = 65;
            max_w = 300;
        } else if (m_resizing_col_index == 5) { // Added
            min_w = 80;
            max_w = 350;
        }
        int new_w = std::clamp(m_resize_orig_width + delta, min_w, max_w);
        m_column_widths[static_cast<size_t>(m_resizing_col_index)] = new_w;
    }
}

void DesktopView::EndColumnResize() {
    m_resizing_col_index = -1;
    SaveColumnWidthsToSettings();
}

int DesktopView::HitTestColumnDivider(int x, int y, size_t scroll_offset) const {
    auto geom = ComputeLayoutGeometry(scroll_offset);
    const int hit_slop = DesignSystem::ScaleForDpi(6, m_metrics.dpi);
    if (y < geom.table_header_rect.top || y >= geom.table_header_rect.bottom) {
        return -1;
    }
    for (size_t i = 0; i < geom.table_columns.size() && i <= 5; ++i) {
        int edge_x = geom.table_columns[i].rect.right;
        if (geom.table_columns[i].rect.width() > 0 && std::abs(x - edge_x) <= hit_slop) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void DesktopView::SaveColumnWidthsToSettings() {
    if (m_column_widths.size() >= 6) {
        std::string val = std::to_string(m_column_widths[0]) + "," +
                          std::to_string(m_column_widths[1]) + "," +
                          std::to_string(m_column_widths[2]) + "," +
                          std::to_string(m_column_widths[3]) + "," +
                          std::to_string(m_column_widths[4]) + "," +
                          std::to_string(m_column_widths[5]);
        auto s = m_controller.GetSettings();
        if (s.table_column_widths != val) {
            s.table_column_widths = val;
            m_controller.UpdateSettings(s);
        }
    }
}

void DesktopView::LoadColumnWidthsFromSettings() {
    auto s = m_controller.GetSettings();
    if (!s.table_column_widths.empty()) {
        std::vector<int> widths;
        std::stringstream ss(s.table_column_widths);
        std::string token;
        while (std::getline(ss, token, ',')) {
            try {
                widths.push_back(std::stoi(token));
            } catch (...) {}
        }
        if (widths.size() >= 6) {
            widths[1] = std::max(widths[1], 150);
            widths[2] = std::max(widths[2], 150);
            widths[5] = std::max(widths[5], 145);
            m_column_widths = std::move(widths);
        }
    }
}

std::vector<TaskModel> DesktopView::GetFilteredAndSortedTasks() const {
    auto all = m_controller.ListTasks();
    std::vector<TaskModel> filtered;
    filtered.reserve(all.size());
    for (const auto& t : all) {
        if (MatchesCategory(t, m_selected_category) && MatchesSearch(t)) {
            filtered.push_back(t);
        }
    }

    auto compute_pct = [](const TaskModel& t) -> double {
        if (t.state == TaskState::kCompleted) return 100.0;
        double pct = (t.total_bytes > 0)
            ? (static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes)
            : t.stream_percent;
        if (t.state == TaskState::kFinalizing) return std::clamp(pct, 0.0, 99.9);
        if (t.state == TaskState::kVerifying) return std::clamp(pct, 0.0, 99.5);
        if (t.state == TaskState::kMuxing) return std::clamp(pct, 0.0, 99.0);
        return std::clamp(pct, 0.0, 99.0);
    };

    std::sort(filtered.begin(), filtered.end(), [&](const TaskModel& a, const TaskModel& b) {
        int cmp = 0;
        switch (m_sort_column) {
            case SortColumn::kFileName:
                cmp = ToLower(a.file_name).compare(ToLower(b.file_name));
                break;
            case SortColumn::kDownloadType:
                cmp = TaskTypeToString(a.task_type).compare(TaskTypeToString(b.task_type));
                break;
            case SortColumn::kStatus:
                cmp = TaskStateToString(a.state).compare(TaskStateToString(b.state));
                break;
            case SortColumn::kProgress: {
                double pa = compute_pct(a);
                double pb = compute_pct(b);
                cmp = (pa < pb) ? -1 : (pa > pb ? 1 : 0);
                break;
            }
            case SortColumn::kSize:
                cmp = (a.total_bytes < b.total_bytes) ? -1 : (a.total_bytes > b.total_bytes ? 1 : 0);
                break;
            case SortColumn::kSpeed:
                cmp = (a.speed_bps < b.speed_bps) ? -1 : (a.speed_bps > b.speed_bps ? 1 : 0);
                break;
            case SortColumn::kEta: {
                uint64_t ra = (a.total_bytes > a.downloaded_bytes) ? (a.total_bytes - a.downloaded_bytes) : 0;
                uint64_t rb = (b.total_bytes > b.downloaded_bytes) ? (b.total_bytes - b.downloaded_bytes) : 0;
                uint64_t ea = (a.speed_bps > 0) ? (ra / a.speed_bps) : UINT64_MAX;
                uint64_t eb = (b.speed_bps > 0) ? (rb / b.speed_bps) : UINT64_MAX;
                cmp = (ea < eb) ? -1 : (ea > eb ? 1 : 0);
                break;
            }
            case SortColumn::kCreatedAt:
            default:
                cmp = (a.created_at_ms < b.created_at_ms) ? -1 : (a.created_at_ms > b.created_at_ms ? 1 : 0);
                break;
        }
        return m_sort_ascending ? (cmp < 0) : (cmp > 0);
    });

    return filtered;
}

std::vector<FormattedTaskRow> DesktopView::BuildVisibleTaskRows(size_t scroll_offset, size_t max_visible_rows) const {
    auto list = GetFilteredAndSortedTasks();
    std::vector<FormattedTaskRow> rows;
    if (scroll_offset >= list.size()) return rows;

    size_t end_idx = std::min(list.size(), scroll_offset + max_visible_rows);
    rows.reserve(end_idx - scroll_offset);

    for (size_t i = scroll_offset; i < end_idx; ++i) {
        const auto& t = list[i];
        FormattedTaskRow row;
        row.task_id = t.task_id;
        row.state = t.state;
        row.category = t.category;
        row.full_filename_tooltip = t.file_name;
        row.display_filename = StripLeadingOrphanCombiningMarks(t.file_name);
        row.is_filename_truncated = false;
        row.is_stream = (t.task_type != TaskType::kHttpFile);

        if (t.task_type == TaskType::kStreamHls) row.file_type_badge = "HLS STREAM";
        else if (t.task_type == TaskType::kStreamDash) row.file_type_badge = "DASH STREAM";
        else row.file_type_badge = t.range_supported ? "HTTP MULTI" : "HTTP FILE";

        if (!t.status_detail.empty()) {
            row.status_text = t.status_detail;
        } else {
            switch (t.state) {
                case TaskState::kQueued:      row.status_text = "Queued"; break;
                case TaskState::kProbing:     row.status_text = row.is_stream ? "Probing manifest..." : "Probing server..."; break;
                case TaskState::kDownloading: row.status_text = row.is_stream ? "Downloading video segments..." : "Downloading..."; break;
                case TaskState::kMuxing:      row.status_text = "Merging tracks (FFmpeg)..."; break;
                case TaskState::kVerifying:   row.status_text = "Verifying output..."; break;
                case TaskState::kFinalizing:  row.status_text = "Finalizing output..."; break;
                case TaskState::kPaused:      row.status_text = t.scheduled ? ("Scheduled (" + t.schedule_time_local + ")") : "Paused"; break;
                case TaskState::kCompleted:   row.status_text = "Completed"; break;
                case TaskState::kFailed:      row.status_text = "Failed (" + (t.error_code.empty() ? "Error" : t.error_code) + ")"; break;
                case TaskState::kCanceled:    row.status_text = "Canceled"; break;
            }
        }

        // Semantic status indicator & progress fill colors
        // Restrained monochromatic + single accent rule from Design Vision Part 3.F
        switch (t.state) {
            case TaskState::kCompleted:
                row.status_indicator_color = m_palette.status_success; // #74747E Dark / #64748B Light
                row.progress_fill_color = (m_palette.mode == ThemeMode::kLight) ? m_palette.accent_primary : m_palette.status_success;
                break;
            case TaskState::kFailed:
            case TaskState::kCanceled:
                row.status_indicator_color = m_palette.status_danger; // #FF4757 Dark / #DC2626 Light
                row.progress_fill_color = m_palette.status_danger;
                break;
            case TaskState::kPaused:
                row.status_indicator_color = m_palette.text_secondary; // #A1A1AA Dark / #64748B Light
                row.progress_fill_color = m_palette.text_secondary;
                break;
            case TaskState::kProbing:
            case TaskState::kMuxing:
            case TaskState::kVerifying:
            case TaskState::kFinalizing:
                row.status_indicator_color = m_palette.text_secondary;
                row.progress_fill_color = m_palette.text_secondary;
                break;
            case TaskState::kDownloading:
                row.status_indicator_color = m_palette.accent_primary; // Coral Dark / Sky Blue Light
                row.progress_fill_color = m_palette.accent_primary;
                break;
            case TaskState::kQueued:
            default:
                row.status_indicator_color = m_palette.text_muted;
                row.progress_fill_color = m_palette.text_muted;
                break;
        }

        std::ostringstream prog_ss;
        if (t.state == TaskState::kCompleted) {
            row.progress_fraction = 1.0;
            row.progress_text = "100.0%";
        } else if (t.total_bytes > 0) {
            double pct = (static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes);
            if (t.state == TaskState::kFinalizing) {
                pct = std::clamp(pct, 0.0, 99.9);
            } else if (t.state == TaskState::kVerifying) {
                pct = std::clamp(pct, 0.0, 99.5);
            } else if (t.state == TaskState::kMuxing) {
                pct = std::clamp(pct, 0.0, 99.0);
            } else {
                pct = std::clamp(pct, 0.0, 99.0);
            }
            row.progress_fraction = pct / 100.0;
            prog_ss << std::fixed << std::setprecision(1) << pct << "%";
            row.progress_text = prog_ss.str();
        } else if (row.is_stream && t.total_segments > 0) {
            double pct = (static_cast<double>(t.completed_segments) * 100.0) / static_cast<double>(t.total_segments);
            if (t.state == TaskState::kFinalizing) {
                pct = std::clamp(pct, 0.0, 99.9);
            } else if (t.state == TaskState::kVerifying) {
                pct = std::clamp(pct, 0.0, 99.5);
            } else if (t.state == TaskState::kMuxing) {
                pct = std::clamp(pct, 0.0, 99.0);
            } else {
                pct = std::clamp(pct, 0.0, 99.0);
            }
            row.progress_fraction = pct / 100.0;
            prog_ss << std::fixed << std::setprecision(1) << pct << "% (" << t.completed_segments << "/" << t.total_segments << " segs)";
            row.progress_text = prog_ss.str();
        } else {
            row.progress_fraction = 0.0;
            row.progress_indeterminate = (t.state == TaskState::kDownloading || t.state == TaskState::kProbing ||
                                          t.state == TaskState::kMuxing || t.state == TaskState::kVerifying ||
                                          t.state == TaskState::kFinalizing);
            row.progress_text = FormatBytes(t.downloaded_bytes) + " (Size Unknown)";
        }

        if (t.state == TaskState::kCompleted) {
            row.size_text = (t.total_bytes > 0) ? FormatSizePretty(t.total_bytes) : (t.downloaded_bytes > 0 ? FormatSizePretty(t.downloaded_bytes) : "—");
        } else if (t.total_bytes > 0) {
            if (t.downloaded_bytes > 0) {
                row.size_text = FormatSizePretty(t.downloaded_bytes) + " / " + FormatSizePretty(t.total_bytes);
            } else {
                row.size_text = FormatSizePretty(t.total_bytes);
            }
        } else {
            if (t.downloaded_bytes > 0) {
                row.size_text = FormatSizePretty(t.downloaded_bytes) + " / —";
            } else {
                row.size_text = "—";
            }
        }
        row.added_time_text = FormatFriendlyTime(t.created_at_ms);
        const bool is_seeding = t.task_type == TaskType::kTorrent &&
            t.status_detail.find("Seeding to BitTorrent swarm") != std::string::npos;
        row.upload_speed_text = (t.upload_speed_bps > 0) ? FormatSpeed(t.upload_speed_bps) :
            ((t.state == TaskState::kCompleted && !is_seeding) ? "" : "0 B/s");
        row.checked = (m_checked_task_ids.count(t.task_id) > 0);

        if (t.state == TaskState::kDownloading) {
            double pct = (t.total_bytes > 0)
                ? std::clamp((static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes), 0.0, 99.0)
                : (row.is_stream && t.total_segments > 0
                    ? std::clamp((static_cast<double>(t.completed_segments) * 100.0) / static_cast<double>(t.total_segments), 0.0, 99.0)
                    : 0.0);
            int ipct = static_cast<int>(pct);
            if (t.total_bytes > 0 || (row.is_stream && t.total_segments > 0)) {
                row.status_text = (m_window_width < DesignSystem::ScaleForDpi(1050, m_metrics.dpi))
                    ? (std::to_string(ipct) + "% · Downloading")
                    : ("Downloading · " + std::to_string(ipct) + "%");
            } else {
                row.status_text = "Downloading";
            }
            row.status_indicator_color = m_palette.accent_primary;
        } else if (t.state == TaskState::kProbing) {
            row.status_text = (!t.status_detail.empty()) ? t.status_detail : "Connecting...";
            row.status_indicator_color = m_palette.text_secondary;
        } else if (t.state == TaskState::kMuxing) {
            row.status_text = "Merging (FFmpeg)...";
            row.status_indicator_color = m_palette.text_secondary;
        } else if (t.state == TaskState::kVerifying) {
            row.status_text = "Verifying...";
            row.status_indicator_color = m_palette.text_secondary;
        } else if (t.state == TaskState::kPaused) {
            if (t.scheduled) {
                row.status_text = "Scheduled (" + t.schedule_time_local + ")";
            } else if (t.total_bytes > 0 && t.downloaded_bytes > 0) {
                double pct = (static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes);
                row.status_text = "Paused · " + std::to_string(static_cast<int>(pct)) + "%";
            } else {
                row.status_text = "Paused";
            }
            row.status_indicator_color = m_palette.text_secondary;
        } else if (t.state == TaskState::kQueued) {
            row.status_text = "Queued";
            row.status_indicator_color = m_palette.text_muted;
        } else if (t.state == TaskState::kCompleted) {
            row.status_text = "Completed";
            row.status_indicator_color = m_palette.status_success;
        } else if (t.state == TaskState::kFailed) {
            row.status_text = t.error_code.empty() ? "Failed" : ("Failed (" + t.error_code + ")");
            row.status_indicator_color = m_palette.status_danger;
        } else if (t.state == TaskState::kCanceled) {
            row.status_text = "Canceled";
            row.status_indicator_color = m_palette.status_danger;
        }

        if (!t.error_code.empty()) {
            row.full_status_tooltip = t.error_code + (!t.error_message.empty() ? (": " + t.error_message) : "");
        } else if (!t.status_detail.empty()) {
            row.full_status_tooltip = t.status_detail;
        } else {
            row.full_status_tooltip = row.status_text;
        }

        row.speed_text = (t.state == TaskState::kDownloading) ? FormatSpeedKbps(t.speed_bps) :
            (t.state == TaskState::kCompleted ? "" : "0 B/s");
        uint64_t rem_bytes = (t.total_bytes > t.downloaded_bytes) ? (t.total_bytes - t.downloaded_bytes) : 0;
        row.eta_text = (t.state == TaskState::kDownloading) ? FormatEta(rem_bytes, t.speed_bps) : "—";

        switch (t.state) {
            case TaskState::kDownloading:
            case TaskState::kProbing:
                row.primary_action_label = "Pause";
                break;
            case TaskState::kPaused:
                row.primary_action_label = "Resume";
                break;
            case TaskState::kQueued:
                row.primary_action_label = "Pause";
                break;
            case TaskState::kFailed:
            case TaskState::kCanceled:
                row.primary_action_label = "Retry";
                break;
            case TaskState::kCompleted:
                row.primary_action_label = "Open";
                break;
            default:
                row.primary_action_label = "Wait";
                break;
        }
        row.selected = (t.task_id == m_selected_task_id);
        rows.push_back(std::move(row));
    }
    return rows;
}

ToolbarState DesktopView::ComputeToolbarState() const {
    ToolbarState st;
    st.can_start_all = true;
    st.can_pause_all = true;
    auto all = m_controller.ListTasks();
    for (const auto& t : all) {
        if (t.state == TaskState::kCompleted || t.state == TaskState::kCanceled) {
            st.can_clear_history = true;
            break;
        }
    }

    if (m_selected_task_id.empty()) return st;
    auto sel = m_controller.GetTask(m_selected_task_id);
    if (!sel.has_value()) return st;

    const auto& t = *sel;
    st.can_remove = true;
    st.can_open_folder = !t.save_dir.empty();

    switch (t.state) {
        case TaskState::kQueued:
            st.can_pause = true;
            st.can_cancel = true;
            break;
        case TaskState::kProbing:
        case TaskState::kDownloading:
        case TaskState::kMuxing:
            st.can_pause = true;
            st.can_cancel = true;
            break;
        case TaskState::kPaused:
            st.can_start = true;
            st.can_resume = true;
            st.can_cancel = true;
            st.can_delete_file = true;
            break;
        case TaskState::kFailed:
        case TaskState::kCanceled:
            st.can_start = true;
            st.can_retry = true;
            break;
        case TaskState::kCompleted:
            st.can_open_file = true;
            st.can_delete_file = true;
            break;
        default:
            break;
    }
    return st;
}

TaskDetailsViewModel DesktopView::BuildSelectedTaskDetails() const {
    TaskDetailsViewModel vm;
    if (m_selected_task_id.empty()) return vm;
    auto sel = m_controller.GetTask(m_selected_task_id);
    if (!sel.has_value()) return vm;

    const auto& t = *sel;
    vm.has_selection = true;
    vm.task_id = t.task_id;
    vm.file_name = StripLeadingOrphanCombiningMarks(t.file_name);
    vm.category = t.category;
    vm.redacted_source_url = Logger::RedactSensitiveData(t.url);
    vm.redacted_page_url = t.page_url.empty() ? "—" : Logger::RedactSensitiveData(t.page_url);
    vm.destination_path = PathSanitizer::PathToUtf8(PathSanitizer::Utf8ToPath(t.save_dir) / PathSanitizer::Utf8ToPath(t.file_name));
    if (t.state == TaskState::kCompleted && t.total_bytes > 0) {
        vm.size_summary = FormatSizePretty(t.total_bytes);
    } else if (t.total_bytes > 0) {
        vm.size_summary = FormatSizePretty(t.downloaded_bytes) + " of " + FormatSizePretty(t.total_bytes);
        if (t.state == TaskState::kDownloading) {
            int ipct = static_cast<int>(std::clamp((static_cast<double>(t.downloaded_bytes) * 100.0) / static_cast<double>(t.total_bytes), 0.0, 99.0));
            vm.size_summary += " (" + std::to_string(ipct) + "%)";
        }
    } else {
        vm.size_summary = FormatSizePretty(t.downloaded_bytes) + " (Total size unknown)";
    }
    vm.progress_summary = t.status_detail.empty() ? TaskStateToString(t.state) : t.status_detail;
    if (vm.progress_summary == "COMPLETED") vm.progress_summary = "Completed";
    if (vm.progress_summary == "PAUSED") vm.progress_summary = "Paused";
    if (vm.progress_summary == "DOWNLOADING") vm.progress_summary = "Downloading";
    if (t.state == TaskState::kCompleted || t.state == TaskState::kPaused) {
        vm.speed_summary.clear();
    } else if (t.speed_bps == 0) {
        vm.speed_summary = "0 B/s";
    } else {
        double b = static_cast<double>(t.speed_bps);
        char sbuf[32];
        if (t.speed_bps >= 1073741824ULL) {
            std::snprintf(sbuf, sizeof(sbuf), "%.1f GB/s", b / 1073741824.0);
        } else if (t.speed_bps >= 1048576ULL) {
            std::snprintf(sbuf, sizeof(sbuf), "%.1f MB/s", b / 1048576.0);
        } else if (t.speed_bps >= 1024ULL) {
            std::snprintf(sbuf, sizeof(sbuf), "%.1f KB/s", b / 1024.0);
        } else {
            std::snprintf(sbuf, sizeof(sbuf), "%llu B/s", static_cast<unsigned long long>(t.speed_bps));
        }
        vm.speed_summary = sbuf;
    }
    vm.created_time_str = FormatTimestampUtc(t.created_at_ms);
    vm.updated_time_str = FormatTimestampUtc(t.updated_at_ms);
    vm.completed_time_str = FormatTimestampUtc(t.completed_at_ms);
    vm.retry_count = t.retry_count;

    if (t.task_type == TaskType::kHttpFile) {
        vm.engine_name = "Brisk Segmented HTTP/1.1 Engine (" + std::to_string(t.max_connections) + " max connections)";
        vm.stream_format = t.range_supported ? "HTTP/1.1 Byte-Range (206 Partial Content)" : "HTTP/1.1 Single-Stream (200 OK)";
        vm.selected_tracks_summary = "N/A (Direct File Transfer — " + std::to_string(t.segments.size()) + " segments)";
    } else {
        vm.engine_name = "N_m3u8DL-RE + FFmpeg Managed Stream Pipeline";
        vm.stream_format = (t.task_type == TaskType::kStreamDash) ? "MPEG-DASH (.mpd)" : "Apple HLS (.m3u8)";
        vm.selected_tracks_summary = "Video: " + t.stream_options.video_filter +
                                     " | Audio: " + t.stream_options.audio_filter +
                                     " | Subtitles: " + t.stream_options.subtitle_filter +
                                     " | Container: ." + t.stream_options.output_format;
    }

    std::ostringstream hdr_ss;
    for (const auto& [k, v] : t.headers) {
        hdr_ss << k << ": " << Logger::RedactSensitiveData(v) << "; ";
    }
    if (!t.auth_headers.empty()) {
        hdr_ss << "Auth/Cookies: [" << t.auth_headers.size() << " protected credential header(s) — REDACTED]";
    }
    vm.redacted_headers_summary = hdr_ss.str().empty() ? "Default headers" : hdr_ss.str();

    vm.error_code = t.error_code;
    if (t.error_code.empty()) {
        vm.error_explanation = "No errors.";
    } else if (t.error_code == "ERR_HTTP_403_EXPIRED_OR_AUTH") {
        vm.error_explanation = t.error_message + " — The server rejected the link or the signed token expired. Use 'Refresh URL' or re-capture from your browser.";
    } else if (t.error_code == "ERR_STREAM_DRM_PROTECTED") {
        vm.error_explanation = t.error_message + " — Encrypted with commercial DRM (Widevine/PlayReady/FairPlay), which Radium does not circumvent.";
    } else if (t.error_code == "ERR_DISK_FULL") {
        vm.error_explanation = t.error_message + " — Free up disk space in the destination folder and click Resume or Retry.";
    } else {
        vm.error_explanation = t.error_message;
    }

    vm.speed_limit_summary = (t.speed_limit_bps > 0) ? FormatSpeed(t.speed_limit_bps) : "Unlimited (Global Cap)";
    vm.priority_summary = TaskPriorityDisplayName(t.priority);
    if (!t.alternate_urls.empty()) {
        vm.alternate_urls_summary = std::to_string(t.alternate_urls.size()) + " mirror URL(s) configured";
    } else {
        vm.alternate_urls_summary = "None (Primary only)";
    }
    if (t.scheduled) {
        vm.schedule_summary = "Starts at " + t.schedule_time_local;
        if (!t.schedule_stop_time_local.empty()) {
            vm.schedule_summary += " | Stops at " + t.schedule_stop_time_local;
        }
    } else if (!t.schedule_stop_time_local.empty()) {
        vm.schedule_summary = "Scheduled stop at " + t.schedule_stop_time_local;
    } else {
        vm.schedule_summary = "Manual queue order";
    }

    return vm;
}

StatusBarViewModel DesktopView::BuildStatusBar() const {
    StatusBarViewModel sb;
    auto tasks = m_controller.ListTasks();
    for (const auto& t : tasks) {
        if (t.state == TaskState::kProbing || t.state == TaskState::kDownloading ||
            t.state == TaskState::kMuxing || t.state == TaskState::kVerifying) {
            ++sb.active_downloads;
            sb.total_speed_bps += t.speed_bps;
        } else if (t.state == TaskState::kQueued) {
            ++sb.remaining_queued;
        }
        sb.total_upload_speed_bps += t.upload_speed_bps;
    }
    sb.formatted_total_speed = FormatSpeed(sb.total_speed_bps);
    sb.formatted_total_upload_speed = FormatSpeed(sb.total_upload_speed_bps);
    sb.speed_limit_state = IsSpeedLimiterActive() ? "ON" : "OFF";
    sb.browser_ipc_listening = (m_ipc_server != nullptr);
    sb.browser_capture_enabled = m_controller.GetSettings().browser_capture_enabled;
    sb.workers_ready = m_controller.AreWorkersAvailable();

    std::ostringstream msg;
    msg << "Active: " << sb.active_downloads
        << " | Speed: " << sb.formatted_total_speed
        << " | Upload: " << sb.formatted_total_upload_speed
        << " | Queued: " << sb.remaining_queued
        << " | Browser Capture: " << (sb.browser_capture_enabled ? "Enabled" : "Paused")
        << " | Stream Workers: " << (sb.workers_ready ? "Ready" : "Missing");
    sb.status_message = msg.str();
    return sb;
}

std::string DesktopView::GetEmptyStateMessage() const {
    auto all = m_controller.ListTasks();
    if (all.empty()) {
        return "No downloads yet. Click '+ Add URL' (Ctrl+N) or use the Radium Browser Extension to capture files and video streams.";
    }
    if (!m_search_query.empty()) {
        return "No downloads match the search filter \"" + m_search_query + "\". Clear the search box to view all tasks.";
    }
    switch (m_selected_category) {
        case SidebarCategory::kInProgress:
            return "No active transfers right now. Select a paused task and click Resume, or add a new URL.";
        case SidebarCategory::kQueued:
            return "No queued downloads waiting to start.";
        case SidebarCategory::kPaused:
            return "No paused downloads.";
        case SidebarCategory::kCompleted:
            return "No completed downloads in history yet.";
        case SidebarCategory::kFailed:
            return "No failed downloads. All transfers are healthy.";
        case SidebarCategory::kVideoDownloads:
            return "No video or HLS/DASH stream downloads yet. Open a video page with the Radium Browser Extension enabled.";
        case SidebarCategory::kScheduled:
            return "No scheduled tasks configured.";
        default:
            return "No tasks in this view.";
    }
}

// -----------------------------------------------------------------------------
// Professional Download Manager Settings Center Implementation
// -----------------------------------------------------------------------------

void DesktopView::OpenSettingsCenter(SettingsCategory initial_category) {
    m_settings_center_open = true;
    m_anim.settings_open_start_ms = GetTickCount64();
    m_settings_category = initial_category;
    m_settings_scroll_offset = 0;
    m_draft_settings = m_controller.GetSettings();
    m_settings_validation_key.clear();
    m_settings_validation_error.clear();
    m_last_feedback_message = "Opened Settings Dialog";
}

void DesktopView::CloseSettingsCenter(bool revert_unsaved) {
    if (revert_unsaved) {
        RevertSettingsChanges();
    }
    m_settings_center_open = false;
    m_settings_scroll_offset = 0;
    m_settings_search_query.clear();
}

void DesktopView::SelectSettingsCategory(SettingsCategory category) {
    m_settings_category = category;
    m_settings_scroll_offset = 0;
    m_last_feedback_message = "Settings category: " + SettingsCategoryToLabel(category);
}

void DesktopView::ScrollSettingsBy(int delta_px) {
    auto sv = BuildSettingsCenterView();
    m_settings_scroll_offset = std::clamp(m_settings_scroll_offset + delta_px, 0, sv.max_scroll);
}

std::string DesktopView::SettingsCategoryToLabel(SettingsCategory category) {
    switch (category) {
        case SettingsCategory::kGeneral:             return GetLocalizationManager().Translate("General");
        case SettingsCategory::kDownloads:           return GetLocalizationManager().Translate("Downloads & Queue");
        case SettingsCategory::kSaveLocations:       return GetLocalizationManager().Translate("Save Locations");
        case SettingsCategory::kConnectionsAndSpeed: return GetLocalizationManager().Translate("Connections & Speed");
        case SettingsCategory::kBrowserIntegration:  return GetLocalizationManager().Translate("Browser Integration");
        case SettingsCategory::kNotifications:       return GetLocalizationManager().Translate("Notifications");
        case SettingsCategory::kAboutAndDiagnostics: return GetLocalizationManager().Translate("About & Diagnostics");
        case SettingsCategory::kKeyboardShortcuts:   return GetLocalizationManager().Translate("Keyboard Shortcuts");
    }
    return GetLocalizationManager().Translate("General");
}

void DesktopView::SetSettingsSearchQuery(std::string query) {
    m_settings_search_query = std::move(query);
    m_settings_scroll_offset = 0;
}

void DesktopView::SetDraftSettings(const AppSettings& draft) {
    m_draft_settings = draft;
    auto v = ValidateSettingsInput(m_draft_settings);
    if (!v.ok()) {
        m_settings_validation_error = v.error->message;
    } else {
        m_settings_validation_key.clear();
        m_settings_validation_error.clear();
    }
}

bool DesktopView::HasUnsavedSettingsChanges() const {
    return m_draft_settings != m_controller.GetSettings();
}

Result<void> DesktopView::ValidateSettingsInput(const AppSettings& candidate) const {
    if (candidate.max_concurrent_tasks < 1 || candidate.max_concurrent_tasks > 16) {
        return Result<void>::Err("ERR_SETTINGS_CONCURRENT_RANGE", "Simultaneous downloads must be between 1 and 16.");
    }
    if (candidate.default_connections_per_task < 1 || candidate.default_connections_per_task > 32) {
        return Result<void>::Err("ERR_SETTINGS_CONNECTIONS_RANGE", "Default connections per task must be between 1 and 32.");
    }
    if (candidate.default_download_dir.empty()) {
        return Result<void>::Err("ERR_SETTINGS_EMPTY_SAVE_DIR", "Default download directory cannot be empty.");
    }
    for (char c : candidate.default_download_dir) {
        if (static_cast<unsigned char>(c) < 0x20) {
            return Result<void>::Err("ERR_SETTINGS_INVALID_SAVE_DIR", "Default download directory contains invalid control characters.");
        }
    }
    if (candidate.stall_timeout_ms < 500 || candidate.stall_timeout_ms > 60000) {
        return Result<void>::Err("ERR_SETTINGS_STALL_TIMEOUT", "Segment stall timeout must be between 500 ms and 60,000 ms.");
    }
    if (candidate.min_split_bytes < 65536ULL || candidate.min_split_bytes > 10485760ULL) {
        return Result<void>::Err("ERR_SETTINGS_MIN_SPLIT", "Minimum segment split size must be between 64 KiB and 10 MiB.");
    }
    if (candidate.min_capture_size_bytes > 10737418240ULL) {
        return Result<void>::Err("ERR_SETTINGS_CAPTURE_SIZE", "Minimum browser capture size cannot exceed 10 GiB.");
    }
    if (candidate.proxy_mode != "direct" && candidate.proxy_mode != "system" && candidate.proxy_mode != "manual") {
        return Result<void>::Err("ERR_SETTINGS_PROXY_MODE", "Proxy mode must be direct, system, or manual.");
    }
    if (candidate.proxy_mode == "manual" && candidate.proxy_server.empty()) {
        return Result<void>::Err("ERR_SETTINGS_PROXY_SERVER", "Enter a manual proxy host and port before selecting Manual proxy mode.");
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::SetDraftSettingByKey(std::string_view key, std::string_view value_str) {
    auto parse_u64 = [](std::string_view sv, uint64_t& out) -> bool {
        if (sv.empty()) return false;
        std::string s(sv);
        char* endp = nullptr;
        long long v = std::strtoll(s.c_str(), &endp, 10);
        if (endp == s.c_str() || *endp != '\0' || v < 0) return false;
        out = static_cast<uint64_t>(v);
        return true;
    };
    auto parse_bool = [](std::string_view sv) -> bool {
        std::string l = ToLower(sv);
        return l == "true" || l == "1" || l == "yes" || l == "on" || l == "enabled";
    };

    m_settings_validation_key.clear();
    m_settings_validation_error.clear();

    if (key == "start_minimized") {
        m_draft_settings.start_minimized = parse_bool(value_str);
    } else if (key == "close_to_tray") {
        m_draft_settings.close_to_tray = parse_bool(value_str);
    } else if (key == "reduced_motion") {
        m_draft_settings.reduced_motion = parse_bool(value_str);
    } else if (key == "theme") {
        if (value_str != "system" && value_str != "light" && value_str != "dark") {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Choose System, Light, or Dark appearance.";
            return Result<void>::Err("ERR_SETTINGS_THEME", m_settings_validation_error);
        }
        m_draft_settings.theme = std::string(value_str);
    } else if (key == "language") {
        if (LocalizationManager::NormalizeLocale(value_str) != value_str) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Choose one of the supported interface languages.";
            return Result<void>::Err("ERR_SETTINGS_LANGUAGE", m_settings_validation_error);
        }
        m_draft_settings.language = std::string(value_str);
    } else if (key == "max_concurrent_tasks") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 1 || v > 16) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Simultaneous downloads must be an integer between 1 and 16.";
            return Result<void>::Err("ERR_SETTINGS_CONCURRENT_RANGE", m_settings_validation_error);
        }
        m_draft_settings.max_concurrent_tasks = static_cast<uint32_t>(v);
    } else if (key == "auto_start_downloads") {
        m_draft_settings.auto_start_downloads = parse_bool(value_str);
    } else if (key == "conflict_policy") {
        if (value_str != "prompt" && value_str != "auto_rename" && value_str != "overwrite" && value_str != "fail_if_exists") {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Choose a valid existing file conflict policy.";
            return Result<void>::Err("ERR_SETTINGS_CONFLICT_POLICY", m_settings_validation_error);
        }
        m_draft_settings.conflict_policy = ConflictPolicyFromString(std::string(value_str));
    } else if (key == "default_download_dir") {
        if (value_str.empty()) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Default download directory cannot be empty.";
            return Result<void>::Err("ERR_SETTINGS_EMPTY_SAVE_DIR", m_settings_validation_error);
        }
        for (char c : value_str) {
            if (static_cast<unsigned char>(c) < 0x20) {
                m_settings_validation_key = std::string(key);
                m_settings_validation_error = "Default download directory contains invalid control characters.";
                return Result<void>::Err("ERR_SETTINGS_INVALID_SAVE_DIR", m_settings_validation_error);
            }
        }
        m_draft_settings.default_download_dir = std::string(value_str);
    } else if (key == "default_connections_per_task") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 1 || v > 32) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Default connections per task must be between 1 and 32.";
            return Result<void>::Err("ERR_SETTINGS_CONNECTIONS_RANGE", m_settings_validation_error);
        }
        m_draft_settings.default_connections_per_task = static_cast<uint32_t>(v);
    } else if (key == "speed_limit_bps") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v)) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Global speed limit must be a non-negative byte/s rate (0 = Unlimited).";
            return Result<void>::Err("ERR_SETTINGS_SPEED_LIMIT", m_settings_validation_error);
        }
        m_draft_settings.speed_limit_bps = v;
    } else if (key == "stall_timeout_ms") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 500 || v > 60000) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Segment stall timeout must be between 500 ms and 60,000 ms.";
            return Result<void>::Err("ERR_SETTINGS_STALL_TIMEOUT", m_settings_validation_error);
        }
        m_draft_settings.stall_timeout_ms = static_cast<int>(v);
    } else if (key == "min_split_bytes") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 65536ULL || v > 10485760ULL) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Minimum segment split size must be between 65,536 B (64 KiB) and 10,485,760 B (10 MiB).";
            return Result<void>::Err("ERR_SETTINGS_MIN_SPLIT", m_settings_validation_error);
        }
        m_draft_settings.min_split_bytes = v;
    } else if (key == "browser_capture_enabled") {
        m_draft_settings.browser_capture_enabled = parse_bool(value_str);
    } else if (key == "min_capture_size_bytes") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v > 10737418240ULL) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Minimum automatic capture size must be between 0 B and 10 GiB.";
            return Result<void>::Err("ERR_SETTINGS_CAPTURE_SIZE", m_settings_validation_error);
        }
        m_draft_settings.min_capture_size_bytes = v;
    } else if (key == "show_completion_notification") {
        m_draft_settings.show_completion_notification = parse_bool(value_str);
    } else if (key == "proxy_mode") {
        if (value_str != "direct" && value_str != "system" && value_str != "manual") {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Choose Direct, Windows System Proxy, or Manual Proxy.";
            return Result<void>::Err("ERR_SETTINGS_PROXY_MODE", m_settings_validation_error);
        }
        m_draft_settings.proxy_mode = std::string(value_str);
    } else if (key == "proxy_server") {
        for (char c : value_str) {
            if (static_cast<unsigned char>(c) < 0x20) {
                m_settings_validation_key = std::string(key);
                m_settings_validation_error = "Manual proxy address contains invalid control characters.";
                return Result<void>::Err("ERR_SETTINGS_PROXY_SERVER", m_settings_validation_error);
            }
        }
        m_draft_settings.proxy_server = std::string(value_str);
    } else if (key == "max_auto_retries") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v > 50) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Maximum automatic retries must be between 0 and 50.";
            return Result<void>::Err("ERR_SETTINGS_RETRIES_RANGE", m_settings_validation_error);
        }
        m_draft_settings.max_auto_retries = static_cast<uint32_t>(v);
    } else if (key == "retry_interval_sec") {
        uint64_t v = 0;
        if (!parse_u64(value_str, v) || v < 1 || v > 600) {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Retry interval must be between 1 and 600 seconds.";
            return Result<void>::Err("ERR_SETTINGS_RETRY_INTERVAL", m_settings_validation_error);
        }
        m_draft_settings.retry_interval_sec = static_cast<uint32_t>(v);
    } else if (key == "queue_completion_action") {
        if (value_str != "NONE" && value_str != "SLEEP" && value_str != "SHUTDOWN") {
            m_settings_validation_key = std::string(key);
            m_settings_validation_error = "Queue post-completion action must be NONE, SLEEP, or SHUTDOWN.";
            return Result<void>::Err("ERR_SETTINGS_COMPLETION_ACTION", m_settings_validation_error);
        }
        m_draft_settings.queue_completion_action = std::string(value_str);
    } else {
        return Result<void>::Err("ERR_SETTINGS_UNKNOWN_KEY", "Unknown setting key: " + std::string(key));
    }

    return Result<void>::Ok();
}

Result<void> DesktopView::CycleOrToggleDraftSetting(std::string_view key) {
    if (key == "start_minimized") {
        m_draft_settings.start_minimized = !m_draft_settings.start_minimized;
    } else if (key == "close_to_tray") {
        m_draft_settings.close_to_tray = !m_draft_settings.close_to_tray;
    } else if (key == "reduced_motion") {
        m_draft_settings.reduced_motion = !m_draft_settings.reduced_motion;
    } else if (key == "max_concurrent_tasks") {
        uint32_t next = (m_draft_settings.max_concurrent_tasks >= 16) ? 1 : (m_draft_settings.max_concurrent_tasks + 1);
        m_draft_settings.max_concurrent_tasks = next;
    } else if (key == "auto_start_downloads") {
        m_draft_settings.auto_start_downloads = !m_draft_settings.auto_start_downloads;
    } else if (key == "conflict_policy") {
        if (m_draft_settings.conflict_policy == ConflictPolicy::kAutoRename) {
            m_draft_settings.conflict_policy = ConflictPolicy::kOverwrite;
        } else if (m_draft_settings.conflict_policy == ConflictPolicy::kOverwrite) {
            m_draft_settings.conflict_policy = ConflictPolicy::kFailIfExists;
        } else if (m_draft_settings.conflict_policy == ConflictPolicy::kFailIfExists) {
            m_draft_settings.conflict_policy = ConflictPolicy::kPrompt;
        } else {
            m_draft_settings.conflict_policy = ConflictPolicy::kAutoRename;
        }
    } else if (key == "default_connections_per_task") {
        const uint32_t presets[] = {1, 2, 4, 8, 16, 24, 32};
        uint32_t next = 8;
        for (size_t i = 0; i < 7; ++i) {
            if (m_draft_settings.default_connections_per_task == presets[i]) {
                next = presets[(i + 1) % 7];
                break;
            }
        }
        m_draft_settings.default_connections_per_task = next;
    } else if (key == "speed_limit_bps") {
        const uint64_t presets[] = {0ULL, 1048576ULL, 5242880ULL, 10485760ULL, 26214400ULL, 52428800ULL};
        uint64_t next = 0ULL;
        for (size_t i = 0; i < 6; ++i) {
            if (m_draft_settings.speed_limit_bps == presets[i]) {
                next = presets[(i + 1) % 6];
                break;
            }
        }
        m_draft_settings.speed_limit_bps = next;
    } else if (key == "stall_timeout_ms") {
        const int presets[] = {1500, 3000, 5000, 10000, 15000};
        int next = 3000;
        for (size_t i = 0; i < 5; ++i) {
            if (m_draft_settings.stall_timeout_ms == presets[i]) {
                next = presets[(i + 1) % 5];
                break;
            }
        }
        m_draft_settings.stall_timeout_ms = next;
    } else if (key == "min_split_bytes") {
        const uint64_t presets[] = {65536ULL, 131072ULL, 262144ULL, 524288ULL, 1048576ULL};
        uint64_t next = 131072ULL;
        for (size_t i = 0; i < 5; ++i) {
            if (m_draft_settings.min_split_bytes == presets[i]) {
                next = presets[(i + 1) % 5];
                break;
            }
        }
        m_draft_settings.min_split_bytes = next;
    } else if (key == "browser_capture_enabled") {
        m_draft_settings.browser_capture_enabled = !m_draft_settings.browser_capture_enabled;
    } else if (key == "min_capture_size_bytes") {
        const uint64_t presets[] = {0ULL, 262144ULL, 524288ULL, 1048576ULL, 5242880ULL, 10485760ULL};
        uint64_t next = 1048576ULL;
        for (size_t i = 0; i < 6; ++i) {
            if (m_draft_settings.min_capture_size_bytes == presets[i]) {
                next = presets[(i + 1) % 6];
                break;
            }
        }
        m_draft_settings.min_capture_size_bytes = next;
    } else if (key == "show_completion_notification") {
        m_draft_settings.show_completion_notification = !m_draft_settings.show_completion_notification;
    } else if (key == "proxy_mode") {
        if (m_draft_settings.proxy_mode == "direct") {
            m_draft_settings.proxy_mode = "system";
        } else if (m_draft_settings.proxy_mode == "system") {
            m_draft_settings.proxy_mode = "manual";
        } else {
            m_draft_settings.proxy_mode = "direct";
        }
    } else if (key == "queue_completion_action") {
        if (m_draft_settings.queue_completion_action == "NONE") {
            m_draft_settings.queue_completion_action = "SLEEP";
        } else if (m_draft_settings.queue_completion_action == "SLEEP") {
            m_draft_settings.queue_completion_action = "SHUTDOWN";
        } else {
            m_draft_settings.queue_completion_action = "NONE";
        }
    } else if (key == "max_auto_retries") {
        uint32_t next = (m_draft_settings.max_auto_retries >= 15) ? 0 : (m_draft_settings.max_auto_retries + 3);
        m_draft_settings.max_auto_retries = next;
    } else if (key == "retry_interval_sec") {
        const uint32_t presets[] = {5, 10, 15, 30, 60};
        uint32_t next = 15;
        for (size_t i = 0; i < 5; ++i) {
            if (m_draft_settings.retry_interval_sec == presets[i]) {
                next = presets[(i + 1) % 5];
                break;
            }
        }
        m_draft_settings.retry_interval_sec = next;
    }
    m_settings_validation_key.clear();
    m_settings_validation_error.clear();
    return Result<void>::Ok();
}

Result<void> DesktopView::ApplySettingsChanges() {
    if (!m_settings_validation_error.empty()) {
        return Result<void>::Err("ERR_SETTINGS_INVALID", m_settings_validation_error);
    }
    auto val = ValidateSettingsInput(m_draft_settings);
    if (!val.ok()) {
        m_settings_validation_error = val.error->message;
        return val;
    }
    return ActionSaveSettings(m_draft_settings);
}

void DesktopView::RevertSettingsChanges() {
    m_draft_settings = m_controller.GetSettings();
    GetLocalizationManager().SetLocale(m_draft_settings.language);
#ifdef _WIN32
    if (m_hwnd) ApplyNativeWindowTheme(m_hwnd, m_palette);
#endif
    m_settings_validation_key.clear();
    m_settings_validation_error.clear();
    m_last_feedback_message = "Reverted unsaved settings changes";
}

std::vector<SettingItemView> DesktopView::BuildAllSettingItems() const {
    const auto saved = m_controller.GetSettings();
    const auto& s = m_draft_settings;
    std::string def_dir = PathSanitizer::GetDefaultDownloadsDir(PathSanitizer::GetExecutableDir()).string();

    std::vector<SettingItemView> items;
    auto add_item = [&](
        std::string key,
        SettingsCategory cat,
        std::string group,
        std::string label,
        std::string desc,
        SettingControlType ctrl,
        std::string val_txt,
        std::string def_txt,
        bool bool_val,
        bool modified,
        bool adv = false) {
        SettingItemView it;
        it.key = std::move(key);
        it.category = cat;
        it.group_title = GetLocalizationManager().Translate(group);
        it.label = GetLocalizationManager().Translate(label);
        it.description = GetLocalizationManager().Translate(desc);
        it.control_type = ctrl;
        it.value_text = GetLocalizationManager().Translate(val_txt);
        it.default_value_text = GetLocalizationManager().Translate(def_txt);
        it.bool_value = bool_val;
        it.is_modified = modified;
        it.is_advanced = adv;
        if (it.key == m_settings_validation_key) {
            it.validation_error = m_settings_validation_error;
        }
        items.push_back(std::move(it));
    };

    // 1. General
    add_item(
        "start_minimized", SettingsCategory::kGeneral, "Startup & Window Behavior",
        "Start Minimised to System Tray",
        "Launch Radium quietly in the system tray without opening the main window on startup.",
        SettingControlType::kToggleSwitch,
        s.start_minimized ? "Enabled" : "Disabled", "Disabled",
        s.start_minimized, s.start_minimized != saved.start_minimized);
    add_item(
        "close_to_tray", SettingsCategory::kGeneral, "Startup & Window Behavior",
        "Minimize to System Tray on Close",
        "Keep active downloads and browser IPC listening in the tray when closing the main window.",
        SettingControlType::kToggleSwitch,
        s.close_to_tray ? "Enabled" : "Disabled", "Enabled",
        s.close_to_tray, s.close_to_tray != saved.close_to_tray);
    add_item(
        "reduced_motion", SettingsCategory::kGeneral, "Interface & Accessibility",
        "Reduce UI Animations",
        "Disable smooth interface transitions and progress animations for accessibility.",
        SettingControlType::kToggleSwitch,
        s.reduced_motion ? "Reduced Motion" : "Standard Animations", "Standard Animations",
        s.reduced_motion, s.reduced_motion != saved.reduced_motion);
    const std::string theme_label = s.theme == "system" ? "System" : (s.theme == "light" ? "Light" : "Dark");
    add_item("theme", SettingsCategory::kGeneral, "Interface & Accessibility",
        "Appearance", "Choose System to follow Windows automatically, or select Light or Dark.",
        SettingControlType::kDropdown, theme_label, "System", false, s.theme != saved.theme);

    // 2. Downloads & Queue
    add_item(
        "max_concurrent_tasks", SettingsCategory::kDownloads, "Queue Concurrency",
        "Maximum Simultaneous Downloads",
        "Active HTTP and stream transfers allowed to run concurrently in the main queue (1–16).",
        SettingControlType::kDropdown,
        std::to_string(s.max_concurrent_tasks) + " tasks", "3 tasks",
        false, s.max_concurrent_tasks != saved.max_concurrent_tasks);
    add_item(
        "auto_start_downloads", SettingsCategory::kDownloads, "Default Task Behavior",
        "Start New Downloads Immediately",
        "Automatically begin transfer when a download or media stream is added from browser or clipboard.",
        SettingControlType::kToggleSwitch,
        s.auto_start_downloads ? "Enabled" : "Queued Paused", "Enabled",
        s.auto_start_downloads, s.auto_start_downloads != saved.auto_start_downloads);
    add_item(
        "conflict_policy", SettingsCategory::kDownloads, "Default Task Behavior",
        "Existing File Conflict Resolution",
        "Action taken when a completed download matches an existing filename in the destination folder.",
        SettingControlType::kDropdown,
        FormatConflictPolicyLabel(s.conflict_policy), FormatConflictPolicyLabel(ConflictPolicy::kPrompt),
        false, s.conflict_policy != saved.conflict_policy);
    add_item(
        "max_auto_retries", SettingsCategory::kDownloads, "Automation & Recovery",
        "Max Automatic Retries",
        "Number of automatic retries on transient network errors before marking failed (0–50).",
        SettingControlType::kDropdown,
        std::to_string(s.max_auto_retries) + " retries", "5 retries",
        false, s.max_auto_retries != saved.max_auto_retries);
    add_item(
        "retry_interval_sec", SettingsCategory::kDownloads, "Automation & Recovery",
        "Base Retry Interval",
        "Initial wait interval in seconds before retry attempt (with exponential backoff).",
        SettingControlType::kDropdown,
        std::to_string(s.retry_interval_sec) + " seconds", "15 seconds",
        false, s.retry_interval_sec != saved.retry_interval_sec);
    add_item(
        "queue_completion_action", SettingsCategory::kDownloads, "Automation & Recovery",
        "Queue Post-Completion Action",
        "System action taken when all active transfers finish (NONE, SLEEP, or SHUTDOWN).",
        SettingControlType::kDropdown,
        s.queue_completion_action, "NONE",
        false, s.queue_completion_action != saved.queue_completion_action);

    // 3. Save Locations
    add_item(
        "default_download_dir", SettingsCategory::kSaveLocations, "Destination Folders",
        "Default Download Directory",
        "Base directory where completed files and muxed media containers are saved.",
        SettingControlType::kFolderSelector,
        s.default_download_dir, def_dir,
        false, s.default_download_dir != saved.default_download_dir);
    if (!s.site_rules.empty()) {
        add_item(
            "site_rules_count", SettingsCategory::kSaveLocations, "Site-Specific Rules",
            "Configured Domain Routing Rules",
            std::to_string(s.site_rules.size()) + " domain classification rules active in SQLite persistence store.",
            SettingControlType::kDiagnosticReadOnly,
            std::to_string(s.site_rules.size()) + " rules", "0 rules",
            false, false);
    }

    // 4. Connections & Speed Limits
    add_item(
        "default_connections_per_task", SettingsCategory::kConnectionsAndSpeed, "Segmented HTTP Engine",
        "Default Connections per Task",
        "Maximum dynamic work-stealing HTTP range connections spawned per file transfer (1–32).",
        SettingControlType::kDropdown,
        std::to_string(s.default_connections_per_task) + " connections", "8 connections",
        false, s.default_connections_per_task != saved.default_connections_per_task);
    add_item(
        "speed_limit_bps", SettingsCategory::kConnectionsAndSpeed, "Bandwidth Throttling",
        "Global Download Speed Limit",
        "Cap aggregate transfer rate across active downloads (0 = Unlimited).",
        SettingControlType::kDropdown,
        FormatBandwidthLimit(s.speed_limit_bps), "Unlimited",
        false, s.speed_limit_bps != saved.speed_limit_bps);
    add_item(
        "stall_timeout_ms", SettingsCategory::kConnectionsAndSpeed, "Advanced Transfer Tuning",
        "Segment Stall Timeout",
        "Inactivity threshold before resetting a stalled HTTP range connection (500–60,000 ms).",
        SettingControlType::kDropdown,
        std::to_string(s.stall_timeout_ms) + " ms", "3000 ms",
        false, s.stall_timeout_ms != saved.stall_timeout_ms, true);
    add_item(
        "min_split_bytes", SettingsCategory::kConnectionsAndSpeed, "Advanced Transfer Tuning",
        "Minimum Work-Stealing Split Size",
        "Smallest remaining byte range eligible for dynamic segment splitting (64 KiB – 10 MiB).",
        SettingControlType::kDropdown,
        FormatBytes(s.min_split_bytes), "128.0 KiB",
        false, s.min_split_bytes != saved.min_split_bytes, true);
    add_item(
        "proxy_mode", SettingsCategory::kConnectionsAndSpeed, "Proxy Configuration",
        "Proxy Connection Mode",
        "Network proxy mode for HTTP/HTTPS transfers (direct, system, manual).",
        SettingControlType::kDropdown,
        s.proxy_mode, "system",
        false, s.proxy_mode != saved.proxy_mode);
    add_item(
        "proxy_server", SettingsCategory::kConnectionsAndSpeed, "Proxy Configuration",
        "Manual Proxy Server",
        "Click to edit the HTTP(S) proxy host and port used by the Manual proxy mode.",
        SettingControlType::kActionButton,
        s.proxy_server.empty() ? "Set proxy" : s.proxy_server, "Not configured",
        false, s.proxy_server != saved.proxy_server);
    add_item(
        "proxy_bypass", SettingsCategory::kConnectionsAndSpeed, "Proxy Configuration",
        "Proxy Bypass List (diagnostic)",
        "Stored bypass pattern passed to the HTTP client; this value is read-only in Settings.",
        SettingControlType::kDiagnosticReadOnly,
        s.proxy_bypass.empty() ? "<local>" : s.proxy_bypass, "<local>",
        false, s.proxy_bypass != saved.proxy_bypass);
    add_item(
        "server_auth_username", SettingsCategory::kConnectionsAndSpeed, "Server & Proxy Authentication",
        "Configured HTTP Auth User (diagnostic)",
        "Stored default credential identity passed to HTTP requests; credential editing is not available here.",
        SettingControlType::kDiagnosticReadOnly,
        s.server_auth_username.empty() ? "(None)" : s.server_auth_username, "(None)",
        false, s.server_auth_username != saved.server_auth_username);

    // 5. Browser Integration
    add_item(
        "browser_capture_enabled", SettingsCategory::kBrowserIntegration, "Extension Capture & Interception",
        "Enable Browser Download & Media Capture",
        "Accept file downloads and HLS/DASH streams from the Radium Browser Extension (hold Alt when clicking a link to bypass).",
        SettingControlType::kToggleSwitch,
        s.browser_capture_enabled ? "Enabled" : "Paused", "Enabled",
        s.browser_capture_enabled, s.browser_capture_enabled != saved.browser_capture_enabled);
    add_item(
        "min_capture_size_bytes", SettingsCategory::kBrowserIntegration, "Extension Capture & Interception",
        "Minimum Automatic File Capture Size",
        "Minimum size threshold for size-based fallback download interception.",
        SettingControlType::kDropdown,
        FormatBytes(s.min_capture_size_bytes), "1.0 MiB",
        false, s.min_capture_size_bytes != saved.min_capture_size_bytes);
    auto bstat = GetBrowserIntegrationStatus();
    const auto ext_onboarding = m_controller.GetExtensionOnboardingState();
    const auto ext_dir = radium::core::ExtensionInstaller::DefaultInstallDir(
        m_controller.GetConfig().state_db_path.parent_path());
    const auto ext_info = radium::core::ExtensionInstaller::InspectInstalledExtension(
        ext_dir, ext_onboarding.installed_commit);
    std::string ext_desc = ext_info.installed
        ? ("Installed v" + ext_info.version + " at: " + ext_info.install_dir.string())
        : ("Download the latest Radium browser extension from GitHub (" +
           std::string(radium::config::UpdateAndReleaseConfig::kGithubRepoUrl) + ").");
    add_item(
        "download_browser_extension", SettingsCategory::kBrowserIntegration, "Browser Extension Package",
        "Download or Update Browser Extension",
        ext_desc,
        SettingControlType::kActionButton,
        ext_info.installed ? ("Update (v" + ext_info.version + ")") : "Download", "",
        ext_info.installed, false);
    add_item(
        "ipc_endpoint_status", SettingsCategory::kBrowserIntegration, "Native Messaging & IPC Status",
        "Local IPC Endpoint (" + std::string(bstat.ipc_server_running ? "Listening" : "Offline") + ")",
        "Host ID: com.radium.downloadmanager  |  Endpoint: " + bstat.ipc_endpoint,
        SettingControlType::kDiagnosticReadOnly,
        bstat.ipc_server_running ? "Connected" : "Standby", "Connected",
        bstat.ipc_server_running, false);

    // 7. Notifications
    add_item(
        "show_completion_notification", SettingsCategory::kNotifications, "System Tray Notifications",
        "Show Completion & Error Notifications",
        "Display a Windows system tray notification when a background download completes or fails.",
        SettingControlType::kToggleSwitch,
        s.show_completion_notification ? "Enabled" : "Muted", "Enabled",
        s.show_completion_notification, s.show_completion_notification != saved.show_completion_notification);

    // 8. Keyboard Shortcuts (read-only; shortcuts are not customizable)
    const auto add_shortcut = [&](std::string key, std::string action, std::string chord) {
        add_item(
            std::move(key), SettingsCategory::kKeyboardShortcuts, "Keyboard Shortcuts",
            std::move(action), "Supported application shortcut.",
            SettingControlType::kDiagnosticReadOnly, std::move(chord), "", false, false);
    };
    add_shortcut("shortcut_add_download", "Add a download", "Ctrl+N");
    add_shortcut("shortcut_open_settings", "Open Settings", "Ctrl+,");
    add_shortcut("shortcut_focus_search", "Focus download search", "Ctrl+K or Ctrl+F");
    add_shortcut("shortcut_pause_resume", "Pause or resume the selected task", "Space");
    add_shortcut("shortcut_retry", "Retry the selected task", "F5");
    add_shortcut("shortcut_open_file", "Open the selected completed file", "Enter");
    add_shortcut("shortcut_remove_task", "Open task removal confirmation", "Delete");
    add_shortcut("shortcut_close_settings", "Close Settings Center", "Esc");
    add_shortcut("shortcut_select_rows", "Move selection between tasks", "Up / Down");
    add_shortcut("shortcut_select_edges", "Select first or last task", "Home / End");

    // 9. About & Diagnostics
    auto ab = GetAboutDialogView();
    add_item(
        "diag_app_summary", SettingsCategory::kAboutAndDiagnostics, "About Radium",
        "Radium Download Manager",
        "Fast, clean download manager and browser video grabber for Windows 10 / 11. Multi-connection acceleration with zero bloat.",
        SettingControlType::kDiagnosticReadOnly,
        "v" + ab.version, "v" + ab.version, true, false);
    add_item(
        "diag_video_fetch", SettingsCategory::kAboutAndDiagnostics, "Video & Stream Fetching",
        "Browser Video Sniffer & Stream Pipeline",
        "Automatically sniffs video streams (HLS m3u8, DASH mpd, and web videos) via the browser extension and muxes quality streams with N_m3u8DL-RE & FFmpeg.",
        SettingControlType::kDiagnosticReadOnly,
        (bstat.nm3u8dl_available && bstat.ffmpeg_available) ? "Active" : "Workers Needed", "Active",
        bstat.nm3u8dl_available && bstat.ffmpeg_available, false);
    add_item(
        "diag_version", SettingsCategory::kAboutAndDiagnostics, "System & License",
        "Platform & License",
        "Windows 10 / 11 x64 (Native Win32 C++20)  |  " + ab.license,
        SettingControlType::kDiagnosticReadOnly,
        "Active", "Active", true, false);
    if (security::IsCurrentUserLocalAdmin()) {
        add_item(
            "admin_startup_log", SettingsCategory::kAboutAndDiagnostics, "Administrator Diagnostics",
            "Download Startup Log & Performance Report",
            "High-resolution millisecond lifecycle log tracking every stage from click to first byte transfer.",
            SettingControlType::kActionButton,
            "Open Log", "", false, false);
    }
    add_item(
        "diag_storage", SettingsCategory::kAboutAndDiagnostics, "System & License",
        "Queue Database",
        "SQLite WAL active at: " + m_controller.GetConfig().state_db_path.string(),
        SettingControlType::kDiagnosticReadOnly,
        "WAL Active", "WAL Active", true, false);
    add_item(
        "check_for_updates", SettingsCategory::kAboutAndDiagnostics, "Application Updates",
        "Check for Updates",
        m_update_check_in_progress
            ? "Checking GitHub for the latest release..."
            : "Query the latest stable GitHub Release and compare it with this installed version.",
        SettingControlType::kActionButton,
        m_update_check_in_progress ? "Checking..." : "Check now", "", false, false);
    add_item(
        "diag_developer", SettingsCategory::kAboutAndDiagnostics, "Developer",
        "Developer",
        ab.developer_name + " and Radium Contributors",
        SettingControlType::kDiagnosticReadOnly,
        ab.developer_name, ab.developer_name, true, false);
    add_item(
        "open_developer_github", SettingsCategory::kAboutAndDiagnostics, "Developer",
        "Developer GitHub Profile",
        ab.developer_github_url,
        SettingControlType::kActionButton,
        "Open Profile", "", false, false);
    add_item(
        "open_github_repo", SettingsCategory::kAboutAndDiagnostics, "Developer",
        "Radium Download Manager Repository",
        ab.repository_url,
        SettingControlType::kActionButton,
        "Open GitHub", "", false, false);

    return items;
}

SettingsCenterViewModel DesktopView::BuildSettingsCenterView() const {
    SettingsCenterViewModel vm;
    vm.is_open = m_settings_center_open;
    vm.active_category = m_settings_category;
    vm.search_query = m_settings_search_query;
    vm.has_unsaved_changes = HasUnsavedSettingsChanges();
    auto val_res = ValidateSettingsInput(m_draft_settings);
    vm.is_valid = m_settings_validation_error.empty() && val_res.ok();
    vm.validation_error = !m_settings_validation_error.empty()
        ? m_settings_validation_error
        : (val_res.ok() ? "" : val_res.error->message);
    vm.browser_status = GetBrowserIntegrationStatus();
    vm.about_info = GetAboutDialogView();

    if (!vm.is_valid) {
        vm.status_banner = "Validation Error: " + vm.validation_error;
    } else if (vm.has_unsaved_changes) {
        vm.status_banner = "Unsaved changes — Click 'Apply & Save' to persist or 'Cancel' to revert.";
    } else {
        vm.status_banner = "All configuration changes saved to SQLite state store.";
    }

    const auto& m = m_metrics;
    int margin_x = DesignSystem::ScaleForDpi(44, m.dpi);
    int margin_y = DesignSystem::ScaleForDpi(36, m.dpi);
    vm.modal_rect = {margin_x, margin_y, m_window_width - margin_x, m_window_height - margin_y};
    int side_w = DesignSystem::ScaleForDpi(210, m.dpi);
    int header_h = DesignSystem::ScaleForDpi(52, m.dpi);
    int footer_h = DesignSystem::ScaleForDpi(52, m.dpi);

    vm.sidebar_rect = {vm.modal_rect.left, vm.modal_rect.top + header_h, vm.modal_rect.left + side_w, vm.modal_rect.bottom - footer_h};
    vm.content_rect = {vm.sidebar_rect.right + 1, vm.modal_rect.top + header_h, vm.modal_rect.right, vm.modal_rect.bottom - footer_h};
    vm.search_box_rect = {
        vm.modal_rect.right - DesignSystem::ScaleForDpi(240, m.dpi) - m.space_md,
        vm.modal_rect.top + (header_h - m.button_height) / 2,
        vm.modal_rect.right - m.space_md,
        vm.modal_rect.top + (header_h + m.button_height) / 2
    };

    int footer_btn_y = vm.modal_rect.bottom - (footer_h + m.button_height) / 2;
    int restore_w = DesignSystem::ScaleForDpi(140, m.dpi);
    int cancel_w = DesignSystem::ScaleForDpi(96, m.dpi);
    int apply_w = DesignSystem::ScaleForDpi(124, m.dpi);

    vm.restore_defaults_rect = {
        vm.modal_rect.left + m.space_md,
        footer_btn_y,
        vm.modal_rect.left + m.space_md + restore_w,
        footer_btn_y + m.button_height
    };
    vm.apply_button_rect = {
        vm.modal_rect.right - m.space_md - apply_w,
        footer_btn_y,
        vm.modal_rect.right - m.space_md,
        footer_btn_y + m.button_height
    };
    vm.cancel_button_rect = {
        vm.apply_button_rect.left - m.space_sm - cancel_w,
        footer_btn_y,
        vm.apply_button_rect.left - m.space_sm,
        footer_btn_y + m.button_height
    };

    auto all_items = BuildAllSettingItems();
    const SettingsCategory all_cats[] = {
        SettingsCategory::kGeneral,
        SettingsCategory::kDownloads,
        SettingsCategory::kSaveLocations,
        SettingsCategory::kConnectionsAndSpeed,
        SettingsCategory::kBrowserIntegration,
        SettingsCategory::kNotifications,
        SettingsCategory::kAboutAndDiagnostics,
        SettingsCategory::kKeyboardShortcuts
    };

    int tab_y = vm.sidebar_rect.top + DesignSystem::ScaleForDpi(8, m.dpi);
    int tab_h = DesignSystem::ScaleForDpi(36, m.dpi);
    int tab_spacing = DesignSystem::ScaleForDpi(4, m.dpi);
    for (SettingsCategory c : all_cats) {
        size_t cnt = 0;
        size_t mod_cnt = 0;
        for (const auto& it : all_items) {
            if (it.category == c) {
                ++cnt;
                if (it.is_modified) ++mod_cnt;
            }
        }
        RectI trc{vm.sidebar_rect.left + DesignSystem::ScaleForDpi(10, m.dpi), tab_y, vm.sidebar_rect.right - DesignSystem::ScaleForDpi(10, m.dpi), tab_y + tab_h};
        vm.categories.push_back({c, SettingsCategoryToLabel(c), cnt, mod_cnt, c == m_settings_category, trc});
        tab_y += tab_h + tab_spacing;
    }

    std::string q = ToLower(m_settings_search_query);
    int card_pad = DesignSystem::ScaleForDpi(14, m.dpi);
    int top_pad = card_pad + DesignSystem::ScaleForDpi(4, m.dpi);
    int bottom_pad = card_pad + DesignSystem::ScaleForDpi(4, m.dpi);
    int row_h = DesignSystem::ScaleForDpi(58, m.dpi);
    int ctrl_w = DesignSystem::ScaleForDpi(160, m.dpi);
    int ctrl_h = DesignSystem::ScaleForDpi(28, m.dpi);

    std::vector<SettingItemView> matched_items;
    matched_items.reserve(all_items.size());
    for (auto& it : all_items) {
        bool matches = false;
        if (!q.empty()) {
            matches = ToLower(it.label).find(q) != std::string::npos ||
                      ToLower(it.description).find(q) != std::string::npos ||
                      ToLower(it.group_title).find(q) != std::string::npos ||
                      ToLower(it.key).find(q) != std::string::npos;
        } else {
            matches = (it.category == m_settings_category);
        }
        if (matches) {
            matched_items.push_back(std::move(it));
        }
    }

    int viewport_h = std::max(1, vm.content_rect.height());
    vm.content_total_height = matched_items.empty()
        ? 0
        : (top_pad + static_cast<int>(matched_items.size()) * row_h + bottom_pad);
    vm.max_scroll = std::max(0, vm.content_total_height - viewport_h);
    vm.scroll_offset = std::clamp(m_settings_scroll_offset, 0, vm.max_scroll);

    int row_y = vm.content_rect.top + top_pad - vm.scroll_offset;
    for (auto& it : matched_items) {
        it.row_rect = {vm.content_rect.left + card_pad, row_y, vm.content_rect.right - card_pad, row_y + row_h};
        it.control_rect = {
            it.row_rect.right - ctrl_w - DesignSystem::ScaleForDpi(16, m.dpi),
            row_y + (row_h - ctrl_h) / 2,
            it.row_rect.right - DesignSystem::ScaleForDpi(16, m.dpi),
            row_y + (row_h + ctrl_h) / 2
        };
        vm.visible_items.push_back(std::move(it));
        row_y += row_h;
    }

    return vm;
}

// -----------------------------------------------------------------------------
// Dialog & Toolbar Action Controllers
// -----------------------------------------------------------------------------


Result<TaskModel> DesktopView::SubmitAddDownloadDialog(const AddDownloadDialogInput& input) {
    bool is_ftp = (input.url.find("ftp://") == 0);
    bool is_torrent = (input.url.find("magnet:?") == 0 || input.url.rfind(".torrent") != std::string::npos);
    bool is_rtsp_mms = (input.url.find("rtsp://") == 0 || input.url.find("mms://") == 0);

#ifdef _WIN32
    if (is_torrent && input.url.rfind(".torrent") != std::string::npos && m_hwnd && IsWindow(m_hwnd)) {
        std::error_code torrent_ec;
        if (std::filesystem::exists(input.url, torrent_ec)) {
            auto parsed_torrent = torrent::TorrentParser::ParseTorrentFile(input.url);
            if (parsed_torrent.ok()) {
                auto tinfo = std::move(*parsed_torrent.value);
                const auto saved_settings = m_controller.GetSettings();
                std::string target_save = input.save_dir.empty()
                    ? (saved_settings.last_download_dir.empty() ? saved_settings.default_download_dir : saved_settings.last_download_dir)
                    : input.save_dir;
                bool start_torrent = input.start_immediately;
                if (!ShowAddTorrentDialogWin32(m_hwnd, *this, m_controller, tinfo, input.url, target_save, start_torrent,
                                               &target_save, &start_torrent)) {
                    m_last_feedback_message = "Torrent file selection canceled";
                    return Result<TaskModel>::Err("ERR_TORRENT_SELECTION_CANCELED", "Torrent file selection canceled");
                }
                TaskModel t;
                t.task_type = TaskType::kTorrent;
                t.url = input.url;
                t.file_name = tinfo.name.empty() ? std::filesystem::path(input.url).filename().string() : tinfo.name;
                t.save_dir = target_save;
                t.total_bytes = tinfo.selected_bytes();
                for (const auto& file : tinfo.files) {
                    t.torrent_file_paths.push_back(file.relative_path.generic_string());
                    t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                }
                auto res = m_controller.AddTask(std::move(t), start_torrent, false);
                if (res.ok()) {
                    m_controller.RememberDownloadDirectory(target_save);
                    m_selected_task_id = res.value->task_id;
                    ScrollTaskIntoView(res.value->task_id, true);
                    m_last_feedback_message = "Added download: " + res.value->file_name;
                }
                return res;
            }
        }
    }
#endif

    if (!is_ftp && !is_torrent && !is_rtsp_mms) {
        auto parsed = brisk::HttpClient::ParseUrl(input.url);
        if (!parsed.ok()) {
            m_last_feedback_message = "Invalid URL: " + parsed.error->message;
            return Result<TaskModel>::Err(parsed.error->code, parsed.error->message);
        }
    }

    TaskModel t;
    t.url = input.url;
    t.file_name = input.custom_filename;
    t.save_dir = input.save_dir;
    t.max_connections = std::clamp(input.max_connections, 1u, 32u);
    t.scheduled = input.scheduled;
    t.schedule_time_local = input.schedule_time_local;

    bool is_youtube = (input.url.find("youtube.com") != std::string::npos || input.url.find("youtu.be") != std::string::npos);
    if (is_ftp) {
        t.task_type = TaskType::kFtp;
    } else if (is_torrent) {
        t.task_type = TaskType::kTorrent;
    } else if (is_rtsp_mms) {
        t.task_type = TaskType::kRtsp;
        t.category = "Video";
    } else if (input.is_stream || is_youtube || input.url.find(".m3u8") != std::string::npos) {
        t.task_type = TaskType::kStreamHls;
        t.category = "Video";
        t.stream_options.video_filter = input.video_filter.empty() ? "best" : input.video_filter;
        t.stream_options.audio_filter = input.audio_filter.empty() ? "best" : input.audio_filter;
        t.stream_options.subtitle_filter = input.subtitle_filter.empty() ? "all" : input.subtitle_filter;
        t.stream_options.output_format = (input.output_format == "mkv") ? "mkv" : "mp4";
        t.stream_options.embed_subtitles = input.embed_subtitles;
        t.stream_options.download_sections = input.download_sections;
        t.stream_options.force_keyframes_at_cuts = input.force_keyframes_at_cuts;
    } else if (input.url.find(".mpd") != std::string::npos) {
        t.task_type = TaskType::kStreamDash;
        t.category = "Video";
        t.stream_options.video_filter = input.video_filter.empty() ? "best" : input.video_filter;
        t.stream_options.audio_filter = input.audio_filter.empty() ? "best" : input.audio_filter;
        t.stream_options.subtitle_filter = input.subtitle_filter.empty() ? "all" : input.subtitle_filter;
        t.stream_options.output_format = (input.output_format == "mkv") ? "mkv" : "mp4";
        t.stream_options.embed_subtitles = input.embed_subtitles;
        t.stream_options.download_sections = input.download_sections;
        t.stream_options.force_keyframes_at_cuts = input.force_keyframes_at_cuts;
    } else {
        t.task_type = TaskType::kHttpFile;
    }

    if (t.file_name.empty()) {
        if (is_youtube) {
            t.file_name = "youtube_video.mp4";
        } else if (is_torrent && input.url.rfind(".torrent") != std::string::npos) {
            t.file_name = std::filesystem::path(input.url).filename().string();
        } else if (is_torrent) {
            t.file_name = "torrent_download";
        } else {
            auto derived = PathSanitizer::ExtractFromUrl(t.url);
            t.file_name = derived.empty() ? "download" : derived;
        }
    }

    if (input.total_bytes > 0) {
        t.total_bytes = input.total_bytes;
    }
    if (input.range_probed) {
        t.range_supported = input.range_supported;
    }

    const std::string selected_dir = t.save_dir;
    auto res = m_controller.AddTask(std::move(t), input.start_immediately, false);
    if (res.ok()) {
        const auto saved_dir = selected_dir.empty() ? res.value->save_dir : selected_dir;
        m_controller.RememberDownloadDirectory(saved_dir);
        m_selected_task_id = res.value->task_id;
        ScrollTaskIntoView(res.value->task_id, true);
        m_last_feedback_message = "Added download: " + res.value->file_name;
    } else {
        m_last_feedback_message = "Failed to add download: " + res.error->message;
    }
    return res;
}

Result<stream::StreamProbeResult> DesktopView::ProbeStreamForDialog(
    const std::string& manifest_url,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers) {
    auto res = m_controller.ProbeStream(manifest_url, headers, auth_headers);
    if (!res.ok()) {
        m_last_feedback_message = "Stream probe failed: " + res.error->message;
    } else {
        m_last_feedback_message = "Discovered " + std::to_string(res.value->tracks.size()) + " stream tracks";
    }
    return res;
}

Result<TaskModel> DesktopView::ConfirmStreamSelectionDialog(
    const stream::StreamProbeResult& probe_result,
    const std::string& page_title,
    const std::string& save_dir,
    const StreamSelectionOptions& chosen_options,
    const std::map<std::string, std::string>& headers,
    const std::map<std::string, std::string>& auth_headers) {

    if (probe_result.tracks.empty()) {
        return Result<TaskModel>::Err("ERR_STREAM_NO_TRACKS", "Cannot create stream task from empty track list");
    }

    TaskModel t;
    t.url = probe_result.manifest_url;
    t.task_type = (probe_result.manifest_url.find(".mpd") != std::string::npos)
                      ? TaskType::kStreamDash
                      : TaskType::kStreamHls;
    std::string fmt = (chosen_options.output_format == "mkv") ? "mkv" : "mp4";
    if (!page_title.empty()) {
        t.file_name = PathSanitizer::SanitizeFileName(page_title) + "." + fmt;
    }
    t.save_dir = save_dir;
    t.headers = headers;
    t.auth_headers = auth_headers;
    t.stream_options = chosen_options;
    t.stream_options.output_format = fmt;

    const std::string selected_dir = t.save_dir;
    auto res = m_controller.AddTask(std::move(t), true, false);
    if (res.ok()) {
        m_controller.RememberDownloadDirectory(selected_dir.empty() ? res.value->save_dir : selected_dir);
        m_selected_task_id = res.value->task_id;
        ScrollTaskIntoView(res.value->task_id, true);
        m_last_feedback_message = "Queued stream download: " + res.value->file_name;
    }
    return res;
}

Result<void> DesktopView::TriggerToolbarAction(ToolbarActionId action_id) {
    switch (action_id) {
        case ToolbarActionId::kStartAll:     return ActionStartAll();
        case ToolbarActionId::kPauseAll:     return ActionPauseAll();
        case ToolbarActionId::kStart:        return ActionStartSelected();
        case ToolbarActionId::kPause:        return ActionPauseSelected();
        case ToolbarActionId::kResume:       return ActionResumeSelected();
        case ToolbarActionId::kRetry:        return ActionRetrySelected();
        case ToolbarActionId::kCancel:       return ActionCancelSelected();
        case ToolbarActionId::kRemove: {
            return ActionRemoveSelectedFromList();
        }
        case ToolbarActionId::kClearHistory: {
            auto r = ActionClearCompletedHistory();
            return r.ok() ? Result<void>::Ok() : Result<void>::Err(r.error->code, r.error->message);
        }
        case ToolbarActionId::kOpenFolder: {
            auto r = ActionOpenDestinationFolder(false);
            return r.ok() ? Result<void>::Ok() : Result<void>::Err(r.error->code, r.error->message);
        }
        case ToolbarActionId::kAddUrl:
            m_last_feedback_message = "Opened Add Download Dialog";
            return Result<void>::Ok();
        case ToolbarActionId::kSettings:
            OpenSettingsCenter(SettingsCategory::kGeneral);
            return Result<void>::Ok();
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::TriggerRowPrimaryAction(const std::string& task_id) {
    SelectTask(task_id);
    auto t = m_controller.GetTask(task_id);
    if (!t.has_value()) return Result<void>::Err("ERR_NOT_FOUND", "Task not found");
    switch (t->state) {
        case TaskState::kDownloading:
        case TaskState::kProbing:
        case TaskState::kQueued:
            return ActionPauseSelected();
        case TaskState::kPaused:
            return ActionResumeSelected();
        case TaskState::kFailed:
        case TaskState::kCanceled:
            return ActionRetrySelected();
        case TaskState::kCompleted: {
            auto r = ActionOpenCompletedFile(false);
            return r.ok() ? Result<void>::Ok() : Result<void>::Err(r.error->code, r.error->message);
        }
        default:
            return Result<void>::Ok();
    }
}

Result<void> DesktopView::ActionStartAll() {
    auto all = m_controller.ListTasks();
    size_t count = 0;
    for (const auto& t : all) {
        if ((t.state == TaskState::kPaused || t.state == TaskState::kFailed || t.state == TaskState::kQueued) && !t.scheduled) {
            count++;
            TriggerActionPulse(t.task_id, "start");
        }
    }
    m_controller.StartAllTasks();
    if (count > 0) {
        m_last_feedback_message = "Started " + std::to_string(count) + " download(s)";
    } else {
        m_last_feedback_message = "No paused downloads to start";
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::ActionPauseAll() {
    auto all = m_controller.ListTasks();
    size_t count = 0;
    for (const auto& t : all) {
        if (t.state == TaskState::kQueued || t.state == TaskState::kProbing ||
            t.state == TaskState::kDownloading || t.state == TaskState::kMuxing) {
            count++;
            TriggerActionPulse(t.task_id, "pause");
        }
    }
    m_controller.PauseAllTasks();
    if (count > 0) {
        m_last_feedback_message = "Paused " + std::to_string(count) + " download(s)";
    } else {
        m_last_feedback_message = "No active downloads to pause";
    }
    return Result<void>::Ok();
}

Result<void> DesktopView::ActionStartSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "start");
        auto r = m_controller.StartTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Started task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionPauseSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "pause");
        auto r = m_controller.PauseTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Paused task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionResumeSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "resume");
        auto r = m_controller.ResumeTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Resumed task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionRetrySelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        TriggerActionPulse(id, "retry");
        auto r = m_controller.RetryTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Retrying task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionCancelSelected() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto r = m_controller.CancelTask(id);
        if (!r.ok()) last_res = r;
    }
    m_last_feedback_message = last_res.ok() ? "Canceled task(s)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionRemoveSelectedFromList() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto r = m_controller.RemoveTask(id, false);
        if (!r.ok()) last_res = r;
    }
    m_checked_task_ids.clear();
    m_select_all_checked = false;
    SelectFirstRow();
    m_last_feedback_message = last_res.ok() ? "Removed task(s) from list (completed file kept on disk)" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionDeleteSelectedWithFile() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto r = m_controller.RemoveTask(id, true);
        if (!r.ok()) last_res = r;
    }
    m_checked_task_ids.clear();
    m_select_all_checked = false;
    SelectFirstRow();
    m_last_feedback_message = last_res.ok() ? "Deleted task(s) and removed output file from disk" : last_res.error->message;
    return last_res;
}

Result<void> DesktopView::ActionMoveSelectedToRecycleBin() {
    if (m_checked_task_ids.empty() && m_selected_task_id.empty()) return Result<void>::Err("ERR_NO_SELECTION", "No task selected");
    std::set<std::string> ids = m_checked_task_ids;
    if (ids.empty()) ids.insert(m_selected_task_id);
    Result<void> last_res = Result<void>::Ok();
    for (const auto& id : ids) {
        auto t = m_controller.GetTask(id);
        if (t.has_value()) {
            std::filesystem::path fp = std::filesystem::path(t->save_dir) / t->file_name;
            std::error_code ec;
            if (std::filesystem::exists(fp, ec)) {
#ifdef _WIN32
                std::wstring wpath = fp.wstring();
                wpath.push_back(L'\0'); // Double-null terminated buffer for SHFileOperationW
                SHFILEOPSTRUCTW op{};
                op.wFunc = FO_DELETE;
                op.pFrom = wpath.c_str();
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
                int ret = SHFileOperationW(&op);
                if (ret != 0 || op.fAnyOperationsAborted) {
                    last_res = Result<void>::Err("ERR_RECYCLE_BIN_FAILED", "Failed to move file to Recycle Bin: error code " + std::to_string(ret));
                    continue; // Do not remove task if file operation failed
                }
#else
                std::filesystem::remove(fp, ec);
#endif
            }
        }
        auto r = m_controller.RemoveTask(id, false);
        if (!r.ok()) last_res = r;
    }
    m_checked_task_ids.clear();
    m_select_all_checked = false;
    SelectFirstRow();
    m_last_feedback_message = last_res.ok() ? "Moved file(s) to Recycle Bin and removed task(s) from list" : last_res.error->message;
    return last_res;
}

Result<int> DesktopView::ActionClearCompletedHistory() {
    auto r = m_controller.ClearCompletedHistory();
    if (r.ok()) {
        m_last_feedback_message = "Cleared " + std::to_string(*r.value) + " completed/canceled item(s) from history";
    }
    return r;
}

Result<std::filesystem::path> DesktopView::ActionOpenCompletedFile(bool dry_run) {
    if (m_selected_task_id.empty()) {
        return Result<std::filesystem::path>::Err("ERR_NO_SELECTION", "No task selected");
    }
    auto t = m_controller.GetTask(m_selected_task_id);
    if (!t.has_value()) {
        return Result<std::filesystem::path>::Err("ERR_NOT_FOUND", "Task not found");
    }
    if (t->error_code == "ERR_THREAT_DETECTED" || t->status_detail.find("Threat detected") != std::string::npos) {
        m_last_feedback_message = "Security Block: Cannot open file because an antivirus threat was detected!";
        return Result<std::filesystem::path>::Err("ERR_THREAT_DETECTED", m_last_feedback_message);
    }
    if (t->state != TaskState::kCompleted) {
        return Result<std::filesystem::path>::Err("ERR_NOT_COMPLETED", "File is not yet completed");
    }
    std::filesystem::path fp = std::filesystem::path(t->save_dir) / t->file_name;
    std::error_code ec;
    if (!std::filesystem::exists(fp, ec)) {
        m_last_feedback_message = "File no longer exists on disk: " + fp.string();
        return Result<std::filesystem::path>::Err("ERR_FILE_MISSING", m_last_feedback_message);
    }
    if (!dry_run) {
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", fp.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#endif
    }
    m_last_feedback_message = "Opened file: " + PathSanitizer::PathToUtf8(fp.filename());
    return Result<std::filesystem::path>::Ok(fp);
}

Result<std::filesystem::path> DesktopView::ActionOpenDestinationFolder(bool dry_run) {
    if (m_selected_task_id.empty()) {
        return Result<std::filesystem::path>::Err("ERR_NO_SELECTION", "No task selected");
    }
    auto t = m_controller.GetTask(m_selected_task_id);
    if (!t.has_value()) {
        return Result<std::filesystem::path>::Err("ERR_NOT_FOUND", "Task not found");
    }
    std::filesystem::path dir(t->save_dir);
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return Result<std::filesystem::path>::Err("ERR_DIR_MISSING", "Destination folder does not exist");
    }
    if (!dry_run) {
#ifdef _WIN32
        std::filesystem::path fp = dir / t->file_name;
        if (std::filesystem::exists(fp, ec)) {
            std::wstring param = L"/select,\"" + fp.wstring() + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", param.c_str(), nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
#endif
    }
    m_last_feedback_message = "Opened destination folder: " + dir.string();
    return Result<std::filesystem::path>::Ok(dir);
}

void DesktopView::ActionSetConcurrentLimit(uint32_t max_tasks) {
    m_controller.SetMaxConcurrentTasks(max_tasks);
    m_draft_settings.max_concurrent_tasks = m_controller.GetMaxConcurrentTasks();
    m_last_feedback_message = "Max concurrent downloads set to " + std::to_string(m_controller.GetMaxConcurrentTasks());
}

void DesktopView::ActionSetSpeedLimit(uint64_t bps) {
    m_controller.SetGlobalSpeedLimit(bps);
    m_draft_settings.speed_limit_bps = bps;
    m_last_feedback_message = (bps == 0) ? "Speed limit set to Unlimited" : ("Speed limit set to " + FormatSpeed(bps));
}

void DesktopView::ActionSetBrowserCapture(bool enabled) {
    auto s = m_controller.GetSettings();
    s.browser_capture_enabled = enabled;
    m_controller.UpdateSettings(s);
    m_draft_settings.browser_capture_enabled = enabled;
    if (m_ipc_server) {
        m_ipc_server->SetCaptureEnabled(enabled);
    }
    m_last_feedback_message = enabled ? "Browser download capture enabled" : "Browser download capture paused";
}

Result<void> DesktopView::ActionSaveSettings(const AppSettings& settings) {
    auto val = ValidateSettingsInput(settings);
    if (!val.ok()) {
        m_settings_validation_error = val.error->message;
        m_last_feedback_message = "Settings validation failed: " + val.error->message;
        return val;
    }
    auto r = m_controller.UpdateSettings(settings);
    if (r.ok()) {
        m_draft_settings = m_controller.GetSettings();
        m_settings_validation_key.clear();
        m_settings_validation_error.clear();
        SetTheme(settings.theme, IsWindowsAppThemeLight());
        GetLocalizationManager().SetLocale(settings.language);
        m_anim.reduced_motion = settings.reduced_motion;
        if (m_ipc_server) {
            m_ipc_server->SetCaptureEnabled(settings.browser_capture_enabled);
        }
        m_last_feedback_message = "Settings saved";
    }
    return r;
}

Result<AppSettings> DesktopView::ActionRestoreDefaultSettings() {
    auto r = m_controller.RestoreDefaultSettings();
    if (r.ok()) {
        m_draft_settings = *r.value;
        m_settings_validation_key.clear();
        m_settings_validation_error.clear();
        SetTheme(r.value->theme, IsWindowsAppThemeLight());
        GetLocalizationManager().SetLocale(r.value->language);
        m_anim.reduced_motion = r.value->reduced_motion;
        if (m_ipc_server) {
            m_ipc_server->SetCaptureEnabled(r.value->browser_capture_enabled);
        }
        m_last_feedback_message = "Restored default settings";
    }
    return r;
}

void DesktopView::RefreshSystemTheme(bool os_prefers_light) {
    if (m_theme_name == "system") SetTheme("system", os_prefers_light);
}

Result<void> DesktopView::ToggleTheme() {
    AppSettings settings = m_controller.GetSettings();
    settings.theme = (m_palette.mode == ThemeMode::kLight) ? "dark" : "light";
    auto saved = m_controller.UpdateSettings(settings);
    if (!saved.ok()) return saved;
    m_draft_settings = m_controller.GetSettings();
    SetTheme(settings.theme, IsWindowsAppThemeLight());
    m_last_feedback_message = settings.theme == "light" ? "Light theme enabled" : "Dark theme enabled";
    return Result<void>::Ok();
}

Result<void> DesktopView::ActionSetTaskSpeedLimit(const std::string& task_id, uint64_t bps) {
    auto r = m_controller.SetTaskSpeedLimit(task_id, bps);
    if (r.ok()) {
        m_last_feedback_message = (bps == 0) ? "Task speed limit set to Unlimited" : ("Task speed limit set to " + FormatSpeed(bps));
    }
    return r;
}

Result<void> DesktopView::ActionAddAlternateUrl(const std::string& task_id, const std::string& alt_url) {
    auto r = m_controller.AddAlternateUrl(task_id, alt_url);
    if (r.ok()) {
        m_last_feedback_message = "Added alternate mirror URL";
    }
    return r;
}

Result<void> DesktopView::ActionSetTaskSchedule(const std::string& task_id, const std::string& start_time_local, const std::string& stop_time_local) {
    auto r = m_controller.SetTaskSchedule(task_id, start_time_local, stop_time_local);
    if (r.ok()) {
        m_last_feedback_message = "Updated task schedule";
    }
    return r;
}

Result<std::vector<crawler::GrabbedItem>> DesktopView::ActionLaunchSiteGrabber(
    const crawler::SiteGrabberConfig& cfg,
    std::atomic<bool>& cancel_requested,
    crawler::GrabbedItemCallback on_item_found) {
    m_last_feedback_message = "Running Site Grabber on " + cfg.start_url + "...";
    auto r = m_controller.CrawlSite(cfg, cancel_requested, on_item_found);
    if (r.ok()) {
        m_last_feedback_message = "Site Grabber completed. Found " + std::to_string(r.value->size()) + " media/file items.";
    } else {
        m_last_feedback_message = "Site Grabber failed: " + r.error->message;
    }
    return r;
}

bool DesktopView::HandleKeyShortcut(const std::string& key_chord) {
    if (key_chord == "Escape" || key_chord == "VK_ESCAPE") {
        if (m_settings_center_open) {
            if (HasUnsavedSettingsChanges()) {
#ifdef _WIN32
                if (m_hwnd) {
                    int res = MessageBoxW(m_hwnd, L"You have unsaved changes. Discard changes and close settings?", L"Discard Changes", MB_YESNO | MB_ICONQUESTION);
                    if (res != IDYES) return true;
                }
#endif
            }
            CloseSettingsCenter(true);
            return true;
        }
        return false;
    }
    if (key_chord == "Down" || key_chord == "VK_DOWN") {
        SelectNextRow();
        return true;
    }
    if (key_chord == "Up" || key_chord == "VK_UP") {
        SelectPrevRow();
        return true;
    }
    if (key_chord == "Home" || key_chord == "VK_HOME") {
        SelectFirstRow();
        return true;
    }
    if (key_chord == "End" || key_chord == "VK_END") {
        SelectLastRow();
        return true;
    }
    if (key_chord == "Space" || key_chord == "VK_SPACE") {
        auto tb = ComputeToolbarState();
        if (tb.can_pause) ActionPauseSelected();
        else if (tb.can_resume) ActionResumeSelected();
        return true;
    }
    if (key_chord == "Delete" || key_chord == "VK_DELETE") {
#ifdef _WIN32
        if (m_hwnd) {
            ShowDeleteConfirmationDialogWin32(m_hwnd, *this, m_controller);
            return true;
        }
#endif
        ActionRemoveSelectedFromList();
        return true;
    }
    if (key_chord == "Shift+Delete") {
#ifdef _WIN32
        if (m_hwnd) {
            ShowDeleteConfirmationDialogWin32(m_hwnd, *this, m_controller);
            return true;
        }
#endif
        ActionDeleteSelectedWithFile();
        return true;
    }
    if (key_chord == "F5" || key_chord == "VK_F5") {
        ActionRetrySelected();
        return true;
    }
    if (key_chord == "Enter" || key_chord == "VK_RETURN") {
        ActionOpenCompletedFile(true);
        return true;
    }
    if (key_chord == "Ctrl+N") {
        TriggerToolbarAction(ToolbarActionId::kAddUrl);
        return true;
    }
    if (key_chord == "Ctrl+,") {
        TriggerToolbarAction(ToolbarActionId::kSettings);
        return true;
    }
    return false;
}

BrowserIntegrationStatusView DesktopView::GetBrowserIntegrationStatus() const {
    BrowserIntegrationStatusView v;
    v.ipc_server_running = (m_ipc_server != nullptr);
    v.ipc_endpoint = m_ipc_server ? m_ipc_server->GetEndpointPath() : ipc::IpcServer::DefaultSocketPath();
    v.capture_enabled = m_controller.GetSettings().browser_capture_enabled;
    const auto& cfg = m_controller.GetConfig();
    std::error_code ec;
    v.nm3u8dl_path = cfg.nm3u8dl_binary_path.string();
    v.nm3u8dl_available = std::filesystem::exists(cfg.nm3u8dl_binary_path, ec);
    v.ffmpeg_path = cfg.ffmpeg_binary_path.string();
    v.ffmpeg_available = std::filesystem::exists(cfg.ffmpeg_binary_path, ec);
    v.supported_browsers = {"Google Chrome (MV3)", "Microsoft Edge (MV3)", "Brave Browser (MV3)", "Mozilla Firefox (MV3)"};
    return v;
}

AboutDialogView DesktopView::GetAboutDialogView() const {
    AboutDialogView a;
    a.upstream_attributions = {
        "N_m3u8DL-RE Stream Downloader (https://github.com/nilaoda/N_m3u8DL-RE) - MIT",
        "FFmpeg Media Muxer (https://ffmpeg.org) - LGPL-2.1+ / LGPL-3.0",
        "Microsoft WIL (https://github.com/microsoft/wil) - MIT",
        "SQLite (https://www.sqlite.org) - Public Domain",
        "nlohmann/json (https://github.com/nlohmann/json) - MIT"
    };
    return a;
}

std::string DesktopView::RenderQueueTableText() const {
    auto rows = BuildVisibleTaskRows(0, 500);
    std::ostringstream oss;
    oss << "========================================================================================================================\n";
    oss << " Radium Download Manager v" << RADIUM_VERSION_STRING << "  |  Category: " << CategoryToLabel(m_selected_category)
        << "  |  Theme: " << m_theme_name << "\n";
    oss << "========================================================================================================================\n";
    oss << std::left
        << std::setw(10) << "TASK ID"
        << std::setw(14) << "TYPE"
        << std::setw(34) << "FILENAME"
        << std::setw(28) << "STATUS / STAGE"
        << std::setw(14) << "PROGRESS"
        << std::setw(18) << "SIZE"
        << std::setw(12) << "SPEED" << "\n";
    oss << "------------------------------------------------------------------------------------------------------------------------\n";
    if (rows.empty()) {
        oss << "  " << GetEmptyStateMessage() << "\n";
    } else {
        for (const auto& r : rows) {
            oss << (r.selected ? "> " : "  ")
                << std::left
                << std::setw(8) << r.task_id.substr(0, 8)
                << std::setw(14) << r.file_type_badge
                << std::setw(34) << r.display_filename
                << std::setw(28) << r.status_text.substr(0, 26)
                << std::setw(14) << r.progress_text.substr(0, 12)
                << std::setw(18) << r.size_text.substr(0, 16)
                << std::setw(12) << r.speed_text << "\n";
        }
    }
    oss << "------------------------------------------------------------------------------------------------------------------------\n";
    auto sb = BuildStatusBar();
    oss << " " << sb.status_message << "\n";
    oss << "========================================================================================================================\n";
    return oss.str();
}

std::string DesktopView::RenderFullDesktopSnapshotText() const {
    std::ostringstream oss;
    auto counts = ComputeCategoryCounts();
    oss << RenderQueueTableText();
    oss << "[Sidebar Counts] All=" << counts.all_downloads
        << " InProgress=" << counts.in_progress
        << " Queued=" << counts.queued
        << " Completed=" << counts.completed
        << " Failed=" << counts.failed
        << " Video=" << counts.video_downloads
        << " Scheduled=" << counts.scheduled << "\n";
    auto details = BuildSelectedTaskDetails();
    if (details.has_selection) {
        oss << "[Details Panel] File=" << details.file_name
            << " | Engine=" << details.engine_name
            << " | Source=" << details.redacted_source_url
            << " | Headers=" << details.redacted_headers_summary << "\n";
    }
    return oss.str();
}

#ifdef _WIN32
// -----------------------------------------------------------------------------
// Native Windows 10 / 11 Win32 + GDI Double-Buffered Dark Interface, Taskbar Progress & Settings Center
// -----------------------------------------------------------------------------
namespace {

struct ConflictCommitPayload {
    bool success{false};
    std::string task_id;
    std::string error_message;
    std::string settings_warning;
};

struct TaskNotificationPayload { std::string task_id; std::wstring title; std::wstring message; bool success{false}; };
struct UpdateCheckPayload { std::wstring title; std::wstring message; std::wstring release_url; bool available{false}; bool matching_installer{false}; };

void StartGitHubUpdateCheck(HWND hwnd) {
    std::thread([hwnd] {
        auto* result = new UpdateCheckPayload;
        result->title = L"Radium Update Check";
        std::atomic<bool> canceled{false};
        std::string response;
        int http_status = 0;
        std::map<std::string, std::string> response_headers;
        auto fetched = brisk::HttpClient::PerformGet(
            std::string(radium::config::UpdateAndReleaseConfig::kApiLatestReleaseUrl),
            {{"Accept", "application/vnd.github+json"},
             {"User-Agent", std::string(radium::config::NetworkPoliciesConfig::kUserAgent)}},
            std::nullopt, std::nullopt, 15000, false, canceled,
            [&http_status, &response_headers](const brisk::HttpResponseMeta& meta) {
                http_status = meta.status_code;
                response_headers = meta.raw_headers;
                return meta.status_code == 200 ? brisk::HeaderAction::kContinue : brisk::HeaderAction::kAbort;
            },
            [&response](const uint8_t* data, size_t len) {
                if (response.size() + len > 2 * 1024 * 1024) return false;
                response.append(reinterpret_cast<const char*>(data), len);
                return true;
            });
        if (http_status != 0) {
            auto eval = radium::core::UpdateManager::EvaluateGitHubReleaseResponse(
                http_status,
                response_headers,
                response,
                RADIUM_VERSION_STRING,
                radium::core::UpdateManager::CurrentArchitecture());
            if (eval.is_rate_limited || http_status == 403 || http_status == 429) {
                // Fallback to direct github.com/releases/latest redirect (zero api.github.com calls)
                std::string final_url;
                int web_status = 0;
                auto web_fetched = brisk::HttpClient::PerformGet(
                    std::string(radium::config::UpdateAndReleaseConfig::kReleasePrefixUrl) + "latest",
                    {{"User-Agent", std::string(radium::config::NetworkPoliciesConfig::kUserAgent)}},
                    std::nullopt, std::nullopt, 15000, false, canceled,
                    [&web_status, &final_url](const brisk::HttpResponseMeta& meta) {
                        web_status = meta.status_code;
                        final_url = meta.final_url;
                        return brisk::HeaderAction::kAbort;
                    },
                    [](const uint8_t*, size_t) { return false; });
                (void)web_fetched;
                if (web_status == 200 || web_status == 301 || web_status == 302) {
                    const std::string tag_marker = "/releases/tag/";
                    auto pos = final_url.find(tag_marker);
                    if (pos != std::string::npos) {
                        std::string tag = final_url.substr(pos + tag_marker.size());
                        auto qpos = tag.find_first_of("?#/");
                        if (qpos != std::string::npos) tag.resize(qpos);
                        auto cmp = radium::CompareSemanticVersions(tag, RADIUM_VERSION_STRING);
                        if (cmp.has_value()) {
                            if (*cmp <= 0) {
                                result->available = false;
                                result->message = Utf8ToW(
                                    "Radium " + std::string(RADIUM_VERSION_STRING) +
                                    " is up to date. Latest stable release: " + tag + ".");
                            } else {
                                result->available = true;
                                result->matching_installer = true;
                                result->release_url = Utf8ToW(final_url);
                                result->message = Utf8ToW(
                                    "A new version of Radium (" + tag + ") is available!\n\nOpen the official release page to download?");
                            }
                            if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_RADIUM_UPDATE_CHECK_COMPLETE, 0, reinterpret_cast<LPARAM>(result))) delete result;
                            return;
                        }
                    } else if (final_url.find("/releases") != std::string::npos) {
                        result->message = L"No published GitHub releases were found for alve33/Radium-Download-Manager yet.";
                        if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_RADIUM_UPDATE_CHECK_COMPLETE, 0, reinterpret_cast<LPARAM>(result))) delete result;
                        return;
                    }
                } else if (web_status == 404) {
                    result->message = L"No published GitHub releases were found for alve33/Radium-Download-Manager yet.";
                    if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_RADIUM_UPDATE_CHECK_COMPLETE, 0, reinterpret_cast<LPARAM>(result))) delete result;
                    return;
                }
            }
            result->available = eval.update_available;
            result->matching_installer = eval.matching_installer;
            result->release_url = Utf8ToW(eval.release_url);
            result->message = Utf8ToW(eval.message);
        } else if (!fetched.ok()) {
            result->message = Utf8ToW(radium::core::UpdateManager::FormatNetworkCheckError(*fetched.error));
        } else {
            result->message = L"GitHub returned an unexpected empty response.";
        }
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_RADIUM_UPDATE_CHECK_COMPLETE, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
}

struct ProbeResultPayload {
    uint64_t request_id{0};
    int64_t content_length{-1};
    bool success{false};
    bool range_supported{false};
    std::string final_filename;
};
constexpr UINT IDT_UI_REFRESH      = 1001;
constexpr int  IDC_SEARCH_EDIT     = 1101;

struct OfferNotificationData {
    TaskModel draft_task;
    std::string task_id;
    std::string url;
    std::string file_name;
    std::string save_dir;
    uint64_t file_size{0};
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> auth_headers;
    std::string speculative_tx_id;
    bool has_precomputed_conflict{false};
    DownloadController::IncomingConflictReport precomputed_conflict;

    OfferNotificationData() = default;
    explicit OfferNotificationData(TaskModel t)
        : draft_task(std::move(t)),
          task_id(draft_task.task_id),
          url(draft_task.url),
          file_name(draft_task.file_name),
          save_dir(draft_task.save_dir),
          file_size(draft_task.total_bytes),
          headers(draft_task.headers),
          auth_headers(draft_task.auth_headers) {}
};

struct TorrentFetchPayload {
    OfferNotificationData offer;
    std::string metainfo_path;
    std::string error_message;
};

bool IsRemoteTorrentUrl(std::string_view url) {
    auto parsed = brisk::HttpClient::ParseUrl(url);
    if (!parsed.ok()) return false;
    std::string path = parsed.value->path_and_query;
    const size_t query = path.find('?');
    if (query != std::string::npos) path.resize(query);
    std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return path.ends_with(".torrent");
}

bool IsRemoteTorrentOffer(const OfferNotificationData& offer) {
    if (IsRemoteTorrentUrl(offer.url)) return true;
    auto parsed = brisk::HttpClient::ParseUrl(offer.url);
    if (!parsed.ok()) return false;
    std::string file_name = offer.file_name;
    std::transform(file_name.begin(), file_name.end(), file_name.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return file_name.ends_with(".torrent");
}

Result<std::string> FetchTorrentMetainfo(const OfferNotificationData& offer) {
    std::map<std::string, std::string> headers = offer.headers;
    for (const auto& [name, value] : offer.auth_headers) headers[name] = value;
    std::string body;
    constexpr size_t kMaxTorrentMetainfoBytes = 16 * 1024 * 1024;
    std::atomic<bool> cancel{false};
    auto response = brisk::HttpClient::PerformGet(
        offer.url, headers, std::nullopt, std::nullopt, 15000, false, cancel,
        [](const brisk::HttpResponseMeta&) { return brisk::HeaderAction::kContinue; },
        [&body, kMaxTorrentMetainfoBytes](const uint8_t* data, size_t len) {
            if (len > kMaxTorrentMetainfoBytes - std::min(body.size(), kMaxTorrentMetainfoBytes)) return false;
            body.append(reinterpret_cast<const char*>(data), len);
            return true;
        });
    if (!response.ok()) {
        return Result<std::string>::Err(response.error->code, response.error->message);
    }
    if (body.empty()) {
        return Result<std::string>::Err("ERR_TORRENT_EMPTY", "The server returned an empty torrent file");
    }

    std::error_code ec;
    const std::filesystem::path torrent_root = radium::PathSanitizer::GetDefaultAppDataDir() / L"tmp" / L"torrents";
    std::filesystem::create_directories(torrent_root, ec);
    if (ec) return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create the torrent metadata folder");

    GUID file_id{};
    if (CoCreateGuid(&file_id) != S_OK) {
        return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create a unique torrent metadata filename");
    }
    wchar_t guid_text[40]{};
    if (StringFromGUID2(file_id, guid_text, static_cast<int>(std::size(guid_text))) == 0) {
        return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create a unique torrent metadata filename");
    }
    std::wstring guid(guid_text);
    guid.erase(std::remove(guid.begin(), guid.end(), L'{'), guid.end());
    guid.erase(std::remove(guid.begin(), guid.end(), L'}'), guid.end());
    const auto task_dir = torrent_root / guid;
    std::filesystem::create_directories(task_dir, ec);
    if (ec) return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not create the task metadata folder");
    const auto final_path = task_dir / L"metainfo.torrent";
    const auto pending_path = task_dir / L"metainfo.tmp";
    {
        std::ofstream output(pending_path, std::ios::binary | std::ios::trunc);
        if (!output || !output.write(body.data(), static_cast<std::streamsize>(body.size()))) {
            std::filesystem::remove(pending_path, ec);
            return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not save the received torrent metadata");
        }
    }
    std::filesystem::rename(pending_path, final_path, ec);
    if (ec) {
        std::filesystem::remove(pending_path, ec);
        return Result<std::string>::Err("ERR_TORRENT_STORAGE", "Could not finalize the received torrent metadata");
    }
    return Result<std::string>::Ok(PathSanitizer::PathToUtf8(final_path));
}

struct TorrentMetadataSelectionRequest {
    torrent::TorrentInfo* info{nullptr};
    bool accepted{false};
};

constexpr int IDM_TRAY_SHOW        = 2001;
constexpr int IDM_TRAY_HIDE        = 2002;
constexpr int IDM_TRAY_PAUSE_ALL   = 2003;
constexpr int IDM_TRAY_RESUME_ALL  = 2004;
constexpr int IDM_TRAY_EXIT        = 2005;

constexpr int IDM_CTX_START        = 3001;
constexpr int IDM_CTX_PAUSE        = 3002;
constexpr int IDM_CTX_RESUME       = 3003;
constexpr int IDM_CTX_RETRY        = 3004;
constexpr int IDM_CTX_CANCEL       = 3005;
constexpr int IDM_CTX_OPEN_FILE    = 3006;
constexpr int IDM_CTX_OPEN_FOLDER  = 3007;
constexpr int IDM_CTX_REMOVE_LIST  = 3008;
constexpr int IDM_CTX_DELETE_DISK  = 3009;
constexpr int IDM_CTX_SETTINGS     = 3010;
constexpr int IDM_CTX_BROWSER_STAT = 3011;
constexpr int IDM_CTX_STARTUP_LOG  = 3026;
constexpr int IDM_CTX_ABOUT        = 3012;
constexpr int IDM_CTX_ANTIVIRUS    = 3013;
constexpr int IDM_CTX_ZIP_PREVIEW   = 3014;
constexpr int IDM_CTX_EXPORT_TASKS  = 3015;
constexpr int IDM_CTX_IMPORT_TASKS  = 3016;
constexpr int IDM_CTX_OPEN_TORRENT  = 3025;
constexpr int IDM_CTX_RENAME        = 3017;
constexpr int IDM_CTX_MOVE_TO       = 3018;
constexpr int IDM_CTX_COPY_URL      = 3019;
constexpr int IDM_CTX_REFRESH_URL   = 3020;
constexpr int IDM_CTX_ADD_MIRROR    = 3021;
constexpr int IDM_CTX_SCHEDULE      = 3022;
constexpr int IDM_CTX_SPEED_LIMIT   = 3024;
constexpr int IDM_CTX_FILE_INTEGRITY = 3023;
constexpr int IDM_CTX_TORRENT_SWARM  = 3027;
constexpr int IDM_CTX_TORRENT_ADD_TRACKER = 3028;
constexpr int IDM_CTX_TORRENT_SEQUENTIAL  = 3029;
constexpr int IDM_CTX_CREATE_TORRENT      = 3030;
constexpr int IDM_CTX_SITE_GRABBER        = 3031;
constexpr int IDM_CTX_QUEUE_MANAGER       = 3032;
constexpr int IDM_CTX_DROP_TARGET         = 3033;
constexpr int IDM_CTX_BATCH_URL           = 3034;
constexpr int IDM_CTX_TRAY_PROGRESS       = 3035;
constexpr int IDM_CTX_EXPORT_EF2          = 3036;
constexpr int IDM_CTX_IMPORT_EF2          = 3037;
constexpr int IDM_CTX_PRIORITY_HIGH       = 3038;
constexpr int IDM_CTX_PRIORITY_NORMAL     = 3039;
constexpr int IDM_CTX_PRIORITY_LOW        = 3040;
constexpr int IDHOT_SHOW_RADIUM           = 9001;

struct Win32WindowContext {
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    HWND hwnd{nullptr};
    HWND hSearchEdit{nullptr};
    HWND hThemeTooltip{nullptr};
    NOTIFYICONDATAW nid{};
    bool tray_added{false};
    std::set<std::string> notified_failure_task_ids;
    HWND hDropTargetWnd{nullptr};
    bool clipboard_listener_added{false};
    bool hotkey_registered{false};
    bool ole_initialized{false};
    std::string last_clipboard_url;
    bool is_dragging_vscroll{false};
    int vscroll_drag_start_y{0};
    size_t vscroll_drag_start_offset{0};
    bool is_vscroll_hovered{false};
    ITaskbarList3* pTaskbarList{nullptr};
    bool com_initialized{false};
    TaskbarProgressInfo last_taskbar_info{};
    bool taskbar_info_initialized{false};
    bool mouse_tracking_active{false};
    bool language_button_pressed{false};
    std::string conflict_dialog_task_id;
    std::wstring conflict_dialog_path;
    int conflict_dialog_rename_number{1};
    bool conflict_dialog_remember{false};
    struct PendingConflictItem {
        TaskModel draft_task;
        std::optional<torrent::TorrentInfo> torrent_info;
        std::string torrent_metainfo_path;
        DownloadController::IncomingConflictReport report;
        bool start_immediately{true};
        bool is_from_new_download_dialog{false};
        bool is_existing_controller_task{false};
    };
    std::deque<PendingConflictItem> pending_conflicts;
    HWND hConflictRenameEdit{nullptr};
    std::string conflict_validation_error;
    bool conflict_dialog_processing{false};
    ConflictPolicy conflict_dialog_processing_choice{ConflictPolicy::kPrompt};
    ipc::IpcServer* ipc_server{nullptr};
    WNDPROC orig_search_proc{nullptr};
    HFONT hEditFont{nullptr};
    bool was_active_last_tick{false};
    IRawElementProviderSimple* pUiaProvider{nullptr};
};

class RadiumTaskListUiaProvider final : public IRawElementProviderSimple {
public:
    explicit RadiumTaskListUiaProvider(HWND hwnd, DesktopView* view)
        : m_ref(1), m_hwnd(hwnd), m_view(view) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&m_ref); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&m_ref);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *ppv = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** pRetVal) override {
        if (!pRetVal) return E_POINTER;
        *pRetVal = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID propertyId, VARIANT* pRetVal) override {
        if (!pRetVal) return E_POINTER;
        VariantInit(pRetVal);
        if (propertyId == UIA_ControlTypePropertyId) {
            pRetVal->vt = VT_I4;
            pRetVal->lVal = UIA_ListControlTypeId;
        } else if (propertyId == UIA_NamePropertyId) {
            pRetVal->vt = VT_BSTR;
            pRetVal->bstrVal = SysAllocString(L"Radium Downloads");
        } else if (propertyId == UIA_IsControlElementPropertyId || propertyId == UIA_IsContentElementPropertyId) {
            pRetVal->vt = VT_BOOL;
            pRetVal->boolVal = VARIANT_TRUE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** pRetVal) override {
        return UiaHostProviderFromHwnd(m_hwnd, pRetVal);
    }

private:
    LONG m_ref;
    HWND m_hwnd;
    DesktopView* m_view;
};

constexpr int IDC_CONFLICT_RENAME_EDIT = 1199;

struct ConflictDialogLayout { RECT card, path, rename_edit, checkbox, rename, overwrite, abort, close; };

ConflictDialogLayout ComputeConflictDialogLayout(int client_w, int client_h, int dpi) {
    const auto scale = [dpi](int v) { return MulDiv(v, dpi, 96); };
    const int width = std::min(scale(444), std::max(scale(280), client_w - scale(32)));
    const int height = std::min(scale(244), std::max(scale(210), client_h - scale(24)));
    const int left = (client_w - width) / 2;
    const int top = (client_h - height) / 2;
    const int pad_x = scale(18);
    ConflictDialogLayout l{};
    l.card = RECT{left, top, left + width, top + height};
    l.close = RECT{left + width - scale(32), top + scale(8), left + width - scale(8), top + scale(32)};
    l.path = RECT{left + pad_x, top + scale(40), left + width - pad_x, top + height - scale(122)};
    l.rename_edit = RECT{left + pad_x, top + height - scale(116), left + width - pad_x, top + height - scale(88)};
    l.checkbox = RECT{left + pad_x, top + height - scale(78), left + width - pad_x, top + height - scale(54)};
    const int button_h = scale(32), gap = scale(8), right = left + width - pad_x, bottom = top + height - scale(14);
    const int abort_w = scale(72), overwrite_w = scale(92), rename_w = scale(96);
    l.abort = RECT{right - abort_w, bottom - button_h, right, bottom};
    l.overwrite = RECT{l.abort.left - gap - overwrite_w, bottom - button_h, l.abort.left - gap, bottom};
    l.rename = RECT{l.overwrite.left - gap - rename_w, bottom - button_h, l.overwrite.left - gap, bottom};
    return l;
}

bool MarkTaskAsFileConflict(DownloadController& controller, const TaskModel& task) {
    TaskModel conflict = task;
    conflict.state = TaskState::kFailed;
    conflict.error_code = "ERR_FILE_EXISTS";
    conflict.error_message = "Destination file already exists: " +
        PathSanitizer::PathToUtf8(PathSanitizer::Utf8ToPath(conflict.save_dir) / PathSanitizer::Utf8ToPath(conflict.file_name));
    conflict.status_detail = "Waiting for a file conflict choice";
    return controller.UpdateTask(conflict).ok();
}

bool TryShowExistingConflictInsteadOfNewDownload(
    HWND main_hwnd,
    DownloadController& controller,
    const std::string& url,
    const std::string& suggested_filename,
    const std::string& requested_save_dir,
    const std::string& existing_task_id = "") {
    if (!main_hwnd || !IsWindow(main_hwnd) || url.empty() ||
        controller.GetSettings().conflict_policy != ConflictPolicy::kPrompt) return false;

    std::string filename = suggested_filename;
    if (filename.empty()) {
        if (url.find("youtube.com") != std::string::npos || url.find("youtu.be") != std::string::npos) {
            filename = "youtube_video.mp4";
        } else {
            filename = PathSanitizer::ExtractFromUrl(url);
        }
    }
    if (filename.empty()) return false;
    std::string save_dir = requested_save_dir.empty()
        ? controller.GetSettings().default_download_dir : requested_save_dir;

    auto normalize_cmp = [](std::string s) {
        const size_t hash_pos = s.find('#');
        if (hash_pos != std::string::npos) s.resize(hash_pos);
        for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
    };
    const std::string norm_url = normalize_cmp(url);
    const std::string norm_target_path = normalize_cmp(
        PathSanitizer::PathToUtf8(PathSanitizer::Utf8ToPath(save_dir) / PathSanitizer::Utf8ToPath(filename)));

    bool exists_in_queue = false;
    for (const auto& existing : controller.ListTasks()) {
        if (!existing_task_id.empty() && existing.task_id == existing_task_id) continue;
        if (normalize_cmp(existing.url) == norm_url) {
            exists_in_queue = true;
            break;
        }
        const std::string existing_path = normalize_cmp(
            PathSanitizer::PathToUtf8(PathSanitizer::Utf8ToPath(existing.save_dir) / PathSanitizer::Utf8ToPath(existing.file_name)));
        if (!existing_path.empty() && existing_path == norm_target_path) {
            exists_in_queue = true;
            break;
        }
    }

    auto destination = PathSanitizer::ResolveSafeOutputPath(save_dir, filename, ConflictPolicy::kPrompt);
    const bool exists_on_disk = (!destination.ok() && destination.error->code == "ERR_FILE_EXISTS");
    if (!exists_on_disk && !exists_in_queue) return false;

    if (!existing_task_id.empty()) {
        auto offer_task = controller.GetTask(existing_task_id);
        if (offer_task.has_value()) {
            offer_task->file_name = filename;
            offer_task->save_dir = save_dir;
            if (!MarkTaskAsFileConflict(controller, *offer_task)) return false;
            SendMessageW(main_hwnd, WM_RADIUM_SHOW_CONFLICT, 0, 0);
            return true;
        }
    }

    TaskModel task;
    task.url = url;
    task.file_name = filename;
    task.save_dir = save_dir;
    auto added = controller.AddTask(std::move(task), false, false);
    if (!added.ok() || added.value->state != TaskState::kPaused) return false;
    if (!MarkTaskAsFileConflict(controller, *added.value)) return false;
    SendMessageW(main_hwnd, WM_RADIUM_SHOW_CONFLICT, 0, 0);
    return true;
}

void InstallModernEditControl(HWND hEdit, uint32_t dpi, const ThemePalette& palette);
void UpdateModernEditPalette(HWND hEdit, uint32_t dpi, const ThemePalette& palette);

void SyncConflictRenameEdit(Win32WindowContext& ctx, const std::string& suggested_name) {
    if (!ctx.hwnd || !IsWindow(ctx.hwnd)) return;
    const UINT dpi = GetDpiForWindow(ctx.hwnd);
    const ThemePalette pal = ctx.view ? ctx.view->GetPalette() : DesignSystem::GetRedesignPalette();
    const bool is_dark = (pal.mode == ThemeMode::kDark);
    RECT cr{};
    GetClientRect(ctx.hwnd, &cr);
    auto l = ComputeConflictDialogLayout(cr.right, cr.bottom, dpi);
    if (!ctx.hConflictRenameEdit) {
        ctx.hConflictRenameEdit = CreateWindowExW(
            0, L"EDIT", Utf8ToW(suggested_name).c_str(),
            WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
            l.rename_edit.left, l.rename_edit.top,
            l.rename_edit.right - l.rename_edit.left, l.rename_edit.bottom - l.rename_edit.top,
            ctx.hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CONFLICT_RENAME_EDIT)),
            GetModuleHandleW(nullptr), nullptr);
        if (ctx.hConflictRenameEdit) {
            if (ctx.hEditFont) {
                SendMessageW(ctx.hConflictRenameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx.hEditFont), TRUE);
            }
            InstallModernEditControl(ctx.hConflictRenameEdit, dpi, pal);
            ApplyNativeEditTheme(ctx.hConflictRenameEdit, is_dark);
        }
    } else {
        MoveWindow(ctx.hConflictRenameEdit, l.rename_edit.left, l.rename_edit.top,
                   l.rename_edit.right - l.rename_edit.left, l.rename_edit.bottom - l.rename_edit.top, TRUE);
        UpdateModernEditPalette(ctx.hConflictRenameEdit, dpi, pal);
        ApplyNativeEditTheme(ctx.hConflictRenameEdit, is_dark);
        SetWindowTextW(ctx.hConflictRenameEdit, Utf8ToW(suggested_name).c_str());
        ShowWindow(ctx.hConflictRenameEdit, SW_SHOW);
        InvalidateRect(ctx.hConflictRenameEdit, nullptr, TRUE);
    }
}

void HideConflictRenameEdit(Win32WindowContext& ctx) {
    if (ctx.hConflictRenameEdit && IsWindow(ctx.hConflictRenameEdit)) {
        ShowWindow(ctx.hConflictRenameEdit, SW_HIDE);
    }
}

void ActivatePendingConflict(Win32WindowContext& ctx, const Win32WindowContext::PendingConflictItem& item) {
    ctx.conflict_dialog_task_id = item.draft_task.task_id;
    std::string display_path = item.report.target_path;
    if (display_path.empty()) {
        display_path = PathSanitizer::PathToUtf8(
            PathSanitizer::Utf8ToPath(item.draft_task.save_dir) / PathSanitizer::Utf8ToPath(item.draft_task.file_name));
    }
    if (!item.report.overwrite_allowed) {
        display_path += "\n(Destination is currently used by an active or paused download — overwrite is disabled)";
    } else if (item.report.kind == DownloadController::IncomingConflictKind::kResumablePartialExists) {
        display_path += "\n(A partial or paused download for this item already exists)";
    } else if (item.report.kind == DownloadController::IncomingConflictKind::kCompletedInHistory) {
        display_path += "\n(This download was previously completed)";
    }
    ctx.conflict_dialog_path = Utf8ToW(display_path);
    ctx.conflict_dialog_remember = false;
    ctx.conflict_validation_error.clear();
    std::string suggested = ctx.controller->ResolveAvailableFileName(
        item.draft_task.save_dir, item.draft_task.file_name,
        item.is_existing_controller_task ? item.draft_task.task_id : "");
    ctx.conflict_dialog_rename_number = 1;
    auto stem = PathSanitizer::Utf8ToPath(suggested).stem().wstring();
    auto open = stem.rfind(L" (");
    if (open != std::wstring::npos && !stem.empty() && stem.back() == L')') {
        try { ctx.conflict_dialog_rename_number = std::stoi(stem.substr(open + 2, stem.size() - open - 3)); }
        catch (...) { ctx.conflict_dialog_rename_number = 1; }
    }
    SyncConflictRenameEdit(ctx, suggested);
}

void EnqueueConflictPrompt(Win32WindowContext& ctx, Win32WindowContext::PendingConflictItem item) {
    if (item.draft_task.task_id.empty()) {
        item.draft_task.task_id = GenerateUuidV4();
    }
    for (const auto& existing : ctx.pending_conflicts) {
        if ((!item.draft_task.task_id.empty() && existing.draft_task.task_id == item.draft_task.task_id) ||
            (!item.draft_task.correlation_id.empty() && existing.draft_task.correlation_id == item.draft_task.correlation_id)) {
            return;
        }
    }
    ctx.pending_conflicts.push_back(std::move(item));
    if (ctx.conflict_dialog_task_id.empty() && !ctx.pending_conflicts.empty()) {
        ActivatePendingConflict(ctx, ctx.pending_conflicts.front());
    }
}

void RefreshConflictDialog(Win32WindowContext& ctx) {
    if (!ctx.conflict_dialog_task_id.empty()) return;
    if (!ctx.pending_conflicts.empty()) {
        ActivatePendingConflict(ctx, ctx.pending_conflicts.front());
    }
}

bool ResolveConflictDialog(Win32WindowContext& ctx, ConflictPolicy choice) {
    if (ctx.conflict_dialog_processing) {
        return false;
    }
    if (ctx.pending_conflicts.empty()) {
        ctx.conflict_dialog_task_id.clear();
        HideConflictRenameEdit(ctx);
        return false;
    }

    auto item = ctx.pending_conflicts.front();
    auto finish_item = [&]() {
        if (ctx.ipc_server && !item.draft_task.task_id.empty()) {
            ctx.ipc_server->ResolvePendingOffer(item.draft_task.task_id);
        }
        ctx.pending_conflicts.pop_front();
        ctx.conflict_dialog_task_id.clear();
        ctx.conflict_dialog_path.clear();
        ctx.conflict_dialog_remember = false;
        ctx.conflict_validation_error.clear();
        HideConflictRenameEdit(ctx);
        if (!ctx.pending_conflicts.empty()) {
            ActivatePendingConflict(ctx, ctx.pending_conflicts.front());
        }
    };

    if (choice == ConflictPolicy::kFailIfExists) {
        if (item.is_existing_controller_task) {
            ctx.controller->CancelTask(item.draft_task.task_id);
        }
        if (item.draft_task.url.rfind("blob:", 0) == 0 && !item.draft_task.part_path.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove(PathSanitizer::Utf8ToPath(item.draft_task.part_path), rm_ec);
        }
        if (!item.torrent_metainfo_path.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove_all(std::filesystem::path(item.torrent_metainfo_path).parent_path(), rm_ec);
        }
        finish_item();
        return true;
    }

    if (choice == ConflictPolicy::kOverwrite && !item.report.overwrite_allowed) {
        ctx.conflict_validation_error = "Cannot overwrite: destination is owned by an active or paused download.";
        return false;
    }

    if (choice == ConflictPolicy::kOverwrite) {
        auto reserved = ctx.controller->CheckDestinationConflict(
            item.draft_task.save_dir, item.draft_task.file_name,
            item.is_existing_controller_task ? item.draft_task.task_id : "");
        if (reserved.kind != core::DuplicateKind::kNone &&
            core::DuplicateDetector::IsTaskReservingDestination(reserved.matched_task.state)) {
            ctx.conflict_validation_error = "Cannot overwrite: destination is owned by an active or paused download.";
            return false;
        }
    }

    TaskModel resolved = item.draft_task;
    if (choice == ConflictPolicy::kAutoRename) {
        std::string chosen_name;
        if (ctx.hConflictRenameEdit && IsWindow(ctx.hConflictRenameEdit)) {
            wchar_t buf[512]{};
            GetWindowTextW(ctx.hConflictRenameEdit, buf, 511);
            chosen_name = WToUtf8(buf);
        }
        chosen_name = PathSanitizer::SanitizeFileName(chosen_name);
        if (chosen_name.empty()) {
            chosen_name = ctx.controller->ResolveAvailableFileName(
                resolved.save_dir, resolved.file_name,
                item.is_existing_controller_task ? resolved.task_id : "");
        } else {
            // Preserve original file extension if user omitted it
            std::filesystem::path orig_p = PathSanitizer::Utf8ToPath(resolved.file_name);
            std::filesystem::path new_p = PathSanitizer::Utf8ToPath(chosen_name);
            if (!orig_p.extension().empty() && new_p.extension().empty()) {
                chosen_name += PathSanitizer::PathToUtf8(orig_p.extension());
            }
        }
        // Verify new destination does not already exist on disk and is not reserved
        std::filesystem::path new_target = PathSanitizer::Utf8ToPath(resolved.save_dir) / PathSanitizer::Utf8ToPath(chosen_name);
        std::error_code ec;
        bool exists_disk = std::filesystem::exists(new_target, ec);
        auto reserved = ctx.controller->CheckDestinationConflict(
            resolved.save_dir, chosen_name,
            item.is_existing_controller_task ? resolved.task_id : "");
        bool is_reserved = (reserved.kind != core::DuplicateKind::kNone &&
                            core::DuplicateDetector::IsTaskReservingDestination(reserved.matched_task.state));
        if (exists_disk || is_reserved) {
            ctx.conflict_validation_error = "That destination already exists or is in use. Choose a different name.";
            return false;
        }
        resolved.file_name = chosen_name;
        resolved.user_edited_filename = true;
    } else if (choice == ConflictPolicy::kOverwrite) {
        resolved.user_edited_filename = true;
    }

    const bool remember_choice = ctx.conflict_dialog_remember;
    resolved.conflict_policy = choice;
    resolved.conflict_policy_explicit = true;

    bool success = false;
    if (item.torrent_info.has_value()) {
        if (remember_choice) {
            auto settings = ctx.controller->GetSettings();
            settings.conflict_policy = choice;
            auto saved = ctx.controller->UpdateSettings(settings);
            if (!saved.ok()) {
                std::wstring message = L"This download choice will be applied, but Radium could not remember it: " + Utf8ToW(saved.error->message);
                MessageBoxW(ctx.hwnd, message.c_str(), L"Could not save preference", MB_OK | MB_ICONWARNING);
            }
        }
        torrent::TorrentInfo tinfo = *item.torrent_info;
        if (choice == ConflictPolicy::kAutoRename) {
            tinfo.name = resolved.file_name;
            if (tinfo.files.size() == 1) {
                tinfo.files[0].relative_path = PathSanitizer::Utf8ToPath(resolved.file_name);
            }
        }
        std::string target_dir = resolved.save_dir;
        bool start_torrent = item.start_immediately;
        std::string selected_category = resolved.category;
        finish_item();
        if (ShowAddTorrentDialogWin32(ctx.hwnd, *ctx.view, *ctx.controller, tinfo,
                                      resolved.url, target_dir, start_torrent,
                                      &target_dir, &start_torrent, &selected_category)) {
            resolved.save_dir = target_dir;
            resolved.category = selected_category.empty() ? "Torrents" : selected_category;
            resolved.total_bytes = tinfo.selected_bytes();
            resolved.torrent_file_paths.clear();
            resolved.torrent_file_selection.clear();
            for (const auto& f : tinfo.files) {
                resolved.torrent_file_paths.push_back(f.relative_path.generic_string());
                resolved.torrent_file_selection.push_back(f.selected ? 1 : 0);
            }
            auto add_res = ctx.controller->AddTask(std::move(resolved), start_torrent, true);
            if (add_res.ok()) {
                ctx.view->SelectTask(add_res.value->task_id);
                ctx.view->ScrollTaskIntoView(add_res.value->task_id, true);
                success = true;
            }
        } else if (!item.torrent_metainfo_path.empty()) {
            std::error_code rm_ec;
            std::filesystem::remove_all(std::filesystem::path(item.torrent_metainfo_path).parent_path(), rm_ec);
        }
        return success;
    }

    ctx.conflict_dialog_processing = true;
    ctx.conflict_dialog_processing_choice = choice;
    ctx.conflict_validation_error.clear();
    if (ctx.hwnd && IsWindow(ctx.hwnd)) {
        InvalidateRect(ctx.hwnd, nullptr, FALSE);
    }

    HWND target_hwnd = ctx.hwnd;
    DownloadController* ctrl = ctx.controller;
    const bool is_existing_task = item.is_existing_controller_task;
    const bool start_immediately = item.start_immediately;

    try {
        std::thread([target_hwnd, ctrl, resolved = std::move(resolved), choice, remember_choice, is_existing_task, start_immediately]() mutable {
            auto* payload = new ConflictCommitPayload;
            try {
                if (remember_choice) {
                    auto settings = ctrl->GetSettings();
                    settings.conflict_policy = choice;
                    auto saved = ctrl->UpdateSettings(settings);
                    if (!saved.ok()) {
                        payload->settings_warning = saved.error->message;
                    }
                }

                if (is_existing_task) {
                    auto changed = ctrl->UpdateTask(resolved);
                    if (changed.ok() && ctrl->RetryTask(resolved.task_id).ok()) {
                        payload->success = true;
                        payload->task_id = resolved.task_id;
                    } else {
                        payload->error_message = changed.ok()
                            ? "Could not restart task for the selected destination."
                            : changed.error->message;
                    }
                } else if (resolved.url.rfind("blob:", 0) == 0 && !resolved.part_path.empty()) {
                    std::filesystem::path staged = PathSanitizer::Utf8ToPath(resolved.part_path);
                    resolved.part_path.clear();
                    auto ingest_res = ctrl->IngestCompletedTask(std::move(resolved), staged);
                    if (ingest_res.ok()) {
                        payload->success = true;
                        payload->task_id = ingest_res.value->task_id;
                    } else {
                        payload->error_message = ingest_res.error->message;
                    }
                } else {
                    auto add_res = ctrl->AddTask(std::move(resolved), start_immediately, true);
                    if (add_res.ok()) {
                        payload->success = true;
                        payload->task_id = add_res.value->task_id;
                    } else {
                        payload->error_message = add_res.error->message;
                    }
                }
            } catch (const std::exception& ex) {
                payload->success = false;
                payload->error_message = ex.what();
            } catch (...) {
                payload->success = false;
                payload->error_message = "Unexpected error while applying conflict resolution.";
            }

            if (!IsWindow(target_hwnd) ||
                !PostMessageW(target_hwnd, WM_RADIUM_CONFLICT_COMMIT_COMPLETE, 0, reinterpret_cast<LPARAM>(payload))) {
                delete payload;
            }
        }).detach();
        return true;
    } catch (...) {
        ctx.conflict_dialog_processing = false;
        ctx.conflict_validation_error = "Could not start background worker for conflict resolution.";
        return false;
    }
}

// Modern Edit Control Subclass: custom padding, Segoe UI typography, subtle borders, and Coral Red focus glow
constexpr UINT_PTR SUBCLASS_MODERN_EDIT_ID = 9001;

struct ModernEditData {
    bool is_focused{false};
    bool is_hovered{false};
    int pad_x{8};
    int pad_y{4};
    int radius{6};
    ThemePalette palette{DesignSystem::GetRedesignPalette()};
};

LRESULT CALLBACK ModernEditSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* data = reinterpret_cast<ModernEditData*>(dwRefData);
    if (!data) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    switch (uMsg) {
        case WM_SETFOCUS: {
            data->is_focused = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_KILLFOCUS: {
            data->is_focused = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_MOUSEMOVE: {
            if (!data->is_hovered) {
                data->is_hovered = true;
                TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hWnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        }
        case WM_MOUSELEAVE: {
            data->is_hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        }
        case WM_PAINT: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            // Draw modern rounded border over non-client / border region
            HDC hdc = GetWindowDC(hWnd);
            if (hdc) {
                RECT wr{};
                GetWindowRect(hWnd, &wr);
                int w = wr.right - wr.left;
                int h = wr.bottom - wr.top;
                bool is_enabled = IsWindowEnabled(hWnd) != FALSE;

                ColorRgba border_rgba = !is_enabled ? data->palette.border_subtle
                                        : (data->is_focused ? data->palette.accent_primary
                                        : (data->is_hovered ? data->palette.border_focus
                                                            : data->palette.border_subtle));

                Gdiplus::Graphics g(hdc);
                g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                float r = static_cast<float>(std::max(2, data->radius));
                auto make_rrect = [&](float inset) {
                    auto path = std::make_unique<Gdiplus::GraphicsPath>();
                    float x = inset;
                    float y = inset;
                    float rw = std::max(1.0f, static_cast<float>(w) - 1.0f - inset * 2.0f);
                    float rh = std::max(1.0f, static_cast<float>(h) - 1.0f - inset * 2.0f);
                    float d = std::min(r * 2.0f, std::min(rw, rh));
                    path->AddArc(x, y, d, d, 180.0f, 90.0f);
                    path->AddArc(x + rw - d, y, d, d, 270.0f, 90.0f);
                    path->AddArc(x + rw - d, y + rh - d, d, d, 0.0f, 90.0f);
                    path->AddArc(x, y + rh - d, d, d, 90.0f, 90.0f);
                    path->CloseFigure();
                    return path;
                };

                if (data->is_focused && is_enabled) {
                    auto glow_path = make_rrect(0.5f);
                    Gdiplus::Pen glow_pen(Gdiplus::Color(60, border_rgba.r, border_rgba.g, border_rgba.b), 3.0f);
                    g.DrawPath(&glow_pen, glow_path.get());
                }
                auto border_path = make_rrect(0.75f);
                Gdiplus::Pen border_pen(Gdiplus::Color(255, border_rgba.r, border_rgba.g, border_rgba.b), 1.5f);
                g.DrawPath(&border_pen, border_path.get());

                ReleaseDC(hWnd, hdc);
            }
            return res;
        }
        case WM_NCPAINT: {
            // Suppress standard Windows 3D border painting
            return 0;
        }
        case WM_NCDESTROY: {
            RemoveWindowSubclass(hWnd, ModernEditSubclassProc, uIdSubclass);
            delete data;
            break;
        }
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void InstallModernEditControl(HWND hEdit, uint32_t dpi, const ThemePalette& palette) {
    if (!hEdit) return;
    int pad_x = DesignSystem::ScaleForDpi(8, dpi);
    int pad_y = DesignSystem::ScaleForDpi(4, dpi);
    int r = DesignSystem::ScaleForDpi(6, dpi);
    SendMessageW(hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(pad_x, pad_x));

    auto* data = new ModernEditData{false, false, pad_x, pad_y, r, palette};
    SetWindowSubclass(hEdit, ModernEditSubclassProc, SUBCLASS_MODERN_EDIT_ID, reinterpret_cast<DWORD_PTR>(data));
}

void UpdateModernEditPalette(HWND hEdit, uint32_t dpi, const ThemePalette& palette) {
    if (!hEdit) return;
    DWORD_PTR raw = 0;
    if (GetWindowSubclass(hEdit, ModernEditSubclassProc, SUBCLASS_MODERN_EDIT_ID, &raw) && raw != 0) {
        auto* data = reinterpret_cast<ModernEditData*>(raw);
        data->pad_x = DesignSystem::ScaleForDpi(8, dpi);
        data->pad_y = DesignSystem::ScaleForDpi(4, dpi);
        data->radius = DesignSystem::ScaleForDpi(6, dpi);
        data->palette = palette;
        SendMessageW(hEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(data->pad_x, data->pad_x));
    }
}

// Modern ComboBox Subclass: custom dark rounded box + crisp Gdiplus chevron (replaces classic Windows dropdown arrow)
constexpr UINT_PTR SUBCLASS_MODERN_COMBO_ID = 9002;

struct ModernComboData {
    bool is_focused{false};
    bool is_hovered{false};
    uint32_t dpi{96};
    int radius{6};
    ThemePalette palette{DesignSystem::GetRedesignPalette()};
    ColorRgba parent_bg{DesignSystem::GetRedesignPalette().app_background};
};

LRESULT CALLBACK ModernComboSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* data = reinterpret_cast<ModernComboData*>(dwRefData);
    if (!data) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    switch (uMsg) {
        case WM_SETFOCUS:
            data->is_focused = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case WM_KILLFOCUS:
            data->is_focused = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case WM_MOUSEMOVE:
            if (!data->is_hovered) {
                data->is_hovered = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hWnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            data->is_hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case CB_SETCURSEL: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            InvalidateRect(hWnd, nullptr, FALSE);
            return res;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCPAINT:
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP hBmp = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

                HBRUSH hParentBr = CreateSolidBrush(data->parent_bg.ToWin32ColorRef());
                FillRect(memDC, &rc, hParentBr);
                DeleteObject(hParentBr);

                bool is_enabled = IsWindowEnabled(hWnd) != FALSE;
                ColorRgba fill_rgba = data->palette.surface_elevated;
                ColorRgba border_rgba = !is_enabled ? data->palette.border_subtle
                                        : (data->is_focused ? data->palette.accent_primary
                                        : (data->is_hovered ? data->palette.border_focus
                                                            : data->palette.border_subtle));

                {
                    Gdiplus::Graphics g(memDC);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    float r = static_cast<float>(std::max(2, data->radius));
                    float inset = 1.0f;
                    float rw = std::max(1.0f, static_cast<float>(w) - 1.0f - inset * 2.0f);
                    float rh = std::max(1.0f, static_cast<float>(h) - 1.0f - inset * 2.0f);
                    float d = std::min(r * 2.0f, std::min(rw, rh));

                    Gdiplus::GraphicsPath path;
                    path.AddArc(inset, inset, d, d, 180.0f, 90.0f);
                    path.AddArc(inset + rw - d, inset, d, d, 270.0f, 90.0f);
                    path.AddArc(inset + rw - d, inset + rh - d, d, d, 0.0f, 90.0f);
                    path.AddArc(inset, inset + rh - d, d, d, 90.0f, 90.0f);
                    path.CloseFigure();

                    Gdiplus::SolidBrush fill_brush(Gdiplus::Color(255, fill_rgba.r, fill_rgba.g, fill_rgba.b));
                    g.FillPath(&fill_brush, &path);

                    if (data->is_focused && is_enabled) {
                        Gdiplus::Pen glow_pen(Gdiplus::Color(55, border_rgba.r, border_rgba.g, border_rgba.b), 3.0f);
                        g.DrawPath(&glow_pen, &path);
                    }
                    Gdiplus::Pen border_pen(Gdiplus::Color(255, border_rgba.r, border_rgba.g, border_rgba.b), 1.3f);
                    g.DrawPath(&border_pen, &path);

                    // Draw downward chevron on right
                    float cx = static_cast<float>(w - DesignSystem::ScaleForDpi(14, data->dpi));
                    float cy = static_cast<float>(h) * 0.5f;
                    float arm = static_cast<float>(DesignSystem::ScaleForDpi(4, data->dpi));
                    ColorRgba chev_col = is_enabled ? data->palette.text_secondary : data->palette.text_muted;
                    Gdiplus::Pen chev_pen(Gdiplus::Color(255, chev_col.r, chev_col.g, chev_col.b), 1.6f);
                    chev_pen.SetStartCap(Gdiplus::LineCapRound);
                    chev_pen.SetEndCap(Gdiplus::LineCapRound);
                    chev_pen.SetLineJoin(Gdiplus::LineJoinRound);
                    Gdiplus::PointF pts[3] = {
                        {cx - arm, cy - arm * 0.45f},
                        {cx,       cy + arm * 0.55f},
                        {cx + arm, cy - arm * 0.45f}
                    };
                    g.DrawLines(&chev_pen, pts, 3);
                }

                int sel = static_cast<int>(SendMessageW(hWnd, CB_GETCURSEL, 0, 0));
                if (sel >= 0) {
                    int len = static_cast<int>(SendMessageW(hWnd, CB_GETLBTEXTLEN, sel, 0));
                    if (len > 0) {
                        std::wstring text(static_cast<size_t>(len) + 1, L'\0');
                        SendMessageW(hWnd, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text.data()));
                        text.resize(wcslen(text.c_str()));

                        HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
                        HGDIOBJ oldFont = hFont ? SelectObject(memDC, hFont) : nullptr;
                        SetBkMode(memDC, TRANSPARENT);
                        SetTextColor(memDC, (is_enabled ? data->palette.text_primary : data->palette.text_muted).ToWin32ColorRef());
                        RECT text_rc{
                            DesignSystem::ScaleForDpi(10, data->dpi), 0,
                            std::max(DesignSystem::ScaleForDpi(12, data->dpi), w - DesignSystem::ScaleForDpi(26, data->dpi)), h
                        };
                        DrawTextW(memDC, text.c_str(), -1, &text_rc,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                        if (oldFont) SelectObject(memDC, oldFont);
                    }
                }

                BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBmp);
                DeleteObject(hBmp);
                DeleteDC(memDC);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, ModernComboSubclassProc, uIdSubclass);
            delete data;
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void InstallModernComboBox(HWND hCombo, uint32_t dpi, const ThemePalette& palette, ColorRgba parent_bg) {
    if (!hCombo) return;
    int r = DesignSystem::ScaleForDpi(6, dpi);
    auto* data = new ModernComboData{false, false, dpi, r, palette, parent_bg};
    SetWindowSubclass(hCombo, ModernComboSubclassProc, SUBCLASS_MODERN_COMBO_ID, reinterpret_cast<DWORD_PTR>(data));
}

// Modern CheckBox Subclass: custom 16x16 rounded box + Gdiplus checkmark + transparent parent_bg fill
constexpr UINT_PTR SUBCLASS_MODERN_CHECKBOX_ID = 9003;

struct ModernCheckBoxData {
    bool is_hovered{false};
    bool is_focused{false};
    uint32_t dpi{96};
    ThemePalette palette{DesignSystem::GetRedesignPalette()};
    ColorRgba parent_bg{DesignSystem::GetRedesignPalette().app_background};
};

LRESULT CALLBACK ModernCheckBoxSubclassProc(
    HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
    UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    auto* data = reinterpret_cast<ModernCheckBoxData*>(dwRefData);
    if (!data) return DefSubclassProc(hWnd, uMsg, wParam, lParam);

    switch (uMsg) {
        case WM_SETFOCUS:
            data->is_focused = true;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case WM_KILLFOCUS:
            data->is_focused = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case WM_MOUSEMOVE:
            if (!data->is_hovered) {
                data->is_hovered = true;
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hWnd, 0};
                TrackMouseEvent(&tme);
                InvalidateRect(hWnd, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            data->is_hovered = false;
            InvalidateRect(hWnd, nullptr, FALSE);
            break;
        case BM_SETCHECK: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            InvalidateRect(hWnd, nullptr, FALSE);
            return res;
        }
        case WM_LBUTTONUP: {
            LRESULT res = DefSubclassProc(hWnd, uMsg, wParam, lParam);
            InvalidateRect(hWnd, nullptr, FALSE);
            return res;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hWnd, &ps);
            RECT rc{};
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0) {
                HDC memDC = CreateCompatibleDC(hdc);
                HBITMAP hBmp = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ oldBmp = SelectObject(memDC, hBmp);

                HBRUSH hParentBr = CreateSolidBrush(data->parent_bg.ToWin32ColorRef());
                FillRect(memDC, &rc, hParentBr);
                DeleteObject(hParentBr);

                bool checked = (SendMessageW(hWnd, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool is_enabled = IsWindowEnabled(hWnd) != FALSE;

                int box_sz = DesignSystem::ScaleForDpi(16, data->dpi);
                float bx = 1.0f;
                float by = (static_cast<float>(h) - static_cast<float>(box_sz)) * 0.5f;
                float bw = static_cast<float>(box_sz);
                float br = static_cast<float>(DesignSystem::ScaleForDpi(4, data->dpi));

                {
                    Gdiplus::Graphics g(memDC);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);

                    Gdiplus::GraphicsPath path;
                    float d = br * 2.0f;
                    path.AddArc(bx, by, d, d, 180.0f, 90.0f);
                    path.AddArc(bx + bw - d, by, d, d, 270.0f, 90.0f);
                    path.AddArc(bx + bw - d, by + bw - d, d, d, 0.0f, 90.0f);
                    path.AddArc(bx, by + bw - d, d, d, 90.0f, 90.0f);
                    path.CloseFigure();

                    if (checked) {
                        ColorRgba acc = data->palette.accent_primary;
                        Gdiplus::SolidBrush fill_b(Gdiplus::Color(is_enabled ? 255 : 140, acc.r, acc.g, acc.b));
                        g.FillPath(&fill_b, &path);

                        Gdiplus::Pen chk_pen(Gdiplus::Color(255, 255, 255, 255), 1.8f);
                        chk_pen.SetStartCap(Gdiplus::LineCapRound);
                        chk_pen.SetEndCap(Gdiplus::LineCapRound);
                        chk_pen.SetLineJoin(Gdiplus::LineJoinRound);
                        Gdiplus::PointF pts[3] = {
                            {bx + bw * 0.24f, by + bw * 0.52f},
                            {bx + bw * 0.44f, by + bw * 0.72f},
                            {bx + bw * 0.78f, by + bw * 0.30f}
                        };
                        g.DrawLines(&chk_pen, pts, 3);
                    } else {
                        ColorRgba bg = data->palette.surface_elevated;
                        ColorRgba brd = data->is_hovered ? data->palette.border_focus : data->palette.border_subtle;
                        Gdiplus::SolidBrush fill_b(Gdiplus::Color(255, bg.r, bg.g, bg.b));
                        g.FillPath(&fill_b, &path);
                        Gdiplus::Pen brd_pen(Gdiplus::Color(255, brd.r, brd.g, brd.b), 1.3f);
                        g.DrawPath(&brd_pen, &path);
                    }

                    if (data->is_focused && is_enabled) {
                        ColorRgba acc = data->palette.accent_primary;
                        Gdiplus::Pen focus_pen(Gdiplus::Color(90, acc.r, acc.g, acc.b), 1.5f);
                        g.DrawPath(&focus_pen, &path);
                    }
                }

                wchar_t buf[256]{};
                int tlen = GetWindowTextW(hWnd, buf, 255);
                if (tlen > 0) {
                    HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hWnd, WM_GETFONT, 0, 0));
                    HGDIOBJ oldFont = hFont ? SelectObject(memDC, hFont) : nullptr;
                    SetBkMode(memDC, TRANSPARENT);
                    SetTextColor(memDC, (is_enabled ? data->palette.text_primary : data->palette.text_muted).ToWin32ColorRef());
                    RECT text_rc{
                        static_cast<LONG>(bx + bw) + DesignSystem::ScaleForDpi(8, data->dpi), 0, w, h
                    };
                    DrawTextW(memDC, buf, tlen, &text_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    if (oldFont) SelectObject(memDC, oldFont);
                }

                BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
                SelectObject(memDC, oldBmp);
                DeleteObject(hBmp);
                DeleteDC(memDC);
            }
            EndPaint(hWnd, &ps);
            return 0;
        }
        case WM_NCDESTROY:
            RemoveWindowSubclass(hWnd, ModernCheckBoxSubclassProc, uIdSubclass);
            delete data;
            break;
    }
    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

void InstallModernCheckBox(HWND hCheck, uint32_t dpi, const ThemePalette& palette, ColorRgba parent_bg) {
    if (!hCheck) return;
    auto* data = new ModernCheckBoxData{false, false, dpi, palette, parent_bg};
    SetWindowSubclass(hCheck, ModernCheckBoxSubclassProc, SUBCLASS_MODERN_CHECKBOX_ID, reinterpret_cast<DWORD_PTR>(data));
}

bool SyncTaskbarProgress(HWND hwnd, ITaskbarList3* pTaskbar, const DesktopView& view, Win32WindowContext* ctx) {
    if (!hwnd || !pTaskbar) return false;
    auto info = view.ComputeTaskbarProgress();
    
    // Prevent flickering: check if taskbar state or relevant values actually changed and were successfully applied previously
    if (ctx && ctx->taskbar_info_initialized) {
        if (ctx->last_taskbar_info.state == info.state &&
            ctx->last_taskbar_info.total_bytes == info.total_bytes) {
            if (info.state == TaskbarProgressState::kNoProgress ||
                info.state == TaskbarProgressState::kIndeterminate ||
                info.state == TaskbarProgressState::kError) {
                return true; // Nothing changed, skip redundant Win32 ITaskbarList3 COM dispatch
            }
            if (info.state == TaskbarProgressState::kNormal || info.state == TaskbarProgressState::kPaused) {
                // If progress value delta is identical or negligible, skip
                if (ctx->last_taskbar_info.completed_bytes == info.completed_bytes) {
                    return true;
                }
            }
        }
    }

    HRESULT hrState = S_OK;
    HRESULT hrVal = S_OK;

    switch (info.state) {
        case TaskbarProgressState::kNoProgress: {
            // Explicitly clear progress value and reset state to TBPF_NOPROGRESS with verification and retry
            bool reset_ok = false;
            for (int attempt = 0; attempt < 3; ++attempt) {
                hrVal = pTaskbar->SetProgressValue(hwnd, 0, 1);
                hrState = pTaskbar->SetProgressState(hwnd, TBPF_NOPROGRESS);
                if (SUCCEEDED(hrState) && SUCCEEDED(hrVal)) {
                    reset_ok = true;
                    break;
                }
                Sleep(10);
            }
            RADIUM_LOG_INFO("UI", "Taskbar progress cleared via TBPF_NOPROGRESS (hrState=" + 
                             std::to_string(static_cast<long>(hrState)) + ", success=" + (reset_ok ? "true" : "false") + ")");
            break;
        }
        case TaskbarProgressState::kIndeterminate:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_INDETERMINATE);
            break;
        case TaskbarProgressState::kNormal:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_NORMAL);
            hrVal = pTaskbar->SetProgressValue(hwnd, info.completed_bytes, std::max<uint64_t>(1ULL, info.total_bytes));
            break;
        case TaskbarProgressState::kPaused:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_PAUSED);
            if (info.total_bytes > 0) {
                hrVal = pTaskbar->SetProgressValue(hwnd, info.completed_bytes, info.total_bytes);
            }
            break;
        case TaskbarProgressState::kError:
            hrState = pTaskbar->SetProgressState(hwnd, TBPF_ERROR);
            break;
    }

    bool success = SUCCEEDED(hrState) && SUCCEEDED(hrVal);
    if (ctx) {
        if (success) {
            ctx->last_taskbar_info = info;
            ctx->taskbar_info_initialized = true;
        } else {
            // If the COM call failed (e.g., explorer restarting, E_FAIL, or invalid hwnd),
            // invalidate cached state so the next UI timer tick will retry.
            ctx->taskbar_info_initialized = false;
        }
    }

    return success;
}

RECT ToWinRect(const RectI& r) {
    return RECT{r.left, r.top, r.right, r.bottom};
}

void FillRectSolid(HDC hdc, const RECT& rc, ColorRgba col) {
    HBRUSH br = CreateSolidBrush(col.ToWin32ColorRef());
    FillRect(hdc, &rc, br);
    DeleteObject(br);
}

void FrameRectBorder(HDC hdc, const RECT& rc, ColorRgba col, int thickness = 1) {
    HBRUSH br = CreateSolidBrush(col.ToWin32ColorRef());
    RECT top_r{rc.left, rc.top, rc.right, rc.top + thickness};
    RECT bot_r{rc.left, rc.bottom - thickness, rc.right, rc.bottom};
    RECT lft_r{rc.left, rc.top, rc.left + thickness, rc.bottom};
    RECT rgt_r{rc.right - thickness, rc.top, rc.right, rc.bottom};
    FillRect(hdc, &top_r, br);
    FillRect(hdc, &bot_r, br);
    FillRect(hdc, &lft_r, br);
    FillRect(hdc, &rgt_r, br);
    DeleteObject(br);
}

void ApplyWindowGlassBackdropWin32(HWND hwnd, const ThemePalette& palette, bool is_transient_dialog = false) {
    if (!hwnd || !IsWindow(hwnd)) return;

    // Match native title bars and non-client controls to the active Radium theme.
    BOOL use_dark = palette.mode != ThemeMode::kLight;
    DwmSetWindowAttribute(hwnd, 20, &use_dark, sizeof(use_dark));

    // 2. Windows 11 rounded corners (DWMWA_WINDOW_CORNER_PREFERENCE = 33, 2 = DWMWCP_ROUND)
    int corner_pref = 2;
    DwmSetWindowAttribute(hwnd, 33, &corner_pref, sizeof(corner_pref));
    if (use_dark) {
        COLORREF border_col = RGB(0x2E, 0x2E, 0x36);
        DwmSetWindowAttribute(hwnd, 34, &border_col, sizeof(border_col));
    }

    // 3. Titlebar styling
    COLORREF titlebar_col = palette.app_background.ToWin32ColorRef();
    DwmSetWindowAttribute(hwnd, 35, &titlebar_col, sizeof(titlebar_col));
    COLORREF titletext_col = palette.text_primary.ToWin32ColorRef();
    DwmSetWindowAttribute(hwnd, 36, &titletext_col, sizeof(titletext_col));

    // 4. Windows 11 Mica / Acrylic backdrop (DWMWA_SYSTEMBACKDROP_TYPE = 38)
    // 2 = DWMSBT_MAINWINDOW (Mica), 3 = DWMSBT_TRANSIENTWINDOW (Acrylic)
    int backdrop_type = is_transient_dialog ? 3 : 2;
    HRESULT hr = DwmSetWindowAttribute(hwnd, 38, &backdrop_type, sizeof(backdrop_type));
    if (FAILED(hr)) {
        // Fallback for Windows 11 21H2 (Build 22000): DWMWA_MICA_EFFECT = 1029
        BOOL mica_on = TRUE;
        DwmSetWindowAttribute(hwnd, 1029, &mica_on, sizeof(mica_on));
    }
    ApplyNativeWindowTheme(hwnd, palette);
}

void DrawGlassPanelGdi(HDC hdc, const RECT& rc, int radius, ColorRgba fill, ColorRgba border, int border_w = 1, bool add_top_specular = true) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;

    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    HRGN hRgn = (radius > 0) ? CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius, radius) : nullptr;
    if (hRgn) {
        SelectClipRgn(hdc, hRgn);
    }

    HDC memDC = CreateCompatibleDC(hdc);
    if (memDC) {
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = 1;
        bmi.bmiHeader.biHeight = 1;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (hbm && bits) {
            uint8_t a = fill.a;
            uint8_t pr = static_cast<uint8_t>((static_cast<uint32_t>(fill.r) * a) / 255u);
            uint8_t pg = static_cast<uint8_t>((static_cast<uint32_t>(fill.g) * a) / 255u);
            uint8_t pb = static_cast<uint8_t>((static_cast<uint32_t>(fill.b) * a) / 255u);
            *reinterpret_cast<uint32_t*>(bits) = (static_cast<uint32_t>(a) << 24) |
                                                  (static_cast<uint32_t>(pr) << 16) |
                                                  (static_cast<uint32_t>(pg) << 8) |
                                                  static_cast<uint32_t>(pb);

            HGDIOBJ oldBm = SelectObject(memDC, hbm);
            BLENDFUNCTION bf{};
            bf.BlendOp = AC_SRC_OVER;
            bf.BlendFlags = 0;
            bf.SourceConstantAlpha = 255;
            bf.AlphaFormat = AC_SRC_ALPHA;

            AlphaBlend(hdc, rc.left, rc.top, w, h, memDC, 0, 0, 1, 1, bf);

            if (add_top_specular && h > 6) {
                uint8_t ha = 36;
                *reinterpret_cast<uint32_t*>(bits) = (static_cast<uint32_t>(ha) << 24) |
                                                      (static_cast<uint32_t>(ha) << 16) |
                                                      (static_cast<uint32_t>(ha) << 8) |
                                                      static_cast<uint32_t>(ha);
                int hl_margin = (radius > 4) ? (radius / 2) : 2;
                int hl_w = std::max(2, w - hl_margin * 2);
                AlphaBlend(hdc, rc.left + hl_margin, rc.top + 1, hl_w, 1, memDC, 0, 0, 1, 1, bf);
            }

            SelectObject(memDC, oldBm);
            DeleteObject(hbm);
        }
        DeleteDC(memDC);
    }

    if (hRgn) {
        SelectClipRgn(hdc, nullptr);
        DeleteObject(hRgn);
    }

    if (border_w > 0) {
        HPEN hPen = CreatePen(PS_SOLID, border_w, border.ToWin32ColorRef());
        HGDIOBJ oldPen = SelectObject(hdc, hPen);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));

        if (radius > 0) {
            RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);
        } else {
            Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        }

        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(hPen);
    }
}

bool SelectFolderWithDialogWin32(HWND hwnd, std::string& io_folder) {
    wchar_t display_name[MAX_PATH]{};
    BROWSEINFOW bi{};
    bi.hwndOwner = hwnd;
    bi.pszDisplayName = display_name;
    bi.lpszTitle = L"Select Default Download Directory for Radium";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return false;
    wchar_t path_buf[MAX_PATH]{};
    bool ok = (SHGetPathFromIDListW(pidl, path_buf) != FALSE);
    CoTaskMemFree(pidl);
    if (ok && path_buf[0] != L'\0') {
        io_folder = WToUtf8(path_buf);
        return true;
    }
    return false;
}

struct QualityChoice {
    std::string id;
    std::string label;
    std::string resolution;
    int height{0};
    int64_t size{-1};
    bool size_estimated{false};
    std::string video_filter;
    std::string audio_filter{"best"};
    std::string video_url;
    std::string paired_audio_url;
    bool is_audio_only{false};
    std::string ext{"mp4"};
    int width{0};
    int itag{0};
};

struct NewDownloadDialogContext {
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    HWND hDlg{nullptr};
    HWND hUrlEdit{nullptr};
    HWND hFilenameEdit{nullptr};
    HWND hSaveDirEdit{nullptr};
    HWND hBrowseBtn{nullptr};
    HWND hQualityCombo{nullptr};
    HWND hFileSizeLabel{nullptr};
    HWND hSpaceLabel{nullptr};
    HWND hStartChk{nullptr};
    HWND hScheduleChk{nullptr};
    HWND hScheduleTimeEdit{nullptr};
    HWND hDownloadBtn{nullptr};
    HWND hCancelBtn{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontTitle{nullptr};
    HFONT hFontSmall{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hEditBrush{nullptr};
    std::string existing_task_id;
    std::string speculative_tx_id;
    std::optional<TaskModel> draft_task;
    std::string initial_filename;
    std::string initial_save_dir;
    int64_t initial_file_size{-1};
    bool probed_range_supported{false};
    bool probed_has_range_info{false};
    uint32_t dpi{96};
    bool submitted{false};
    bool is_submitting{false};
    std::vector<QualityChoice> quality_choices;
    bool has_manifest_qualities{false};
    std::string bound_url;

    uint64_t current_probe_id{0};
    std::shared_ptr<std::atomic<bool>> dlg_alive{std::make_shared<std::atomic<bool>>(true)};
};

void DrawRoundedBoxGdi(HDC hdc, const RECT& rc, int radius, ColorRgba bg, ColorRgba border, int border_w);

LRESULT CALLBACK NewDownloadDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<NewDownloadDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<NewDownloadDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_TIMER: {
            if (wParam == 5020 && ctx && ctx->controller && !ctx->speculative_tx_id.empty()) {
                if (auto snap = ctx->controller->GetSpeculativeSnapshot(ctx->speculative_tx_id)) {
                    if (snap->total_bytes > 0 && ctx->hFileSizeLabel) {
                        std::string sz_str = DesktopView::FormatSizePretty(snap->total_bytes);
                        std::wstring txt = L"Download Size: " + Utf8ToW(sz_str);
                        SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                    }
                    if (snap->range_supported) {
                        ctx->probed_has_range_info = true;
                        ctx->probed_range_supported = true;
                    }
                }
                return 0;
            }
            break;
        }
        case WM_RADIUM_PROBE_COMPLETE: {
            auto* p = reinterpret_cast<ProbeResultPayload*>(lParam);
            if (!p) return 0;
            std::unique_ptr<ProbeResultPayload> auto_cleanup(p);
            if (ctx && ctx->dlg_alive && *(ctx->dlg_alive) && p->request_id == ctx->current_probe_id) {
                if (p->success && p->content_length > 0) {
                    ctx->initial_file_size = p->content_length;
                    ctx->probed_range_supported = p->range_supported;
                    ctx->probed_has_range_info = true;
                    std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(p->content_length));
                    std::wstring txt = L"Download Size: " + Utf8ToW(sz_str);
                    SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                } else {
                    SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Unknown size");
                }
                if (!p->final_filename.empty()) {
                    wchar_t fn_buf[512]{};
                    GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                    std::string cur_fn = WToUtf8(fn_buf);
                    if (cur_fn.empty() || cur_fn == "download") {
                        SetWindowTextW(ctx->hFilenameEdit, Utf8ToW(p->final_filename).c_str());
                    }
                }
                if (ctx->existing_task_id.empty() &&
                    ctx->controller->GetSettings().conflict_policy == ConflictPolicy::kPrompt) {
                    wchar_t url_buf[2048]{}, fn_buf[512]{}, dir_buf[MAX_PATH]{};
                    GetWindowTextW(ctx->hUrlEdit, url_buf, 2047);
                    GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                    GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                    TaskModel check_t = ctx->draft_task.value_or(TaskModel{});
                    check_t.url = WToUtf8(url_buf);
                    check_t.file_name = WToUtf8(fn_buf);
                    check_t.save_dir = WToUtf8(dir_buf);
                    auto report = ctx->controller->InspectIncomingDownload(check_t);
                    HWND owner = GetWindow(hwnd, GW_OWNER);
                    if (report.has_conflict() && owner && IsWindow(owner)) {
                        if (report.kind == DownloadController::IncomingConflictKind::kActiveOrQueuedSameDownload &&
                            report.matched_task.has_value()) {
                            ctx->view->SelectTask(report.matched_task->task_id);
                            ctx->view->ScrollTaskIntoView(report.matched_task->task_id, true);
                        } else {
                            auto* item = new Win32WindowContext::PendingConflictItem;
                            item->draft_task = std::move(check_t);
                            item->report = std::move(report);
                            item->start_immediately = true;
                            if (!PostMessageW(owner, WM_RADIUM_SHOW_CONFLICT, 0, reinterpret_cast<LPARAM>(item))) {
                                delete item;
                            }
                        }
                        ctx->submitted = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }
            }
            return 0;
        }
        case DM_GETDEFID: {
            return MAKELRESULT(5008, DC_HASDEFID);
        }
        case DM_SETDEFID: {
            return TRUE;
        }
        case WM_CREATE: {
            ctx->hDlg = hwnd;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;
            ctx->hFontBody = CreateFontW(
                -DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold = CreateFontW(
                -DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontTitle = CreateFontW(
                -DesignSystem::ScaleForDpi(19, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI Variable Display");
            ctx->hFontSmall = CreateFontW(
                -DesignSystem::ScaleForDpi(12, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            const auto& palette = ctx->view->GetPalette();
            ctx->hBgBrush = CreateSolidBrush(palette.app_background.ToWin32ColorRef());
            ctx->hEditBrush = CreateSolidBrush(palette.surface_elevated.ToWin32ColorRef());

            HINSTANCE hInst = GetModuleHandleW(nullptr);

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int full_w = DesignSystem::ScaleForDpi(532, dpi);
            int edit_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kControlHeight, dpi);

            // URL Edit (Row 0: Label at 76, Control at 98)
            int y_url = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(0), dpi);
            ctx->hUrlEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, y_url, full_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5001)), hInst, nullptr);
            SendMessageW(ctx->hUrlEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hUrlEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Paste or enter download URL (HTTP, HTTPS, FTP, or video link)..."));
            InstallModernEditControl(ctx->hUrlEdit, dpi, ctx->view->GetPalette());

            // Filename Edit (Row 1: Label at 136, Control at 158)
            int y_fn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(1), dpi);
            ctx->hFilenameEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, y_fn, full_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5002)), hInst, nullptr);
            SendMessageW(ctx->hFilenameEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hFilenameEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"File name (leave blank to auto-detect from server)"));
            InstallModernEditControl(ctx->hFilenameEdit, dpi, ctx->view->GetPalette());

            // Save Dir Edit & Browse Button (Row 2: Label at 196, Control at 218)
            auto s = ctx->controller->GetSettings();
            const auto& preferred_dir = s.last_download_dir.empty() ? s.default_download_dir : s.last_download_dir;
            std::wstring def_dir = ctx->initial_save_dir.empty() ? Utf8ToW(preferred_dir) : Utf8ToW(ctx->initial_save_dir);
            int y_dir = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(2), dpi);
            int browse_w = DesignSystem::ScaleForDpi(100, dpi);
            int dir_w = full_w - browse_w - DesignSystem::ScaleForDpi(10, dpi);
            ctx->hSaveDirEdit = CreateWindowExW(0, L"EDIT", def_dir.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, y_dir, dir_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5003)), hInst, nullptr);
            SendMessageW(ctx->hSaveDirEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hSaveDirEdit, dpi, ctx->view->GetPalette());

            ctx->hBrowseBtn = CreateWindowExW(0, L"BUTTON", L"Browse...",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                pad_x + dir_w + DesignSystem::ScaleForDpi(10, dpi), y_dir, browse_w, edit_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5004)), hInst, nullptr);

            // Quality / Format Combobox (Row 3: Label at 256, Control at 278)
            int y_q = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(3), dpi);
            int q_w = DesignSystem::ScaleForDpi(280, dpi);
            ctx->hQualityCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                pad_x, y_q, q_w, DesignSystem::ScaleForDpi(180, dpi), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5005)), hInst, nullptr);
            SendMessageW(ctx->hQualityCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernComboBox(ctx->hQualityCombo, dpi, ctx->view->GetPalette(), ctx->view->GetPalette().app_background);
            bool is_video_stream = false;
            std::optional<TaskModel> existing_task;
            if (!ctx->existing_task_id.empty()) {
                auto t = ctx->controller->GetTask(ctx->existing_task_id);
                if (t.has_value()) {
                    existing_task = t;
                    if (ctx->initial_file_size <= 0 && t->total_bytes > 0) {
                        ctx->initial_file_size = static_cast<int64_t>(t->total_bytes);
                    }
                    if (t->task_type == TaskType::kStreamHls || t->task_type == TaskType::kStreamDash ||
                        t->category == "Video" || PathSanitizer::IsWebpageWatchUrl(t->url)) {
                        is_video_stream = true;
                    }
                }
            } else if (ctx->draft_task.has_value()) {
                existing_task = ctx->draft_task;
                if (ctx->initial_file_size <= 0 && ctx->draft_task->total_bytes > 0) {
                    ctx->initial_file_size = static_cast<int64_t>(ctx->draft_task->total_bytes);
                }
                if (ctx->draft_task->task_type == TaskType::kStreamHls || ctx->draft_task->task_type == TaskType::kStreamDash ||
                    ctx->draft_task->category == "Video" || PathSanitizer::IsWebpageWatchUrl(ctx->draft_task->url)) {
                    is_video_stream = true;
                }
            }

            ctx->quality_choices.clear();
            ctx->has_manifest_qualities = false;
            if (existing_task.has_value() && !existing_task->stream_options.cached_manifest_body.empty() &&
                (existing_task->stream_options.cached_manifest_body.front() == '{' || existing_task->stream_options.cached_manifest_body.front() == '[')) {
                try {
                    auto root = radium::json::Parse(existing_task->stream_options.cached_manifest_body);
                    if (root.is_object() && root["qualities"].is_array()) {
                        for (const auto& q : root["qualities"].as_array()) {
                            QualityChoice qc;
                            qc.id = q["id"].as_string();
                            qc.label = q["label"].as_string();
                            qc.resolution = q["resolution"].as_string();
                            qc.height = static_cast<int>(q["height"].as_int64(0));
                            qc.width = static_cast<int>(q["width"].as_int64(0));
                            qc.itag = static_cast<int>(q["itag"].as_int64(0));
                            qc.size = q["size"].as_int64(-1);
                            qc.size_estimated = q["sizeEstimated"].as_bool(false);
                            qc.video_filter = q["videoFilter"].as_string(qc.id);
                            qc.audio_filter = "best";
                            qc.video_url = q["videoUrl"].as_string();
                            qc.paired_audio_url = q["pairedAudioUrl"].as_string();
                            qc.is_audio_only = q["isAudioOnly"].as_bool(false);
                            qc.ext = q["ext"].as_string(qc.is_audio_only ? "m4a" : "mp4");
                            if (qc.label.empty()) {
                                qc.label = !qc.resolution.empty() ? qc.resolution : qc.id;
                            }
                            ctx->quality_choices.push_back(std::move(qc));
                        }
                        if (!ctx->quality_choices.empty()) {
                            ctx->has_manifest_qualities = true;
                        }
                    }
                } catch (...) {}
            }

            if (ctx->quality_choices.empty()) {
                if (is_video_stream) {
                    ctx->quality_choices.push_back({"best", "Best Available Quality (Auto)", "Auto", 0, ctx->initial_file_size, true, "best", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"1080p", "1080p Full HD", "1080p", 1080, -1, false, "1080p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"720p", "720p HD", "720p", 720, -1, false, "720p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"480p", "480p SD", "480p", 480, -1, false, "480p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"360p", "360p Standard", "360p", 360, -1, false, "360p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"144p", "144p Compact", "144p", 144, -1, false, "144p", "best", "", "", false, "mp4"});
                    ctx->quality_choices.push_back({"audio", "Audio Only (M4A)", "Audio", 0, -1, false, "audio", "best", "", "", true, "m4a"});
                } else {
                    ctx->quality_choices.push_back({"original", "Original / Direct File (Server Default)", "", 0, ctx->initial_file_size, false, "best", "best", "", "", false, ""});
                }
            }

            int sel_idx = 0;
            std::string initial_vfilter = existing_task.has_value() ? existing_task->stream_options.video_filter : "best";
            std::string low_init = ToLowerStr(initial_vfilter);
            int target_itag = 0;
            if (existing_task.has_value() && !existing_task->stream_options.cached_manifest_body.empty() &&
                (existing_task->stream_options.cached_manifest_body.front() == '{' || existing_task->stream_options.cached_manifest_body.front() == '[')) {
                try {
                    auto mroot = radium::json::Parse(existing_task->stream_options.cached_manifest_body);
                    if (mroot.is_object()) {
                        if (mroot["selectedItag"].as_int64(0) > 0) target_itag = static_cast<int>(mroot["selectedItag"].as_int64(0));
                        else if (mroot["itag"].as_int64(0) > 0) target_itag = static_cast<int>(mroot["itag"].as_int64(0));
                        if (low_init.empty() && mroot["resolution"].is_string()) {
                            low_init = ToLowerStr(mroot["resolution"].as_string());
                        }
                    }
                } catch (...) {}
            }

            bool matched_explicit = false;
            for (size_t i = 0; i < ctx->quality_choices.size(); ++i) {
                const auto& qc = ctx->quality_choices[i];
                std::wstring disp = Utf8ToW(qc.label);
                if (qc.size > 0) {
                    std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
                    disp += qc.size_estimated ? (L" (~" + Utf8ToW(sz_str) + L" est.)") : (L" (" + Utf8ToW(sz_str) + L")");
                }
                SendMessageW(ctx->hQualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(disp.c_str()));
                if (!matched_explicit) {
                    if (target_itag > 0 && qc.itag == target_itag) {
                        sel_idx = static_cast<int>(i);
                        matched_explicit = true;
                    } else if (!low_init.empty() && low_init != "best" && low_init != "auto") {
                        std::string low_id = ToLowerStr(qc.id);
                        std::string low_res = ToLowerStr(qc.resolution);
                        std::string low_vf = ToLowerStr(qc.video_filter);
                        if (low_id == low_init || low_res == low_init || low_vf == low_init ||
                            (!low_id.empty() && low_init.find(low_id) != std::string::npos) ||
                            (!low_init.empty() && low_id.find(low_init) != std::string::npos)) {
                            sel_idx = static_cast<int>(i);
                            matched_explicit = true;
                        }
                    }
                }
            }
            if (!matched_explicit && !ctx->quality_choices.empty()) {
                sel_idx = 0;
                for (size_t i = 0; i < ctx->quality_choices.size(); ++i) {
                    if (!ctx->quality_choices[i].is_audio_only) {
                        sel_idx = static_cast<int>(i);
                        break;
                    }
                }
            }
            SendMessageW(ctx->hQualityCombo, CB_SETCURSEL, sel_idx, 0);

            // File Size Label
            std::wstring sz_text = L"Size: Unknown";
            if (sel_idx < static_cast<int>(ctx->quality_choices.size()) && ctx->quality_choices[sel_idx].size > 0) {
                const auto& qc = ctx->quality_choices[sel_idx];
                std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
                sz_text = std::wstring(L"Size: ") + (qc.size_estimated ? L"~" : L"") + Utf8ToW(sz_str) + (qc.size_estimated ? L" (estimated)" : L"");
            } else if (ctx->initial_file_size > 0) {
                sz_text = L"Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(ctx->initial_file_size)));
            }
            int sz_x = pad_x + q_w + DesignSystem::ScaleForDpi(16, dpi);
            int sz_w = full_w - q_w - DesignSystem::ScaleForDpi(16, dpi);
            ctx->hFileSizeLabel = CreateWindowExW(0, L"STATIC", sz_text.c_str(),
                WS_CHILD | WS_VISIBLE,
                sz_x, y_q + DesignSystem::ScaleForDpi(2, dpi), sz_w, DesignSystem::ScaleForDpi(22, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5012)), hInst, nullptr);
            SendMessageW(ctx->hFileSizeLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);

            // Disk Space Label
            std::string space_text = "Disk space: Checking...";
            auto space_res = PathSanitizer::CheckAvailableDiskSpace(s.default_download_dir, 1024);
            if (space_res.ok() && space_res.value.has_value()) {
                space_text = "Free space: " + DesktopView::FormatSizePretty(*space_res.value) + " available on destination drive";
            }
            int y_space = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kSpaceLabelTop, dpi);
            ctx->hSpaceLabel = CreateWindowExW(0, L"STATIC", Utf8ToW(space_text).c_str(),
                WS_CHILD | WS_VISIBLE,
                pad_x, y_space, full_w, DesignSystem::ScaleForDpi(20, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5006)), hInst, nullptr);
            SendMessageW(ctx->hSpaceLabel, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);

            // Start Immediately Checkbox
            int y_start = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kStartChkTop, dpi);
            int start_w = DesignSystem::ScaleForDpi(206, dpi);
            ctx->hStartChk = CreateWindowExW(0, L"BUTTON", L"Start download immediately",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                pad_x, y_start, start_w, DesignSystem::ScaleForDpi(24, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5007)), hInst, nullptr);
            SendMessageW(ctx->hStartChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hStartChk, BM_SETCHECK, s.auto_start_downloads ? BST_CHECKED : BST_UNCHECKED, 0);
            InstallModernCheckBox(ctx->hStartChk, dpi, ctx->view->GetPalette(), ctx->view->GetPalette().app_background);

            // Schedule Checkbox & Time Edit
            int y_sched = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kScheduleChkTop, dpi);
            int sched_w = DesignSystem::ScaleForDpi(130, dpi);
            int sched_x = pad_x + start_w + DesignSystem::ScaleForDpi(8, dpi);
            ctx->hScheduleChk = CreateWindowExW(0, L"BUTTON", L"Schedule at",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                sched_x, y_sched, sched_w, DesignSystem::ScaleForDpi(24, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5010)), hInst, nullptr);
            SendMessageW(ctx->hScheduleChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernCheckBox(ctx->hScheduleChk, dpi, ctx->view->GetPalette(), ctx->view->GetPalette().app_background);

            ctx->hScheduleTimeEdit = CreateWindowExW(0, L"EDIT", L"23:00",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | WS_DISABLED,
                sched_x + sched_w + DesignSystem::ScaleForDpi(8, dpi), y_sched,
                DesignSystem::ScaleForDpi(72, dpi), DesignSystem::ScaleForDpi(24, dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5011)), hInst, nullptr);
            SendMessageW(ctx->hScheduleTimeEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hScheduleTimeEdit, dpi, ctx->view->GetPalette());

            // Download & Cancel Action Buttons
            int y_btn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonsTop, dpi);
            int btn_action_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonHeight, dpi);
            int cancel_w = DesignSystem::ScaleForDpi(100, dpi);
            int dl_btn_w = DesignSystem::ScaleForDpi(124, dpi);
            int cancel_x = pad_x + full_w - cancel_w;
            int dl_x = cancel_x - dl_btn_w - DesignSystem::ScaleForDpi(10, dpi);

            ctx->hDownloadBtn = CreateWindowExW(0, L"BUTTON", L"Download",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                dl_x, y_btn, dl_btn_w, btn_action_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5008)), hInst, nullptr);

            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                cancel_x, y_btn, cancel_w, btn_action_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(5009)), hInst, nullptr);

            return 0;
        }
        case WM_SIZE: {
            if (!ctx || !ctx->hUrlEdit) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int client_w = rc.right - rc.left;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;
            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int full_w = client_w - pad_x * 2;
            if (full_w < DesignSystem::ScaleForDpi(300, dpi)) full_w = DesignSystem::ScaleForDpi(300, dpi);
            int edit_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kControlHeight, dpi);

            // Row 0: URL Edit
            int y_url = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(0), dpi);
            SetWindowPos(ctx->hUrlEdit, nullptr, pad_x, y_url, full_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 1: Filename Edit
            int y_fn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(1), dpi);
            SetWindowPos(ctx->hFilenameEdit, nullptr, pad_x, y_fn, full_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 2: Save Location & Browse
            int y_dir = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(2), dpi);
            int browse_w = DesignSystem::ScaleForDpi(100, dpi);
            int dir_w = full_w - browse_w - DesignSystem::ScaleForDpi(10, dpi);
            SetWindowPos(ctx->hSaveDirEdit, nullptr, pad_x, y_dir, dir_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hBrowseBtn, nullptr, pad_x + dir_w + DesignSystem::ScaleForDpi(10, dpi), y_dir, browse_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 3: Quality Combo & Size Label
            int y_q = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::ControlTop(3), dpi);
            int q_w = DesignSystem::ScaleForDpi(280, dpi);
            if (q_w > full_w - DesignSystem::ScaleForDpi(160, dpi)) q_w = full_w - DesignSystem::ScaleForDpi(160, dpi);
            SetWindowPos(ctx->hQualityCombo, nullptr, pad_x, y_q, q_w, DesignSystem::ScaleForDpi(180, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            int sz_x = pad_x + q_w + DesignSystem::ScaleForDpi(16, dpi);
            int sz_w = full_w - q_w - DesignSystem::ScaleForDpi(16, dpi);
            SetWindowPos(ctx->hFileSizeLabel, nullptr, sz_x, y_q + DesignSystem::ScaleForDpi(2, dpi), sz_w, DesignSystem::ScaleForDpi(22, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 4: Space Label
            int y_space = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kSpaceLabelTop, dpi);
            SetWindowPos(ctx->hSpaceLabel, nullptr, pad_x, y_space, full_w, DesignSystem::ScaleForDpi(20, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 5: Start & Schedule Checkboxes
            int y_start = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kStartChkTop, dpi);
            int start_w = DesignSystem::ScaleForDpi(206, dpi);
            SetWindowPos(ctx->hStartChk, nullptr, pad_x, y_start, start_w, DesignSystem::ScaleForDpi(24, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            int y_sched = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kScheduleChkTop, dpi);
            int sched_w = DesignSystem::ScaleForDpi(130, dpi);
            int sched_x = pad_x + start_w + DesignSystem::ScaleForDpi(8, dpi);
            SetWindowPos(ctx->hScheduleChk, nullptr, sched_x, y_sched, sched_w, DesignSystem::ScaleForDpi(24, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hScheduleTimeEdit, nullptr, sched_x + sched_w + DesignSystem::ScaleForDpi(8, dpi), y_sched, DesignSystem::ScaleForDpi(72, dpi), DesignSystem::ScaleForDpi(24, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            // Row 6: Buttons aligned to bottom right
            int y_btn = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonsTop, dpi);
            int btn_action_h = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonHeight, dpi);
            int cancel_w = DesignSystem::ScaleForDpi(100, dpi);
            int dl_btn_w = DesignSystem::ScaleForDpi(124, dpi);
            int cancel_x = pad_x + full_w - cancel_w;
            int dl_x = cancel_x - dl_btn_w - DesignSystem::ScaleForDpi(10, dpi);
            SetWindowPos(ctx->hCancelBtn, nullptr, cancel_x, y_btn, cancel_w, btn_action_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hDownloadBtn, nullptr, dl_x, y_btn, dl_btn_w, btn_action_h, SWP_NOZORDER | SWP_NOACTIVATE);

            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;
            HDC hdcBtn = dis->hDC;
            RECT rcBtn = dis->rcItem;
            bool is_selected = (dis->itemState & ODS_SELECTED) != 0;
            bool is_focused = (dis->itemState & ODS_FOCUS) != 0;
            uint32_t btn_dpi = ctx->dpi ? ctx->dpi : 96;
            FillRect(hdcBtn, &rcBtn, ctx->hBgBrush);

            if (dis->CtlID == 5008) { // Download button (Coral accent #FF4757)
                int btn_r = DesignSystem::ScaleForDpi(8, btn_dpi);
                const auto& accent = ctx->view->GetPalette().accent_primary;
                ColorRgba bg_col = is_selected ? accent.Interpolate(ColorRgba{0, 0, 0, 255}, 0.15f) : (is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.12f) : accent);
                ColorRgba border_col = is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.32f) : bg_col;
                DrawRoundedBoxGdi(hdcBtn, rcBtn, btn_r, bg_col, border_col, is_focused ? 1 : 0);
                SelectObject(hdcBtn, ctx->hFontBold);
                SetTextColor(hdcBtn, RGB(255, 255, 255));
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Download", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 5009) { // Cancel button (Glass elevated)
                int btn_r = DesignSystem::ScaleForDpi(8, btn_dpi);
                const auto& pal = ctx->view->GetPalette();
                ColorRgba bg_col = is_selected ? pal.surface_pressed : pal.surface_primary;
                ColorRgba border_col = is_focused ? pal.border_focus : pal.border_subtle;
                DrawRoundedBoxGdi(hdcBtn, rcBtn, btn_r, bg_col, border_col, 1);
                SelectObject(hdcBtn, ctx->hFontBody);
                SetTextColor(hdcBtn, pal.text_primary.ToWin32ColorRef());
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Cancel", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 5004) { // Browse button (Glass secondary)
                int btn_r = DesignSystem::ScaleForDpi(6, btn_dpi);
                const auto& pal = ctx->view->GetPalette();
                ColorRgba bg_col = is_selected ? pal.surface_pressed : pal.surface_primary;
                ColorRgba border_col = is_focused ? pal.border_focus : pal.border_subtle;
                DrawRoundedBoxGdi(hdcBtn, rcBtn, btn_r, bg_col, border_col, 1);
                SelectObject(hdcBtn, ctx->hFontBody);
                SetTextColor(hdcBtn, pal.text_primary.ToWin32ColorRef());
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Browse...", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            const auto& palette = ctx->view->GetPalette();
            SetTextColor(hdcEdit, palette.text_primary.ToWin32ColorRef());
            SetBkColor(hdcEdit, palette.surface_elevated.ToWin32ColorRef());
            return reinterpret_cast<LRESULT>(ctx->hEditBrush);
        }
        case WM_CTLCOLORLISTBOX: {
            HDC hdcList = reinterpret_cast<HDC>(wParam);
            const auto& palette = ctx->view->GetPalette();
            SetBkMode(hdcList, OPAQUE);
            SetBkColor(hdcList, palette.surface_primary.ToWin32ColorRef());
            SetTextColor(hdcList, palette.text_primary.ToWin32ColorRef());
            return reinterpret_cast<LRESULT>(ctx->hBgBrush);
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdcStatic = reinterpret_cast<HDC>(wParam);
            HWND hCtl = reinterpret_cast<HWND>(lParam);
            if (hCtl == ctx->hQualityCombo) {
                SetBkMode(hdcStatic, OPAQUE);
                const auto& palette = ctx->view->GetPalette();
                SetBkColor(hdcStatic, palette.surface_elevated.ToWin32ColorRef());
                SetTextColor(hdcStatic, palette.text_primary.ToWin32ColorRef());
                return reinterpret_cast<LRESULT>(ctx->hEditBrush);
            } else if (hCtl == ctx->hSpaceLabel) {
                SetTextColor(hdcStatic, ctx->view->GetPalette().text_muted.ToWin32ColorRef());
            } else if (hCtl == ctx->hFileSizeLabel) {
                SetTextColor(hdcStatic, ctx->view->GetPalette().text_secondary.ToWin32ColorRef());
            } else {
                SetTextColor(hdcStatic, ctx->view->GetPalette().text_secondary.ToWin32ColorRef());
            }
            SetBkColor(hdcStatic, ctx->view->GetPalette().app_background.ToWin32ColorRef());
            return reinterpret_cast<LRESULT>(ctx->hBgBrush);
        }
        case WM_CTLCOLORBTN: {
            return reinterpret_cast<LRESULT>(ctx->hBgBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;
            const auto& pal = ctx->view->GetPalette();

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);

            // Refined Header: Title & Subtitle
            SelectObject(hdc, ctx->hFontTitle);
            SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
            SetBkMode(hdc, TRANSPARENT);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kHeaderTitleTop, dpi), L"New Download", 12);

            SelectObject(hdc, ctx->hFontSmall);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kHeaderSubtitleTop, dpi),
                     L"Configure download source, file name and destination folder", 59);

            // Clean 1px hairline divider
            ColorRgba hairline = (pal.mode == ThemeMode::kLight)
                ? pal.border_subtle : ColorRgba{0xFF, 0xFF, 0xFF, 18};
            RECT div_rc{
                pad_x,
                DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kDividerTop, dpi),
                rc.right - pad_x,
                DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kDividerTop + 1, dpi)
            };
            FillRectSolid(hdc, div_rc, hairline);

            // Field labels: separate non-overlapping rows above each control
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(0), dpi), L"Download URL", 12);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(1), dpi), L"File Name", 9);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(2), dpi), L"Save Location", 13);
            TextOutW(hdc, pad_x, DesignSystem::ScaleForDpi(NewDownloadDialogLayout::LabelTop(3), dpi), L"Quality / Format", 16);

            // Dedicated bottom footer bar with 1px top hairline
            int footer_top = DesignSystem::ScaleForDpi(NewDownloadDialogLayout::kButtonsTop - 10, dpi);
            if (footer_top < rc.bottom) {
                ColorRgba footer_bg = (pal.mode == ThemeMode::kLight)
                    ? pal.surface_primary : ColorRgba{0x14, 0x14, 0x17, 255};
                RECT footer_rc{0, footer_top, rc.right, rc.bottom};
                FillRectSolid(hdc, footer_rc, footer_bg);
                RECT footer_line{0, footer_top, rc.right, footer_top + 1};
                FillRectSolid(hdc, footer_line, hairline);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == 5001 && code == EN_CHANGE) {
                wchar_t url_buf[1024]{};
                GetWindowTextW(ctx->hUrlEdit, url_buf, 1023);
                std::string u = WToUtf8(url_buf);
                while (!u.empty() && (u.front() == ' ' || u.front() == '\t')) u.erase(0, 1);
                while (!u.empty() && (u.back() == ' ' || u.back() == '\t')) u.pop_back();

                wchar_t fn_buf[512]{};
                GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                std::string cur_fn = WToUtf8(fn_buf);

                if (cur_fn.empty() || cur_fn == "youtube_video.mp4" || cur_fn == "download") {
                    if (u.find("youtube.com") != std::string::npos || u.find("youtu.be") != std::string::npos) {
                        SetWindowTextW(ctx->hFilenameEdit, L"youtube_video.mp4");
                    } else if (!u.empty()) {
                        auto derived = PathSanitizer::ExtractFromUrl(u);
                        if (!derived.empty()) {
                            SetWindowTextW(ctx->hFilenameEdit, Utf8ToW(derived).c_str());
                        }
                    }
                }

                bool is_stream_url = (u.find("youtube.com") != std::string::npos ||
                                      u.find("youtu.be") != std::string::npos ||
                                      u.find(".m3u8") != std::string::npos ||
                                      u.find(".mpd") != std::string::npos);

                // If URL hasn't changed from what was initially populated and we already have manifest qualities or probed size, don't re-probe
                bool url_unchanged = (!ctx->bound_url.empty() && ctx->bound_url == u);
                if (!url_unchanged) {
                    ctx->bound_url = u;
                }

                int count = static_cast<int>(SendMessageW(ctx->hQualityCombo, CB_GETCOUNT, 0, 0));
                bool has_stream_items = (count > 1);
                if (is_stream_url && !has_stream_items && !ctx->has_manifest_qualities) {
                    SendMessageW(ctx->hQualityCombo, CB_RESETCONTENT, 0, 0);
                    ctx->quality_choices.clear();
                    ctx->quality_choices.push_back({"best", "Best Available Quality (Auto)", "Auto", 0, -1, true, "best", "best", "", "", false});
                    ctx->quality_choices.push_back({"1080p", "1080p Full HD", "1080p", 1080, -1, false, "1080p", "best", "", "", false});
                    ctx->quality_choices.push_back({"720p", "720p HD", "720p", 720, -1, false, "720p", "best", "", "", false});
                    ctx->quality_choices.push_back({"480p", "480p SD", "480p", 480, -1, false, "480p", "best", "", "", false});
                    ctx->quality_choices.push_back({"360p", "360p Standard", "360p", 360, -1, false, "360p", "best", "", "", false});
                    ctx->quality_choices.push_back({"144p", "144p Compact", "144p", 144, -1, false, "144p", "best", "", "", false});
                    ctx->quality_choices.push_back({"audio", "Audio Only (M4A)", "Audio", 0, -1, false, "audio", "best", "", "", true});
                    for (const auto& qc : ctx->quality_choices) {
                        SendMessageW(ctx->hQualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utf8ToW(qc.label).c_str()));
                    }
                    SendMessageW(ctx->hQualityCombo, CB_SETCURSEL, 0, 0);
                } else if (!is_stream_url && has_stream_items && !ctx->has_manifest_qualities) {
                    SendMessageW(ctx->hQualityCombo, CB_RESETCONTENT, 0, 0);
                    ctx->quality_choices.clear();
                    ctx->quality_choices.push_back({"original", "Original / Direct File (Server Default)", "", 0, -1, false, "best", "best", "", "", false, ""});
                    SendMessageW(ctx->hQualityCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Original / Direct File (Server Default)"));
                    SendMessageW(ctx->hQualityCombo, CB_SETCURSEL, 0, 0);
                }

                // If this is a stream URL or we already have manifest qualities, display the quality size or initial size directly without an HTTP file probe
                if (is_stream_url || ctx->has_manifest_qualities) {
                    int cur = static_cast<int>(SendMessageW(ctx->hQualityCombo, CB_GETCURSEL, 0, 0));
                    if (cur >= 0 && static_cast<size_t>(cur) < ctx->quality_choices.size() && ctx->quality_choices[static_cast<size_t>(cur)].size > 0) {
                        const auto& qc = ctx->quality_choices[static_cast<size_t>(cur)];
                        std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
                        std::wstring txt = std::wstring(L"Size: ") + (qc.size_estimated ? L"~" : L"") + Utf8ToW(sz_str) + (qc.size_estimated ? L" (estimated)" : L"");
                        SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                    } else if (ctx->initial_file_size > 0) {
                        std::wstring s_sz = L"Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(ctx->initial_file_size)));
                        SetWindowTextW(ctx->hFileSizeLabel, s_sz.c_str());
                    } else {
                        SetWindowTextW(ctx->hFileSizeLabel, L"Size: Unknown");
                    }
                    return 0;
                }

                // If URL was unchanged and we already have a known initial file size (e.g. from intercepted offer or existing task), don't wipe it
                if (url_unchanged && ctx->initial_file_size > 0) {
                    std::wstring s_sz = L"Download Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(ctx->initial_file_size)));
                    SetWindowTextW(ctx->hFileSizeLabel, s_sz.c_str());
                    return 0;
                }

                // Check size probing for local torrent or remote HTTP/HTTPS resource
                std::error_code tor_ec;
                if (u.rfind(".torrent") != std::string::npos && std::filesystem::exists(u, tor_ec)) {
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(u);
                    if (parsed.ok()) {
                        uint64_t sel_b = parsed.value->selected_bytes();
                        uint64_t tot_b = parsed.value->total_length;
                        std::string sz_str = DesktopView::FormatSizePretty(sel_b > 0 ? sel_b : tot_b);
                        std::wstring txt = L"Download Size: " + Utf8ToW(sz_str);
                        SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                    } else {
                        SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Unknown size");
                    }
                } else if (u.rfind("blob:", 0) == 0 && ctx->initial_file_size > 0) {
                    std::wstring s_sz = L"Download Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(ctx->initial_file_size)));
                    SetWindowTextW(ctx->hFileSizeLabel, s_sz.c_str());
                } else if (!ctx->speculative_tx_id.empty() && url_unchanged) {
                    if (auto snap = ctx->controller->GetSpeculativeSnapshot(ctx->speculative_tx_id); snap && snap->total_bytes > 0) {
                        std::wstring s_sz = L"Download Size: " + Utf8ToW(DesktopView::FormatSizePretty(snap->total_bytes));
                        SetWindowTextW(ctx->hFileSizeLabel, s_sz.c_str());
                    } else if (ctx->initial_file_size > 0) {
                        std::wstring s_sz = L"Download Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(ctx->initial_file_size)));
                        SetWindowTextW(ctx->hFileSizeLabel, s_sz.c_str());
                    } else {
                        SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Receiving metadata…");
                    }
                } else if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) {
                    SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Checking size…");
                    uint64_t req_id = ++ctx->current_probe_id;
                    auto alive_flag = ctx->dlg_alive;
                    HWND target_hwnd = hwnd;
                    std::thread([target_hwnd, alive_flag, req_id, u]() {
                        auto probe_res = radium::brisk::HttpClient::ProbeResource(u, {}, {}, {}, false, 5000);
                        if (!alive_flag || !(*alive_flag) || !IsWindow(target_hwnd)) return;
                        auto* payload = new ProbeResultPayload();
                        payload->request_id = req_id;
                        if (probe_res.ok()) {
                            payload->success = true;
                            payload->content_length = probe_res.value->has_content_length ? static_cast<int64_t>(probe_res.value->content_length) : -1;
                            payload->range_supported = (probe_res.value->status_code == 206 && probe_res.value->is_partial_content) ||
                                                       (probe_res.value->accept_ranges_bytes || (!probe_res.value->accept_ranges_none && probe_res.value->status_code == 206));
                            std::string fn = PathSanitizer::ExtractFromContentDisposition(probe_res.value->content_disposition);
                            if (fn.empty()) {
                                fn = PathSanitizer::ExtractFromUrl(probe_res.value->final_url.empty() ? u : probe_res.value->final_url);
                            }
                            payload->final_filename = PathSanitizer::SanitizeFileName(fn);
                        } else {
                            payload->success = false;
                            payload->content_length = -1;
                        }
                        PostMessageW(target_hwnd, WM_RADIUM_PROBE_COMPLETE, 0, reinterpret_cast<LPARAM>(payload));
                    }).detach();
                } else if (u.empty()) {
                    SetWindowTextW(ctx->hFileSizeLabel, L"Download Size: Unknown size");
                }
                return 0;
            }

            if (id == 5005 && code == CBN_SELCHANGE) {
                int cur = static_cast<int>(SendMessageW(ctx->hQualityCombo, CB_GETCURSEL, 0, 0));
                if (cur >= 0 && static_cast<size_t>(cur) < ctx->quality_choices.size()) {
                    const auto& qc = ctx->quality_choices[static_cast<size_t>(cur)];
                    if (qc.size > 0) {
                        std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
                        std::wstring txt = std::wstring(L"Size: ") + (qc.size_estimated ? L"~" : L"") + Utf8ToW(sz_str) + (qc.size_estimated ? L" (estimated)" : L"");
                        SetWindowTextW(ctx->hFileSizeLabel, txt.c_str());
                    } else {
                        SetWindowTextW(ctx->hFileSizeLabel, L"Size: Unknown");
                    }

                    if (!ctx->speculative_tx_id.empty() && ctx->existing_task_id.empty() && ctx->draft_task.has_value()) {
                        std::string new_vf = qc.video_filter.empty() ? (!qc.id.empty() ? qc.id : "best") : qc.video_filter;
                        std::string new_af = qc.audio_filter.empty() ? "best" : qc.audio_filter;
                        std::string cur_vf = ctx->draft_task->stream_options.video_filter;
                        if (new_vf != cur_vf || qc.itag > 0) {
                            ctx->controller->CancelSpeculativeTransaction(ctx->speculative_tx_id);
                            TaskModel next_draft = *ctx->draft_task;
                            next_draft.task_id.clear();
                            next_draft.part_path.clear();
                            next_draft.downloaded_bytes = 0;
                            next_draft.stream_percent = 0.0;
                            next_draft.completed_segments = 0;
                            next_draft.total_segments = 0;
                            next_draft.stream_options.video_filter = new_vf;
                            next_draft.stream_options.audio_filter = new_af;
                            if (!qc.ext.empty()) {
                                next_draft.stream_options.output_format = qc.ext;
                            }
                            if (!qc.resolution.empty() && qc.resolution != "Auto" && qc.resolution != "Original") {
                                wchar_t fn_buf[512]{};
                                GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                                std::string base_fn = WToUtf8(fn_buf);
                                if (!base_fn.empty()) {
                                    static const std::regex kResTagRe(R"(\s*\[\s*\d{3,4}p[_\w]*\s*\])", std::regex::icase);
                                    base_fn = std::regex_replace(base_fn, kResTagRe, "");
                                    auto dot_pos = base_fn.rfind('.');
                                    std::string stem = (dot_pos != std::string::npos) ? base_fn.substr(0, dot_pos) : base_fn;
                                    std::string ext = !qc.ext.empty() ? qc.ext : ((dot_pos != std::string::npos) ? base_fn.substr(dot_pos + 1) : "mp4");
                                    next_draft.file_name = stem + " [" + qc.resolution + "]." + ext;
                                    SetWindowTextW(ctx->hFilenameEdit, Utf8ToW(next_draft.file_name).c_str());
                                }
                            }
                            if (!qc.video_url.empty() || qc.itag > 0) {
                                try {
                                    radium::json::Value mroot = (!next_draft.stream_options.cached_manifest_body.empty() &&
                                                                 next_draft.stream_options.cached_manifest_body.front() == '{')
                                        ? radium::json::Parse(next_draft.stream_options.cached_manifest_body)
                                        : radium::json::Value(radium::json::Object{});
                                    if (mroot.is_object()) {
                                        if (!qc.video_url.empty()) mroot["videoUrl"] = qc.video_url;
                                        if (!qc.paired_audio_url.empty()) mroot["audioUrl"] = qc.paired_audio_url;
                                        else if (!qc.video_url.empty()) mroot["audioUrl"] = "";
                                        if (qc.itag > 0) mroot["selectedItag"] = static_cast<int64_t>(qc.itag);
                                        if (!qc.resolution.empty()) mroot["resolution"] = qc.resolution;
                                        next_draft.stream_options.cached_manifest_body = mroot.dump();
                                    }
                                } catch (...) {}
                            }
                            auto new_res = ctx->controller->BeginSpeculativeOffer(next_draft, nullptr);
                            if (new_res.ok()) {
                                ctx->speculative_tx_id = *new_res.value;
                                next_draft.task_id = ctx->speculative_tx_id;
                                ctx->draft_task = std::move(next_draft);
                            } else {
                                ctx->speculative_tx_id.clear();
                            }
                        }
                    }
                }
                return 0;
            }

            // Live destination disk space checking on edit change or blur
            if (id == 5003 && (code == EN_CHANGE || code == EN_KILLFOCUS)) {
                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string chosen_dir = WToUtf8(dir_buf);
                if (!chosen_dir.empty()) {
                    auto space_res = PathSanitizer::CheckAvailableDiskSpace(chosen_dir, 1024);
                    if (space_res.ok() && space_res.value.has_value()) {
                        std::string space_text = "Free space: " + DesktopView::FormatSizePretty(*space_res.value) + " available on destination drive";
                        SetWindowTextW(ctx->hSpaceLabel, Utf8ToW(space_text).c_str());
                    } else {
                        SetWindowTextW(ctx->hSpaceLabel, L"Disk space: Checking...");
                    }
                }
                return 0;
            }

            if (id == 5004 && code == BN_CLICKED) { // Browse
                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string chosen_dir = WToUtf8(dir_buf);
                if (SelectFolderWithDialogWin32(hwnd, chosen_dir)) {
                    SetWindowTextW(ctx->hSaveDirEdit, Utf8ToW(chosen_dir).c_str());
                    auto space_res = PathSanitizer::CheckAvailableDiskSpace(chosen_dir, 1024);
                    if (space_res.ok() && space_res.value.has_value()) {
                        std::string space_text = "Free space: " + DesktopView::FormatSizePretty(*space_res.value) + " available on destination drive";
                        SetWindowTextW(ctx->hSpaceLabel, Utf8ToW(space_text).c_str());
                    }
                }
                return 0;
            }

            if (id == 5007 && code == BN_CLICKED) { // Start immediately toggled
                bool start_chk = (SendMessageW(ctx->hStartChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                if (start_chk) {
                    SendMessageW(ctx->hScheduleChk, BM_SETCHECK, BST_UNCHECKED, 0);
                    EnableWindow(ctx->hScheduleTimeEdit, FALSE);
                }
                return 0;
            }

            if (id == 5010 && code == BN_CLICKED) { // Schedule toggled
                bool sched = (SendMessageW(ctx->hScheduleChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                EnableWindow(ctx->hScheduleTimeEdit, sched ? TRUE : FALSE);
                if (sched) {
                    SendMessageW(ctx->hStartChk, BM_SETCHECK, BST_UNCHECKED, 0);
                }
                return 0;
            }

            if ((id == 5008 || id == IDOK) && (code == BN_CLICKED || code == 0 || code == 1)) { // Download
                if (ctx->is_submitting) {
                    return 0;
                }
                ctx->is_submitting = true;

                wchar_t url_buf[2048]{};
                GetWindowTextW(ctx->hUrlEdit, url_buf, 2047);
                std::string u = WToUtf8(url_buf);
                while (!u.empty() && (u.front() == ' ' || u.front() == '\t')) u.erase(0, 1);
                while (!u.empty() && (u.back() == ' ' || u.back() == '\t')) u.pop_back();

                if (u.empty()) {
                    ctx->is_submitting = false;
                    MessageBoxW(hwnd, L"Please enter a valid download URL.", L"Missing URL", MB_OK | MB_ICONWARNING);
                    SetFocus(ctx->hUrlEdit);
                    return 0;
                }

                // If entered URL is a local .torrent file, close New Download dialog and open dedicated Add Torrent dialog
                std::error_code tor_ec;
                if (u.rfind(".torrent") != std::string::npos && std::filesystem::exists(u, tor_ec)) {
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(u);
                    if (parsed.ok()) {
                        auto tinfo = std::move(*parsed.value);
                        HWND parent_wnd = GetWindow(hwnd, GW_OWNER);
                        if (!parent_wnd) parent_wnd = hwnd;
                        const auto settings = ctx->controller->GetSettings();
                        std::string target_save = settings.last_download_dir.empty()
                            ? settings.default_download_dir : settings.last_download_dir;
                        bool start_torrent = true;
                        DestroyWindow(hwnd);
                        if (ShowAddTorrentDialogWin32(parent_wnd, *ctx->view, *ctx->controller, tinfo, u, target_save,
                                                      start_torrent, &target_save, &start_torrent)) {
                            TaskModel t;
                            t.task_type = TaskType::kTorrent;
                            t.url = u;
                            t.file_name = tinfo.name.empty() ? std::filesystem::path(u).filename().string() : tinfo.name;
                            t.save_dir = target_save;
                            t.total_bytes = tinfo.selected_bytes();
                            for (const auto& file : tinfo.files) {
                                t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                            }
                            auto res = ctx->controller->AddTask(std::move(t), start_torrent, false);
                            if (res.ok()) {
                                ctx->controller->RememberDownloadDirectory(target_save);
                                ctx->view->SelectTask(res.value->task_id);
                                ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                            }
                        }
                        return 0;
                    }
                }

                wchar_t fn_buf[512]{};
                GetWindowTextW(ctx->hFilenameEdit, fn_buf, 511);
                std::string fn = WToUtf8(fn_buf);

                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string sd = WToUtf8(dir_buf);
                if (sd.empty()) {
                    const auto settings = ctx->controller->GetSettings();
                    sd = settings.last_download_dir.empty() ? settings.default_download_dir : settings.last_download_dir;
                }

                int cur_sel = static_cast<int>(SendMessageW(ctx->hQualityCombo, CB_GETCURSEL, 0, 0));
                std::string vfilter = "best";
                std::string afilter = "best";
                int64_t chosen_sz = -1;
                if (cur_sel >= 0 && static_cast<size_t>(cur_sel) < ctx->quality_choices.size()) {
                    const auto& qc = ctx->quality_choices[static_cast<size_t>(cur_sel)];
                    vfilter = qc.video_filter.empty() ? (!qc.id.empty() ? qc.id : "best") : qc.video_filter;
                    afilter = qc.audio_filter.empty() ? "best" : qc.audio_filter;
                    chosen_sz = qc.size;
                } else if (cur_sel >= 0) {
                    wchar_t sel_buf[128]{};
                    SendMessageW(ctx->hQualityCombo, CB_GETLBTEXT, cur_sel, reinterpret_cast<LPARAM>(sel_buf));
                    std::wstring wsel(sel_buf);
                    vfilter = (wsel.find(L"Audio Only") != std::wstring::npos) ? "audio" : "best";
                }

                bool start_now = (SendMessageW(ctx->hStartChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                bool sched = (SendMessageW(ctx->hScheduleChk, BM_GETCHECK, 0, 0) == BST_CHECKED);

                AddDownloadDialogInput in;
                in.url = u;
                in.custom_filename = fn;
                in.save_dir = sd;
                in.max_connections = ctx->controller->GetSettings().default_connections_per_task;
                in.start_immediately = start_now;
                in.scheduled = sched;
                if (sched) {
                    wchar_t tbuf[64]{};
                    GetWindowTextW(ctx->hScheduleTimeEdit, tbuf, 63);
                    in.schedule_time_local = WToUtf8(tbuf);
                }
                in.video_filter = vfilter;
                in.audio_filter = afilter;
                if (chosen_sz > 0) {
                    in.total_bytes = static_cast<uint64_t>(chosen_sz);
                } else if (ctx->initial_file_size > 0) {
                    in.total_bytes = static_cast<uint64_t>(ctx->initial_file_size);
                }
                in.range_supported = ctx->probed_range_supported;
                in.range_probed = ctx->probed_has_range_info;
                bool is_yt = (u.find("youtube.com") != std::string::npos || u.find("youtu.be") != std::string::npos);
                if (is_yt || u.find(".m3u8") != std::string::npos || u.find(".mpd") != std::string::npos) {
                    in.is_stream = true;
                }

                // Compute the actual candidate/final filename
                std::string final_fn = fn;
                bool is_video_stream_cand = (in.is_stream || is_yt);
                if (cur_sel >= 0 && static_cast<size_t>(cur_sel) < ctx->quality_choices.size()) {
                    const auto& qc = ctx->quality_choices[static_cast<size_t>(cur_sel)];
                    if (is_video_stream_cand && !qc.resolution.empty() && qc.resolution != "Auto" && qc.resolution != "Original") {
                        static const std::regex kResTagRe(R"(\s*\[\s*\d{3,4}p[_\w]*\s*\])", std::regex::icase);
                        std::string base_fn = std::regex_replace(final_fn, kResTagRe, "");
                        auto dot_pos = base_fn.rfind('.');
                        std::string stem = (dot_pos != std::string::npos) ? base_fn.substr(0, dot_pos) : base_fn;
                        std::string ext = (dot_pos != std::string::npos) ? base_fn.substr(dot_pos + 1) : "mp4";
                        if (!qc.is_audio_only && !qc.ext.empty()) ext = qc.ext;
                        final_fn = stem + " [" + qc.resolution + "]." + ext;
                    }
                }
                if (final_fn.empty()) {
                    final_fn = is_yt ? "youtube_video.mp4" : PathSanitizer::ExtractFromUrl(u);
                }

                auto conflict_pol = ctx->controller->GetSettings().conflict_policy;

                TaskModel candidate = ctx->draft_task.value_or(TaskModel{});
                if (!ctx->existing_task_id.empty()) {
                    if (auto existing_opt = ctx->controller->GetTask(ctx->existing_task_id)) {
                        candidate = *existing_opt;
                    }
                }
                candidate.url = u;
                candidate.file_name = final_fn;
                candidate.save_dir = sd;
                candidate.user_edited_filename = true;
                candidate.max_connections = ctx->controller->GetSettings().default_connections_per_task;
                candidate.scheduled = sched;
                if (sched) {
                    wchar_t tbuf[64]{};
                    GetWindowTextW(ctx->hScheduleTimeEdit, tbuf, 63);
                    candidate.schedule_time_local = WToUtf8(tbuf);
                }
                if (in.is_stream) {
                    if (candidate.task_type == TaskType::kHttpFile) {
                        candidate.task_type = (u.find(".mpd") != std::string::npos) ? TaskType::kStreamDash : TaskType::kStreamHls;
                    }
                    candidate.category = "Video";
                    candidate.stream_options.video_filter = vfilter;
                    candidate.stream_options.audio_filter = afilter;
                }
                if (chosen_sz > 0) candidate.total_bytes = static_cast<uint64_t>(chosen_sz);
                else if (ctx->initial_file_size > 0 && candidate.total_bytes == 0) candidate.total_bytes = static_cast<uint64_t>(ctx->initial_file_size);
                if (ctx->probed_has_range_info) candidate.range_supported = ctx->probed_range_supported;

                auto report = ctx->controller->InspectIncomingDownload(
                    candidate,
                    !ctx->existing_task_id.empty() ? ctx->existing_task_id : ctx->speculative_tx_id);
                if (report.kind == DownloadController::IncomingConflictKind::kActiveOrQueuedSameDownload && report.matched_task.has_value()) {
                    if (!ctx->speculative_tx_id.empty()) {
                        ctx->controller->CancelSpeculativeTransaction(ctx->speculative_tx_id);
                    }
                    ctx->view->SelectTask(report.matched_task->task_id);
                    ctx->view->ScrollTaskIntoView(report.matched_task->task_id, true);
                    ctx->submitted = true;
                    DestroyWindow(hwnd);
                    return 0;
                }
                if (report.has_conflict() && conflict_pol == ConflictPolicy::kPrompt) {
                    if (!ctx->speculative_tx_id.empty()) {
                        ctx->controller->CancelSpeculativeTransaction(ctx->speculative_tx_id);
                    }
                    HWND owner = GetWindow(hwnd, GW_OWNER);
                    if (owner && IsWindow(owner)) {
                        auto* item = new Win32WindowContext::PendingConflictItem;
                        item->draft_task = std::move(candidate);
                        item->report = std::move(report);
                        item->start_immediately = start_now;
                        item->is_from_new_download_dialog = true;
                        item->is_existing_controller_task = !ctx->existing_task_id.empty();
                        if (!PostMessageW(owner, WM_RADIUM_SHOW_CONFLICT, 0, reinterpret_cast<LPARAM>(item))) {
                            delete item;
                        }
                    }
                    ctx->submitted = true;
                    DestroyWindow(hwnd);
                    return 0;
                }

                RADIUM_LOG_INFO("UI", "NEW_DOWNLOAD_SUBMIT_ATTEMPT: url=" + u + " filename=" + fn + " quality=" + vfilter);

                if (!ctx->speculative_tx_id.empty() && ctx->existing_task_id.empty()) {
                    auto acc_res = ctx->controller->AcceptSpeculativeTransaction(
                        ctx->speculative_tx_id,
                        final_fn,
                        sd,
                        start_now,
                        sched,
                        in.schedule_time_local,
                        conflict_pol);
                    if (acc_res.ok()) {
                        ctx->controller->RememberDownloadDirectory(sd);
                        ctx->view->SelectTask(acc_res.value->task_id);
                        ctx->view->ScrollTaskIntoView(acc_res.value->task_id, true);
                        ctx->submitted = true;
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }

                if (ctx->draft_task.has_value() && ctx->existing_task_id.empty()) {
                    if (candidate.url.rfind("blob:", 0) == 0 && !candidate.part_path.empty()) {
                        std::filesystem::path staged = PathSanitizer::Utf8ToPath(candidate.part_path);
                        candidate.part_path.clear();
                        auto ingest_res = ctx->controller->IngestCompletedTask(std::move(candidate), staged);
                        if (!ingest_res.ok()) {
                            ctx->is_submitting = false;
                            MessageBoxW(hwnd, Utf8ToW("Could not save download:\n" + ingest_res.error->message).c_str(),
                                        L"Error Adding Download", MB_OK | MB_ICONERROR);
                            return 0;
                        }
                        ctx->view->SelectTask(ingest_res.value->task_id);
                        ctx->view->ScrollTaskIntoView(ingest_res.value->task_id, true);
                    } else {
                        auto add_res = ctx->controller->AddTask(std::move(candidate), start_now, false);
                        if (!add_res.ok()) {
                            ctx->is_submitting = false;
                            MessageBoxW(hwnd, Utf8ToW("Could not add download:\n" + add_res.error->message).c_str(),
                                        L"Error Adding Download", MB_OK | MB_ICONERROR);
                            return 0;
                        }
                        ctx->controller->RememberDownloadDirectory(sd);
                        ctx->view->SelectTask(add_res.value->task_id);
                        ctx->view->ScrollTaskIntoView(add_res.value->task_id, true);
                    }
                    ctx->submitted = true;
                    DestroyWindow(hwnd);
                    return 0;
                }

                bool destination_conflict = false;
                if (conflict_pol == ConflictPolicy::kPrompt || conflict_pol == ConflictPolicy::kFailIfExists) {
                    auto destination = PathSanitizer::ResolveSafeOutputPath(sd, final_fn, ConflictPolicy::kPrompt);
                    destination_conflict = !destination.ok() && destination.error->code == "ERR_FILE_EXISTS";
                    if (destination_conflict) in.start_immediately = false;
                }

                if (!ctx->existing_task_id.empty()) {
                    auto t_opt = ctx->controller->GetTask(ctx->existing_task_id);
                    if (t_opt.has_value()) {
                        auto t = *t_opt;
                        bool is_video_stream = (t.task_type == TaskType::kStreamHls || t.task_type == TaskType::kStreamDash || t.category == "Video" || is_yt);
                        std::string old_fn = t.file_name;
                        std::string old_sd = t.save_dir;
                        if (!final_fn.empty()) t.file_name = final_fn;
                        else if (!fn.empty()) t.file_name = fn;
                        if (!sd.empty()) t.save_dir = sd;
                        t.scheduled = sched;
                        if (sched) {
                            wchar_t tbuf[64]{};
                            GetWindowTextW(ctx->hScheduleTimeEdit, tbuf, 63);
                            t.schedule_time_local = WToUtf8(tbuf);
                        }
                        t.stream_options.video_filter = vfilter;
                        t.stream_options.audio_filter = afilter;
                        if (chosen_sz > 0) {
                            t.total_bytes = static_cast<uint64_t>(chosen_sz);
                        } else if (ctx->initial_file_size > 0) {
                            t.total_bytes = static_cast<uint64_t>(ctx->initial_file_size);
                        }
                        if (ctx->probed_has_range_info) {
                            t.range_supported = ctx->probed_range_supported;
                        }

                        if (cur_sel >= 0 && static_cast<size_t>(cur_sel) < ctx->quality_choices.size()) {
                            const auto& qc = ctx->quality_choices[static_cast<size_t>(cur_sel)];
                            if (qc.is_audio_only) {
                                t.stream_options.output_format = "m4a";
                                t.category = "Audio";
                            } else if (!qc.ext.empty() && is_video_stream) {
                                t.stream_options.output_format = qc.ext;
                            }
                            // Clean up file name if it has an outdated resolution tag (e.g. [360p])
                            // ONLY applies to video streams with a resolution tag! Never to direct files or "Original"!
                            if (is_video_stream && !qc.resolution.empty() && qc.resolution != "Auto" && qc.resolution != "Original") {
                                static const std::regex kResTagRe(R"(\s*\[\s*\d{3,4}p[_\w]*\s*\])", std::regex::icase);
                                std::string base_fn = std::regex_replace(t.file_name, kResTagRe, "");
                                auto dot_pos = base_fn.rfind('.');
                                std::string stem = (dot_pos != std::string::npos) ? base_fn.substr(0, dot_pos) : base_fn;
                                std::string ext = (dot_pos != std::string::npos) ? base_fn.substr(dot_pos + 1) : t.stream_options.output_format;
                                if (!qc.is_audio_only && !qc.ext.empty()) ext = qc.ext;
                                t.file_name = stem + " [" + qc.resolution + "]." + ext;
                            }
                            if (!t.stream_options.cached_manifest_body.empty() &&
                                (t.stream_options.cached_manifest_body.front() == '{' || t.stream_options.cached_manifest_body.front() == '[')) {
                                try {
                                    auto jroot = radium::json::Parse(t.stream_options.cached_manifest_body);
                                    if (jroot.is_object()) {
                                        if (!qc.video_url.empty()) jroot["videoUrl"] = qc.video_url;
                                        if (!qc.paired_audio_url.empty()) jroot["audioUrl"] = qc.paired_audio_url;
                                        jroot["requiresCipherExtraction"] = qc.video_url.empty();
                                        jroot["requiresAudioMux"] = !qc.paired_audio_url.empty();
                                        if (!qc.resolution.empty()) jroot["resolution"] = qc.resolution;
                                        if (!qc.video_filter.empty()) jroot["videoFilter"] = qc.video_filter;
                                        if (qc.itag > 0) jroot["selectedItag"] = qc.itag;
                                        t.stream_options.cached_manifest_body = jroot.dump();
                                    }
                                } catch (...) {}
                            }
                        }

                        // If task was already completed (e.g. chunked transfer ingestion), ensure file on disk matches any rename
                        if (t.state == TaskState::kCompleted) {
                            std::filesystem::path old_p = std::filesystem::path(old_sd) / old_fn;
                            std::filesystem::path new_p = std::filesystem::path(t.save_dir) / t.file_name;
                            if (old_p != new_p) {
                                std::error_code ren_ec;
                                if (std::filesystem::exists(old_p, ren_ec)) {
                                    std::filesystem::create_directories(new_p.parent_path(), ren_ec);
                                    std::filesystem::rename(old_p, new_p, ren_ec);
                                    if (ren_ec) {
                                        ren_ec.clear();
                                        std::filesystem::copy_file(old_p, new_p, std::filesystem::copy_options::overwrite_existing, ren_ec);
                                        std::filesystem::remove(old_p, ren_ec);
                                    }
                                }
                            }
                        }

                        if (destination_conflict) {
                            t.state = TaskState::kFailed;
                            t.error_code = "ERR_FILE_EXISTS";
                            t.error_message = "Destination file already exists: " +
                                PathSanitizer::PathToUtf8(PathSanitizer::Utf8ToPath(t.save_dir) / PathSanitizer::Utf8ToPath(t.file_name));
                            t.status_detail = "Waiting for a file conflict choice";
                            ctx->controller->UpdateTask(t);
                            ctx->submitted = true;
                            HWND owner = GetWindow(hwnd, GW_OWNER);
                            if (owner && IsWindow(owner)) PostMessageW(owner, WM_RADIUM_SHOW_CONFLICT, 0, 0);
                            RADIUM_LOG_INFO("UI", "NEW_DOWNLOAD_CONFLICT_DETECTED: taskId=" + ctx->existing_task_id + " file=" + t.file_name);
                            DestroyWindow(hwnd);
                            return 0;
                        }

                        bool was_already_active = (t.state == TaskState::kProbing || t.state == TaskState::kDownloading || t.state == TaskState::kMuxing);
                        if (t.state == TaskState::kFailed || t.state == TaskState::kPaused || t.state == TaskState::kQueued) {
                            t.state = start_now ? TaskState::kQueued : TaskState::kPaused;
                            t.status_detail = start_now ? "Queued" : "Paused";
                            t.error_code.clear();
                            t.error_message.clear();
                        }

                        ctx->controller->UpdateTask(t);
                        if (start_now && !was_already_active && t.state != TaskState::kCompleted) {
                            ctx->controller->StartTask(ctx->existing_task_id);
                        }
                        ctx->submitted = true;
                        RADIUM_LOG_INFO("UI", "NEW_DOWNLOAD_UPDATED: taskId=" + ctx->existing_task_id);
                        DestroyWindow(hwnd);
                        return 0;
                    }
                }

                auto res = ctx->view->SubmitAddDownloadDialog(in);
                if (!res.ok()) {
                    ctx->is_submitting = false;
                    std::wstring err = L"Could not add download:\n" + Utf8ToW(res.error->message);
                    MessageBoxW(hwnd, err.c_str(), L"Error Adding Download", MB_OK | MB_ICONERROR);
                    return 0;
                }

                if (destination_conflict && res.value->state == TaskState::kPaused) {
                    if (!MarkTaskAsFileConflict(*ctx->controller, *res.value)) {
                        ctx->is_submitting = false;
                        MessageBoxW(hwnd, L"The existing file was detected, but Radium could not prepare the conflict warning.",
                                    L"File Conflict", MB_OK | MB_ICONERROR);
                        return 0;
                    }
                    ctx->submitted = true;
                    HWND owner = GetWindow(hwnd, GW_OWNER);
                    if (owner && IsWindow(owner)) PostMessageW(owner, WM_RADIUM_SHOW_CONFLICT, 0, 0);
                    DestroyWindow(hwnd);
                    return 0;
                }

                ctx->submitted = true;
                RADIUM_LOG_INFO("UI", "NEW_DOWNLOAD_SUBMITTED: taskId=" + res.value->task_id + " file=" + res.value->file_name);
                DestroyWindow(hwnd);
                return 0;
            }

            if ((id == 5009 || id == IDCANCEL) && (code == BN_CLICKED || code == 0 || code == 1)) { // Cancel
                if (!ctx->speculative_tx_id.empty()) {
                    ctx->controller->CancelSpeculativeTransaction(ctx->speculative_tx_id);
                }
                if (ctx->draft_task.has_value() && ctx->draft_task->url.rfind("blob:", 0) == 0 && !ctx->draft_task->part_path.empty()) {
                    std::error_code rm_ec;
                    std::filesystem::remove(PathSanitizer::Utf8ToPath(ctx->draft_task->part_path), rm_ec);
                }
                if (!ctx->existing_task_id.empty()) {
                    auto t_opt = ctx->controller->GetTask(ctx->existing_task_id);
                    ctx->controller->CancelTask(ctx->existing_task_id);
                    ctx->controller->RemoveTask(ctx->existing_task_id, false);
                    if (t_opt.has_value() && t_opt->url.rfind("blob:", 0) == 0 && !t_opt->part_path.empty()) {
                        std::error_code rm_ec;
                        std::filesystem::remove(PathSanitizer::Utf8ToPath(t_opt->part_path), rm_ec);
                    }
                }
                DestroyWindow(hwnd);
                return 0;
            }

            return 0;
        }
        case WM_DESTROY: {
            if (!ctx->speculative_tx_id.empty()) {
                KillTimer(hwnd, 5020);
                if (!ctx->submitted) {
                    ctx->controller->CancelSpeculativeTransaction(ctx->speculative_tx_id);
                }
            }
            if (ctx && ctx->dlg_alive) {
                *(ctx->dlg_alive) = false;
            }
            if (!ctx->submitted && ctx->draft_task.has_value() &&
                ctx->draft_task->url.rfind("blob:", 0) == 0 && !ctx->draft_task->part_path.empty()) {
                std::error_code rm_ec;
                std::filesystem::remove(PathSanitizer::Utf8ToPath(ctx->draft_task->part_path), rm_ec);
            }
            if (!ctx->submitted && !ctx->existing_task_id.empty()) {
                auto t_opt = ctx->controller->GetTask(ctx->existing_task_id);
                ctx->controller->CancelTask(ctx->existing_task_id);
                ctx->controller->RemoveTask(ctx->existing_task_id, false);
                if (t_opt.has_value() && t_opt->url.rfind("blob:", 0) == 0 && !t_opt->part_path.empty()) {
                    std::error_code rm_ec;
                    std::filesystem::remove(PathSanitizer::Utf8ToPath(t_opt->part_path), rm_ec);
                }
            }
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
            if (ctx->hFontSmall) DeleteObject(ctx->hFontSmall);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            if (ctx->hEditBrush) DeleteObject(ctx->hEditBrush);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

std::string PromptTextModalWin32(
    HWND parent,
    const std::wstring& title,
    const std::wstring& prompt,
    const std::string& initial_value,
    const ThemePalette& /*palette*/) {
    struct PromptDlgState {
        bool done{false};
        bool ok{false};
        std::string value;
        HWND hEdit{nullptr};
    } st;
    st.value = initial_value;

    const wchar_t* cls_name = L"RadiumPromptModalClass";
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = [](HWND hwnd, UINT m, WPARAM wp, LPARAM lp) -> LRESULT {
        auto* s = reinterpret_cast<PromptDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            return TRUE;
        }
        if (m == WM_COMMAND && s) {
            if (LOWORD(wp) == IDOK) {
                if (s->hEdit) {
                    int len = GetWindowTextLengthW(s->hEdit);
                    std::wstring w(static_cast<size_t>(len) + 1, L'\0');
                    GetWindowTextW(s->hEdit, w.data(), len + 1);
                    w.resize(static_cast<size_t>(len));
                    int u8 = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
                    s->value.resize(static_cast<size_t>(u8));
                    if (u8 > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s->value.data(), u8, nullptr, nullptr);
                }
                s->ok = true;
                s->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
            if (LOWORD(wp) == IDCANCEL) {
                s->ok = false;
                s->done = true;
                DestroyWindow(hwnd);
                return 0;
            }
        }
        if (m == WM_CLOSE && s) {
            s->ok = false;
            s->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        return DefWindowProcW(hwnd, m, wp, lp);
    };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = cls_name;
    RegisterClassExW(&wc);

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME, cls_name, title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 210, parent, nullptr, wc.hInstance, &st);
    if (!hDlg) return {};

    CreateWindowExW(0, L"STATIC", prompt.c_str(), WS_CHILD | WS_VISIBLE, 16, 12, 416, 68, hDlg, nullptr, wc.hInstance, nullptr);
    st.hEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToW(initial_value).c_str(),
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 16, 86, 416, 26, hDlg, nullptr, wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 260, 128, 80, 28, hDlg, reinterpret_cast<HMENU>(IDOK), wc.hInstance, nullptr);
    CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE, 352, 128, 80, 28, hDlg, reinterpret_cast<HMENU>(IDCANCEL), wc.hInstance, nullptr);
    if (st.hEdit) SetFocus(st.hEdit);

    EnableWindow(parent, FALSE);
    MSG msg{};
    while (!st.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return st.ok ? st.value : std::string{};
}

void ShowNewDownloadDialogWin32(
    HWND hwnd_parent,
    DesktopView& view,
    DownloadController& controller,
    const std::string& initial_url,
    const std::string& suggested_filename = "",
    const std::string& save_dir = "",
    int64_t file_size = -1,
    const std::string& existing_task_id = "",
    std::optional<TaskModel> draft_task = std::nullopt,
    const std::string& speculative_tx_id = "") {

    struct PendingOfferItem {
        std::string url;
        std::string filename;
        std::string save_dir;
        int64_t file_size{-1};
        std::string task_id;
    };
    static std::vector<PendingOfferItem> s_pending_offers;

    // Avoid overlapping modal dialogs while never dropping incoming browser offers:
    // if a dialog is already open, queue this offer and show it when the current one closes.
    HWND existing_dlg = FindWindowW(L"Radium_NewDownloadDlgClass", nullptr);
    if (existing_dlg && IsWindow(existing_dlg)) {
        if (!existing_task_id.empty() || !initial_url.empty()) {
            bool dup = false;
            for (const auto& p : s_pending_offers) {
                if ((!existing_task_id.empty() && p.task_id == existing_task_id) ||
                    (existing_task_id.empty() && p.url == initial_url && p.filename == suggested_filename)) {
                    dup = true;
                    break;
                }
            }
            if (!dup) {
                s_pending_offers.push_back({initial_url, suggested_filename, save_dir, file_size, existing_task_id});
            }
        }
        if (!IsWindowVisible(existing_dlg) || IsIconic(existing_dlg)) {
            ShowWindow(existing_dlg, SW_RESTORE);
        }
        BringWindowToTop(existing_dlg);
        SetForegroundWindow(existing_dlg);
        SetActiveWindow(existing_dlg);
        return;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = NewDownloadDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_NewDownloadDlgClass";
        RegisterClassExW(&wc);
        class_registered = true;
    }

    UINT dpi = 96;
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        HMODULE hUser32 = GetModuleHandleW(L"user32.dll");
        if (hUser32) {
            using fnGetDpiForWindow = UINT(WINAPI*)(HWND);
            auto pfnGetDpi = reinterpret_cast<fnGetDpiForWindow>(GetProcAddress(hUser32, "GetDpiForWindow"));
            if (pfnGetDpi) {
                dpi = pfnGetDpi(hwnd_parent);
            }
        }
    }
    if (dpi == 0) dpi = 96;

    NewDownloadDialogContext ctx;
    ctx.view = &view;
    ctx.controller = &controller;
    ctx.existing_task_id = existing_task_id;
    ctx.speculative_tx_id = speculative_tx_id;
    ctx.draft_task = std::move(draft_task);
    ctx.initial_filename = suggested_filename;
    ctx.initial_save_dir = save_dir;
    if (ctx.initial_save_dir.empty()) {
        const auto settings = controller.GetSettings();
        ctx.initial_save_dir = settings.last_download_dir.empty() ? settings.default_download_dir : settings.last_download_dir;
    }
    ctx.initial_file_size = file_size;
    ctx.dpi = dpi;

    std::string candidate_url = initial_url;
    if (candidate_url.empty() && OpenClipboard(hwnd_parent)) {
        HANDLE hData = GetClipboardData(CF_UNICODETEXT);
        if (hData) {
            const wchar_t* wtxt = static_cast<const wchar_t*>(GlobalLock(hData));
            if (wtxt) {
                std::string s = WToUtf8(wtxt);
                if (s.rfind("http://", 0) == 0 || s.rfind("https://", 0) == 0 || s.rfind("ftp://", 0) == 0 ||
                    s.rfind("magnet:?", 0) == 0 || s.rfind(".torrent") != std::string::npos) {
                    candidate_url = s;
                }
                GlobalUnlock(hData);
            }
        }
        CloseClipboard();
    }

    // Direct routing: If URL or clipboard points to a local .torrent file, open dedicated Add Torrent dialog directly
    if (!candidate_url.empty()) {
        std::error_code tor_ec;
        if (candidate_url.rfind(".torrent") != std::string::npos && std::filesystem::exists(candidate_url, tor_ec)) {
            auto parsed = torrent::TorrentParser::ParseTorrentFile(candidate_url);
            if (parsed.ok()) {
                auto tinfo = std::move(*parsed.value);
                const auto saved_settings = controller.GetSettings();
                std::string target_save = save_dir.empty()
                    ? (saved_settings.last_download_dir.empty() ? saved_settings.default_download_dir : saved_settings.last_download_dir)
                    : save_dir;
                bool start_torrent = true;
                if (ShowAddTorrentDialogWin32(hwnd_parent, view, controller, tinfo, candidate_url, target_save,
                                              start_torrent, &target_save, &start_torrent)) {
                    TaskModel t;
                    t.task_type = TaskType::kTorrent;
                    t.url = candidate_url;
                    t.file_name = tinfo.name.empty() ? std::filesystem::path(candidate_url).filename().string() : tinfo.name;
                    t.save_dir = target_save;
                    t.total_bytes = tinfo.selected_bytes();
                    for (const auto& file : tinfo.files) {
                        t.torrent_file_paths.push_back(file.relative_path.generic_string());
                        t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                    }
                    auto res = controller.AddTask(std::move(t), start_torrent, false);
                    if (res.ok()) {
                        controller.RememberDownloadDirectory(target_save);
                        view.SelectTask(res.value->task_id);
                        view.ScrollTaskIntoView(res.value->task_id, true);
                    }
                }
                return;
            }
        }
    }

    if (!candidate_url.empty() && existing_task_id.empty() && speculative_tx_id.empty() &&
        controller.GetSettings().conflict_policy == ConflictPolicy::kPrompt) {
        TaskModel check_t = ctx.draft_task.value_or(TaskModel{});
        check_t.url = candidate_url;
        if (check_t.file_name.empty()) check_t.file_name = suggested_filename;
        if (check_t.save_dir.empty()) check_t.save_dir = ctx.initial_save_dir;
        auto report = controller.InspectIncomingDownload(check_t);
        if (report.kind == DownloadController::IncomingConflictKind::kActiveOrQueuedSameDownload &&
            report.matched_task.has_value()) {
            view.SelectTask(report.matched_task->task_id);
            view.ScrollTaskIntoView(report.matched_task->task_id, true);
            return;
        }
        if (report.has_conflict() && hwnd_parent && IsWindow(hwnd_parent)) {
            check_t.file_name = report.resolved_file_name;
            check_t.save_dir = report.resolved_save_dir;
            auto* item = new Win32WindowContext::PendingConflictItem;
            item->draft_task = std::move(check_t);
            item->report = std::move(report);
            item->start_immediately = true;
            SendMessageW(hwnd_parent, WM_RADIUM_SHOW_CONFLICT, 0, reinterpret_cast<LPARAM>(item));
            return;
        }
    }

    RECT work_rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0);
    int screen_w = work_rc.right - work_rc.left;
    int screen_h = work_rc.bottom - work_rc.top;
    if (screen_w <= 0) screen_w = GetSystemMetrics(SM_CXSCREEN);
    if (screen_h <= 0) screen_h = GetSystemMetrics(SM_CYSCREEN);

    RECT parent_rc{};
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        GetWindowRect(hwnd_parent, &parent_rc);
    }
    int dlg_w = DesignSystem::ScaleForDpi(520, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(480, dpi);
    int dlg_x = 0;
    int dlg_y = 0;

    bool parent_is_interactive = (hwnd_parent && IsWindow(hwnd_parent) && IsWindowVisible(hwnd_parent) && !IsIconic(hwnd_parent));
    HWND owner_hwnd = parent_is_interactive ? hwnd_parent : nullptr;

    if (!parent_is_interactive || parent_rc.left < -10000 || parent_rc.top < -10000 ||
        (parent_rc.right - parent_rc.left) < 200 || (parent_rc.bottom - parent_rc.top) < 200) {
        dlg_x = work_rc.left + (screen_w - dlg_w) / 2;
        dlg_y = work_rc.top + (screen_h - dlg_h) / 2;
    } else {
        dlg_x = parent_rc.left + (parent_rc.right - parent_rc.left - dlg_w) / 2;
        dlg_y = parent_rc.top + (parent_rc.bottom - parent_rc.top - dlg_h) / 2;
    }
    if (dlg_x < work_rc.left) dlg_x = work_rc.left + 20;
    if (dlg_y < work_rc.top) dlg_y = work_rc.top + 20;

    ctx.bound_url = candidate_url;

    // Clean modern dialog frame without redundant 3D modal borders or nested outlines
    DWORD ex_style = (owner_hwnd ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT;
    HWND hDlg = CreateWindowExW(
        ex_style,
        L"Radium_NewDownloadDlgClass",
        L"New Download \u2014 Radium",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        owner_hwnd, nullptr, hInst, &ctx);

    if (!hDlg) return;
    ctx.hDlg = hDlg;
    if (!ctx.speculative_tx_id.empty()) {
        controller.RecordSpeculativeDialogVisible(ctx.speculative_tx_id);
        SetTimer(hDlg, 5020, 150, nullptr);
    }

    HICON hDlgIcon = CreateRadiumAppIcon();
    if (hDlgIcon) {
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hDlgIcon));
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hDlgIcon));
    }

    ApplyWindowGlassBackdropWin32(hDlg, view.GetPalette(), true /* is_transient_dialog */);

    if (!candidate_url.empty()) {
        SetWindowTextW(ctx.hUrlEdit, Utf8ToW(candidate_url).c_str());
        SendMessageW(ctx.hUrlEdit, EM_SETSEL, 0, -1);
        if (candidate_url.find("youtube.com") != std::string::npos || candidate_url.find("youtu.be") != std::string::npos) {
            SetWindowTextW(ctx.hFilenameEdit, L"youtube_video.mp4");
        } else {
            auto derived = PathSanitizer::ExtractFromUrl(candidate_url);
            if (!derived.empty()) {
                SetWindowTextW(ctx.hFilenameEdit, Utf8ToW(derived).c_str());
            }
        }
    }
    if (!suggested_filename.empty()) {
        SetWindowTextW(ctx.hFilenameEdit, Utf8ToW(suggested_filename).c_str());
    }
    if (!save_dir.empty()) {
        SetWindowTextW(ctx.hSaveDirEdit, Utf8ToW(save_dir).c_str());
    }
    if (file_size > 0) {
        std::wstring s_sz = L"Download Size: " + Utf8ToW(DesktopView::FormatSizePretty(static_cast<uint64_t>(file_size)));
        SetWindowTextW(ctx.hFileSizeLabel, s_sz.c_str());
    } else if (ctx.has_manifest_qualities) {
        int cur = static_cast<int>(SendMessageW(ctx.hQualityCombo, CB_GETCURSEL, 0, 0));
        if (cur >= 0 && static_cast<size_t>(cur) < ctx.quality_choices.size() && ctx.quality_choices[static_cast<size_t>(cur)].size > 0) {
            const auto& qc = ctx.quality_choices[static_cast<size_t>(cur)];
            std::string sz_str = DesktopView::FormatSizePretty(static_cast<uint64_t>(qc.size));
            std::wstring txt = std::wstring(L"Size: ") + (qc.size_estimated ? L"~" : L"") + Utf8ToW(sz_str) + (qc.size_estimated ? L" (estimated)" : L"");
            SetWindowTextW(ctx.hFileSizeLabel, txt.c_str());
        }
    } else if (!candidate_url.empty()) {
        SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(5001, EN_CHANGE), reinterpret_cast<LPARAM>(ctx.hUrlEdit));
    }

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, FALSE);
    }
    SetWindowPos(hDlg, HWND_TOP, dlg_x, dlg_y, dlg_w, dlg_h, SWP_SHOWWINDOW);
    ShowWindow(hDlg, SW_SHOWNORMAL);
    BringWindowToTop(hDlg);
    SetForegroundWindow(hDlg);
    SetActiveWindow(hDlg);
    SetFocus(ctx.hUrlEdit);
    RADIUM_LOG_INFO("UI", "NEW_DOWNLOAD_DIALOG_SHOWN: taskId=" + existing_task_id + " url=" + candidate_url + " filename=" + suggested_filename);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(5009, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hCancelBtn));
                break;
            } else if (msg.wParam == VK_RETURN && GetFocus() != ctx.hBrowseBtn && GetFocus() != ctx.hCancelBtn) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(5008, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hDownloadBtn));
                if (!IsWindow(hDlg)) break;
                continue;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, TRUE);
        SetForegroundWindow(owner_hwnd);
        InvalidateRect(owner_hwnd, nullptr, TRUE);
    }

    if (!s_pending_offers.empty()) {
        PendingOfferItem next = std::move(s_pending_offers.front());
        s_pending_offers.erase(s_pending_offers.begin());
        ShowNewDownloadDialogWin32(
            hwnd_parent, view, controller, next.url, next.filename, next.save_dir, next.file_size, next.task_id);
    }
}

void ShowAddUrlQuickDialogWin32(HWND hwnd, DesktopView& view, DownloadController& controller) {
    ShowNewDownloadDialogWin32(hwnd, view, controller, "");
}

void CopyTextToClipboardWin32(HWND hwnd, const std::string& text) {
    if (text.empty()) return;
    std::wstring wtext = Utf8ToW(text);
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    size_t bytes = (wtext.size() + 1) * sizeof(wchar_t);
    HGLOBAL hGlob = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hGlob) {
        void* p = GlobalLock(hGlob);
        if (p) {
            std::memcpy(p, wtext.c_str(), bytes);
            GlobalUnlock(hGlob);
            SetClipboardData(CF_UNICODETEXT, hGlob);
        }
    }
    CloseClipboard();
}

struct SimpleInputDialogContext {
    std::wstring title;
    std::wstring prompt;
    std::wstring initial_value;
    std::wstring result_value;
    bool submitted{false};
    HWND hEdit{nullptr};
    HWND hOkBtn{nullptr};
    HWND hCancelBtn{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hEditBrush{nullptr};
    ThemePalette palette{DesignSystem::GetRedesignPalette()};
};

LRESULT CALLBACK SimpleInputDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SimpleInputDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<SimpleInputDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            uint32_t dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;

            ctx->hFontBody = CreateFontW(-DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold = CreateFontW(-DesignSystem::ScaleForDpi(15, dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hBgBrush = CreateSolidBrush(ctx->palette.app_background.ToWin32ColorRef());
            ctx->hEditBrush = CreateSolidBrush(ctx->palette.surface_elevated.ToWin32ColorRef());

            HINSTANCE hInst = GetModuleHandleW(nullptr);
            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int edit_y = DesignSystem::ScaleForDpi(60, dpi);
            int edit_h = DesignSystem::ScaleForDpi(28, dpi);
            int edit_w = DesignSystem::ScaleForDpi(432, dpi);

            ctx->hEdit = CreateWindowExW(0, L"EDIT", ctx->initial_value.c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                pad_x, edit_y, edit_w, edit_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7001)), hInst, nullptr);
            SendMessageW(ctx->hEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hEdit, EM_SETSEL, 0, -1);
            InstallModernEditControl(ctx->hEdit, dpi, ctx->palette);

            int btn_y = DesignSystem::ScaleForDpi(108, dpi);
            int btn_w = DesignSystem::ScaleForDpi(100, dpi);
            int btn_h = DesignSystem::ScaleForDpi(32, dpi);
            int cancel_x = pad_x + edit_w - btn_w;
            int ok_x = cancel_x - btn_w - DesignSystem::ScaleForDpi(12, dpi);

            ctx->hOkBtn = CreateWindowExW(0, L"BUTTON", L"OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                ok_x, btn_y, btn_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7002)), hInst, nullptr);

            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                cancel_x, btn_y, btn_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(7003)), hInst, nullptr);
            return 0;
        }
        case WM_SIZE: {
            if (!ctx || !ctx->hEdit) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int client_w = rc.right - rc.left;
            int client_h = rc.bottom - rc.top;
            uint32_t dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int edit_y = DesignSystem::ScaleForDpi(60, dpi);
            int edit_h = DesignSystem::ScaleForDpi(28, dpi);
            int edit_w = std::max(DesignSystem::ScaleForDpi(200, dpi), client_w - pad_x * 2);
            SetWindowPos(ctx->hEdit, nullptr, pad_x, edit_y, edit_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);

            int btn_h = DesignSystem::ScaleForDpi(32, dpi);
            int btn_w = DesignSystem::ScaleForDpi(100, dpi);
            int btn_y = client_h - btn_h - DesignSystem::ScaleForDpi(18, dpi);
            int cancel_x = pad_x + edit_w - btn_w;
            int ok_x = cancel_x - btn_w - DesignSystem::ScaleForDpi(12, dpi);
            SetWindowPos(ctx->hCancelBtn, nullptr, cancel_x, btn_y, btn_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hOkBtn, nullptr, ok_x, btn_y, btn_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            bool is_sel = (dis->itemState & ODS_SELECTED) != 0;
            if (dis->CtlID == 7002) { // OK button (Coral Red accent #FF4757)
                ColorRgba bg = is_sel ? ColorRgba{0xE0, 0x43, 0x4B, 255} : ColorRgba{0xFF, 0x4F, 0x57, 255};
                DrawRoundedBoxGdi(hdc, rc, 16, bg, bg, 0);
                SelectObject(hdc, ctx->hFontBold);
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"OK", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 7003) { // Cancel button
                ColorRgba bg = is_sel ? ctx->palette.surface_pressed : ctx->palette.surface_primary;
                ColorRgba border = ctx->palette.border_subtle;
                DrawRoundedBoxGdi(hdc, rc, 16, bg, border, 1);
                SelectObject(hdc, ctx->hFontBody);
                SetTextColor(hdc, ctx->palette.text_primary.ToWin32ColorRef());
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Cancel", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_CTLCOLOREDIT: {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdcEdit, ctx->palette.text_primary.ToWin32ColorRef());
            SetBkColor(hdcEdit, ctx->palette.surface_elevated.ToWin32ColorRef());
            return reinterpret_cast<LRESULT>(ctx->hEditBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            uint32_t dpi = GetDpiForWindow(hwnd);
            if (dpi == 0) dpi = 96;

            int pad_x = DesignSystem::ScaleForDpi(24, dpi);
            int prompt_y = DesignSystem::ScaleForDpi(20, dpi);
            int prompt_h = DesignSystem::ScaleForDpi(32, dpi);

            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, ctx->palette.text_primary.ToWin32ColorRef());
            RECT prc{pad_x, prompt_y, rc.right - pad_x, prompt_y + prompt_h};
            DrawTextW(hdc, ctx->prompt.c_str(), -1, &prc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 7002) { // OK
                wchar_t buf[2048]{};
                GetWindowTextW(ctx->hEdit, buf, 2047);
                ctx->result_value = buf;
                ctx->submitted = true;
                DestroyWindow(hwnd);
                return 0;
            } else if (id == 7003) { // Cancel
                ctx->submitted = false;
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            if (ctx->hEditBrush) DeleteObject(ctx->hEditBrush);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowSimpleInputDialogWin32(HWND hwnd_parent, const std::wstring& title, const std::wstring& prompt, const std::wstring& initial_value, std::wstring& out_value) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SimpleInputDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_SimpleInputDialogClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    SimpleInputDialogContext ctx;
    ctx.title = title;
    ctx.prompt = prompt;
    ctx.initial_value = initial_value;
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        auto* owner_ctx = reinterpret_cast<Win32WindowContext*>(GetWindowLongPtrW(hwnd_parent, GWLP_USERDATA));
        if (owner_ctx && owner_ctx->view) ctx.palette = owner_ctx->view->GetPalette();
    }

    UINT dpi = 96;
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        dpi = GetDpiForWindow(hwnd_parent);
    }
    if (dpi == 0) dpi = 96;

    RECT prc{};
    GetWindowRect(hwnd_parent, &prc);
    int dlg_w = DesignSystem::ScaleForDpi(480, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(190, dpi);
    int dlg_x = prc.left + (prc.right - prc.left - dlg_w) / 2;
    int dlg_y = prc.top + (prc.bottom - prc.top - dlg_h) / 2;

    HWND hDlg = CreateWindowExW(
        (hwnd_parent ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT,
        L"Radium_SimpleInputDialogClass",
        title.c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        hwnd_parent, nullptr, hInst, &ctx);

    if (!hDlg) return false;

    HICON hDlgIcon = CreateRadiumAppIcon();
    if (hDlgIcon) {
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hDlgIcon));
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hDlgIcon));
    }

    ApplyWindowGlassBackdropWin32(hDlg, ctx.palette, true /* is_transient_dialog */);

    EnableWindow(hwnd_parent, FALSE);
    SetForegroundWindow(hDlg);
    SetFocus(ctx.hEdit);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(hDlg);
                break;
            } else if (msg.wParam == VK_RETURN) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(7002, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hOkBtn));
                if (!IsWindow(hDlg)) break;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    EnableWindow(hwnd_parent, TRUE);
    SetForegroundWindow(hwnd_parent);
    if (ctx.submitted) {
        out_value = ctx.result_value;
        return true;
    }
    return false;
}

struct DeleteConfirmDialogContext {
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    std::vector<std::string> task_ids;
    std::vector<std::pair<std::string, std::string>> files_info;
    UINT dpi{96};
    HWND hDlg{nullptr};
    HWND owner{nullptr};
    HWND hRemoveBtn{nullptr};
    HWND hPermanentBtn{nullptr};
    HWND hCancelBtn{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontTitle{nullptr};
    HFONT hFontSmall{nullptr};
    HBRUSH hBgBrush{nullptr};
};

LRESULT CALLBACK DeleteConfirmDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<DeleteConfirmDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<DeleteConfirmDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            ctx->hDlg = hwnd;
            ctx->dpi = GetDpiForWindow(hwnd);
            if (ctx->dpi == 0) ctx->dpi = 96;

            ctx->hFontBody = CreateFontW(-DesignSystem::ScaleForDpi(14, ctx->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold = CreateFontW(-DesignSystem::ScaleForDpi(14, ctx->dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontTitle = CreateFontW(-DesignSystem::ScaleForDpi(18, ctx->dpi), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontSmall = CreateFontW(-DesignSystem::ScaleForDpi(12, ctx->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hBgBrush = CreateSolidBrush(ctx->view->GetPalette().app_background.ToWin32ColorRef());

            HINSTANCE hInst = GetModuleHandleW(nullptr);

            int btn_h = DesignSystem::ScaleForDpi(36, ctx->dpi);
            int pad = DesignSystem::ScaleForDpi(18, ctx->dpi);
            int perm_w = DesignSystem::ScaleForDpi(158, ctx->dpi);
            int remove_w = DesignSystem::ScaleForDpi(138, ctx->dpi);
            int cancel_w = DesignSystem::ScaleForDpi(90, ctx->dpi);
            int spacing = DesignSystem::ScaleForDpi(8, ctx->dpi);

            RECT client_rc{};
            GetClientRect(hwnd, &client_rc);
            int btn_y = (client_rc.bottom > 0) ? (client_rc.bottom - btn_h - pad) : DesignSystem::ScaleForDpi(180, ctx->dpi);
            int buttons_w = perm_w + remove_w + cancel_w + spacing * 2;
            int btn_x = std::max(pad, (static_cast<int>(client_rc.right) - buttons_w) / 2);

            // 1. Permanently Delete Button (Coral Red Accent #FF4757)
            ctx->hPermanentBtn = CreateWindowExW(0, L"BUTTON", L"Permanently Delete",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                btn_x, btn_y, perm_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(6001)), hInst, nullptr);

            // 2. Remove from List Button (keeps output files)
            ctx->hRemoveBtn = CreateWindowExW(0, L"BUTTON", L"Remove from List",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON | BS_OWNERDRAW,
                btn_x + perm_w + spacing, btn_y, remove_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(6004)), hInst, nullptr);

            // 3. Cancel Button (Safe Default, Initially Focused)
            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW,
                btn_x + perm_w + spacing + remove_w + spacing, btn_y, cancel_w, btn_h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(6003)), hInst, nullptr);

            return 0;
        }
        case WM_CTLCOLORBTN: {
            return reinterpret_cast<LRESULT>(ctx->hBgBrush);
        }
        case WM_SIZE: {
            if (!ctx || !ctx->hPermanentBtn) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            (void)rc;
            int client_h = rc.bottom - rc.top;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;

            int btn_h = DesignSystem::ScaleForDpi(36, dpi);
            int pad = DesignSystem::ScaleForDpi(18, dpi);
            int spacing = DesignSystem::ScaleForDpi(8, dpi);
            int btn_y = client_h - btn_h - pad;

            int perm_w = DesignSystem::ScaleForDpi(158, dpi);
            int remove_w = DesignSystem::ScaleForDpi(138, dpi);
            int cancel_w = DesignSystem::ScaleForDpi(90, dpi);
            int buttons_w = perm_w + remove_w + cancel_w + spacing * 2;
            int btn_x = std::max(pad, (static_cast<int>(rc.right - rc.left) - buttons_w) / 2);
            SetWindowPos(ctx->hPermanentBtn, nullptr, btn_x, btn_y, perm_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hRemoveBtn, nullptr, btn_x + perm_w + spacing, btn_y, remove_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hCancelBtn, nullptr, btn_x + perm_w + spacing + remove_w + spacing, btn_y, cancel_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;
            HDC hdc = dis->hDC;
            RECT rc = dis->rcItem;
            bool is_sel = (dis->itemState & ODS_SELECTED) != 0;
            bool is_focused = (dis->itemState & ODS_FOCUS) != 0;
            int r = DesignSystem::ScaleForDpi(6, ctx->dpi);
            FillRect(hdc, &rc, ctx->hBgBrush);

            if (dis->CtlID == 6001) { // Permanently Delete (Coral Red Accent #FF4757)
                const auto& accent = ctx->view->GetPalette().accent_primary;
                ColorRgba bg = is_sel ? accent.Interpolate(ColorRgba{0, 0, 0, 255}, 0.15f) : (is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.12f) : accent);
                ColorRgba border = is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.32f) : bg;
                DrawRoundedBoxGdi(hdc, rc, r, bg, border, is_focused ? 1 : 0);
                SelectObject(hdc, ctx->hFontBold);
                SetTextColor(hdc, RGB(255, 255, 255));
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Permanently Delete", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 6003) { // Cancel
                const auto& pal = ctx->view->GetPalette();
                ColorRgba bg = is_sel ? pal.surface_pressed : pal.surface_primary;
                ColorRgba border = is_focused ? pal.border_focus : pal.border_subtle;
                DrawRoundedBoxGdi(hdc, rc, r, bg, border, 1);
                SelectObject(hdc, ctx->hFontBody);
                SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Cancel", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == 6004) { // Remove from List
                const auto& pal = ctx->view->GetPalette();
                ColorRgba bg = is_sel ? pal.surface_pressed : pal.surface_primary;
                ColorRgba border = is_focused ? pal.border_focus : pal.border_subtle;
                DrawRoundedBoxGdi(hdc, rc, r, bg, border, 1);
                SelectObject(hdc, ctx->hFontBold);
                SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
                SetBkMode(hdc, TRANSPARENT);
                DrawTextW(hdc, L"Remove from List", -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            int pad = DesignSystem::ScaleForDpi(18, ctx->dpi);
            int content_x = pad;
            int content_w = rc.right - rc.left - pad * 2;

            // Title: choose how to remove the selected download(s)
            SetBkMode(hdc, TRANSPARENT);
            SelectObject(hdc, ctx->hFontTitle);
            SetTextColor(hdc, ctx->view->GetPalette().text_primary.ToWin32ColorRef());
            int title_y = pad;
            RECT title_rc{content_x, title_y, content_x + content_w, title_y + DesignSystem::ScaleForDpi(26, ctx->dpi)};
            DrawTextW(hdc, L"Remove Download", -1, &title_rc, DT_LEFT | DT_TOP | DT_SINGLELINE);
            const auto& pal = ctx->view->GetPalette();
            RECT divider{content_x, title_y + DesignSystem::ScaleForDpi(26, ctx->dpi), content_x + content_w,
                         title_y + DesignSystem::ScaleForDpi(27, ctx->dpi)};
            FillRectSolid(hdc, divider, pal.border_subtle);
            RECT accent_divider{divider.left, divider.top, divider.left + DesignSystem::ScaleForDpi(38, ctx->dpi), divider.bottom};
            FillRectSolid(hdc, accent_divider, pal.accent_primary);

            // Subtitle / question with middle truncation
            SelectObject(hdc, ctx->hFontBody);
            SetTextColor(hdc, ctx->view->GetPalette().text_secondary.ToWin32ColorRef());
            std::wstring q_text;
            if (ctx->files_info.size() == 1) {
                std::string fname = ctx->files_info[0].first;
                if (fname.size() > 42) {
                    auto dot = fname.rfind('.');
                    std::string ext = (dot != std::string::npos) ? fname.substr(dot) : "";
                    std::string stem = (dot != std::string::npos) ? fname.substr(0, dot) : fname;
                    if (stem.size() > 28) {
                        stem = stem.substr(0, 20) + "..." + stem.substr(stem.size() - 6);
                    }
                    fname = stem + ext;
                }
                q_text = L"Choose how to remove \u201c" + Utf8ToW(fname) + L"\u201d.";
            } else {
                q_text = L"Choose how to remove " + std::to_wstring(ctx->files_info.size()) + L" selected downloads.";
            }
            int q_y = title_y + DesignSystem::ScaleForDpi(28, ctx->dpi);
            RECT q_rc{content_x, q_y, content_x + content_w, q_y + DesignSystem::ScaleForDpi(22, ctx->dpi)};
            DrawTextW(hdc, q_text.c_str(), -1, &q_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Path & hint
            if (!ctx->files_info.empty()) {
                std::wstring path_str;
                if (ctx->files_info.size() == 1) {
                    path_str = L"Location: " + Utf8ToW(ctx->files_info[0].second) + (ctx->files_info[0].second.empty() || ctx->files_info[0].second.back() == '\\' || ctx->files_info[0].second.back() == '/' ? L"" : L"\\") + Utf8ToW(ctx->files_info[0].first);
                } else {
                    path_str = L"Target Folder: " + Utf8ToW(ctx->files_info[0].second) + L" (" + std::to_wstring(ctx->files_info.size()) + L" items)";
                }
                SelectObject(hdc, ctx->hFontSmall);
                SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
                int path_y = q_y + DesignSystem::ScaleForDpi(22, ctx->dpi);
                RECT p_rc{content_x, path_y, content_x + content_w, path_y + DesignSystem::ScaleForDpi(20, ctx->dpi)};
                DrawTextW(hdc, path_str.c_str(), -1, &p_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_PATH_ELLIPSIS);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 6001) { // Permanently Delete
                auto res = ctx->view->ActionDeleteSelectedWithFile();
                if (!res.ok()) {
                    std::string err = res.error.has_value() ? res.error->message : "Unknown error";
                    MessageBoxW(hwnd, Utf8ToW("Deletion failed: " + err).c_str(),
                        L"Error \u2014 Radium", MB_ICONERROR | MB_OK);
                }
                if (ctx->owner && IsWindow(ctx->owner)) {
                    EnableWindow(ctx->owner, TRUE);
                    SetActiveWindow(ctx->owner);
                    SetForegroundWindow(ctx->owner);
                }
                DestroyWindow(hwnd);
                return 0;
            } else if (id == 6003) { // Cancel
                if (ctx->owner && IsWindow(ctx->owner)) {
                    EnableWindow(ctx->owner, TRUE);
                    SetActiveWindow(ctx->owner);
                    SetForegroundWindow(ctx->owner);
                }
                DestroyWindow(hwnd);
                return 0;
            } else if (id == 6004) { // Remove from List
                auto res = ctx->view->ActionRemoveSelectedFromList();
                if (!res.ok()) {
                    std::string err = res.error.has_value() ? res.error->message : "Unknown error";
                    MessageBoxW(hwnd, Utf8ToW("Remove failed: " + err).c_str(), L"Error \u2014 Radium", MB_ICONERROR | MB_OK);
                }
                if (ctx->owner && IsWindow(ctx->owner)) {
                    EnableWindow(ctx->owner, TRUE);
                    SetActiveWindow(ctx->owner);
                    SetForegroundWindow(ctx->owner);
                }
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_DESTROY: {
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
            if (ctx->hFontSmall) DeleteObject(ctx->hFontSmall);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowDeleteConfirmationDialogWin32(HWND hwnd_parent, DesktopView& view, DownloadController& controller) {
    auto checked = view.GetCheckedTaskIds();
    std::vector<std::string> ids;
    if (!checked.empty()) {
        ids.assign(checked.begin(), checked.end());
    } else if (!view.GetSelectedTaskId().empty()) {
        ids.push_back(view.GetSelectedTaskId());
    }
    if (ids.empty()) return;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = DeleteConfirmDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_DeleteConfirmDlgClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    DeleteConfirmDialogContext ctx;
    ctx.owner = hwnd_parent;
    ctx.view = &view;
    ctx.controller = &controller;
    ctx.task_ids = ids;
    for (const auto& tid : ids) {
        auto t = controller.GetTask(tid);
        if (t.has_value()) {
            ctx.files_info.push_back({t->file_name, t->save_dir});
        }
    }

    UINT dpi = GetDpiForWindow(hwnd_parent);
    if (dpi == 0) dpi = 96;
    ctx.dpi = dpi;
    int dlg_w = DesignSystem::ScaleForDpi(500, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(220, dpi);

    RECT work_rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0);
    int screen_w = work_rc.right - work_rc.left;
    int screen_h = work_rc.bottom - work_rc.top;
    if (screen_w <= 0) screen_w = GetSystemMetrics(SM_CXSCREEN);
    if (screen_h <= 0) screen_h = GetSystemMetrics(SM_CYSCREEN);

    RECT prc{};
    bool parent_is_interactive = (hwnd_parent && IsWindow(hwnd_parent) && IsWindowVisible(hwnd_parent) && !IsIconic(hwnd_parent));
    if (parent_is_interactive) {
        GetWindowRect(hwnd_parent, &prc);
    }
    int dlg_x = 0;
    int dlg_y = 0;
    if (!parent_is_interactive || prc.left < -10000 || prc.top < -10000 ||
        (prc.right - prc.left) < 200 || (prc.bottom - prc.top) < 200) {
        dlg_x = work_rc.left + (screen_w - dlg_w) / 2;
        dlg_y = work_rc.top + (screen_h - dlg_h) / 2;
    } else {
        dlg_x = prc.left + (prc.right - prc.left - dlg_w) / 2;
        dlg_y = prc.top + (prc.bottom - prc.top - dlg_h) / 2;
    }
    if (dlg_x < work_rc.left) dlg_x = work_rc.left + 20;
    if (dlg_y < work_rc.top) dlg_y = work_rc.top + 20;

    HWND owner_hwnd = parent_is_interactive ? hwnd_parent : nullptr;
    DWORD ex_style = (owner_hwnd ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT;

    HWND hDlg = CreateWindowExW(
        ex_style,
        L"Radium_DeleteConfirmDlgClass",
        L"Remove Download \u2014 Radium",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        owner_hwnd, nullptr, hInst, &ctx);

    if (!hDlg) return;

    HICON hDlgIcon = CreateRadiumAppIcon();
    if (hDlgIcon) {
        SendMessageW(hDlg, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hDlgIcon));
        SendMessageW(hDlg, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hDlgIcon));
    }

    ApplyWindowGlassBackdropWin32(hDlg, ctx.view->GetPalette(), true /* is_transient_dialog */);

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, FALSE);
    }
    SetWindowPos(hDlg, HWND_TOP, dlg_x, dlg_y, dlg_w, dlg_h, SWP_SHOWWINDOW);
    ShowWindow(hDlg, SW_SHOWNORMAL);
    BringWindowToTop(hDlg);
    SetForegroundWindow(hDlg);
    SetActiveWindow(hDlg);
    SetFocus(ctx.hCancelBtn); // Safe default focus

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                DestroyWindow(hDlg);
                break;
            } else if (msg.wParam == VK_RETURN) {
                HWND focused = GetFocus();
                if (focused == ctx.hPermanentBtn) {
                    SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(6001, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hPermanentBtn));
                } else if (focused == ctx.hRemoveBtn) {
                    SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(6004, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hRemoveBtn));
                } else {
                    // Default enter action: Safe option (Cancel)
                    DestroyWindow(hDlg);
                }
                if (!IsWindow(hDlg)) break;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (owner_hwnd && IsWindow(owner_hwnd)) {
        EnableWindow(owner_hwnd, TRUE);
        SetForegroundWindow(owner_hwnd);
        InvalidateRect(owner_hwnd, nullptr, TRUE);
    }
}

namespace {

void DrawCaretGlyphGdi(HDC hdc, int cx, int cy, ColorRgba col, bool up = false);
void DrawRoundedBoxGdi(HDC hdc, const RECT& rc, int radius, ColorRgba bg, ColorRgba border, int border_w);

void PaintSettingsCenterOverlayWin32(HDC hdc, DesktopView& view, const RECT& client_rc, HFONT hFontBody, HFONT hFontBold) {
    const auto& pal = view.GetPalette();
    const auto& m = view.GetMetrics();
    auto sv = view.BuildSettingsCenterView();

    // 1. Entrance transition scrim fade
    float p = view.GetAnimationState().ComputeTransitionProgress(view.GetAnimationState().settings_open_start_ms, 150);
    uint8_t scrim_alpha = static_cast<uint8_t>(200 * p);
    DrawGlassPanelGdi(hdc, client_rc, 0, ColorRgba{0x08, 0x08, 0x0A, scrim_alpha}, ColorRgba{0, 0, 0, 0}, 0, false);

    // 2. Clean, dark, minimal settings container window matching reference screenshot
    int slide_offset_y = static_cast<int>((1.0f - p) * 10.0f);
    RECT modal_rc = ToWinRect(sv.modal_rect);
    modal_rc.top += slide_offset_y;
    modal_rc.bottom += slide_offset_y;

    // Palette tokens tuned to reference design
    ColorRgba window_bg = pal.surface_primary;
    ColorRgba window_border = pal.border_subtle;
    ColorRgba card_bg = pal.surface_elevated;
    ColorRgba card_border = pal.border_subtle;
    ColorRgba divider_col = pal.border_subtle;
    ColorRgba sel_tab_bg = pal.surface_selected;
    ColorRgba sel_tab_border = pal.border_subtle;
    ColorRgba coral_red = pal.accent_primary;

    // Draw main settings window
    DrawRoundedBoxGdi(hdc, modal_rc, DesignSystem::ScaleForDpi(12, m.dpi), window_bg, window_border, 1);

    int header_h = DesignSystem::ScaleForDpi(52, m.dpi);
    int footer_h = DesignSystem::ScaleForDpi(52, m.dpi);

    // Header Title (clean, no heavy gradient/glass box)
    RECT title_rc{modal_rc.left + DesignSystem::ScaleForDpi(20, m.dpi), modal_rc.top, modal_rc.right - DesignSystem::ScaleForDpi(20, m.dpi), modal_rc.top + header_h};
    SelectObject(hdc, hFontBold);
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    DrawTextW(hdc, L"Radium Settings & Configuration Center", -1, &title_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Sidebar categories
    SelectObject(hdc, hFontBody);
    for (const auto& cat : sv.categories) {
        RECT crc = ToWinRect(cat.rect);
        if (cat.selected) {
            // Subtle dark highlighted background with rounded corners
            DrawRoundedBoxGdi(hdc, crc, DesignSystem::ScaleForDpi(8, m.dpi), sel_tab_bg, sel_tab_border, 1);

            // Slim coral red indicator on the left of the selected item
            int bar_w = DesignSystem::ScaleForDpi(3, m.dpi);
            int bar_pad = DesignSystem::ScaleForDpi(7, m.dpi);
            RECT ind_rc{crc.left + DesignSystem::ScaleForDpi(4, m.dpi), crc.top + bar_pad, crc.left + DesignSystem::ScaleForDpi(4, m.dpi) + bar_w, crc.bottom - bar_pad};
            DrawRoundedBoxGdi(hdc, ind_rc, bar_w / 2, coral_red, coral_red, 0);

            SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        } else {
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        }

        std::string lbl = cat.label;
        if (cat.modified_count > 0) {
            lbl += " *";
        }
        std::wstring wlbl = Utf8ToW(lbl);
        RECT trc = crc;
        trc.left += DesignSystem::ScaleForDpi(18, m.dpi);
        DrawTextW(hdc, wlbl.c_str(), -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }

    // Group related settings inside clean, rounded container (clipped strictly to scroll viewport)
    RECT scroll_clip_rc = ToWinRect(sv.content_rect);
    scroll_clip_rc.top += slide_offset_y;
    scroll_clip_rc.bottom += slide_offset_y - 1;
    int saved_scroll_dc = SaveDC(hdc);
    IntersectClipRect(hdc, scroll_clip_rc.left, scroll_clip_rc.top, scroll_clip_rc.right, scroll_clip_rc.bottom);

    if (!sv.visible_items.empty()) {
        RECT group_card_rc{
            sv.visible_items.front().row_rect.left,
            sv.visible_items.front().row_rect.top + slide_offset_y,
            sv.visible_items.back().row_rect.right,
            sv.visible_items.back().row_rect.bottom + slide_offset_y
        };
        DrawRoundedBoxGdi(hdc, group_card_rc, DesignSystem::ScaleForDpi(10, m.dpi), card_bg, card_border, 1);

        // Render rows and controls
        for (size_t i = 0; i < sv.visible_items.size(); ++i) {
            const auto& item = sv.visible_items[i];
            RECT rrc = ToWinRect(item.row_rect);
            rrc.top += slide_offset_y;
            rrc.bottom += slide_offset_y;

            // Subtle divider line between rows (except last item)
            if (i + 1 < sv.visible_items.size()) {
                HPEN hDivPen = CreatePen(PS_SOLID, 1, divider_col.ToWin32ColorRef());
                HGDIOBJ oldDivPen = SelectObject(hdc, hDivPen);
                MoveToEx(hdc, rrc.left + 1, rrc.bottom, nullptr);
                LineTo(hdc, rrc.right - 1, rrc.bottom);
                SelectObject(hdc, oldDivPen);
                DeleteObject(hDivPen);
            }

            // Setting label (prominent)
            RECT lbl_rc = rrc;
            lbl_rc.left += DesignSystem::ScaleForDpi(18, m.dpi);
            lbl_rc.top += DesignSystem::ScaleForDpi(10, m.dpi);
            lbl_rc.right = item.control_rect.left - DesignSystem::ScaleForDpi(16, m.dpi);
            lbl_rc.bottom = rrc.top + DesignSystem::ScaleForDpi(30, m.dpi);
            SelectObject(hdc, hFontBold);
            SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
            std::wstring wtitle = Utf8ToW(item.label + (item.is_modified ? "  [Modified]" : ""));
            DrawTextW(hdc, wtitle.c_str(), -1, &lbl_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Setting description (directly below setting name)
            RECT desc_rc = rrc;
            desc_rc.left += DesignSystem::ScaleForDpi(18, m.dpi);
            desc_rc.top = lbl_rc.bottom;
            desc_rc.right = item.control_rect.left - DesignSystem::ScaleForDpi(16, m.dpi);
            desc_rc.bottom = rrc.bottom - DesignSystem::ScaleForDpi(8, m.dpi);
            SelectObject(hdc, hFontBody);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring wdesc = Utf8ToW(item.description);
            DrawTextW(hdc, wdesc.c_str(), -1, &desc_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);

            // Control element aligned consistently on the right
            RECT ctrl_rc = ToWinRect(item.control_rect);
            ctrl_rc.top += slide_offset_y;
            ctrl_rc.bottom += slide_offset_y;
            if (item.control_type == SettingControlType::kToggleSwitch) {
                // Redesigned modern switch matching reference screenshot
                int sw_w = DesignSystem::ScaleForDpi(42, m.dpi);
                int sw_h = DesignSystem::ScaleForDpi(22, m.dpi);
                int sw_x = ctrl_rc.right - sw_w;
                int sw_y = ctrl_rc.top + (ctrl_rc.bottom - ctrl_rc.top - sw_h) / 2;
                RECT sw_rc{sw_x, sw_y, sw_x + sw_w, sw_y + sw_h};

                ColorRgba track_bg = item.bool_value ? coral_red : ColorRgba{0x36, 0x38, 0x42, 255};
                ColorRgba track_border = item.bool_value ? coral_red : ColorRgba{0x44, 0x46, 0x50, 255};
                DrawRoundedBoxGdi(hdc, sw_rc, sw_h / 2, track_bg, track_border, 0);

                int thumb_sz = sw_h - 4;
                int thumb_x = item.bool_value ? (sw_rc.right - thumb_sz - 2) : (sw_rc.left + 2);
                int thumb_y = sw_y + 2;
                RECT thumb_rc{thumb_x, thumb_y, thumb_x + thumb_sz, thumb_y + thumb_sz};
                DrawRoundedBoxGdi(hdc, thumb_rc, thumb_sz / 2, ColorRgba{255, 255, 255, 255}, ColorRgba{255, 255, 255, 255}, 0);
            } else {
                // Modernized button / dropdown / text field
                bool is_action = (item.control_type == SettingControlType::kActionButton);
                bool is_checking_updates = (is_action && item.key == "check_for_updates" && item.value_text == "Checking...");
                ColorRgba ctrl_bg = is_action ? (is_checking_updates ? pal.surface_elevated : coral_red) : pal.surface_primary;
                ColorRgba ctrl_border = is_action ? (is_checking_updates ? pal.border_focus : coral_red) : pal.border_subtle;
                DrawRoundedBoxGdi(hdc, ctrl_rc, DesignSystem::ScaleForDpi(6, m.dpi), ctrl_bg, ctrl_border, 1);

                SelectObject(hdc, is_action ? hFontBold : hFontBody);
                SetTextColor(hdc, is_action ? (is_checking_updates ? pal.text_primary.ToWin32ColorRef() : RGB(255, 255, 255)) : pal.text_primary.ToWin32ColorRef());
                std::wstring wval = Utf8ToW(item.value_text);
                if (is_checking_updates) {
                    int spin_sz = DesignSystem::ScaleForDpi(12, m.dpi);
                    int spin_x = ctrl_rc.left + DesignSystem::ScaleForDpi(10, m.dpi);
                    int spin_y = ctrl_rc.top + (ctrl_rc.bottom - ctrl_rc.top - spin_sz) / 2;
                    float start_deg = static_cast<float>((GetTickCount64() / 3) % 360);
                    Gdiplus::Graphics g(hdc);
                    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::Pen track_pen(Gdiplus::Color(70, 255, 255, 255), 2.0f);
                    g.DrawEllipse(&track_pen, spin_x, spin_y, spin_sz, spin_sz);
                    Gdiplus::Pen arc_pen(Gdiplus::Color(255, coral_red.r, coral_red.g, coral_red.b), 2.0f);
                    arc_pen.SetStartCap(Gdiplus::LineCapRound);
                    arc_pen.SetEndCap(Gdiplus::LineCapRound);
                    g.DrawArc(&arc_pen, spin_x, spin_y, spin_sz, spin_sz, start_deg, 250.0f);

                    RECT txt_rc = ctrl_rc;
                    txt_rc.left = spin_x + spin_sz + DesignSystem::ScaleForDpi(6, m.dpi);
                    txt_rc.right -= DesignSystem::ScaleForDpi(8, m.dpi);
                    DrawTextW(hdc, wval.c_str(), -1, &txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                } else if (item.control_type == SettingControlType::kDropdown) {
                    RECT txt_rc = ctrl_rc;
                    txt_rc.right -= DesignSystem::ScaleForDpi(18, m.dpi);
                    txt_rc.left += DesignSystem::ScaleForDpi(10, m.dpi);
                    DrawTextW(hdc, wval.c_str(), -1, &txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    DrawCaretGlyphGdi(hdc, ctrl_rc.right - DesignSystem::ScaleForDpi(12, m.dpi), ctrl_rc.top + (ctrl_rc.bottom - ctrl_rc.top) / 2, ColorRgba{0x8E, 0x92, 0xA0, 255}, false);
                } else {
                    DrawTextW(hdc, wval.c_str(), -1, &ctrl_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                }
            }
        }
    }

    // Subtle slim scrollbar indicator when content overflows viewport
    if (sv.max_scroll > 0 && sv.content_total_height > 0) {
        int track_top = scroll_clip_rc.top + DesignSystem::ScaleForDpi(8, m.dpi);
        int track_bottom = scroll_clip_rc.bottom - DesignSystem::ScaleForDpi(8, m.dpi);
        int track_h = std::max(12, track_bottom - track_top);
        int vp_h = std::max(1, static_cast<int>(scroll_clip_rc.bottom - scroll_clip_rc.top));
        int thumb_h = std::max(DesignSystem::ScaleForDpi(24, m.dpi),
            static_cast<int>(static_cast<long long>(track_h) * vp_h / sv.content_total_height));
        int thumb_travel = std::max(0, track_h - thumb_h);
        int thumb_y = track_top + (sv.max_scroll > 0 ? static_cast<int>(static_cast<long long>(thumb_travel) * sv.scroll_offset / sv.max_scroll) : 0);
        int sb_w = DesignSystem::ScaleForDpi(4, m.dpi);
        RECT sb_rc{scroll_clip_rc.right - DesignSystem::ScaleForDpi(8, m.dpi), thumb_y,
                   scroll_clip_rc.right - DesignSystem::ScaleForDpi(8, m.dpi) + sb_w, thumb_y + thumb_h};
        DrawRoundedBoxGdi(hdc, sb_rc, sb_w / 2, pal.border_subtle, pal.border_subtle, 0);
    }

    RestoreDC(hdc, saved_scroll_dc);

    // Sticky footer bar attached to bottom of settings window (opaque fill + 1px top separator)
    RECT ftr_rc{modal_rc.left + 1, modal_rc.bottom - footer_h, modal_rc.right - 1, modal_rc.bottom - 1};
    RECT ftr_fill{ftr_rc.left, ftr_rc.top, ftr_rc.right, modal_rc.bottom - DesignSystem::ScaleForDpi(12, m.dpi)};
    if (ftr_fill.bottom > ftr_fill.top) {
        FillRectSolid(hdc, ftr_fill, window_bg);
    }
    HPEN hFtrPen = CreatePen(PS_SOLID, 1, divider_col.ToWin32ColorRef());
    HGDIOBJ oldFtrPen = SelectObject(hdc, hFtrPen);
    MoveToEx(hdc, ftr_rc.left, ftr_rc.top, nullptr);
    LineTo(hdc, ftr_rc.right, ftr_rc.top);
    SelectObject(hdc, oldFtrPen);
    DeleteObject(hFtrPen);

    // Restore Defaults button
    RECT rst_rc = ToWinRect(sv.restore_defaults_rect);
    DrawRoundedBoxGdi(hdc, rst_rc, DesignSystem::ScaleForDpi(6, m.dpi), pal.surface_primary, pal.border_subtle, 1);
    SelectObject(hdc, hFontBody);
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    const std::wstring restore_text = Utf8ToW(GetLocalizationManager().Translate("Restore defaults"));
    DrawTextW(hdc, restore_text.c_str(), -1, &rst_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Cancel button
    RECT can_rc = ToWinRect(sv.cancel_button_rect);
    DrawRoundedBoxGdi(hdc, can_rc, DesignSystem::ScaleForDpi(6, m.dpi), pal.surface_primary, pal.border_subtle, 1);
    const std::wstring cancel_text = Utf8ToW(GetLocalizationManager().Translate("Cancel"));
    DrawTextW(hdc, cancel_text.c_str(), -1, &can_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Primary Apply & Save button in coral red
    RECT app_rc = ToWinRect(sv.apply_button_rect);
    ColorRgba app_bg = sv.is_valid ? coral_red : pal.status_danger;
    DrawRoundedBoxGdi(hdc, app_rc, DesignSystem::ScaleForDpi(6, m.dpi), app_bg, app_bg, 0);
    SelectObject(hdc, hFontBold);
    SetTextColor(hdc, RGB(255, 255, 255));
    const std::wstring apply_text = Utf8ToW(GetLocalizationManager().Translate("Apply & Save"));
    DrawTextW(hdc, apply_text.c_str(), -1, &app_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    // Status message beside Restore Defaults
    RECT banner_rc{rst_rc.right + DesignSystem::ScaleForDpi(14, m.dpi), ftr_rc.top, can_rc.left - DesignSystem::ScaleForDpi(14, m.dpi), ftr_rc.bottom};
    SelectObject(hdc, hFontBody);
    SetTextColor(hdc, sv.is_valid ? RGB(0x9E, 0xA2, 0xAD) : pal.status_danger.ToWin32ColorRef());
    std::wstring wbanner = Utf8ToW(sv.status_banner);
    DrawTextW(hdc, wbanner.c_str(), -1, &banner_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}
void DrawRoundedBoxGdi(HDC hdc, const RECT& rc, int radius, ColorRgba bg, ColorRgba border, int border_w) {
    HPEN hPen = (border_w > 0) ? CreatePen(PS_SOLID, border_w, border.ToWin32ColorRef()) : CreatePen(PS_NULL, 0, 0);
    HBRUSH hBrush = CreateSolidBrush(bg.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);

    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, radius, radius);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawCheckboxGdi(HDC hdc, const RECT& rc, bool checked, const ThemePalette& pal) {
    const bool is_light = (pal.mode == ThemeMode::kLight);
    ColorRgba actual_bg = checked ? pal.accent_primary : (is_light ? ColorRgba{0xFF, 0xFF, 0xFF, 255} : pal.surface_elevated);
    ColorRgba actual_border = checked ? pal.accent_primary : (is_light ? ColorRgba{0x94, 0xA3, 0xB8, 255} : ColorRgba{0x52, 0x52, 0x5E, 255});

    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float x = static_cast<float>(rc.left);
    float y = static_cast<float>(rc.top);
    float w = static_cast<float>(std::max<LONG>(4, rc.right - rc.left));
    float h = static_cast<float>(std::max<LONG>(4, rc.bottom - rc.top));
    float r = std::min(4.0f, std::min(w, h) * 0.3f);
    float d = r * 2.0f;

    Gdiplus::GraphicsPath path;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();

    Gdiplus::SolidBrush fill_b(Gdiplus::Color(255, actual_bg.r, actual_bg.g, actual_bg.b));
    g.FillPath(&fill_b, &path);

    if (!checked) {
        Gdiplus::Pen brd_pen(Gdiplus::Color(255, actual_border.r, actual_border.g, actual_border.b), 1.2f);
        g.DrawPath(&brd_pen, &path);
    } else {
        Gdiplus::Pen chk_pen(Gdiplus::Color(255, 255, 255, 255), 1.8f);
        chk_pen.SetStartCap(Gdiplus::LineCapRound);
        chk_pen.SetEndCap(Gdiplus::LineCapRound);
        chk_pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::PointF pts[3] = {
            {x + w * 0.23f, y + h * 0.52f},
            {x + w * 0.43f, y + h * 0.72f},
            {x + w * 0.78f, y + h * 0.29f}
        };
        g.DrawLines(&chk_pen, pts, 3);
    }
}

void DrawCheckboxGdi(HDC hdc, const RECT& rc, bool checked, ColorRgba bg, ColorRgba border, ColorRgba check_col) {
    ColorRgba actual_bg = checked ? border : bg;
    ColorRgba actual_border = border;
    DrawRoundedBoxGdi(hdc, rc, 4, actual_bg, actual_border, 1);
    if (checked) {
        Gdiplus::Graphics g(hdc);
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        float x = static_cast<float>(rc.left);
        float y = static_cast<float>(rc.top);
        float w = static_cast<float>(std::max<LONG>(4, rc.right - rc.left));
        float h = static_cast<float>(std::max<LONG>(4, rc.bottom - rc.top));
        Gdiplus::Pen chk_pen(Gdiplus::Color(255, check_col.r, check_col.g, check_col.b), 1.8f);
        chk_pen.SetStartCap(Gdiplus::LineCapRound);
        chk_pen.SetEndCap(Gdiplus::LineCapRound);
        chk_pen.SetLineJoin(Gdiplus::LineJoinRound);
        Gdiplus::PointF pts[3] = {
            {x + w * 0.23f, y + h * 0.52f},
            {x + w * 0.43f, y + h * 0.72f},
            {x + w * 0.78f, y + h * 0.29f}
        };
        g.DrawLines(&chk_pen, pts, 3);
    }
}

void DrawDocFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(col.a, col.r, col.g, col.b), 1.6f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);
    float fold = fw * 0.35f;

    Gdiplus::PointF pts[5] = {
        {fx, fy},
        {fx + fw - fold, fy},
        {fx + fw, fy + fold},
        {fx + fw, fy + fh},
        {fx, fy + fh}
    };
    g.DrawPolygon(&pen, pts, 5);
    g.DrawLine(&pen, fx + fw - fold, fy, fx + fw - fold, fy + fold);
    g.DrawLine(&pen, fx + fw - fold, fy + fold, fx + fw, fy + fold);
}

void DrawVideoFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Premium Lucide video icon: rounded camera body + angled lens
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b), 1.6f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);

    // Box body: 68% width
    float body_w = fw * 0.68f;
    float body_h = fh * 0.72f;
    float body_y = fy + (fh - body_h) * 0.5f;
    g.DrawRectangle(&pen, fx, body_y, body_w, body_h);

    // Lens trapezoid
    Gdiplus::PointF lens[3] = {
        {fx + body_w, fy + fh * 0.38f},
        {fx + fw, fy + fh * 0.22f},
        {fx + fw, fy + fh * 0.78f}
    };
    g.DrawLines(&pen, lens, 3);
    g.DrawLine(&pen, fx + fw, fy + fh * 0.78f, fx + body_w, fy + fh * 0.62f);
}

void DrawAudioFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Premium Lucide music icon: eighth notes connected with beam
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b), 1.6f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);

    // Stems & Beam
    g.DrawLine(&pen, fx + fw * 0.35f, fy + fh * 0.25f, fx + fw * 0.85f, fy + fh * 0.12f);
    g.DrawLine(&pen, fx + fw * 0.35f, fy + fh * 0.25f, fx + fw * 0.35f, fy + fh * 0.75f);
    g.DrawLine(&pen, fx + fw * 0.85f, fy + fh * 0.12f, fx + fw * 0.85f, fy + fh * 0.65f);

    // Note heads
    Gdiplus::SolidBrush br(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b));
    g.FillEllipse(&br, fx + fw * 0.15f, fy + fh * 0.65f, fw * 0.26f, fh * 0.22f);
    g.FillEllipse(&br, fx + fw * 0.65f, fy + fh * 0.55f, fw * 0.26f, fh * 0.22f);
}

void DrawArchiveFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    // Premium Lucide archive box icon
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b), 1.6f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);

    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(w);
    float fh = static_cast<float>(h);

    // Top lid box
    g.DrawRectangle(&pen, fx + 1.0f, fy + 2.0f, fw - 2.0f, fh * 0.28f);
    // Body
    Gdiplus::PointF body[4] = {
        {fx + 2.5f, fy + 2.0f + fh * 0.28f},
        {fx + 2.5f, fy + fh - 1.0f},
        {fx + fw - 2.5f, fy + fh - 1.0f},
        {fx + fw - 2.5f, fy + 2.0f + fh * 0.28f}
    };
    g.DrawLines(&pen, body, 4);
    // Handle slot
    g.DrawLine(&pen, fx + fw * 0.38f, fy + fh * 0.58f, fx + fw * 0.62f, fy + fh * 0.58f);
}

void DrawPdfFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = static_cast<float>(x + w / 2);
    float cy = static_cast<float>(y + h / 2 + 1);
    Gdiplus::Pen pen(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b), 1.3f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    // Subtle PDF indicator mark
    g.DrawLine(&pen, cx - 4.0f, cy - 2.0f, cx + 4.0f, cy - 2.0f);
    g.DrawLine(&pen, cx - 4.0f, cy + 2.0f, cx + 1.0f, cy + 2.0f);
}

void DrawImageFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = static_cast<float>(x + w / 2);
    float cy = static_cast<float>(y + h / 2 + 1);
    Gdiplus::Pen pen(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b), 1.2f);
    g.DrawEllipse(&pen, cx + 1.0f, cy - 4.0f, 3.5f, 3.5f);
    Gdiplus::PointF m1[3] = {{cx - 5.5f, cy + 5.0f}, {cx - 2.0f, cy - 0.5f}, {cx + 2.0f, cy + 5.0f}};
    g.DrawLines(&pen, m1, 3);
    Gdiplus::PointF m2[3] = {{cx - 0.5f, cy + 5.0f}, {cx + 2.5f, cy + 1.5f}, {cx + 5.5f, cy + 5.0f}};
    g.DrawLines(&pen, m2, 3);
}

void DrawExecutableFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = static_cast<float>(x + w / 2);
    float cy = static_cast<float>(y + h / 2 + 1);
    Gdiplus::Pen pen(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b), 1.2f);
    g.DrawRectangle(&pen, cx - 5.0f, cy - 4.0f, 10.0f, 8.0f);
    g.DrawLine(&pen, cx - 5.0f, cy - 1.5f, cx + 5.0f, cy - 1.5f);
}

void DrawDocumentFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = static_cast<float>(x + w / 2);
    float cy = static_cast<float>(y + h / 2 + 1);
    Gdiplus::Pen pen(Gdiplus::Color(accent.a, accent.r, accent.g, accent.b), 1.3f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, cx - 4.0f, cy - 3.0f, cx + 4.0f, cy - 3.0f);
    g.DrawLine(&pen, cx - 4.0f, cy, cx + 4.0f, cy);
    g.DrawLine(&pen, cx - 4.0f, cy + 3.0f, cx + 2.0f, cy + 3.0f);
}

void DrawGenericFileIconGdi(HDC hdc, int x, int y, int w, int h, ColorRgba accent) {
    DrawDocFileIconGdi(hdc, x, y, w, h, accent);
}

void DrawFileTypeIconGdi(HDC hdc, int x, int y, int w, int h, const FormattedTaskRow& r, const ThemePalette& pal) {
    std::string name = ToLower(r.full_filename_tooltip.empty() ? r.display_filename : r.full_filename_tooltip);
    auto ext_pos = name.rfind('.');
    std::string ext = (ext_pos != std::string::npos) ? name.substr(ext_pos) : "";

    ColorRgba glyphCol = (pal.mode == ThemeMode::kLight) ? ColorRgba{0x33, 0x41, 0x55, 255} : ColorRgba{0xD4, 0xD4, 0xD8, 255};

    // Draw vector glyph directly without background box, scaled ~1.2x larger
    int pad = static_cast<int>(static_cast<float>(w) * 0.12f);
    int gx = x + pad;
    int gy = y + pad;
    int gw = w - pad * 2;
    int gh = h - pad * 2;

    if (r.category == "PDF" || ext == ".pdf") {
        DrawPdfFileIconGdi(hdc, gx, gy, gw, gh, glyphCol);
    } else if (r.category == "Audio" || ext == ".mp3" || ext == ".m4a" || ext == ".aac" ||
               ext == ".flac" || ext == ".wav" || ext == ".ogg" || ext == ".opus") {
        DrawAudioFileIconGdi(hdc, gx, gy, gw, gh, glyphCol);
    } else if (r.category == "Compressed" || ext == ".zip" || ext == ".rar" || ext == ".7z" ||
               ext == ".tar" || ext == ".gz" || ext == ".bz2" || ext == ".xz" || ext == ".iso") {
        DrawArchiveFileIconGdi(hdc, gx, gy, gw, gh, glyphCol);
    } else if (r.category == "Image" || ext == ".png" || ext == ".jpg" || ext == ".jpeg" ||
               ext == ".gif" || ext == ".webp" || ext == ".svg" || ext == ".bmp" || ext == ".ico") {
        DrawImageFileIconGdi(hdc, gx, gy, gw, gh, glyphCol);
    } else if (r.category == "Programs" || ext == ".exe" || ext == ".msi" || ext == ".dll" ||
               ext == ".bat" || ext == ".cmd" || ext == ".ps1") {
        DrawExecutableFileIconGdi(hdc, gx, gy, gw, gh, glyphCol);
    } else if (r.is_stream || r.category == "Video" || ext == ".mp4" || ext == ".mkv" || ext == ".webm" ||
               ext == ".avi" || ext == ".mov" || ext == ".flv" || ext == ".ts" || ext == ".m4v" || ext == ".m3u8" || ext == ".mpd") {
        DrawVideoFileIconGdi(hdc, gx, gy, gw, gh, glyphCol);
    } else {
        DrawDocumentFileIconGdi(hdc, gx, gy, gw, gh, glyphCol);
    }
}

void DrawPauseGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    Gdiplus::Pen pen(Gdiplus::Color(col.a, col.r, col.g, col.b), 2.2f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    g.DrawLine(&pen, cx - 3.0f, cy - 5.0f, cx - 3.0f, cy + 5.0f);
    g.DrawLine(&pen, cx + 3.0f, cy - 5.0f, cx + 3.0f, cy + 5.0f);
}

void DrawPlayGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    float sz = 4.5f;
    Gdiplus::PointF pts[3] = {
        {cx - sz + 1.0f, cy - sz},
        {cx + sz + 0.5f, cy},
        {cx - sz + 1.0f, cy + sz}
    };
    Gdiplus::SolidBrush br(Gdiplus::Color(col.a, col.r, col.g, col.b));
    g.FillPolygon(&br, pts, 3);
}

void DrawTrashGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen p(Gdiplus::Color(col.a, col.r, col.g, col.b), 1.5f);
    p.SetStartCap(Gdiplus::LineCapRound);
    p.SetEndCap(Gdiplus::LineCapRound);

    float t_cx = (rc.left + rc.right) * 0.5f;
    float t_cy = (rc.top + rc.bottom) * 0.5f;
    g.DrawLine(&p, t_cx - 6.0f, t_cy - 4.0f, t_cx + 6.0f, t_cy - 4.0f);
    g.DrawLine(&p, t_cx - 2.5f, t_cy - 6.0f, t_cx + 2.5f, t_cy - 6.0f);
    Gdiplus::PointF body[4] = {
        {t_cx - 4.5f, t_cy - 4.0f},
        {t_cx - 3.5f, t_cy + 5.5f},
        {t_cx + 3.5f, t_cy + 5.5f},
        {t_cx + 4.5f, t_cy - 4.0f}
    };
    g.DrawLines(&p, body, 4);
    g.DrawLine(&p, t_cx - 1.5f, t_cy - 1.5f, t_cx - 1.5f, t_cy + 3.5f);
    g.DrawLine(&p, t_cx + 1.5f, t_cy - 1.5f, t_cx + 1.5f, t_cy + 3.5f);
}

void DrawFolderGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen p(Gdiplus::Color(col.a, col.r, col.g, col.b), 1.5f);
    p.SetStartCap(Gdiplus::LineCapRound);
    p.SetEndCap(Gdiplus::LineCapRound);
    p.SetLineJoin(Gdiplus::LineJoinRound);

    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    Gdiplus::PointF pts[7] = {
        {cx - 6.5f, cy - 4.5f},
        {cx - 1.5f, cy - 4.5f},
        {cx,        cy - 2.5f},
        {cx + 6.5f, cy - 2.5f},
        {cx + 6.5f, cy + 5.0f},
        {cx - 6.5f, cy + 5.0f},
        {cx - 6.5f, cy - 4.5f}
    };
    g.DrawLines(&p, pts, 7);
}

void DrawCloseGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen p(Gdiplus::Color(col.a, col.r, col.g, col.b), 1.6f);
    p.SetStartCap(Gdiplus::LineCapRound);
    p.SetEndCap(Gdiplus::LineCapRound);

    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    g.DrawLine(&p, cx - 4.5f, cy - 4.5f, cx + 4.5f, cy + 4.5f);
    g.DrawLine(&p, cx + 4.5f, cy - 4.5f, cx - 4.5f, cy + 4.5f);
}

static std::wstring MiddleTruncateFilename(HDC hdc, const std::wstring& filename, int max_w) {
    if (filename.empty() || max_w <= 0) return filename;
    SIZE sz{};
    if (GetTextExtentPoint32W(hdc, filename.c_str(), static_cast<int>(filename.length()), &sz) && sz.cx <= max_w) {
        return filename;
    }
    auto dot = filename.rfind(L'.');
    std::wstring ext;
    std::wstring stem = filename;
    if (dot != std::wstring::npos && dot > 0 && filename.length() - dot <= 12) {
        ext = filename.substr(dot);
        stem = filename.substr(0, dot);
    }
    const std::wstring ellipsis = L"...";
    int left_len = static_cast<int>(stem.length());
    int keep_tail = std::min(4, left_len / 3);
    std::wstring tail = (keep_tail > 0 && stem.length() >= static_cast<size_t>(keep_tail))
        ? stem.substr(stem.length() - keep_tail) + ext : ext;
    int keep_head = left_len - keep_tail;
    while (keep_head > 2) {
        std::wstring candidate = stem.substr(0, keep_head) + ellipsis + tail;
        GetTextExtentPoint32W(hdc, candidate.c_str(), static_cast<int>(candidate.length()), &sz);
        if (sz.cx <= max_w) return candidate;
        keep_head--;
    }
    while (keep_head >= 1) {
        std::wstring candidate = stem.substr(0, keep_head) + ellipsis + ext;
        GetTextExtentPoint32W(hdc, candidate.c_str(), static_cast<int>(candidate.length()), &sz);
        if (sz.cx <= max_w) return candidate;
        keep_head--;
    }
    return ellipsis + ext;
}

void DrawSearchGlassGdi(HDC hdc, int x, int y, int size, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::Pen pen(Gdiplus::Color(col.a, col.r, col.g, col.b), 1.8f);
    float radius = static_cast<float>(size - 4);
    g.DrawEllipse(&pen, static_cast<float>(x), static_cast<float>(y), radius, radius);
    g.DrawLine(&pen, static_cast<float>(x) + radius * 0.85f, static_cast<float>(y) + radius * 0.85f,
               static_cast<float>(x + size + 2), static_cast<float>(y + size + 2));
}

void DrawGearGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    Gdiplus::Pen pen(Gdiplus::Color(col.a, col.r, col.g, col.b), 1.8f);

    float r_outer = 6.0f;
    g.DrawEllipse(&pen, cx - r_outer, cy - r_outer, r_outer * 2.0f, r_outer * 2.0f);
    float r_inner = 2.5f;
    g.DrawEllipse(&pen, cx - r_inner, cy - r_inner, r_inner * 2.0f, r_inner * 2.0f);

    float tooth = 2.5f;
    for (int i = 0; i < 8; ++i) {
        float angle = i * 3.14159265f / 4.0f;
        float x1 = cx + std::cos(angle) * r_outer;
        float y1 = cy + std::sin(angle) * r_outer;
        float x2 = cx + std::cos(angle) * (r_outer + tooth);
        float y2 = cy + std::sin(angle) * (r_outer + tooth);
        g.DrawLine(&pen, x1, y1, x2, y2);
    }
}

void DrawGlobeGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    Gdiplus::Graphics g(hdc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    float cx = (rc.left + rc.right) * 0.5f;
    float cy = (rc.top + rc.bottom) * 0.5f;
    float radius = std::max(5.0f, static_cast<float>(std::min(rc.right - rc.left, rc.bottom - rc.top)) / 3.0f);
    Gdiplus::Pen pen(Gdiplus::Color(col.a, col.r, col.g, col.b), 1.5f);

    g.DrawEllipse(&pen, cx - radius, cy - radius, radius * 2.0f, radius * 2.0f);
    g.DrawEllipse(&pen, cx - radius * 0.5f, cy - radius, radius, radius * 2.0f);
    g.DrawLine(&pen, cx - radius, cy, cx + radius, cy);
    float lat_y = radius * 0.5f;
    float lat_x = std::sqrt(std::max(0.0f, radius * radius - lat_y * lat_y));
    g.DrawLine(&pen, cx - lat_x, cy - lat_y, cx + lat_x, cy - lat_y);
    g.DrawLine(&pen, cx - lat_x, cy + lat_y, cx + lat_x, cy + lat_y);
}

void DrawThemeGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col, ColorRgba cutout, bool show_moon) {
    const int cx = (rc.left + rc.right) / 2;
    const int cy = (rc.top + rc.bottom) / 2;
    const int glyph_size = static_cast<int>(std::min(rc.right - rc.left, rc.bottom - rc.top));
    const int radius = std::max(4, glyph_size / 4);
    const int stroke = std::max(1, glyph_size / 18);
    HPEN pen = CreatePen(PS_SOLID, stroke, col.ToWin32ColorRef());
    HPEN cutout_pen = CreatePen(PS_SOLID, 1, cutout.ToWin32ColorRef());
    HBRUSH brush = CreateSolidBrush(col.ToWin32ColorRef());
    HBRUSH cutout_brush = CreateSolidBrush(cutout.ToWin32ColorRef());
    HGDIOBJ old_pen = SelectObject(hdc, pen);
    HGDIOBJ old_brush = SelectObject(hdc, brush);
    if (show_moon) {
        Ellipse(hdc, cx - radius, cy - radius, cx + radius + 1, cy + radius + 1);
        HGDIOBJ previous_brush = SelectObject(hdc, cutout_brush);
        HGDIOBJ previous_pen = SelectObject(hdc, cutout_pen);
        const int inner_radius = std::max(3, radius - 1);
        const int crescent_offset = std::max(2, radius / 2);
        Ellipse(hdc, cx - inner_radius + crescent_offset, cy - inner_radius - 1,
                cx + inner_radius + crescent_offset, cy + inner_radius + 1);
        SelectObject(hdc, previous_pen);
        SelectObject(hdc, previous_brush);
    } else {
        // A clean outlined center and short, evenly spaced rays read as a
        // modern sun glyph at toolbar scale without the heavy spoked look.
        const int center_radius = std::max(2, glyph_size / 11);
        const int ring_radius = std::max(center_radius + 2, glyph_size / 5);
        HGDIOBJ previous_brush = SelectObject(hdc, cutout_brush);
        HGDIOBJ previous_pen = SelectObject(hdc, pen);
        Ellipse(hdc, cx - ring_radius, cy - ring_radius, cx + ring_radius + 1, cy + ring_radius + 1);
        SelectObject(hdc, brush);
        Ellipse(hdc, cx - center_radius, cy - center_radius,
                cx + center_radius + 1, cy + center_radius + 1);
        for (int ray = 0; ray < 8; ++ray) {
            const double angle = ray * 3.14159265358979323846 / 4.0;
            const int ray_inner = ring_radius + std::max(2, glyph_size / 14);
            const int ray_outer = ray_inner + std::max(2, glyph_size / 12);
            const int x1 = cx + static_cast<int>(ray_inner * std::cos(angle));
            const int y1 = cy + static_cast<int>(ray_inner * std::sin(angle));
            const int x2 = cx + static_cast<int>(ray_outer * std::cos(angle));
            const int y2 = cy + static_cast<int>(ray_outer * std::sin(angle));
            MoveToEx(hdc, x1, y1, nullptr);
            LineTo(hdc, x2, y2);
        }
        SelectObject(hdc, previous_pen);
        SelectObject(hdc, previous_brush);
    }
    SelectObject(hdc, old_brush);
    SelectObject(hdc, old_pen);
    DeleteObject(brush);
    DeleteObject(cutout_brush);
    DeleteObject(cutout_pen);
    DeleteObject(pen);
}

void DrawSlidersGlyphGdi(HDC hdc, const RECT& rc, ColorRgba col) {
    DrawGearGlyphGdi(hdc, rc, col);
}

void DrawCaretGlyphGdi(HDC hdc, int cx, int cy, ColorRgba col, bool up) {
    int w = 3;
    POINT pts[3];
    if (up) {
        pts[0] = {cx - w, cy + 2};
        pts[1] = {cx, cy - 2};
        pts[2] = {cx + w, cy + 2};
    } else {
        pts[0] = {cx - w, cy - 2};
        pts[1] = {cx, cy + 2};
        pts[2] = {cx + w, cy - 2};
    }
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HBRUSH hBrush = CreateSolidBrush(col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    Polygon(hdc, pts, 3);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawFunnelGlyphGdi(HDC hdc, int x, int y, int size, ColorRgba col) {
    POINT pts[4] = {
        {x, y},
        {x + size, y},
        {x + size / 2 + 1, y + size - 2},
        {x + size / 2 - 1, y + size - 2}
    };
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HBRUSH hBrush = CreateSolidBrush(col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, hBrush);
    Polygon(hdc, pts, 4);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hBrush);
    DeleteObject(hPen);
}

void DrawHamburgerGlyphGdi(HDC hdc, int x, int y, int w, int h, ColorRgba col) {
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    int y0 = y + 1;
    int y1 = y + h / 2;
    int y2 = y + h - 2;
    MoveToEx(hdc, x, y0, nullptr); LineTo(hdc, x + w, y0);
    MoveToEx(hdc, x, y1, nullptr); LineTo(hdc, x + w, y1);
    MoveToEx(hdc, x, y2, nullptr); LineTo(hdc, x + w, y2);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

void DrawTimerClockGdi(HDC hdc, int x, int y, int radius, ColorRgba col) {
    HPEN hPen = CreatePen(PS_SOLID, 1, col.ToWin32ColorRef());
    HGDIOBJ oldPen = SelectObject(hdc, hPen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));

    Ellipse(hdc, x - radius, y - radius, x + radius, y + radius);
    MoveToEx(hdc, x, y, nullptr);
    LineTo(hdc, x, y - radius + 3);
    MoveToEx(hdc, x, y, nullptr);
    LineTo(hdc, x + radius - 4, y);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(hPen);
}

HICON CreateRadiumAppIcon() {
    static HICON s_cached_icon = nullptr;
    if (s_cached_icon) return s_cached_icon;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    s_cached_icon = LoadIconW(hInst, MAKEINTRESOURCEW(1));
    if (s_cached_icon) return s_cached_icon;

    int size = 32;
    HDC hdcScreen = GetDC(nullptr);
    HDC hdc = CreateCompatibleDC(hdcScreen);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = size;
    bi.bmiHeader.biHeight = -size;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP hbmColor = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ oldBm = SelectObject(hdc, hbmColor);

    RECT rc{0, 0, size, size};
    HBRUSH bgBr = CreateSolidBrush(RGB(0x16, 0x16, 0x18));
    FillRect(hdc, &rc, bgBr);
    DeleteObject(bgBr);

    RECT sqRc{2, 2, size - 2, size - 2};
    DrawRoundedBoxGdi(hdc, sqRc, 8, {0xFF, 0x47, 0x57, 255}, {0xFF, 0x47, 0x57, 255}, 1);

    HFONT hFont = CreateFontW(
        -20, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(hdc, hFont);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(255, 255, 255));
    RECT textRc{2, 2, size - 2, size - 2};
    DrawTextW(hdc, L"R", 1, &textRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(hdc, oldFont);
    DeleteObject(hFont);

    SelectObject(hdc, oldBm);
    DeleteDC(hdc);
    ReleaseDC(nullptr, hdcScreen);

    HBITMAP hbmMask = CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = hbmColor;
    s_cached_icon = CreateIconIndirect(&ii);
    DeleteObject(hbmColor);
    DeleteObject(hbmMask);
    return s_cached_icon;
}

// -----------------------------------------------------------------------------
// Radium Dedicated Add Torrent Dialog (qBittorrent Organization & Dark Glass Styling)
// -----------------------------------------------------------------------------
constexpr int kAddTorModeCombo       = 8201;
constexpr int kAddTorSaveDirEdit     = 8202;
constexpr int kAddTorBrowseBtn       = 8203;
constexpr int kAddTorIncompleteChk   = 8204;
constexpr int kAddTorRememberPathChk = 8205;
constexpr int kAddTorCategoryCombo   = 8206;
constexpr int kAddTorDefaultCatChk   = 8207;
constexpr int kAddTorTagsEdit        = 8208;
constexpr int kAddTorStartChk        = 8209;
constexpr int kAddTorStopCondCombo   = 8210;
constexpr int kAddTorQueueTopChk     = 8211;
constexpr int kAddTorSkipHashChk     = 8212;
constexpr int kAddTorSequentialChk   = 8213;
constexpr int kAddTorFirstLastChk    = 8214;
constexpr int kAddTorLayoutCombo     = 8215;
constexpr int kAddTorNeverShowChk    = 8216;
constexpr int kAddTorSaveTorrentBtn  = 8217;
constexpr int kAddTorSelectAllBtn    = 8218;
constexpr int kAddTorSelectNoneBtn   = 8219;
constexpr int kAddTorFilterEdit      = 8220;
constexpr int kAddTorFileList        = 8221;
constexpr int kAddTorOkBtn           = 8222;
constexpr int kAddTorCancelBtn       = 8223;

std::string FormatAddTorrentBytes(uint64_t bytes) {
    const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    std::ostringstream out;
    out << std::fixed << std::setprecision(unit == 0 ? 0 : 2) << value << ' ' << units[unit];
    return out.str();
}

struct AddTorrentTreeNode {
    int id{0};
    int parent_id{-1};
    int depth{0};
    bool is_dir{false};
    bool expanded{true};
    bool checked{true};
    int check_state{1}; // 0 = unchecked, 1 = checked, 2 = partial
    int file_idx{-1};
    uint64_t size{0};
    std::wstring name;
    std::string full_path;
    std::vector<int> children_ids;
};

struct AddTorrentDialogContext {
    HWND hDlg{nullptr};
    HWND owner{nullptr};
    DesktopView* view{nullptr};
    DownloadController* controller{nullptr};
    torrent::TorrentInfo* info{nullptr};
    std::string torrent_file_path;
    std::string target_save_dir;
    bool start_immediately{true};
    bool add_to_queue_top{false};
    bool skip_hash_check{false};
    bool sequential_download{false};
    bool first_last_piece_priority{false};
    bool remember_save_path{true};
    bool use_incomplete_path{false};
    bool never_show_again{false};
    std::string selected_category;
    std::string selected_tags;
    bool accepted{false};
    uint32_t dpi{96};

    // Controls
    HWND hModeCombo{nullptr};
    HWND hSaveDirEdit{nullptr};
    HWND hBrowseBtn{nullptr};
    HWND hIncompleteChk{nullptr};
    HWND hRememberPathChk{nullptr};
    HWND hCategoryCombo{nullptr};
    HWND hDefaultCatChk{nullptr};
    HWND hTagsEdit{nullptr};
    HWND hStartChk{nullptr};
    HWND hStopCondCombo{nullptr};
    HWND hQueueTopChk{nullptr};
    HWND hSkipHashChk{nullptr};
    HWND hSequentialChk{nullptr};
    HWND hFirstLastChk{nullptr};
    HWND hLayoutCombo{nullptr};
    HWND hNeverShowChk{nullptr};
    HWND hSaveTorrentBtn{nullptr};

    HWND hSelectAllBtn{nullptr};
    HWND hSelectNoneBtn{nullptr};
    HWND hFilterEdit{nullptr};
    HWND hFileList{nullptr};
    HWND hSummaryStatic{nullptr};

    HWND hOkBtn{nullptr};
    HWND hCancelBtn{nullptr};

    // Fonts & GDI Resources
    HFONT hFontTitle{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontSmall{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hPanelBrush{nullptr};
    HBRUSH hEditBrush{nullptr};

    // Tree data
    std::vector<AddTorrentTreeNode> all_nodes;
    std::vector<int> visible_node_indices;
    std::string filter_text;

    void BuildTree() {
        all_nodes.clear();
        if (!info) return;

        // Root node
        AddTorrentTreeNode root;
        root.id = 0;
        root.parent_id = -1;
        root.depth = 0;
        root.is_dir = true;
        root.expanded = true;
        root.checked = true;
        root.check_state = 1;
        root.name = Utf8ToW(info->name.empty() ? "Torrent Contents" : info->name);
        root.full_path = info->name;
        root.size = 0;
        all_nodes.push_back(root);

        for (size_t f_idx = 0; f_idx < info->files.size(); ++f_idx) {
            const auto& file = info->files[f_idx];
            std::string p_str = file.relative_path.generic_string();
            // Split path components
            std::vector<std::string> parts;
            std::string cur;
            for (char c : p_str) {
                if (c == '/' || c == '\\') {
                    if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
                } else {
                    cur.push_back(c);
                }
            }
            if (!cur.empty()) parts.push_back(cur);

            int current_parent_id = 0;
            int current_depth = 1;
            std::string accumulated_path = info->name;

            for (size_t p_i = 0; p_i < parts.size(); ++p_i) {
                bool is_leaf = (p_i == parts.size() - 1);
                accumulated_path += "/" + parts[p_i];

                int found_child_id = -1;
                for (int cid : all_nodes[current_parent_id].children_ids) {
                    if (all_nodes[cid].name == Utf8ToW(parts[p_i])) {
                        found_child_id = cid;
                        break;
                    }
                }

                if (found_child_id == -1) {
                    AddTorrentTreeNode node;
                    node.id = static_cast<int>(all_nodes.size());
                    node.parent_id = current_parent_id;
                    node.depth = current_depth;
                    node.is_dir = !is_leaf;
                    node.expanded = true;
                    node.checked = is_leaf ? file.selected : true;
                    node.check_state = node.checked ? 1 : 0;
                    node.file_idx = is_leaf ? static_cast<int>(f_idx) : -1;
                    node.size = is_leaf ? file.length : 0;
                    node.name = Utf8ToW(parts[p_i]);
                    node.full_path = accumulated_path;

                    all_nodes[current_parent_id].children_ids.push_back(node.id);
                    all_nodes.push_back(node);
                    found_child_id = node.id;
                }
                current_parent_id = found_child_id;
                current_depth++;
            }
        }

        // Compute directory sizes
        RecalculateDirectorySizes(0);
        UpdateCheckStatesFromLeaves();
        RebuildVisibleList();
    }

    uint64_t RecalculateDirectorySizes(int node_id) {
        if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return 0;
        if (!all_nodes[node_id].is_dir) return all_nodes[node_id].size;
        uint64_t total = 0;
        for (int cid : all_nodes[node_id].children_ids) {
            total += RecalculateDirectorySizes(cid);
        }
        all_nodes[node_id].size = total;
        return total;
    }

    void UpdateCheckStatesFromLeaves() {
        for (int i = static_cast<int>(all_nodes.size()) - 1; i >= 0; --i) {
            if (all_nodes[i].is_dir) {
                if (all_nodes[i].children_ids.empty()) {
                    all_nodes[i].check_state = all_nodes[i].checked ? 1 : 0;
                    continue;
                }
                int checked_count = 0;
                int partial_count = 0;
                for (int cid : all_nodes[i].children_ids) {
                    if (all_nodes[cid].check_state == 1) checked_count++;
                    else if (all_nodes[cid].check_state == 2) partial_count++;
                }
                if (checked_count == static_cast<int>(all_nodes[i].children_ids.size())) {
                    all_nodes[i].check_state = 1;
                    all_nodes[i].checked = true;
                } else if (checked_count == 0 && partial_count == 0) {
                    all_nodes[i].check_state = 0;
                    all_nodes[i].checked = false;
                } else {
                    all_nodes[i].check_state = 2; // Partial
                    all_nodes[i].checked = true;
                }
            } else {
                all_nodes[i].check_state = all_nodes[i].checked ? 1 : 0;
            }
        }
    }

    void SetNodeChecked(int node_id, bool checked) {
        if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return;
        all_nodes[node_id].checked = checked;
        all_nodes[node_id].check_state = checked ? 1 : 0;
        for (int cid : all_nodes[node_id].children_ids) {
            SetNodeChecked(cid, checked);
        }
    }

    void ToggleNode(int node_id) {
        if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return;
        bool new_state = (all_nodes[node_id].check_state == 0);
        SetNodeChecked(node_id, new_state);
        UpdateCheckStatesFromLeaves();
        SyncToFileInfo();
        RebuildVisibleList();
    }

    void SyncToFileInfo() {
        if (!info) return;
        for (const auto& node : all_nodes) {
            if (!node.is_dir && node.file_idx >= 0 && static_cast<size_t>(node.file_idx) < info->files.size()) {
                info->files[static_cast<size_t>(node.file_idx)].selected = (node.check_state == 1);
            }
        }
    }

    void RebuildVisibleList() {
        visible_node_indices.clear();
        std::string filter_low = ToLower(filter_text);
        std::function<void(int)> collect = [&](int node_id) {
            if (node_id < 0 || node_id >= static_cast<int>(all_nodes.size())) return;
            const auto& node = all_nodes[node_id];

            bool matches_filter = true;
            if (!filter_low.empty()) {
                std::string full_low = ToLower(node.full_path);
                matches_filter = (full_low.find(filter_low) != std::string::npos);
            }

            if (matches_filter || filter_low.empty()) {
                visible_node_indices.push_back(node_id);
            }

            if (node.is_dir && (node.expanded || !filter_low.empty())) {
                for (int cid : node.children_ids) {
                    collect(cid);
                }
            }
        };

        if (!all_nodes.empty()) {
            collect(0);
        }

        if (hFileList && IsWindow(hFileList)) {
            SendMessageW(hFileList, LB_RESETCONTENT, 0, 0);
            SendMessageW(hFileList, LB_SETCOUNT, static_cast<WPARAM>(visible_node_indices.size()), 0);
            InvalidateRect(hFileList, nullptr, FALSE);
        }
    }

    std::wstring GetSummaryText() const {
        if (!info) return L"";
        uint64_t sel_bytes = 0;
        size_t sel_files = 0;
        for (const auto& f : info->files) {
            if (f.selected) {
                sel_bytes += f.length;
                sel_files++;
            }
        }
        std::string s = "Selected " + FormatAddTorrentBytes(sel_bytes) + " of " + FormatAddTorrentBytes(info->total_length) +
                        " (" + std::to_string(sel_files) + "/" + std::to_string(info->files.size()) + " files)";
        return Utf8ToW(s);
    }
};

LRESULT CALLBACK AddTorrentDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<AddTorrentDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<AddTorrentDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        if (ctx) ctx->hDlg = hwnd;
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            HINSTANCE hInst = GetModuleHandleW(nullptr);
            ctx->dpi = ctx->view ? ctx->view->GetMetrics().dpi : 96;
            if (ctx->dpi == 0) ctx->dpi = 96;

            int f_title_sz = -DesignSystem::ScaleForDpi(16, ctx->dpi);
            int f_body_sz  = -DesignSystem::ScaleForDpi(12, ctx->dpi);
            int f_small_sz = -DesignSystem::ScaleForDpi(11, ctx->dpi);

            ctx->hFontTitle = CreateFontW(f_title_sz, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBold  = CreateFontW(f_body_sz, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBody  = CreateFontW(f_body_sz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontSmall = CreateFontW(f_small_sz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

            const auto& palette = ctx->view->GetPalette();
            ctx->hBgBrush = CreateSolidBrush(palette.app_background.ToWin32ColorRef());
            ctx->hPanelBrush = CreateSolidBrush(palette.surface_primary.ToWin32ColorRef());
            ctx->hEditBrush = CreateSolidBrush(palette.surface_elevated.ToWin32ColorRef());

            RECT rcClient{};
            GetClientRect(hwnd, &rcClient);
            int w = rcClient.right - rcClient.left;
            int h = rcClient.bottom - rcClient.top;

            int left_w = DesignSystem::ScaleForDpi(330, ctx->dpi);
            int margin = DesignSystem::ScaleForDpi(14, ctx->dpi);
            int ctrl_h = DesignSystem::ScaleForDpi(26, ctx->dpi);
            int combo_h = DesignSystem::ScaleForDpi(200, ctx->dpi);

            // --- LEFT PANE CONTROLS ---
            int left_x = margin + DesignSystem::ScaleForDpi(12, ctx->dpi);
            int left_ctrl_w = left_w - DesignSystem::ScaleForDpi(24, ctx->dpi);

            // 1. Management Mode (label at 24, combo at 42)
            ctx->hModeCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(42, ctx->dpi), left_ctrl_w, combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorModeCombo)), hInst, nullptr);
            SendMessageW(ctx->hModeCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Manual"));
            SendMessageW(ctx->hModeCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Automatic"));
            SendMessageW(ctx->hModeCombo, CB_SETCURSEL, 0, 0);
            InstallModernComboBox(ctx->hModeCombo, ctx->dpi, palette, palette.surface_primary);

            // 2. Save Path (label at 78, edit at 96, checkboxes at 128, 150)
            int y_save = DesignSystem::ScaleForDpi(96, ctx->dpi);
            int edit_w = left_ctrl_w - DesignSystem::ScaleForDpi(34, ctx->dpi);
            ctx->hSaveDirEdit = CreateWindowExW(0, L"EDIT", Utf8ToW(ctx->target_save_dir).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                left_x, y_save, edit_w, ctrl_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSaveDirEdit)), hInst, nullptr);
            SendMessageW(ctx->hSaveDirEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            InstallModernEditControl(ctx->hSaveDirEdit, ctx->dpi, palette);

            ctx->hBrowseBtn = CreateWindowExW(0, L"BUTTON", L"...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                left_x + edit_w + DesignSystem::ScaleForDpi(6, ctx->dpi), y_save, DesignSystem::ScaleForDpi(28, ctx->dpi), ctrl_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorBrowseBtn)), hInst, nullptr);

            ctx->hIncompleteChk = CreateWindowExW(0, L"BUTTON", L"Use another path for incomplete torrent",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(128, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorIncompleteChk)), hInst, nullptr);
            SendMessageW(ctx->hIncompleteChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            InstallModernCheckBox(ctx->hIncompleteChk, ctx->dpi, palette, palette.surface_primary);

            ctx->hRememberPathChk = CreateWindowExW(0, L"BUTTON", L"Remember last used save path",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(150, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorRememberPathChk)), hInst, nullptr);
            SendMessageW(ctx->hRememberPathChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            SendMessageW(ctx->hRememberPathChk, BM_SETCHECK, BST_CHECKED, 0);
            InstallModernCheckBox(ctx->hRememberPathChk, ctx->dpi, palette, palette.surface_primary);

            // 3. Category (label at 180, combo at 198, chk at 230)
            ctx->hCategoryCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(198, ctx->dpi), left_ctrl_w, combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorCategoryCombo)), hInst, nullptr);
            SendMessageW(ctx->hCategoryCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"(None)"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Torrents"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Video"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Music"));
            SendMessageW(ctx->hCategoryCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Software"));
            SendMessageW(ctx->hCategoryCombo, CB_SETCURSEL, 1, 0);
            InstallModernComboBox(ctx->hCategoryCombo, ctx->dpi, palette, palette.surface_primary);

            ctx->hDefaultCatChk = CreateWindowExW(0, L"BUTTON", L"Set as default category",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(230, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorDefaultCatChk)), hInst, nullptr);
            SendMessageW(ctx->hDefaultCatChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            InstallModernCheckBox(ctx->hDefaultCatChk, ctx->dpi, palette, palette.surface_primary);

            // 4. Tags (label at 260, edit at 278)
            ctx->hTagsEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(278, ctx->dpi), left_ctrl_w, ctrl_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorTagsEdit)), hInst, nullptr);
            SendMessageW(ctx->hTagsEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hTagsEdit, 0x1501 /* EM_SETCUEBANNER */, TRUE, reinterpret_cast<LPARAM>(L"e.g., anime, 1080p, season-1"));
            InstallModernEditControl(ctx->hTagsEdit, ctx->dpi, palette);

            // 5. Transfer & Queue (label at 314, start chk at 332)
            ctx->hStartChk = CreateWindowExW(0, L"BUTTON", L"Start torrent",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(332, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorStartChk)), hInst, nullptr);
            SendMessageW(ctx->hStartChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
            SendMessageW(ctx->hStartChk, BM_SETCHECK, ctx->start_immediately ? BST_CHECKED : BST_UNCHECKED, 0);
            InstallModernCheckBox(ctx->hStartChk, ctx->dpi, palette, palette.surface_primary);

            // 6. Stop Condition (label at 360, combo at 378, checkboxes at 410..476)
            ctx->hStopCondCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(378, ctx->dpi), left_ctrl_w, combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorStopCondCombo)), hInst, nullptr);
            SendMessageW(ctx->hStopCondCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hStopCondCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
            SendMessageW(ctx->hStopCondCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Metadata received"));
            SendMessageW(ctx->hStopCondCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Files checked"));
            SendMessageW(ctx->hStopCondCombo, CB_SETCURSEL, 0, 0);
            InstallModernComboBox(ctx->hStopCondCombo, ctx->dpi, palette, palette.surface_primary);

            ctx->hQueueTopChk = CreateWindowExW(0, L"BUTTON", L"Add to top of queue",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(410, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorQueueTopChk)), hInst, nullptr);
            SendMessageW(ctx->hQueueTopChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            InstallModernCheckBox(ctx->hQueueTopChk, ctx->dpi, palette, palette.surface_primary);

            ctx->hSkipHashChk = CreateWindowExW(0, L"BUTTON", L"Skip hash check",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(432, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSkipHashChk)), hInst, nullptr);
            SendMessageW(ctx->hSkipHashChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            InstallModernCheckBox(ctx->hSkipHashChk, ctx->dpi, palette, palette.surface_primary);

            ctx->hSequentialChk = CreateWindowExW(0, L"BUTTON", L"Download in sequential order",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(454, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSequentialChk)), hInst, nullptr);
            SendMessageW(ctx->hSequentialChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            InstallModernCheckBox(ctx->hSequentialChk, ctx->dpi, palette, palette.surface_primary);

            ctx->hFirstLastChk = CreateWindowExW(0, L"BUTTON", L"Download first and last pieces first",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(476, ctx->dpi), left_ctrl_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorFirstLastChk)), hInst, nullptr);
            SendMessageW(ctx->hFirstLastChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            InstallModernCheckBox(ctx->hFirstLastChk, ctx->dpi, palette, palette.surface_primary);

            // 7. Content Layout (label at 504, combo at 522)
            ctx->hLayoutCombo = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_TABSTOP,
                left_x, DesignSystem::ScaleForDpi(522, ctx->dpi), left_ctrl_w, combo_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorLayoutCombo)), hInst, nullptr);
            SendMessageW(ctx->hLayoutCombo, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hLayoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Original"));
            SendMessageW(ctx->hLayoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Create subfolder"));
            SendMessageW(ctx->hLayoutCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Don't create subfolder"));
            SendMessageW(ctx->hLayoutCombo, CB_SETCURSEL, 0, 0);
            InstallModernComboBox(ctx->hLayoutCombo, ctx->dpi, palette, palette.surface_primary);

            // Bottom left (inside 56px footer bar)
            ColorRgba footer_bg = (palette.mode == ThemeMode::kLight)
                ? palette.surface_primary : ColorRgba{0x14, 0x14, 0x17, 255};
            int bot_y = h - DesignSystem::ScaleForDpi(45, ctx->dpi);
            ctx->hNeverShowChk = CreateWindowExW(0, L"BUTTON", L"Never show again",
                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | WS_TABSTOP,
                margin, bot_y + DesignSystem::ScaleForDpi(6, ctx->dpi), DesignSystem::ScaleForDpi(140, ctx->dpi), DesignSystem::ScaleForDpi(22, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorNeverShowChk)), hInst, nullptr);
            SendMessageW(ctx->hNeverShowChk, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontSmall), TRUE);
            InstallModernCheckBox(ctx->hNeverShowChk, ctx->dpi, palette, footer_bg);

            ctx->hSaveTorrentBtn = CreateWindowExW(0, L"BUTTON", L"Save .torrent...",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                margin + DesignSystem::ScaleForDpi(150, ctx->dpi), bot_y, DesignSystem::ScaleForDpi(130, ctx->dpi), DesignSystem::ScaleForDpi(34, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSaveTorrentBtn)), hInst, nullptr);

            // --- RIGHT PANE CONTROLS ---
            int right_x = margin + left_w + DesignSystem::ScaleForDpi(12, ctx->dpi);
            int right_w = w - right_x - margin;

            // Toolbar
            int r_top = DesignSystem::ScaleForDpi(14, ctx->dpi);
            ctx->hSelectAllBtn = CreateWindowExW(0, L"BUTTON", L"Select All",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                right_x, r_top, DesignSystem::ScaleForDpi(90, ctx->dpi), DesignSystem::ScaleForDpi(28, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSelectAllBtn)), hInst, nullptr);

            ctx->hSelectNoneBtn = CreateWindowExW(0, L"BUTTON", L"Select None",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                right_x + DesignSystem::ScaleForDpi(98, ctx->dpi), r_top, DesignSystem::ScaleForDpi(90, ctx->dpi), DesignSystem::ScaleForDpi(28, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorSelectNoneBtn)), hInst, nullptr);

            int filter_x = right_x + DesignSystem::ScaleForDpi(200, ctx->dpi);
            int filter_w = right_w - DesignSystem::ScaleForDpi(200, ctx->dpi);
            ctx->hFilterEdit = CreateWindowExW(0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                filter_x, r_top, filter_w, DesignSystem::ScaleForDpi(28, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorFilterEdit)), hInst, nullptr);
            SendMessageW(ctx->hFilterEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hFilterEdit, 0x1501 /* EM_SETCUEBANNER */, TRUE, reinterpret_cast<LPARAM>(L"Filter files in torrent..."));
            InstallModernEditControl(ctx->hFilterEdit, ctx->dpi, ctx->view->GetPalette());

            // File list
            int list_top = r_top + DesignSystem::ScaleForDpi(62, ctx->dpi);
            int list_h = h - list_top - DesignSystem::ScaleForDpi(80, ctx->dpi);
            ctx->hFileList = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | LBS_OWNERDRAWFIXED | LBS_NODATA | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                right_x, list_top, right_w, list_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorFileList)), hInst, nullptr);
            SendMessageW(ctx->hFileList, LB_SETITEMHEIGHT, 0, DesignSystem::ScaleForDpi(28, ctx->dpi));

            // Summary
            int sum_y = list_top + list_h + DesignSystem::ScaleForDpi(8, ctx->dpi);
            ctx->hSummaryStatic = CreateWindowExW(0, L"STATIC", ctx->GetSummaryText().c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                right_x, sum_y, right_w, DesignSystem::ScaleForDpi(20, ctx->dpi),
                hwnd, nullptr, hInst, nullptr);
            SendMessageW(ctx->hSummaryStatic, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

            // Bottom action buttons
            int btn_h = DesignSystem::ScaleForDpi(34, ctx->dpi);
            int ok_w = DesignSystem::ScaleForDpi(130, ctx->dpi);
            int can_w = DesignSystem::ScaleForDpi(100, ctx->dpi);
            int can_x = right_x + right_w - can_w;
            int ok_x = can_x - ok_w - DesignSystem::ScaleForDpi(10, ctx->dpi);
            int action_y = h - DesignSystem::ScaleForDpi(46, ctx->dpi);

            ctx->hOkBtn = CreateWindowExW(0, L"BUTTON", L"Add Torrent",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                ok_x, action_y, ok_w, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorOkBtn)), hInst, nullptr);

            ctx->hCancelBtn = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | BS_OWNERDRAW | WS_TABSTOP,
                can_x, action_y, can_w, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddTorCancelBtn)), hInst, nullptr);

            ctx->BuildTree();
            return 0;
        }

        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            HWND hCtl = reinterpret_cast<HWND>(lParam);
            if (hCtl == ctx->hModeCombo || hCtl == ctx->hCategoryCombo ||
                hCtl == ctx->hStopCondCombo || hCtl == ctx->hLayoutCombo) {
                const auto& palette = ctx->view->GetPalette();
                SetBkMode(hdc, OPAQUE);
                SetBkColor(hdc, palette.surface_elevated.ToWin32ColorRef());
                SetTextColor(hdc, palette.text_primary.ToWin32ColorRef());
                return reinterpret_cast<INT_PTR>(ctx->hEditBrush);
            }
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ctx->view->GetPalette().text_secondary.ToWin32ColorRef());
            return reinterpret_cast<INT_PTR>(ctx->hBgBrush);
        }

        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            const auto& palette = ctx->view->GetPalette();
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, palette.surface_elevated.ToWin32ColorRef());
            SetTextColor(hdc, palette.text_primary.ToWin32ColorRef());
            return reinterpret_cast<INT_PTR>(ctx->hEditBrush);
        }

        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            const auto& palette = ctx->view->GetPalette();
            SetBkMode(hdc, OPAQUE);
            SetBkColor(hdc, palette.surface_primary.ToWin32ColorRef());
            SetTextColor(hdc, palette.text_primary.ToWin32ColorRef());
            return reinterpret_cast<INT_PTR>(ctx->hPanelBrush);
        }

        case WM_SIZE: {
            if (!ctx || !ctx->hFileList) break;
            RECT rc{};
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            uint32_t dpi = ctx->dpi ? ctx->dpi : 96;

            int left_w = DesignSystem::ScaleForDpi(330, dpi);
            int margin = DesignSystem::ScaleForDpi(14, dpi);
            int right_x = margin + left_w + margin;
            int right_w = w - right_x - margin;
            if (right_w < DesignSystem::ScaleForDpi(200, dpi)) right_w = DesignSystem::ScaleForDpi(200, dpi);

            int r_top = DesignSystem::ScaleForDpi(18, dpi);
            int filter_x = right_x + DesignSystem::ScaleForDpi(200, dpi);
            int filter_w = std::max(DesignSystem::ScaleForDpi(100, dpi), right_w - DesignSystem::ScaleForDpi(200, dpi));
            SetWindowPos(ctx->hSelectAllBtn, nullptr, right_x, r_top, DesignSystem::ScaleForDpi(90, dpi), DesignSystem::ScaleForDpi(28, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hSelectNoneBtn, nullptr, right_x + DesignSystem::ScaleForDpi(98, dpi), r_top, DesignSystem::ScaleForDpi(90, dpi), DesignSystem::ScaleForDpi(28, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hFilterEdit, nullptr, filter_x, r_top, filter_w, DesignSystem::ScaleForDpi(28, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            int list_top = r_top + DesignSystem::ScaleForDpi(62, dpi);
            int list_h = std::max(DesignSystem::ScaleForDpi(100, dpi), h - list_top - DesignSystem::ScaleForDpi(80, dpi));
            SetWindowPos(ctx->hFileList, nullptr, right_x, list_top, right_w, list_h, SWP_NOZORDER | SWP_NOACTIVATE);

            int sum_y = list_top + list_h + DesignSystem::ScaleForDpi(8, dpi);
            SetWindowPos(ctx->hSummaryStatic, nullptr, right_x, sum_y, right_w, DesignSystem::ScaleForDpi(20, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            int bot_y = h - DesignSystem::ScaleForDpi(45, dpi);
            SetWindowPos(ctx->hNeverShowChk, nullptr, margin, bot_y + DesignSystem::ScaleForDpi(6, dpi), DesignSystem::ScaleForDpi(140, dpi), DesignSystem::ScaleForDpi(22, dpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hSaveTorrentBtn, nullptr, margin + DesignSystem::ScaleForDpi(150, dpi), bot_y, DesignSystem::ScaleForDpi(130, dpi), DesignSystem::ScaleForDpi(34, dpi), SWP_NOZORDER | SWP_NOACTIVATE);

            int btn_h = DesignSystem::ScaleForDpi(34, dpi);
            int ok_w = DesignSystem::ScaleForDpi(130, dpi);
            int can_w = DesignSystem::ScaleForDpi(100, dpi);
            int can_x = right_x + right_w - can_w;
            int ok_x = can_x - ok_w - DesignSystem::ScaleForDpi(10, dpi);
            int action_y = h - DesignSystem::ScaleForDpi(45, dpi);
            SetWindowPos(ctx->hCancelBtn, nullptr, can_x, action_y, can_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);
            SetWindowPos(ctx->hOkBtn, nullptr, ok_x, action_y, ok_w, btn_h, SWP_NOZORDER | SWP_NOACTIVATE);

            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }

        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);
            if (mi->CtlID == kAddTorFileList) {
                mi->itemHeight = DesignSystem::ScaleForDpi(28, ctx->dpi);
                return TRUE;
            }
            return FALSE;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rcClient{};
            GetClientRect(hwnd, &rcClient);
            FillRect(hdc, &rcClient, ctx->hBgBrush);

            const auto& pal = ctx->view ? ctx->view->GetPalette() : DesignSystem::GetRedesignPalette();
            int left_w = DesignSystem::ScaleForDpi(330, ctx->dpi);
            int margin = DesignSystem::ScaleForDpi(14, ctx->dpi);
            int footer_h = DesignSystem::ScaleForDpi(56, ctx->dpi);
            int footer_top = rcClient.bottom - footer_h;

            // Left pane card (clean rounded surface above the footer bar)
            RECT left_card{margin, margin, margin + left_w, std::max(margin + 20, footer_top - margin)};
            DrawRoundedBoxGdi(hdc, left_card, DesignSystem::ScaleForDpi(10, ctx->dpi),
                              pal.surface_primary, pal.border_subtle, 1);

            // Group labels on left pane (exact match to WM_CREATE control positions)
            SelectObject(hdc, ctx->hFontBold);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());

            int lbl_x = margin + DesignSystem::ScaleForDpi(12, ctx->dpi);
            TextOutW(hdc, lbl_x, DesignSystem::ScaleForDpi(24, ctx->dpi), L"Management Mode", 15);
            TextOutW(hdc, lbl_x, DesignSystem::ScaleForDpi(78, ctx->dpi), L"Save Path", 9);
            TextOutW(hdc, lbl_x, DesignSystem::ScaleForDpi(180, ctx->dpi), L"Category", 8);
            TextOutW(hdc, lbl_x, DesignSystem::ScaleForDpi(260, ctx->dpi), L"Tags", 4);
            TextOutW(hdc, lbl_x, DesignSystem::ScaleForDpi(314, ctx->dpi), L"Transfer & Queue", 16);
            TextOutW(hdc, lbl_x, DesignSystem::ScaleForDpi(360, ctx->dpi), L"Stop Condition", 14);
            TextOutW(hdc, lbl_x, DesignSystem::ScaleForDpi(504, ctx->dpi), L"Content Layout", 14);

            // Header for file list on right pane
            int right_x = margin + left_w + DesignSystem::ScaleForDpi(12, ctx->dpi);
            int right_w = rcClient.right - right_x - margin;
            int hdr_y = DesignSystem::ScaleForDpi(50, ctx->dpi);
            int hdr_h = DesignSystem::ScaleForDpi(24, ctx->dpi);
            RECT hdr_rc{right_x, hdr_y, right_x + right_w, hdr_y + hdr_h};
            DrawRoundedBoxGdi(hdc, hdr_rc, DesignSystem::ScaleForDpi(4, ctx->dpi),
                              pal.surface_elevated, pal.border_subtle, 1);

            // Dedicated full-width bottom footer bar with 1px top hairline
            if (footer_top < rcClient.bottom) {
                ColorRgba footer_bg = (pal.mode == ThemeMode::kLight)
                    ? pal.surface_primary : ColorRgba{0x14, 0x14, 0x17, 255};
                ColorRgba hairline = (pal.mode == ThemeMode::kLight)
                    ? pal.border_subtle : ColorRgba{0xFF, 0xFF, 0xFF, 18};
                RECT footer_rc{0, footer_top, rcClient.right, rcClient.bottom};
                FillRectSolid(hdc, footer_rc, footer_bg);
                RECT footer_line{0, footer_top, rcClient.right, footer_top + 1};
                FillRectSolid(hdc, footer_line, hairline);
            }

            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            TextOutW(hdc, right_x + DesignSystem::ScaleForDpi(38, ctx->dpi), hdr_y + DesignSystem::ScaleForDpi(4, ctx->dpi), L"Name", 4);

            int size_col_w = DesignSystem::ScaleForDpi(110, ctx->dpi);
            int prio_col_w = DesignSystem::ScaleForDpi(90, ctx->dpi);
            int size_x = right_x + right_w - size_col_w - prio_col_w;
            int prio_x = right_x + right_w - prio_col_w;

            TextOutW(hdc, size_x, hdr_y + DesignSystem::ScaleForDpi(4, ctx->dpi), L"Size", 4);
            TextOutW(hdc, prio_x, hdr_y + DesignSystem::ScaleForDpi(4, ctx->dpi), L"Priority", 8);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) return FALSE;

            if (dis->CtlID == kAddTorFileList) {
                if (dis->itemID == static_cast<UINT>(-1) || dis->itemID >= ctx->visible_node_indices.size()) return FALSE;
                int node_id = ctx->visible_node_indices[dis->itemID];
                const auto& node = ctx->all_nodes[node_id];

                HDC hdcItem = dis->hDC;
                RECT rc = dis->rcItem;

                const ThemePalette pal = ctx->view ? ctx->view->GetPalette() : DesignSystem::GetRedesignPalette();
                bool is_selected = (dis->itemState & ODS_SELECTED) != 0;
                ColorRgba row_bg = is_selected ? pal.surface_selected : ((dis->itemID % 2 == 0) ? pal.surface_primary : pal.app_background);
                HBRUSH brBg = CreateSolidBrush(row_bg.ToWin32ColorRef());
                FillRect(hdcItem, &rc, brBg);
                DeleteObject(brBg);
                if (is_selected) {
                    RECT accent_bar{rc.left, rc.top, rc.left + DesignSystem::ScaleForDpi(3, ctx->dpi), rc.bottom};
                    FillRectSolid(hdcItem, accent_bar, pal.accent_primary);
                }

                int indent = node.depth * DesignSystem::ScaleForDpi(18, ctx->dpi) + DesignSystem::ScaleForDpi(8, ctx->dpi);

                // Caret expand/collapse glyph if folder
                if (node.is_dir) {
                    RECT caret_rc{rc.left + indent - DesignSystem::ScaleForDpi(14, ctx->dpi), rc.top + (rc.bottom - rc.top) / 2 - 4,
                                  rc.left + indent - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.top + (rc.bottom - rc.top) / 2 + 4};
                    int cx = (caret_rc.left + caret_rc.right) / 2;
                    int cy = (caret_rc.top + caret_rc.bottom) / 2;
                    DrawCaretGlyphGdi(hdcItem, cx, cy, pal.text_muted, !node.expanded);
                }

                // Checkbox
                RECT chk_rc{rc.left + indent, rc.top + (rc.bottom - rc.top) / 2 - DesignSystem::ScaleForDpi(7, ctx->dpi),
                            rc.left + indent + DesignSystem::ScaleForDpi(14, ctx->dpi), rc.top + (rc.bottom - rc.top) / 2 + DesignSystem::ScaleForDpi(7, ctx->dpi)};

                if (node.check_state == 1) {
                    DrawCheckboxGdi(hdcItem, chk_rc, true, pal);
                } else if (node.check_state == 2) { // Partial
                    DrawRoundedBoxGdi(hdcItem, chk_rc, 4, ColorRgba{pal.accent_primary.r, pal.accent_primary.g, pal.accent_primary.b, 200}, pal.accent_primary, 1);
                    RECT bar{chk_rc.left + 3, chk_rc.top + (chk_rc.bottom - chk_rc.top) / 2 - 1, chk_rc.right - 3, chk_rc.top + (chk_rc.bottom - chk_rc.top) / 2 + 1};
                    FillRectSolid(hdcItem, bar, ColorRgba{255, 255, 255, 255});
                } else {
                    DrawCheckboxGdi(hdcItem, chk_rc, false, pal);
                }

                // Icon
                int icon_x = rc.left + indent + DesignSystem::ScaleForDpi(20, ctx->dpi);
                int icon_y = rc.top + (rc.bottom - rc.top) / 2 - DesignSystem::ScaleForDpi(8, ctx->dpi);
                int icon_sz = DesignSystem::ScaleForDpi(16, ctx->dpi);

                if (node.is_dir) {
                    // Golden yellow folder icon
                    RECT f_top{icon_x, icon_y + 1, icon_x + icon_sz / 2, icon_y + 4};
                    FillRectSolid(hdcItem, f_top, ColorRgba{0xF5, 0xB0, 0x41, 255});
                    RECT f_body{icon_x, icon_y + 3, icon_x + icon_sz, icon_y + icon_sz};
                    DrawRoundedBoxGdi(hdcItem, f_body, 3, ColorRgba{0xFA, 0xCA, 0x64, 255}, ColorRgba{0xE6, 0x9B, 0x20, 255}, 1);
                } else {
                    // Colorful file type icon
                    FormattedTaskRow dummy_row;
                    dummy_row.display_filename = WToUtf8(node.name);
                    DrawFileTypeIconGdi(hdcItem, icon_x, icon_y, icon_sz, icon_sz, dummy_row, ctx->view ? ctx->view->GetPalette() : DesignSystem::GetRedesignPalette());
                }

                // File name
                int name_x = icon_x + icon_sz + DesignSystem::ScaleForDpi(8, ctx->dpi);
                int size_col_w = DesignSystem::ScaleForDpi(110, ctx->dpi);
                int prio_col_w = DesignSystem::ScaleForDpi(90, ctx->dpi);
                int size_x = rc.right - size_col_w - prio_col_w;
                int prio_x = rc.right - prio_col_w;

                RECT name_rc{name_x, rc.top, size_x - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.bottom};
                SelectObject(hdcItem, node.is_dir ? ctx->hFontBold : ctx->hFontBody);
                SetBkMode(hdcItem, TRANSPARENT);
                SetTextColor(hdcItem, is_selected ? pal.text_primary.ToWin32ColorRef() : (node.is_dir ? pal.text_primary.ToWin32ColorRef() : pal.text_secondary.ToWin32ColorRef()));
                DrawTextW(hdcItem, node.name.c_str(), -1, &name_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

                // Size
                RECT sz_rc{size_x, rc.top, prio_x - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.bottom};
                SelectObject(hdcItem, ctx->hFontSmall);
                SetTextColor(hdcItem, pal.text_secondary.ToWin32ColorRef());
                std::wstring sz_txt = Utf8ToW(FormatAddTorrentBytes(node.size));
                DrawTextW(hdcItem, sz_txt.c_str(), -1, &sz_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                // Priority
                RECT prio_rc{prio_x, rc.top, rc.right - DesignSystem::ScaleForDpi(6, ctx->dpi), rc.bottom};
                DrawTextW(hdcItem, node.is_dir ? L"" : L"Normal", -1, &prio_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

                return TRUE;
            }

            // Buttons
            HDC hdcBtn = dis->hDC;
            RECT rcBtn = dis->rcItem;
            bool is_selected = (dis->itemState & ODS_SELECTED) != 0;
            bool is_focused  = (dis->itemState & ODS_FOCUS) != 0;
            FillRect(hdcBtn, &rcBtn, ctx->hBgBrush);

            if (dis->CtlID == kAddTorOkBtn) { // Primary Coral Red
                const auto& accent = ctx->view->GetPalette().accent_primary;
                ColorRgba bg_col = is_selected ? accent.Interpolate(ColorRgba{0, 0, 0, 255}, 0.15f) : (is_focused ? accent.Interpolate(ColorRgba{255, 255, 255, 255}, 0.12f) : accent);
                DrawRoundedBoxGdi(hdcBtn, rcBtn, DesignSystem::ScaleForDpi(8, ctx->dpi), bg_col, bg_col, 0);
                SelectObject(hdcBtn, ctx->hFontBold);
                SetTextColor(hdcBtn, RGB(255, 255, 255));
                SetBkMode(hdcBtn, TRANSPARENT);
                DrawTextW(hdcBtn, L"Add Torrent", -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            } else if (dis->CtlID == kAddTorCancelBtn || dis->CtlID == kAddTorBrowseBtn ||
                       dis->CtlID == kAddTorSelectAllBtn || dis->CtlID == kAddTorSelectNoneBtn || dis->CtlID == kAddTorSaveTorrentBtn) {
                const auto& pal = ctx->view->GetPalette();
                ColorRgba bg_col = is_selected ? pal.surface_pressed : pal.surface_primary;
                ColorRgba border_col = is_focused ? pal.border_focus : pal.border_subtle;
                DrawRoundedBoxGdi(hdcBtn, rcBtn, DesignSystem::ScaleForDpi(6, ctx->dpi), bg_col, border_col, 1);
                SelectObject(hdcBtn, ctx->hFontBody);
                SetTextColor(hdcBtn, pal.text_primary.ToWin32ColorRef());
                SetBkMode(hdcBtn, TRANSPARENT);

                const wchar_t* label = L"";
                if (dis->CtlID == kAddTorCancelBtn) label = L"Cancel";
                else if (dis->CtlID == kAddTorBrowseBtn) label = L"...";
                else if (dis->CtlID == kAddTorSelectAllBtn) label = L"Select All";
                else if (dis->CtlID == kAddTorSelectNoneBtn) label = L"Select None";
                else if (dis->CtlID == kAddTorSaveTorrentBtn) label = L"Save .torrent...";

                DrawTextW(hdcBtn, label, -1, &rcBtn, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            return FALSE;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);

            if (id == kAddTorBrowseBtn && code == BN_CLICKED) {
                std::string chosen = ctx->target_save_dir;
                if (SelectFolderWithDialogWin32(hwnd, chosen)) {
                    ctx->target_save_dir = chosen;
                    SetWindowTextW(ctx->hSaveDirEdit, Utf8ToW(chosen).c_str());
                }
                return 0;
            } else if (id == kAddTorSaveTorrentBtn && code == BN_CLICKED) {
                std::filesystem::path source(ctx->torrent_file_path);
                if (source.empty() || !std::filesystem::is_regular_file(source)) {
                    MessageBoxW(hwnd, L"The source torrent file is no longer available.", L"Save Torrent", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                std::wstring source_name = source.filename().wstring();
                std::array<wchar_t, MAX_PATH> destination{};
                const size_t copy_len = std::min(source_name.size(), destination.size() - 1);
                std::copy_n(source_name.data(), copy_len, destination.data());
                OPENFILENAMEW dialog{};
                dialog.lStructSize = sizeof(dialog);
                dialog.hwndOwner = hwnd;
                dialog.lpstrFile = destination.data();
                dialog.nMaxFile = static_cast<DWORD>(destination.size());
                dialog.lpstrFilter = L"BitTorrent files (*.torrent)\0*.torrent\0All files (*.*)\0*.*\0\0";
                dialog.nFilterIndex = 1;
                dialog.lpstrDefExt = L"torrent";
                dialog.Flags = OFN_EXPLORER | OFN_PATHMUSTEXIST | OFN_OVERWRITEPROMPT;
                if (GetSaveFileNameW(&dialog) && !CopyFileW(source.c_str(), destination.data(), FALSE)) {
                    DWORD error = GetLastError();
                    std::wstring message = L"Could not save the torrent file (Windows error " + std::to_wstring(error) + L").";
                    MessageBoxW(hwnd, message.c_str(), L"Save Torrent", MB_OK | MB_ICONERROR);
                }
                return 0;
            } else if (id == kAddTorSelectAllBtn && code == BN_CLICKED) {
                if (!ctx->all_nodes.empty()) {
                    ctx->SetNodeChecked(0, true);
                    ctx->UpdateCheckStatesFromLeaves();
                    ctx->SyncToFileInfo();
                    ctx->RebuildVisibleList();
                    SetWindowTextW(ctx->hSummaryStatic, ctx->GetSummaryText().c_str());
                }
                return 0;
            } else if (id == kAddTorSelectNoneBtn && code == BN_CLICKED) {
                if (!ctx->all_nodes.empty()) {
                    ctx->SetNodeChecked(0, false);
                    ctx->UpdateCheckStatesFromLeaves();
                    ctx->SyncToFileInfo();
                    ctx->RebuildVisibleList();
                    SetWindowTextW(ctx->hSummaryStatic, ctx->GetSummaryText().c_str());
                }
                return 0;
            } else if (id == kAddTorFilterEdit && code == EN_CHANGE) {
                wchar_t buf[256]{};
                GetWindowTextW(ctx->hFilterEdit, buf, 255);
                ctx->filter_text = WToUtf8(buf);
                ctx->RebuildVisibleList();
                return 0;
            } else if (id == kAddTorFileList && code == LBN_SELCHANGE) {
                LRESULT cur = SendMessageW(ctx->hFileList, LB_GETCURSEL, 0, 0);
                if (cur >= 0 && static_cast<size_t>(cur) < ctx->visible_node_indices.size()) {
                    int node_id = ctx->visible_node_indices[static_cast<size_t>(cur)];
                    ctx->ToggleNode(node_id);
                    SetWindowTextW(ctx->hSummaryStatic, ctx->GetSummaryText().c_str());
                }
                return 0;
            } else if (id == kAddTorFileList && code == LBN_DBLCLK) {
                LRESULT cur = SendMessageW(ctx->hFileList, LB_GETCURSEL, 0, 0);
                if (cur >= 0 && static_cast<size_t>(cur) < ctx->visible_node_indices.size()) {
                    int node_id = ctx->visible_node_indices[static_cast<size_t>(cur)];
                    if (ctx->all_nodes[node_id].is_dir) {
                        ctx->all_nodes[node_id].expanded = !ctx->all_nodes[node_id].expanded;
                        ctx->RebuildVisibleList();
                    }
                }
                return 0;
            } else if ((id == kAddTorOkBtn || id == IDOK) && (code == BN_CLICKED || code == 0 || code == 1)) {
                // Ensure at least one file is selected
                bool any_selected = false;
                if (ctx->info) {
                    for (const auto& f : ctx->info->files) {
                        if (f.selected) { any_selected = true; break; }
                    }
                }
                if (!any_selected) {
                    MessageBoxW(hwnd, L"Select at least one file to continue download.", L"No Files Selected", MB_OK | MB_ICONWARNING);
                    return 0;
                }

                // Read save dir
                wchar_t dir_buf[MAX_PATH]{};
                GetWindowTextW(ctx->hSaveDirEdit, dir_buf, MAX_PATH - 1);
                std::string sd = WToUtf8(dir_buf);
                if (!sd.empty()) ctx->target_save_dir = sd;

                ctx->start_immediately = (SendMessageW(ctx->hStartChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->add_to_queue_top = (SendMessageW(ctx->hQueueTopChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->skip_hash_check = (SendMessageW(ctx->hSkipHashChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->sequential_download = (SendMessageW(ctx->hSequentialChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->first_last_piece_priority = (SendMessageW(ctx->hFirstLastChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->remember_save_path = (SendMessageW(ctx->hRememberPathChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->use_incomplete_path = (SendMessageW(ctx->hIncompleteChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                ctx->never_show_again = (SendMessageW(ctx->hNeverShowChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                wchar_t category_buf[64]{};
                const LRESULT category_index = SendMessageW(ctx->hCategoryCombo, CB_GETCURSEL, 0, 0);
                if (category_index >= 0) {
                    SendMessageW(ctx->hCategoryCombo, CB_GETLBTEXT, category_index, reinterpret_cast<LPARAM>(category_buf));
                    ctx->selected_category = WToUtf8(category_buf);
                    if (ctx->selected_category == "(None)") ctx->selected_category = "General";
                }

                ctx->accepted = true;
                DestroyWindow(hwnd);
                return 0;
            } else if ((id == kAddTorCancelBtn || id == IDCANCEL) && (code == BN_CLICKED || code == 0 || code == 1)) {
                ctx->accepted = false;
                DestroyWindow(hwnd);
                return 0;
            }
            return 0;
        }

        case WM_CLOSE:
            ctx->accepted = false;
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
            if (ctx->hFontSmall) DeleteObject(ctx->hFontSmall);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            if (ctx->hPanelBrush) DeleteObject(ctx->hPanelBrush);
            if (ctx->hEditBrush) DeleteObject(ctx->hEditBrush);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowAddTorrentDialogWin32(
    HWND hwnd_parent,
    DesktopView& view,
    DownloadController& controller,
    torrent::TorrentInfo& info,
    const std::string& torrent_file_path,
    const std::string& initial_save_dir,
    bool start_immediately,
    std::string* selected_save_dir,
    bool* selected_start_immediately,
    std::string* selected_category) {

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool class_registered = false;
    if (!class_registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = AddTorrentDialogProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        HICON hAppIcon = CreateRadiumAppIcon();
        wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = hAppIcon;
        wc.lpszClassName = L"Radium_AddTorrentDlgClass";
        RegisterClassExW(&wc);
        class_registered = true;
    }

    uint32_t dpi = view.GetMetrics().dpi;
    if (dpi == 0) dpi = 96;

    std::string s_dir = initial_save_dir;
    if (s_dir.empty()) {
        auto settings = controller.GetSettings();
        s_dir = settings.last_download_dir.empty() ? settings.default_download_dir : settings.last_download_dir;
    }

    AddTorrentDialogContext ctx;
    ctx.owner = hwnd_parent;
    ctx.view = &view;
    ctx.controller = &controller;
    ctx.info = &info;
    ctx.torrent_file_path = torrent_file_path;
    ctx.target_save_dir = s_dir;
    ctx.start_immediately = start_immediately;
    ctx.dpi = dpi;

    int dlg_w = DesignSystem::ScaleForDpi(1040, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(660, dpi);

    RECT work_rc{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_rc, 0);
    int screen_w = work_rc.right - work_rc.left;
    int screen_h = work_rc.bottom - work_rc.top;
    if (screen_w <= 0) screen_w = GetSystemMetrics(SM_CXSCREEN);
    if (screen_h <= 0) screen_h = GetSystemMetrics(SM_CYSCREEN);

    int dlg_x = work_rc.left + (screen_w - dlg_w) / 2;
    int dlg_y = work_rc.top + (screen_h - dlg_h) / 2;

    HWND hDlg = CreateWindowExW(
        (hwnd_parent ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT,
        L"Radium_AddTorrentDlgClass",
        Utf8ToW("Add Torrent \u2014 " + (info.name.empty() ? "Radium" : info.name)).c_str(),
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        hwnd_parent, nullptr, hInst, &ctx);

    if (!hDlg) return false;
    ctx.hDlg = hDlg;

    ApplyWindowGlassBackdropWin32(hDlg, view.GetPalette(), true);

    if (hwnd_parent && IsWindow(hwnd_parent)) {
        EnableWindow(hwnd_parent, FALSE);
    }

    ShowWindow(hDlg, SW_SHOWNORMAL);
    BringWindowToTop(hDlg);
    SetForegroundWindow(hDlg);
    SetActiveWindow(hDlg);
    SetFocus(ctx.hFilterEdit);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(kAddTorCancelBtn, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hCancelBtn));
                break;
            } else if (msg.wParam == VK_RETURN && GetFocus() != ctx.hBrowseBtn && GetFocus() != ctx.hCancelBtn) {
                SendMessageW(hDlg, WM_COMMAND, MAKEWPARAM(kAddTorOkBtn, BN_CLICKED), reinterpret_cast<LPARAM>(ctx.hOkBtn));
                if (!IsWindow(hDlg)) break;
                continue;
            }
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (IsWindow(hDlg)) {
        DestroyWindow(hDlg);
    }

    if (hwnd_parent && IsWindow(hwnd_parent)) {
        EnableWindow(hwnd_parent, TRUE);
        SetForegroundWindow(hwnd_parent);
        InvalidateRect(hwnd_parent, nullptr, TRUE);
    }

    if (ctx.accepted) {
        if (selected_save_dir) *selected_save_dir = ctx.target_save_dir;
        if (selected_start_immediately) *selected_start_immediately = ctx.start_immediately;
        if (selected_category) *selected_category = ctx.selected_category;
    }

    return ctx.accepted;
}


void PaintDetailsPanelWin32(HDC hdc, DesktopView& view, const RectI& rect, HFONT hFontBody, HFONT hFontBold, HFONT hFontSmall) {
    (void)hFontBody;
    if (rect.height() <= 0) return;
    const auto& pal = view.GetPalette();
    const auto& m = view.GetMetrics();
    auto details = view.BuildSelectedTaskDetails();
    if (!details.has_selection) return;

    RECT det_rc = ToWinRect(rect);

    // Docked bottom inspector according to Design Vision Part 3.G / Part 4
    FillRectSolid(hdc, det_rc, pal.surface_primary);
    RECT top_div{det_rc.left, det_rc.top, det_rc.right, det_rc.top + 1};
    FillRectSolid(hdc, top_div, pal.border_subtle);

    RECT card_rc = det_rc;

    auto geometry = view.ComputeLayoutGeometry(view.GetScrollOffset());
    for (size_t i = 0; i < geometry.details_action_buttons.size(); ++i) {
        const auto& action = geometry.details_action_buttons[i];
        RECT button_rc = ToWinRect(action.rect);
        bool hovered = view.GetHoveredDetailsBtnIndex() == static_cast<int>(i);
        const bool is_primary = (action.action_id == DetailsActionId::kResume);
        const bool is_open_file = (action.action_id == DetailsActionId::kOpenFile);
        const bool is_remove = (action.action_id == DetailsActionId::kRemoveHistory);
        const bool is_light = (pal.mode == ThemeMode::kLight);

        ColorRgba bg = pal.surface_elevated;
        ColorRgba border = pal.border_subtle;
        ColorRgba txt_col = pal.text_secondary;
        if (is_primary) {
            bg = pal.accent_primary;
            border = pal.accent_primary;
            txt_col = ColorRgba{255, 255, 255, 255};
        } else if (is_open_file) {
            bg = is_light ? ColorRgba{0xFF, 0xFF, 0xFF, 255} : ColorRgba{0x32, 0x32, 0x38, 255};
            border = is_light ? ColorRgba{0x94, 0xA3, 0xB8, 255} : ColorRgba{0x4B, 0x4B, 0x56, 255};
            txt_col = is_light ? ColorRgba{0x0F, 0x17, 0x2A, 255} : ColorRgba{0xF5, 0xF5, 0xF7, 255};
            if (hovered) {
                bg = is_light ? ColorRgba{0xF8, 0xFA, 0xFC, 255} : ColorRgba{0x3C, 0x3C, 0x44, 255};
            }
        } else {
            bg = is_light ? ColorRgba{0xEA, 0xEC, 0xEF, 255} : ColorRgba{0x24, 0x24, 0x28, 255};
            border = is_light ? ColorRgba{0xD1, 0xD5, 0xDB, 255} : ColorRgba{0x34, 0x34, 0x38, 255};
            txt_col = is_light ? ColorRgba{0x47, 0x55, 0x69, 255} : ColorRgba{0xA1, 0xA1, 0xAA, 255};
            if (hovered) {
                bg = is_remove ? (is_light ? ColorRgba{0xDC, 0x26, 0x26, 255} : ColorRgba{0xFF, 0x47, 0x57, 255}) : pal.surface_hover;
                if (is_remove) border = bg;
                txt_col = is_remove ? ColorRgba{255, 255, 255, 255} : pal.text_primary;
            }
        }
        DrawRoundedBoxGdi(hdc, button_rc, DesignSystem::ScaleForDpi(6, m.dpi), bg, border, 1);
        const bool is_compact_icon = (button_rc.right - button_rc.left <= DesignSystem::ScaleForDpi(44, m.dpi));
        if (is_remove) {
            DrawTrashGlyphGdi(hdc, button_rc, txt_col);
        } else if (is_compact_icon && action.action_id == DetailsActionId::kOpenFolder) {
            DrawFolderGlyphGdi(hdc, button_rc, txt_col);
        } else if (is_compact_icon && action.action_id == DetailsActionId::kCancel) {
            DrawCloseGlyphGdi(hdc, button_rc, txt_col);
        } else {
            SelectObject(hdc, is_open_file ? hFontBold : hFontSmall);
            SetTextColor(hdc, txt_col.ToWin32ColorRef());
            std::wstring label = Utf8ToW(action.label);
            int text_inset = DesignSystem::ScaleForDpi(8, m.dpi);
            RECT label_rc{button_rc.left + text_inset, button_rc.top, button_rc.right - text_inset, button_rc.bottom};
            DrawTextW(hdc, label.c_str(), -1, &label_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }

    int line_h = DesignSystem::ScaleForDpi(24, m.dpi);
    int top_y = card_rc.top + (card_rc.bottom - card_rc.top - line_h * 3) / 2;
    int text_stack_center_y = top_y + (line_h * 3) / 2;

    // Left Icon Box (42x42px, vertically centered against the 3-line text stack)
    int icon_box_w = DesignSystem::ScaleForDpi(42, m.dpi);
    int icon_box_h = DesignSystem::ScaleForDpi(42, m.dpi);
    int icon_box_x = card_rc.left + DesignSystem::ScaleForDpi(14, m.dpi);
    int icon_box_y = text_stack_center_y - icon_box_h / 2;
    RECT icon_box_rc{icon_box_x, icon_box_y, icon_box_x + icon_box_w, icon_box_y + icon_box_h};
    DrawRoundedBoxGdi(hdc, icon_box_rc, DesignSystem::ScaleForDpi(8, m.dpi), pal.surface_elevated, pal.border_subtle, 1);

    // Media / File Icon inside icon box
    FormattedTaskRow dummy_row{};
    dummy_row.display_filename = details.file_name;
    dummy_row.full_filename_tooltip = details.file_name;
    dummy_row.category = details.category;
    dummy_row.is_stream = (details.stream_format.find("HLS") != std::string::npos || details.stream_format.find("DASH") != std::string::npos);
    int icon_w = DesignSystem::ScaleForDpi(24, m.dpi);
    int icon_h = DesignSystem::ScaleForDpi(26, m.dpi);
    int icon_x = icon_box_rc.left + (icon_box_w - icon_w) / 2;
    int icon_y = icon_box_rc.top + (icon_box_h - icon_h) / 2;
    DrawFileTypeIconGdi(hdc, icon_x, icon_y, icon_w, icon_h, dummy_row, pal);

    // Text content bounds
    int text_left = icon_box_rc.right + DesignSystem::ScaleForDpi(14, m.dpi);
    int text_right = geometry.details_action_buttons.empty()
        ? (card_rc.right - DesignSystem::ScaleForDpi(16, m.dpi))
        : (geometry.details_action_buttons.front().rect.left - DesignSystem::ScaleForDpi(14, m.dpi));
    int total_avail_w = text_right - text_left;
    if (total_avail_w <= 50) return;

    if (total_avail_w >= DesignSystem::ScaleForDpi(300, m.dpi)) {
        // Two-column responsive layout (Always visible in >= 760px windows)
        int col1_w = (total_avail_w * 48) / 100 - DesignSystem::ScaleForDpi(8, m.dpi);
        int col2_x = text_left + col1_w + DesignSystem::ScaleForDpi(18, m.dpi);
        int col2_w = text_right - col2_x;
        RECT details_divider{col2_x - DesignSystem::ScaleForDpi(9, m.dpi),
            card_rc.top + DesignSystem::ScaleForDpi(18, m.dpi),
            col2_x - DesignSystem::ScaleForDpi(8, m.dpi),
            card_rc.bottom - DesignSystem::ScaleForDpi(18, m.dpi)};
        FillRectSolid(hdc, details_divider, pal.border_subtle);

        const int col1_text_right = col2_x - DesignSystem::ScaleForDpi(14, m.dpi);

        // Col 1, Row 1: Filename + Engine Badge
        RECT fn_rc{text_left, top_y, col1_text_right, top_y + line_h};
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring wname = Utf8ToW(details.file_name);
        DrawTextW(hdc, wname.c_str(), -1, &fn_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 1, Row 2: Size | Speed | Progress summary
        RECT stat_rc{text_left, top_y + line_h, col1_text_right, top_y + line_h * 2};
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring wstat = Utf8ToW(details.size_summary);
        if (!details.speed_summary.empty()) {
            wstat += L" \u2022 " + Utf8ToW(details.speed_summary);
        }
        if (!details.progress_summary.empty()) {
            wstat += L" \u2022 " + Utf8ToW(details.progress_summary);
        }
        DrawTextW(hdc, wstat.c_str(), -1, &stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 1, Row 3: Destination Folder
        RECT dest_rc{text_left, top_y + line_h * 2, col1_text_right, top_y + line_h * 3};
        SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
        std::wstring wdest = L"Location: " + Utf8ToW(DestinationFolderForDisplay(details.destination_path));
        DrawTextW(hdc, wdest.c_str(), -1, &dest_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 2, Row 1: Source URL (redacted)
        RECT url_rc{col2_x, top_y, col2_x + col2_w, top_y + line_h};
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring wurl = L"Source: " + Utf8ToW(details.redacted_source_url.empty() ? details.redacted_page_url : details.redacted_source_url);
        DrawTextW(hdc, wurl.c_str(), -1, &url_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 2, Row 2: Timestamps (collapse duplicate same-day date prefix)
        RECT time_rc{col2_x, top_y + line_h, col2_x + col2_w, top_y + line_h * 2};
        std::wstring wtime = L"Added: " + Utf8ToW(details.created_time_str);
        if (!details.completed_time_str.empty() && details.completed_time_str != "\u2014") {
            std::string fin_disp = details.completed_time_str;
            auto c_comma = details.created_time_str.find(", ");
            auto f_comma = details.completed_time_str.find(", ");
            if (c_comma != std::string::npos && f_comma != std::string::npos &&
                details.created_time_str.substr(0, c_comma) == details.completed_time_str.substr(0, f_comma)) {
                fin_disp = details.completed_time_str.substr(f_comma + 2);
            }
            wtime += L" \u2022 Finished: " + Utf8ToW(fin_disp);
        }
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        DrawTextW(hdc, wtime.c_str(), -1, &time_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        // Col 2, Row 3: Error explanation or stream details
        RECT err_rc{col2_x, top_y + line_h * 2, col2_x + col2_w, top_y + line_h * 3};
        if (!details.error_explanation.empty()) {
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring werr = L"Status: " + Utf8ToW(details.error_explanation);
            DrawTextW(hdc, werr.c_str(), -1, &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (!details.stream_format.empty()) {
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring wstream = L"Stream: " + Utf8ToW(details.stream_format);
            if (!details.selected_tracks_summary.empty()) {
                wstream += L" (" + Utf8ToW(details.selected_tracks_summary) + L")";
            }
            DrawTextW(hdc, wstream.c_str(), -1, &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (!details.speed_limit_summary.empty() && details.speed_limit_summary != "Unlimited") {
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring wlimit = L"Priority: " + Utf8ToW(details.priority_summary.empty() ? "Normal" : details.priority_summary)
                                + L" \u2022 Speed Limit: " + Utf8ToW(details.speed_limit_summary);
            DrawTextW(hdc, wlimit.c_str(), -1, &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        } else if (!details.priority_summary.empty()) {
            SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
            std::wstring wprio = L"Priority: " + Utf8ToW(details.priority_summary);
            DrawTextW(hdc, wprio.c_str(), -1, &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    } else {
        // Compact single-column layout for narrower windows
        RECT fn_rc{text_left, top_y, text_right, top_y + line_h};
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring wname = Utf8ToW(details.file_name);
        DrawTextW(hdc, wname.c_str(), -1, &fn_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT stat_rc{text_left, top_y + line_h, text_right, top_y + line_h * 2};
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring wstat = Utf8ToW(details.size_summary);
        if (!details.speed_summary.empty()) wstat += L" \u2022 " + Utf8ToW(details.speed_summary);
        if (!details.progress_summary.empty()) wstat += L" \u2022 " + Utf8ToW(details.progress_summary);
        DrawTextW(hdc, wstat.c_str(), -1, &stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        RECT dest_rc{text_left, top_y + line_h * 2, text_right, top_y + line_h * 3};
        SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
        std::wstring wdest = L"Location: " + Utf8ToW(DestinationFolderForDisplay(details.destination_path));
        DrawTextW(hdc, wdest.c_str(), -1, &dest_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    }
}

void PaintMainWindow(HWND hwnd, DesktopView& view) {
    PAINTSTRUCT ps{};
    HDC hdc_screen = BeginPaint(hwnd, &ps);

    RECT client_rc{};
    GetClientRect(hwnd, &client_rc);
    int w = client_rc.right - client_rc.left;
    int h = client_rc.bottom - client_rc.top;

    HDC hdc = CreateCompatibleDC(hdc_screen);
    HBITMAP hbm = CreateCompatibleBitmap(hdc_screen, std::max(1, w), std::max(1, h));
    HGDIOBJ old_bm = SelectObject(hdc, hbm);
    SetLayout(hdc_screen, 0);
    SetLayout(hdc, 0);

    const auto& pal = view.GetPalette();
    auto geom = view.ComputeLayoutGeometry(view.GetScrollOffset());

    const auto& m = view.GetMetrics();
    auto all_tasks = view.GetFilteredAndSortedTasks();
    int row_h_calc = std::max(1, geom.task_row_height);
    size_t visible_count = static_cast<size_t>(std::max(1, geom.table_body_rect.height() / row_h_calc));
    ShowScrollBar(hwnd, SB_VERT, FALSE);
    auto* ctx = reinterpret_cast<Win32WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // 0. Base Matte Charcoal Canvas
    FillRectSolid(hdc, client_rc, pal.app_background);
    SetBkMode(hdc, TRANSPARENT);

    HFONT hFontTitle = CreateFontW(
        -m.font_title_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBody = CreateFontW(
        -m.font_body_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontBold = CreateFontW(
        -m.font_body_strong_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmall = CreateFontW(
        -m.font_caption_px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT hFontSmallBold = CreateFontW(
        -m.font_caption_px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = SelectObject(hdc, hFontBody);

    // Fixed left navigation rail with a flexible bottom anchored Settings item.
    RECT sidebar_rc = ToWinRect(geom.sidebar_rect);
    FillRectSolid(hdc, sidebar_rc, pal.surface_primary);
    RECT sidebar_divider{sidebar_rc.right - 1, 0, sidebar_rc.right, h};
    FillRectSolid(hdc, sidebar_divider, pal.border_subtle);

    // Modern macOS Sidebar: Clean muted section header
    RECT section_hdr_rc{sidebar_rc.left + DesignSystem::ScaleForDpi(16, m.dpi), DesignSystem::ScaleForDpi(18, m.dpi),
                        sidebar_rc.right - DesignSystem::ScaleForDpi(16, m.dpi), DesignSystem::ScaleForDpi(36, m.dpi)};
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
    DrawTextW(hdc, L"LIBRARY", -1, &section_hdr_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const bool compact_sidebar_paint = geom.sidebar_rect.width() <= DesignSystem::ScaleForDpi(168, m.dpi);

    for (size_t i = 0; i < geom.sidebar_items.size(); ++i) {
        const auto& item = geom.sidebar_items[i];
        RECT item_rc = ToWinRect(item.rect);
        bool selected = item.selected;
        bool hovered = view.GetHoveredSidebarIndex() == static_cast<int>(i);
        const bool is_light = (pal.mode == ThemeMode::kLight);
        const int pill_r = DesignSystem::ScaleForDpi(8, m.dpi);

        if (selected) {
            ColorRgba active_bg = is_light ? ColorRgba{0xFF, 0xFF, 0xFF, 255} : ColorRgba{0x28, 0x28, 0x2E, 255};
            ColorRgba active_bd = is_light ? ColorRgba{0xCB, 0xD5, 0xE1, 255} : ColorRgba{0x36, 0x36, 0x3F, 255};
            DrawRoundedBoxGdi(hdc, item_rc, pill_r, active_bg, active_bd, 1);
        } else if (hovered) {
            ColorRgba hover_bg = is_light ? ColorRgba{0xEA, 0xEC, 0xEF, 255} : ColorRgba{0x22, 0x22, 0x26, 255};
            DrawRoundedBoxGdi(hdc, item_rc, pill_r, hover_bg, hover_bg, 0);
        }

        // Distinct 16x16px vector outline icons (NO outer circles)
        int pad_x = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 8 : 10, m.dpi);
        int icon_s = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 15 : 16, m.dpi);
        float ix = static_cast<float>(item_rc.left + pad_x);
        float iy = static_cast<float>(item_rc.top + (item_rc.bottom - item_rc.top - icon_s) / 2);
        float fis = static_cast<float>(icon_s);
        ColorRgba nav_icon_color = selected ? pal.accent_primary : (is_light ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0x74, 0x74, 0x7E, 255});

        {
            Gdiplus::Graphics gNav(hdc);
            gNav.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pNav(Gdiplus::Color(nav_icon_color.a, nav_icon_color.r, nav_icon_color.g, nav_icon_color.b), 1.6f);
            pNav.SetStartCap(Gdiplus::LineCapRound);
            pNav.SetEndCap(Gdiplus::LineCapRound);
            pNav.SetLineJoin(Gdiplus::LineJoinRound);

            if (i == 0) {
                // "All Downloads": Lucide 'layers' outline
                Gdiplus::PointF diamond[4] = {
                    {ix + fis * 0.5f, iy + 1.5f},
                    {ix + fis - 1.0f, iy + fis * 0.25f + 1.5f},
                    {ix + fis * 0.5f, iy + fis * 0.5f + 1.0f},
                    {ix + 1.0f, iy + fis * 0.25f + 1.5f}
                };
                gNav.DrawPolygon(&pNav, diamond, 4);
                Gdiplus::PointF mid_layer[3] = {
                    {ix + 1.0f, iy + fis * 0.5f + 2.5f},
                    {ix + fis * 0.5f, iy + fis * 0.75f + 1.0f},
                    {ix + fis - 1.0f, iy + fis * 0.5f + 2.5f}
                };
                gNav.DrawLines(&pNav, mid_layer, 3);
                Gdiplus::PointF bot_layer[3] = {
                    {ix + 1.0f, iy + fis * 0.75f + 2.0f},
                    {ix + fis * 0.5f, iy + fis - 0.5f},
                    {ix + fis - 1.0f, iy + fis * 0.75f + 2.0f}
                };
                gNav.DrawLines(&pNav, bot_layer, 3);
            } else if (i == 1) {
                // "In Progress": Lucide 'download' (arrow into tray, no circle)
                float cx = ix + fis * 0.5f;
                gNav.DrawLine(&pNav, cx, iy + 2.0f, cx, iy + fis * 0.68f);
                Gdiplus::PointF arrow[3] = {
                    {cx - fis * 0.24f, iy + fis * 0.45f},
                    {cx, iy + fis * 0.68f},
                    {cx + fis * 0.24f, iy + fis * 0.45f}
                };
                gNav.DrawLines(&pNav, arrow, 3);
                Gdiplus::PointF tray[4] = {
                    {ix + 2.0f, iy + fis * 0.68f},
                    {ix + 2.0f, iy + fis - 2.0f},
                    {ix + fis - 2.0f, iy + fis - 2.0f},
                    {ix + fis - 2.0f, iy + fis * 0.68f}
                };
                gNav.DrawLines(&pNav, tray, 4);
            } else if (i == 2) {
                // "Completed": Lucide 'check-check' (double checkmark, no circle)
                Gdiplus::PointF chk1[3] = {
                    {ix + 1.5f, iy + fis * 0.55f},
                    {ix + fis * 0.35f, iy + fis * 0.76f},
                    {ix + fis * 0.72f, iy + fis * 0.24f}
                };
                gNav.DrawLines(&pNav, chk1, 3);
                Gdiplus::PointF chk2[3] = {
                    {ix + fis * 0.48f, iy + fis * 0.68f},
                    {ix + fis * 0.56f, iy + fis * 0.76f},
                    {ix + fis - 1.5f, iy + fis * 0.24f}
                };
                gNav.DrawLines(&pNav, chk2, 3);
            } else if (i == 3) {
                // "Paused": standalone rounded pause bars (no circle)
                float bar_top = iy + 2.5f;
                float bar_bot = iy + fis - 2.5f;
                gNav.DrawLine(&pNav, ix + fis * 0.34f, bar_top, ix + fis * 0.34f, bar_bot);
                gNav.DrawLine(&pNav, ix + fis * 0.66f, bar_top, ix + fis * 0.66f, bar_bot);
            }
        }

        // Sculpted counter capsule badge on ALL 4 library items
        const std::wstring count_str = std::to_wstring(item.count);
        SelectObject(hdc, hFontSmallBold);
        SIZE cnt_sz{};
        GetTextExtentPoint32W(hdc, count_str.c_str(), static_cast<int>(count_str.size()), &cnt_sz);
        int badge_h = DesignSystem::ScaleForDpi(20, m.dpi);
        int badge_pad_x = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 5 : 7, m.dpi);
        int badge_min_w = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 22 : 26, m.dpi);
        int badge_w = std::max(badge_min_w, static_cast<int>(cnt_sz.cx) + badge_pad_x * 2);
        int badge_edge_pad = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 6 : 8, m.dpi);
        int badge_top = item_rc.top + (item_rc.bottom - item_rc.top - badge_h) / 2;
        RECT badge_rc{item_rc.right - badge_edge_pad - badge_w, badge_top, item_rc.right - badge_edge_pad, badge_top + badge_h};

        ColorRgba badge_bg = selected
            ? (is_light ? ColorRgba{0xE0, 0xF2, 0xFE, 255} : ColorRgba{0x3F, 0x23, 0x27, 255})
            : (is_light ? ColorRgba{0xE2, 0xE8, 0xF0, 255} : ColorRgba{0x24, 0x24, 0x29, 255});
        ColorRgba badge_fg = selected
            ? (is_light ? ColorRgba{0x02, 0x84, 0xC7, 255} : ColorRgba{0xFF, 0x6B, 0x78, 255})
            : (is_light ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0x74, 0x74, 0x7E, 255});
        DrawRoundedBoxGdi(hdc, badge_rc, DesignSystem::ScaleForDpi(6, m.dpi), badge_bg, badge_bg, 0);
        SetTextColor(hdc, badge_fg.ToWin32ColorRef());
        DrawTextW(hdc, count_str.c_str(), -1, &badge_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        int label_gap = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 7 : 10, m.dpi);
        RECT label_rc{item_rc.left + pad_x + icon_s + label_gap, item_rc.top,
                      badge_rc.left - DesignSystem::ScaleForDpi(4, m.dpi), item_rc.bottom};
        SelectObject(hdc, compact_sidebar_paint ? (selected ? hFontSmallBold : hFontSmall) : (selected ? hFontBold : hFontBody));
        ColorRgba nav_label_color = selected ? pal.text_primary : (is_light ? ColorRgba{0x47, 0x55, 0x69, 255} : ColorRgba{0xA1, 0xA1, 0xAA, 255});
        SetTextColor(hdc, nav_label_color.ToWin32ColorRef());
        std::wstring label = Utf8ToW(item.label);
        DrawTextW(hdc, label.c_str(), -1, &label_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    const bool is_light_footer = (pal.mode == ThemeMode::kLight);
    RECT sidebar_language_rc = ToWinRect(geom.sidebar_language_rect);
    RECT lang_div{sidebar_rc.left + DesignSystem::ScaleForDpi(10, m.dpi),
                  sidebar_language_rc.top - DesignSystem::ScaleForDpi(10, m.dpi),
                  sidebar_rc.right - DesignSystem::ScaleForDpi(10, m.dpi),
                  sidebar_language_rc.top - DesignSystem::ScaleForDpi(10, m.dpi) + 1};
    FillRectSolid(hdc, lang_div, pal.border_subtle);

    const bool language_hovered = view.GetHoveredDetailsBtnIndex() == -3;
    if (language_hovered) {
        DrawRoundedBoxGdi(hdc, sidebar_language_rc, DesignSystem::ScaleForDpi(8, m.dpi),
            is_light_footer ? ColorRgba{0xEA, 0xEC, 0xEF, 255} : ColorRgba{0x22, 0x22, 0x26, 255}, ColorRgba{0, 0, 0, 0}, 0);
    }
    int f_pad_x = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 8 : 10, m.dpi);
    int f_gap_x = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 7 : 10, m.dpi);
    int f_icon_s = DesignSystem::ScaleForDpi(compact_sidebar_paint ? 15 : 16, m.dpi);
    int f_icon_y_off = (sidebar_language_rc.bottom - sidebar_language_rc.top - f_icon_s) / 2;
    RECT language_icon_rc{sidebar_language_rc.left + f_pad_x, sidebar_language_rc.top + f_icon_y_off,
                          sidebar_language_rc.left + f_pad_x + f_icon_s, sidebar_language_rc.top + f_icon_y_off + f_icon_s};
    DrawGlobeGlyphGdi(hdc, language_icon_rc, language_hovered ? pal.accent_primary : (is_light_footer ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0x74, 0x74, 0x7E, 255}));

    // Locale capsule badge (e.g. "EN", "AR")
    std::string lang_code = GetLocalizationManager().GetLocale();
    if (lang_code.size() > 2) lang_code.resize(2);
    for (char& ch : lang_code) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (lang_code.empty()) lang_code = "EN";
    std::wstring wlang_code = Utf8ToW(lang_code);
    int lang_badge_h = DesignSystem::ScaleForDpi(20, m.dpi);
    int lang_badge_w = DesignSystem::ScaleForDpi(28, m.dpi);
    int lang_badge_pad = DesignSystem::ScaleForDpi(8, m.dpi);
    int lang_badge_top = sidebar_language_rc.top + (sidebar_language_rc.bottom - sidebar_language_rc.top - lang_badge_h) / 2;
    RECT lang_badge_rc{sidebar_language_rc.right - lang_badge_pad - lang_badge_w, lang_badge_top,
                       sidebar_language_rc.right - lang_badge_pad, lang_badge_top + lang_badge_h};
    ColorRgba lang_badge_bg = is_light_footer ? ColorRgba{0xE2, 0xE8, 0xF0, 255} : ColorRgba{0x26, 0x26, 0x2B, 255};
    ColorRgba lang_badge_fg = is_light_footer ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0xA1, 0xA1, 0xAA, 255};
    DrawRoundedBoxGdi(hdc, lang_badge_rc, DesignSystem::ScaleForDpi(6, m.dpi), lang_badge_bg, lang_badge_bg, 0);
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, lang_badge_fg.ToWin32ColorRef());
    DrawTextW(hdc, wlang_code.c_str(), -1, &lang_badge_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    RECT language_label_rc{language_icon_rc.right + f_gap_x, sidebar_language_rc.top,
                           lang_badge_rc.left - DesignSystem::ScaleForDpi(6, m.dpi), sidebar_language_rc.bottom};
    SelectObject(hdc, compact_sidebar_paint ? hFontSmall : hFontBody);
    SetTextColor(hdc, language_hovered ? pal.text_primary.ToWin32ColorRef() : (is_light_footer ? ColorRgba{0x47, 0x55, 0x69, 255}.ToWin32ColorRef() : ColorRgba{0xA1, 0xA1, 0xAA, 255}.ToWin32ColorRef()));
    const std::wstring language_text = Utf8ToW(GetLocalizationManager().Translate("Language"));
    DrawTextW(hdc, language_text.c_str(), -1, &language_label_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT sidebar_settings_rc = ToWinRect(geom.sidebar_settings_rect);
    bool settings_hovered = view.GetHoveredDetailsBtnIndex() == -2;
    if (settings_hovered) {
        DrawRoundedBoxGdi(hdc, sidebar_settings_rc, DesignSystem::ScaleForDpi(8, m.dpi),
            is_light_footer ? ColorRgba{0xEA, 0xEC, 0xEF, 255} : ColorRgba{0x22, 0x22, 0x26, 255}, ColorRgba{0, 0, 0, 0}, 0);
    }
    int s_icon_y_off = (sidebar_settings_rc.bottom - sidebar_settings_rc.top - f_icon_s) / 2;
    RECT sidebar_gear_rc{sidebar_settings_rc.left + f_pad_x, sidebar_settings_rc.top + s_icon_y_off,
                         sidebar_settings_rc.left + f_pad_x + f_icon_s, sidebar_settings_rc.top + s_icon_y_off + f_icon_s};
    DrawGearGlyphGdi(hdc, sidebar_gear_rc, settings_hovered ? pal.accent_primary : (is_light_footer ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0x74, 0x74, 0x7E, 255}));
    RECT settings_label_rc{sidebar_gear_rc.right + f_gap_x, sidebar_settings_rc.top,
                           sidebar_settings_rc.right - DesignSystem::ScaleForDpi(8, m.dpi), sidebar_settings_rc.bottom};
    SelectObject(hdc, compact_sidebar_paint ? hFontSmall : hFontBody);
    SetTextColor(hdc, settings_hovered ? pal.text_primary.ToWin32ColorRef() : (is_light_footer ? ColorRgba{0x47, 0x55, 0x69, 255}.ToWin32ColorRef() : ColorRgba{0xA1, 0xA1, 0xAA, 255}.ToWin32ColorRef()));
    const std::wstring settings_text = Utf8ToW(GetLocalizationManager().Translate("Settings"));
    DrawTextW(hdc, settings_text.c_str(), -1, &settings_label_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 1. Toolbar in the main content region.
    RECT tb_rc = ToWinRect(geom.toolbar_rect);
    FillRectSolid(hdc, tb_rc, (pal.mode == ThemeMode::kLight) ? ColorRgba{0xFF, 0xFF, 0xFF, 255} : pal.surface_primary);
    RECT tb_div{tb_rc.left, tb_rc.bottom - 1, tb_rc.right, tb_rc.bottom};
    FillRectSolid(hdc, tb_div, pal.border_subtle);

    // 1.1 + Add download Pill Button (smooth hover and press)
    RECT add_rc = ToWinRect(geom.add_download_button_rect);
    bool add_hovered = (view.GetHoveredToolbarIndex() == 0);
    bool add_pressed = (view.GetAnimationState().pressed_toolbar_idx == 0);
    float add_hp = add_hovered ? view.GetAnimationState().ComputeTransitionProgress(view.GetAnimationState().toolbar_hover_start_ms, 140) : 0.0f;
    ColorRgba add_bg = pal.accent_primary;
    if (add_pressed) {
        add_bg = pal.accent_primary.Interpolate(ColorRgba{0x00, 0x00, 0x00, 255}, 0.15f);
    } else if (add_hp > 0.0f) {
        add_bg = pal.accent_primary.Interpolate(pal.accent_hover, add_hp);
    }
    DrawRoundedBoxGdi(hdc, add_rc, DesignSystem::ScaleForDpi(6, m.dpi), add_bg, add_bg, 0);
    SelectObject(hdc, compact_sidebar_paint ? hFontSmallBold : hFontBold);
    SetTextColor(hdc, RGB(255, 255, 255));
    std::wstring add_download_text = L"+ " + Utf8ToW(GetLocalizationManager().Translate("Add Download"));
    DrawTextW(hdc, add_download_text.c_str(), -1, &add_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // 1.2 Sort Dropdown
    RECT sort_rc = ToWinRect(geom.sort_dropdown_rect);
    if (sort_rc.right > sort_rc.left) {
        DrawRoundedBoxGdi(hdc, sort_rc, DesignSystem::ScaleForDpi(6, m.dpi), pal.surface_elevated, pal.border_subtle, 1);
        const bool sort_compact = (sort_rc.right - sort_rc.left <= DesignSystem::ScaleForDpi(60, m.dpi));
        DrawHamburgerGlyphGdi(hdc, sort_rc.left + DesignSystem::ScaleForDpi(sort_compact ? 11 : 14, m.dpi),
            sort_rc.top + (sort_rc.bottom - sort_rc.top) / 2 - 5, 12, 10, pal.text_muted);
        if (!sort_compact) {
            RECT sort_txt_rc{sort_rc.left + 32, sort_rc.top, sort_rc.right - 18, sort_rc.bottom};
            SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
            std::wstring sort_label = Utf8ToW(GetLocalizationManager().Translate("Sort"));
            DrawTextW(hdc, sort_label.c_str(), -1, &sort_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }
        DrawCaretGlyphGdi(hdc, sort_rc.right - DesignSystem::ScaleForDpi(12, m.dpi),
            sort_rc.top + (sort_rc.bottom - sort_rc.top) / 2, pal.text_muted, false);
    }

    // 1.3 Filter Dropdown
    RECT filter_rc = ToWinRect(geom.filter_dropdown_rect);
    if (filter_rc.right > filter_rc.left) {
        DrawRoundedBoxGdi(hdc, filter_rc, DesignSystem::ScaleForDpi(6, m.dpi), pal.surface_elevated, pal.border_subtle, 1);
        {
            Gdiplus::Graphics gFilt(hdc);
            gFilt.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            Gdiplus::Pen pFilt(Gdiplus::Color(pal.text_muted.a, pal.text_muted.r, pal.text_muted.g, pal.text_muted.b), 1.4f);
            pFilt.SetStartCap(Gdiplus::LineCapRound);
            pFilt.SetEndCap(Gdiplus::LineCapRound);
            pFilt.SetLineJoin(Gdiplus::LineJoinRound);
            float fx = static_cast<float>(filter_rc.left + DesignSystem::ScaleForDpi(10, m.dpi));
            float fs = static_cast<float>(DesignSystem::ScaleForDpi(12, m.dpi));
            float fy = static_cast<float>(filter_rc.top + (filter_rc.bottom - filter_rc.top - static_cast<int>(fs)) / 2);
            Gdiplus::PointF funnel[7] = {
                {fx, fy + 1.0f},
                {fx + fs, fy + 1.0f},
                {fx + fs * 0.62f, fy + fs * 0.52f},
                {fx + fs * 0.62f, fy + fs - 1.0f},
                {fx + fs * 0.38f, fy + fs - 2.5f},
                {fx + fs * 0.38f, fy + fs * 0.52f},
                {fx, fy + 1.0f}
            };
            gFilt.DrawLines(&pFilt, funnel, 7);
        }
        const bool filt_compact = (filter_rc.right - filter_rc.left <= DesignSystem::ScaleForDpi(92, m.dpi));
        RECT filter_txt_rc{filter_rc.left + DesignSystem::ScaleForDpi(28, m.dpi), filter_rc.top,
                           filter_rc.right - DesignSystem::ScaleForDpi(18, m.dpi), filter_rc.bottom};
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring wcat = (view.GetSelectedCategory() == SidebarCategory::kAllDownloads)
            ? (filt_compact ? L"All" : Utf8ToW(GetLocalizationManager().Translate("All Files")))
            : Utf8ToW(view.CategoryToLabel(view.GetSelectedCategory()));
        DrawTextW(hdc, wcat.c_str(), -1, &filter_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        DrawCaretGlyphGdi(hdc, filter_rc.right - DesignSystem::ScaleForDpi(12, m.dpi),
            filter_rc.top + (filter_rc.bottom - filter_rc.top) / 2, pal.text_muted, false);
    }

    RECT theme_rc = ToWinRect(geom.theme_button_rect);
    POINT theme_cursor{};
    GetCursorPos(&theme_cursor);
    ScreenToClient(hwnd, &theme_cursor);
    const bool theme_hovered = geom.theme_button_rect.Contains(theme_cursor.x, theme_cursor.y);
    const bool theme_pressed = view.GetAnimationState().pressed_toolbar_idx == -2;
    const ColorRgba theme_button_bg = theme_pressed ? pal.surface_pressed :
        (theme_hovered ? pal.surface_hover : pal.surface_primary);
    DrawRoundedBoxGdi(hdc, theme_rc, DesignSystem::ScaleForDpi(8, m.dpi),
        theme_button_bg, pal.border_subtle, 1);
    ColorRgba theme_icon_col = theme_hovered ? pal.accent_primary : pal.text_secondary;
    DrawThemeGlyphGdi(hdc, theme_rc, theme_icon_col, theme_button_bg,
        pal.mode == ThemeMode::kLight);

    // 1.4 Tools & Global Menu Button
    RECT tools_rc = ToWinRect(geom.tools_button_rect);
    if (tools_rc.right > tools_rc.left) {
        const bool tools_hovered = geom.tools_button_rect.Contains(theme_cursor.x, theme_cursor.y);
        const bool tools_pressed = view.GetAnimationState().pressed_toolbar_idx == -3;
        const ColorRgba tools_bg = tools_pressed ? pal.surface_pressed :
            (tools_hovered ? pal.surface_hover : pal.surface_primary);
        DrawRoundedBoxGdi(hdc, tools_rc, DesignSystem::ScaleForDpi(8, m.dpi), tools_bg, pal.border_subtle, 1);
        ColorRgba tools_icon_col = tools_hovered ? pal.accent_primary : pal.text_secondary;
        const bool tools_compact = (tools_rc.right - tools_rc.left <= DesignSystem::ScaleForDpi(44, m.dpi));
        int glyph_x = tools_compact
            ? (tools_rc.left + (tools_rc.right - tools_rc.left - 12) / 2)
            : (tools_rc.left + DesignSystem::ScaleForDpi(10, m.dpi));
        DrawHamburgerGlyphGdi(hdc, glyph_x, tools_rc.top + (tools_rc.bottom - tools_rc.top) / 2 - 5, 12, 10, tools_icon_col);
        if (!tools_compact) {
            RECT tools_txt_rc{glyph_x + DesignSystem::ScaleForDpi(16, m.dpi), tools_rc.top,
                              tools_rc.right - DesignSystem::ScaleForDpi(6, m.dpi), tools_rc.bottom};
            SelectObject(hdc, hFontBody);
            SetTextColor(hdc, tools_hovered ? pal.text_primary.ToWin32ColorRef() : pal.text_secondary.ToWin32ColorRef());
            std::wstring tools_label = Utf8ToW(GetLocalizationManager().Translate("Tools"));
            DrawTextW(hdc, tools_label.c_str(), -1, &tools_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
    }

    // 1.5 Dynamic Context Action Buttons (macOS segmented action cluster)
    int first_icon_idx = -1;
    int last_icon_idx = -1;
    for (size_t btn_idx = 1; btn_idx < geom.toolbar_buttons.size() - 1; ++btn_idx) {
        const auto& btn = geom.toolbar_buttons[btn_idx];
        if (btn.rect.width() <= 0 || btn.rect.height() <= 0) continue;
        const bool is_icon_action = btn.action_id == ToolbarActionId::kStart ||
            btn.action_id == ToolbarActionId::kPause || btn.action_id == ToolbarActionId::kRemove;
        if (is_icon_action) {
            if (first_icon_idx == -1) first_icon_idx = static_cast<int>(btn_idx);
            last_icon_idx = static_cast<int>(btn_idx);
        }
    }

    if (first_icon_idx != -1 && last_icon_idx != -1) {
        RECT first_rc = ToWinRect(geom.toolbar_buttons[first_icon_idx].rect);
        RECT last_rc = ToWinRect(geom.toolbar_buttons[last_icon_idx].rect);
        RECT group_rc{
            std::min(first_rc.left, last_rc.left),
            first_rc.top,
            std::max(first_rc.right, last_rc.right),
            first_rc.bottom
        };
        ColorRgba grp_bg = (pal.mode == ThemeMode::kLight) ? ColorRgba{0xF1, 0xF5, 0xF9, 255} : ColorRgba{0x26, 0x26, 0x2A, 255};
        ColorRgba grp_border = (pal.mode == ThemeMode::kLight) ? ColorRgba{0xE2, 0xE8, 0xF0, 255} : ColorRgba{0x34, 0x34, 0x38, 255};
        DrawRoundedBoxGdi(hdc, group_rc, DesignSystem::ScaleForDpi(6, m.dpi), grp_bg, grp_border, 1);

        for (int idx = first_icon_idx; idx < last_icon_idx; ++idx) {
            RECT btn_rc = ToWinRect(geom.toolbar_buttons[idx].rect);
            RECT div_rc{btn_rc.right - 1, group_rc.top + DesignSystem::ScaleForDpi(4, m.dpi), btn_rc.right, group_rc.bottom - DesignSystem::ScaleForDpi(4, m.dpi)};
            FillRectSolid(hdc, div_rc, grp_border);
        }
    }

    for (size_t btn_idx = 1; btn_idx < geom.toolbar_buttons.size() - 1; ++btn_idx) {
        const auto& btn = geom.toolbar_buttons[btn_idx];
        if (btn.rect.width() <= 0 || btn.rect.height() <= 0) continue;
        RECT brc = ToWinRect(btn.rect);
        bool hovered = (view.GetHoveredToolbarIndex() == static_cast<int>(btn_idx));
        bool pressed = (view.GetAnimationState().pressed_toolbar_idx == static_cast<int>(btn_idx));

        const bool is_start_all = btn.action_id == ToolbarActionId::kStartAll;
        const bool is_pause_all = btn.action_id == ToolbarActionId::kPauseAll;
        const bool is_clear_history = btn.action_id == ToolbarActionId::kClearHistory;
        const bool is_icon_action = btn.action_id == ToolbarActionId::kStart ||
            btn.action_id == ToolbarActionId::kPause || btn.action_id == ToolbarActionId::kRemove;

        bool has_selection = !view.GetSelectedTaskId().empty();

        if (is_icon_action) {
            if ((hovered || pressed) && has_selection) {
                ColorRgba btn_bg = hovered ? pal.surface_hover : pal.surface_pressed;
                DrawRoundedBoxGdi(hdc, brc, DesignSystem::ScaleForDpi(4, m.dpi), btn_bg, ColorRgba{0, 0, 0, 0}, 0);
            }
        } else {
            // Secondary button pill styling
            ColorRgba pill_bg = hovered ? pal.surface_hover : (pressed ? pal.surface_pressed : pal.surface_elevated);
            ColorRgba pill_border = (pal.mode == ThemeMode::kLight) ? ColorRgba{0xE2, 0xE8, 0xF0, 255} : ColorRgba{0x2E, 0x2E, 0x34, 255};
            DrawRoundedBoxGdi(hdc, brc, DesignSystem::ScaleForDpi(6, m.dpi), pill_bg, pill_border, 1);
        }

        SelectObject(hdc, hFontSmallBold);
        ColorRgba txt_col = hovered ? pal.text_primary : pal.text_secondary;
        if (is_icon_action && !has_selection) {
            txt_col.a = 90;
        }

        if (is_icon_action) {
            RECT glyph_rc{brc.left + DesignSystem::ScaleForDpi(9, m.dpi), brc.top + DesignSystem::ScaleForDpi(9, m.dpi),
                brc.right - DesignSystem::ScaleForDpi(9, m.dpi), brc.bottom - DesignSystem::ScaleForDpi(9, m.dpi)};
            if (btn.action_id == ToolbarActionId::kStart) {
                DrawPlayGlyphGdi(hdc, glyph_rc, txt_col);
            } else if (btn.action_id == ToolbarActionId::kPause) {
                DrawPauseGlyphGdi(hdc, glyph_rc, txt_col);
            } else {
                DrawTrashGlyphGdi(hdc, glyph_rc, txt_col);
            }
            continue;
        }

        SetTextColor(hdc, txt_col.ToWin32ColorRef());
        std::wstring wlbl = Utf8ToW(btn.label);
        RECT label_rc = brc;
        const bool is_compact_batch = (brc.right - brc.left <= DesignSystem::ScaleForDpi(72, m.dpi));
        if (is_compact_batch && (is_start_all || is_pause_all || is_clear_history)) {
            int g_pad_l = DesignSystem::ScaleForDpi(8, m.dpi);
            int g_sz = DesignSystem::ScaleForDpi(10, m.dpi);
            int g_top = brc.top + (brc.bottom - brc.top - g_sz) / 2;
            RECT glyph_rc{brc.left + g_pad_l, g_top, brc.left + g_pad_l + g_sz, g_top + g_sz};
            if (is_start_all) {
                DrawPlayGlyphGdi(hdc, glyph_rc, txt_col);
                wlbl = L"All";
            } else if (is_pause_all) {
                DrawPauseGlyphGdi(hdc, glyph_rc, txt_col);
                wlbl = L"All";
            } else {
                RECT trash_rc{brc.left + DesignSystem::ScaleForDpi(7, m.dpi), brc.top + (brc.bottom - brc.top - DesignSystem::ScaleForDpi(12, m.dpi)) / 2,
                              brc.left + DesignSystem::ScaleForDpi(19, m.dpi), brc.top + (brc.bottom - brc.top + DesignSystem::ScaleForDpi(12, m.dpi)) / 2};
                DrawTrashGlyphGdi(hdc, trash_rc, txt_col);
                wlbl = L"Clear";
            }
            label_rc.left = glyph_rc.right + DesignSystem::ScaleForDpi(5, m.dpi);
            label_rc.right = brc.right - DesignSystem::ScaleForDpi(5, m.dpi);
            DrawTextW(hdc, wlbl.c_str(), -1, &label_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            continue;
        }
        if (is_start_all) {
            DrawPlayGlyphGdi(hdc, {brc.left + DesignSystem::ScaleForDpi(10, m.dpi), brc.top + DesignSystem::ScaleForDpi(11, m.dpi),
                brc.left + DesignSystem::ScaleForDpi(22, m.dpi), brc.bottom - DesignSystem::ScaleForDpi(11, m.dpi)}, txt_col);
            label_rc.left = brc.left + DesignSystem::ScaleForDpi(28, m.dpi);
            label_rc.right = brc.right - DesignSystem::ScaleForDpi(6, m.dpi);
        } else if (is_pause_all) {
            DrawPauseGlyphGdi(hdc, {brc.left + DesignSystem::ScaleForDpi(10, m.dpi), brc.top + DesignSystem::ScaleForDpi(11, m.dpi),
                brc.left + DesignSystem::ScaleForDpi(22, m.dpi), brc.bottom - DesignSystem::ScaleForDpi(11, m.dpi)}, txt_col);
            label_rc.left = brc.left + DesignSystem::ScaleForDpi(28, m.dpi);
            label_rc.right = brc.right - DesignSystem::ScaleForDpi(6, m.dpi);
        } else if (is_clear_history) {
            RECT broom_rc{brc.left + DesignSystem::ScaleForDpi(10, m.dpi), brc.top + DesignSystem::ScaleForDpi(10, m.dpi),
                          brc.left + DesignSystem::ScaleForDpi(22, m.dpi), brc.bottom - DesignSystem::ScaleForDpi(10, m.dpi)};
            DrawTrashGlyphGdi(hdc, broom_rc, txt_col);
            label_rc.left = brc.left + DesignSystem::ScaleForDpi(28, m.dpi);
            label_rc.right = brc.right - DesignSystem::ScaleForDpi(6, m.dpi);
        }
        DrawTextW(hdc, wlbl.c_str(), -1, &label_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    // 1.6 Search Capsule (macOS Search Bar with 8px radius)
    RECT search_rc = ToWinRect(geom.search_box_rect);
    DrawRoundedBoxGdi(hdc, search_rc, DesignSystem::ScaleForDpi(8, m.dpi), pal.surface_elevated, pal.border_subtle, 1);
    DrawSearchGlassGdi(hdc, search_rc.left + 14, search_rc.top + (search_rc.bottom - search_rc.top) / 2 - 6, 13, pal.text_muted);
    const bool modal_overlay_active = view.IsSettingsCenterOpen() || (ctx && !ctx->conflict_dialog_task_id.empty());
    if (!ctx || !ctx->hSearchEdit || modal_overlay_active) {
        RECT search_txt_rc{search_rc.left + 36, search_rc.top, search_rc.right - 14, search_rc.bottom};
        SelectObject(hdc, hFontSmall);
        const std::string& q = view.GetSearchQuery();
        SetTextColor(hdc, (q.empty() ? pal.text_muted : pal.text_primary).ToWin32ColorRef());
        std::wstring search_disp = q.empty() ? L"Search downloads..." : Utf8ToW(q);
        DrawTextW(hdc, search_disp.c_str(), -1, &search_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }

    // 2. Table Column Headers (Apple muted uppercase subheadings)
    RECT header_rc = ToWinRect(geom.table_header_rect);
    DrawCheckboxGdi(hdc, ToWinRect(geom.select_all_checkbox_rect), view.IsSelectAllChecked(), pal);
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
    RECT clip_hdr_rc{};
    bool needs_header = (IntersectRect(&clip_hdr_rc, &ps.rcPaint, &header_rc) != 0);
    if (needs_header) {
    for (size_t col_idx = 0; col_idx < geom.table_columns.size(); ++col_idx) {
        const auto& col = geom.table_columns[col_idx];
        if (col.rect.width() <= 0) continue;
        RECT crc = ToWinRect(col.rect);
        std::wstring wt = Utf8ToW(col.title);
        UINT align_flags = (col.sort_column == SortColumn::kSize || col.sort_column == SortColumn::kSpeed ||
                            col.sort_column == SortColumn::kDownloadType || col.sort_column == SortColumn::kCreatedAt)
            ? (DT_RIGHT | DT_VCENTER | DT_SINGLELINE)
            : (DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (align_flags & DT_RIGHT) {
            const bool is_rightmost_col = (crc.right >= header_rc.right - 2);
            crc.right -= DesignSystem::ScaleForDpi(is_rightmost_col ? 16 : 8, m.dpi);
        } else if (col.sort_column == SortColumn::kStatus) {
            crc.left += DesignSystem::ScaleForDpi(16, m.dpi);
        } else if (col.sort_column == SortColumn::kFileName) {
            crc.left += DesignSystem::ScaleForDpi(52, m.dpi);
        }
        DrawTextW(hdc, wt.c_str(), -1, &crc, align_flags);
    }
    RECT hdr_div{header_rc.left, header_rc.bottom - 1, header_rc.right, header_rc.bottom};
    FillRectSolid(hdc, hdr_div, pal.border_subtle);
    }

    // 3. Task Rows
    RECT table_body_winrc = ToWinRect(geom.table_body_rect);
    RECT clip_check_rc{};
    bool needs_rows = (IntersectRect(&clip_check_rc, &ps.rcPaint, &table_body_winrc) != 0);
    auto rows = needs_rows ? view.BuildVisibleTaskRows(view.GetScrollOffset(), geom.task_rows.size()) : std::vector<FormattedTaskRow>{};
    if (needs_rows && rows.empty()) {
        RECT empty_rc = ToWinRect(geom.table_body_rect);
        int empty_h = empty_rc.bottom - empty_rc.top;
        int empty_w = empty_rc.right - empty_rc.left;
        int center_y = empty_rc.top + empty_h / 2;
        int box_w = std::min(DesignSystem::ScaleForDpi(480, m.dpi), std::max(DesignSystem::ScaleForDpi(280, m.dpi), empty_w - 60));
        int box_left = empty_rc.left + (empty_w - box_w) / 2;

        // Card container for empty state
        RECT card_rc{box_left, center_y - DesignSystem::ScaleForDpi(80, m.dpi),
                     box_left + box_w, center_y + DesignSystem::ScaleForDpi(75, m.dpi)};
        DrawRoundedBoxGdi(hdc, card_rc, DesignSystem::ScaleForDpi(12, m.dpi),
                          pal.surface_primary, pal.border_subtle, 1);

        // Heading title
        RECT title_rc{card_rc.left + 16, card_rc.top + DesignSystem::ScaleForDpi(16, m.dpi),
                      card_rc.right - 16, card_rc.top + DesignSystem::ScaleForDpi(42, m.dpi)};
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        std::wstring title_str = (view.GetSelectedCategory() == SidebarCategory::kAllDownloads) ? L"No Downloads in Queue" : L"No Items in this Category";
        DrawTextW(hdc, title_str.c_str(), -1, &title_rc, DT_CENTER | DT_TOP | DT_SINGLELINE);

        // Subtitle guidance message
        RECT msg_rc{card_rc.left + 20, card_rc.top + DesignSystem::ScaleForDpi(44, m.dpi),
                    card_rc.right - 20, card_rc.top + DesignSystem::ScaleForDpi(96, m.dpi)};
        SelectObject(hdc, hFontBody);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        std::wstring emsg = Utf8ToW(view.GetEmptyStateMessage());
        DrawTextW(hdc, emsg.c_str(), -1, &msg_rc, DT_CENTER | DT_TOP | DT_WORDBREAK);

        // Action hint pill / shortcut
        RECT hint_rc{card_rc.left + (box_w - DesignSystem::ScaleForDpi(200, m.dpi)) / 2,
                     card_rc.bottom - DesignSystem::ScaleForDpi(38, m.dpi),
                     card_rc.left + (box_w + DesignSystem::ScaleForDpi(200, m.dpi)) / 2,
                     card_rc.bottom - DesignSystem::ScaleForDpi(12, m.dpi)};
        DrawRoundedBoxGdi(hdc, hint_rc, DesignSystem::ScaleForDpi(6, m.dpi),
                          pal.surface_hover, pal.border_subtle, 1);
        SelectObject(hdc, hFontSmallBold);
        SetTextColor(hdc, pal.accent_primary.ToWin32ColorRef());
        DrawTextW(hdc, L"Press Ctrl+N to Add URL", -1, &hint_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    } else {
        for (size_t i = 0; i < rows.size() && i < geom.task_rows.size(); ++i) {
            const auto& rl = geom.task_rows[i];
            RECT rrc = ToWinRect(rl.row_rect);
            RECT row_clip{};
            if (!IntersectRect(&row_clip, &ps.rcPaint, &rrc)) continue;
            const auto& r = rows[i];

            // Selection keeps its short animation, but pointer hover changes
            // immediately when WM_MOUSEMOVE invalidates the old and new rows.
            {
                bool is_selected = r.selected;
                bool is_hovered = (view.GetHoveredRowIndex() == static_cast<int>(i));
                const auto& anim = view.GetAnimationState();
                if (is_selected) {
                    float sel_p = anim.ComputeTransitionProgress(anim.row_select_start_ms, 120);
                    ColorRgba row_col = ColorRgba{0x00, 0x00, 0x00, 0}.Interpolate(pal.surface_selected, sel_p);
                    FillRectSolid(hdc, rrc, row_col);
                    // Crisp 3px vertical accent bar flush against the left edge
                    RECT sel_bar{rrc.left, rrc.top, rrc.left + DesignSystem::ScaleForDpi(3, m.dpi), rrc.bottom};
                    FillRectSolid(hdc, sel_bar, pal.accent_primary);
                } else if (is_hovered) {
                    FillRectSolid(hdc, rrc, pal.surface_hover);
                }
            }

            // Subtle separator line with left inset (48px)
            RECT row_div{rrc.left + DesignSystem::ScaleForDpi(48, m.dpi), rrc.bottom - 1, rrc.right - DesignSystem::ScaleForDpi(16, m.dpi), rrc.bottom};
            FillRectSolid(hdc, row_div, pal.border_subtle);

            RECT chk_rc = ToWinRect(rl.checkbox_rect);
            DrawCheckboxGdi(hdc, chk_rc, r.checked, pal);

            // Icon placed directly after checkbox
            int icon_w = DesignSystem::ScaleForDpi(26, m.dpi);
            int icon_h = DesignSystem::ScaleForDpi(26, m.dpi);
            int icon_x = chk_rc.right + DesignSystem::ScaleForDpi(10, m.dpi);
            int icon_y = rrc.top + (rrc.bottom - rrc.top - icon_h) / 2;
            DrawFileTypeIconGdi(hdc, icon_x, icon_y, icon_w, icon_h, r, pal);

            SelectObject(hdc, hFontBold);
            const ColorRgba row_primary_text = pal.text_primary;
            SetTextColor(hdc, row_primary_text.ToWin32ColorRef());
            int name_right = std::min(rl.filename_rect.right, geom.table_columns[0].rect.right);
            
            // Clean row presentation according to Design Vision Part 3.E
            // Show inline progress bar ONLY when 0% < progress < 100%
            const bool show_progress_bar = (r.state != TaskState::kCompleted && r.progress_fraction > 0.001 && r.progress_fraction < 0.999);
            if (!show_progress_bar) {
                // Single-line centered vertically with high-contrast text, NO empty/finished progress bar
                int cy = rrc.top + (rrc.bottom - rrc.top) / 2;
                RECT fn_rc{icon_x + icon_w + DesignSystem::ScaleForDpi(12, m.dpi), cy - DesignSystem::ScaleForDpi(12, m.dpi),
                           std::max(icon_x + icon_w + DesignSystem::ScaleForDpi(12, m.dpi), name_right - DesignSystem::ScaleForDpi(10, m.dpi)),
                           cy + DesignSystem::ScaleForDpi(13, m.dpi)};
                std::wstring wfn = Utf8ToW(r.display_filename);
                if (fn_rc.right > fn_rc.left) {
                    std::wstring trunc_fn = MiddleTruncateFilename(hdc, wfn, fn_rc.right - fn_rc.left);
                    DrawTextW(hdc, trunc_fn.c_str(), -1, &fn_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
            } else {
                // In-Progress / Paused (>0%): Filename on top, sleek progress bar beneath with clean 6px+ gap
                RECT fn_rc{icon_x + icon_w + DesignSystem::ScaleForDpi(12, m.dpi), rrc.top + DesignSystem::ScaleForDpi(3, m.dpi),
                           std::max(icon_x + icon_w + DesignSystem::ScaleForDpi(12, m.dpi), name_right - DesignSystem::ScaleForDpi(10, m.dpi)),
                           rrc.top + DesignSystem::ScaleForDpi(25, m.dpi)};
                std::wstring wfn = Utf8ToW(r.display_filename);
                if (fn_rc.right > fn_rc.left) {
                    std::wstring trunc_fn = MiddleTruncateFilename(hdc, wfn, fn_rc.right - fn_rc.left);
                    DrawTextW(hdc, trunc_fn.c_str(), -1, &fn_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }

                int bar_x = fn_rc.left;
                int bar_y = rrc.top + DesignSystem::ScaleForDpi(38, m.dpi);
                int bar_max_w = std::min(DesignSystem::ScaleForDpi(220, m.dpi), std::max(0, static_cast<int>(fn_rc.right - fn_rc.left)));
                int bar_h = DesignSystem::ScaleForDpi(3, m.dpi);
                if (bar_max_w > 0) {
                    RECT track_rc{bar_x, bar_y, bar_x + bar_max_w, bar_y + bar_h};
                    DrawRoundedBoxGdi(hdc, track_rc, 2, pal.progress_track, pal.progress_track, 0);

                    auto& interp_map = view.GetAnimationState().progress_interpolated;
                    float actual_frac = static_cast<float>(std::clamp(r.progress_fraction, 0.0, 1.0));
                    float disp_frac = actual_frac;
                    if (view.GetAnimationState().IsActive()) {
                        auto it = interp_map.find(r.task_id);
                        if (it == interp_map.end()) {
                            interp_map[r.task_id] = actual_frac;
                            disp_frac = actual_frac;
                        } else {
                            float prev = it->second;
                            float next = prev + (actual_frac - prev) * 0.15f;
                            it->second = next;
                            disp_frac = next;
                        }
                    }
                    int fill_w = static_cast<int>(static_cast<double>(bar_max_w) * disp_frac);
                    if (fill_w > 0) {
                        ColorRgba fill_col = (r.state == TaskState::kDownloading)
                            ? pal.accent_primary
                            : ((pal.mode == ThemeMode::kLight) ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0xA1, 0xA1, 0xAA, 255});
                        RECT fill_rc{bar_x, bar_y, bar_x + fill_w, bar_y + bar_h};
                        DrawRoundedBoxGdi(hdc, fill_rc, 2, fill_col, fill_col, 0);
                    }
                }
            }

            // Column 1: Size
            if (geom.table_columns.size() > 1 && geom.table_columns[1].rect.width() > 0) {
                RECT sz_rc = ToWinRect(geom.table_columns[1].rect);
                sz_rc.top = rrc.top;
                sz_rc.bottom = rrc.bottom;
                sz_rc.right -= DesignSystem::ScaleForDpi(8, m.dpi);
                SelectObject(hdc, hFontBody);
                const bool is_unknown = (r.size_text.find("Unknown") != std::string::npos || r.size_text == "—");
                SetTextColor(hdc, is_unknown ? pal.text_muted.ToWin32ColorRef() : row_primary_text.ToWin32ColorRef());
                std::wstring wsz = is_unknown ? L"—" : Utf8ToW(r.size_text);
                DrawTextW(hdc, wsz.c_str(), -1, &sz_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Column 2: Status
            if (geom.table_columns.size() > 2 && geom.table_columns[2].rect.width() > 0) {
                RECT st_rc = ToWinRect(geom.table_columns[2].rect);
                st_rc.top = rrc.top;
                st_rc.bottom = rrc.bottom;

                int icon_sz = DesignSystem::ScaleForDpi(16, m.dpi);
                int icon_cy = (st_rc.top + st_rc.bottom) / 2;
                int icon_top = icon_cy - icon_sz / 2;
                int status_left = st_rc.left + DesignSystem::ScaleForDpi(16, m.dpi);
                RECT status_icon_rc{status_left, icon_top, status_left + icon_sz, icon_top + icon_sz};

                {
                    Gdiplus::Graphics gStat(hdc);
                    gStat.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                    Gdiplus::Pen pStat(Gdiplus::Color(r.status_indicator_color.a, r.status_indicator_color.r,
                                                      r.status_indicator_color.g, r.status_indicator_color.b), 1.8f);
                    pStat.SetStartCap(Gdiplus::LineCapRound);
                    pStat.SetEndCap(Gdiplus::LineCapRound);

                    float isz = static_cast<float>(icon_sz);
                    float ifx = static_cast<float>(status_icon_rc.left);
                    float ify = static_cast<float>(icon_top);
                    float icy = static_cast<float>(icon_cy);

                    if (r.state == TaskState::kCompleted) {
                        ColorRgba chkCol = (pal.mode == ThemeMode::kLight) ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0x71, 0x71, 0x7A, 255};
                        Gdiplus::Pen pChk(Gdiplus::Color(chkCol.a, chkCol.r, chkCol.g, chkCol.b), 1.8f);
                        pChk.SetStartCap(Gdiplus::LineCapRound);
                        pChk.SetEndCap(Gdiplus::LineCapRound);
                        Gdiplus::PointF chk_pts[3] = {
                            {ifx + 2.0f, icy + 0.5f},
                            {ifx + 5.5f, ify + isz - 3.5f},
                            {ifx + isz - 2.0f, ify + 3.0f}
                        };
                        gStat.DrawLines(&pChk, chk_pts, 3);
                    } else if (r.state == TaskState::kPaused) {
                        gStat.DrawEllipse(&pStat, ifx, ify, isz, isz);
                        gStat.DrawLine(&pStat, ifx + isz * 0.5f - 2.5f, ify + 4.0f, ifx + isz * 0.5f - 2.5f, ify + isz - 4.0f);
                        gStat.DrawLine(&pStat, ifx + isz * 0.5f + 2.5f, ify + 4.0f, ifx + isz * 0.5f + 2.5f, ify + isz - 4.0f);
                    } else {
                        float dot_r = static_cast<float>(DesignSystem::ScaleForDpi(4, m.dpi));
                        Gdiplus::SolidBrush bDot(Gdiplus::Color(r.status_indicator_color.a, r.status_indicator_color.r,
                                                               r.status_indicator_color.g, r.status_indicator_color.b));
                        gStat.FillEllipse(&bDot, ifx, icy - dot_r, dot_r * 2.0f, dot_r * 2.0f);
                    }
                }

                RECT st_txt_rc{status_left + icon_sz + 8, st_rc.top, st_rc.right - 4, st_rc.bottom};
                SelectObject(hdc, hFontBody);
                ColorRgba status_txt_col = (r.state == TaskState::kCompleted)
                    ? ((pal.mode == ThemeMode::kLight) ? ColorRgba{0x64, 0x74, 0x8B, 255} : ColorRgba{0x8E, 0x8E, 0x96, 255})
                    : r.status_indicator_color;
                SetTextColor(hdc, status_txt_col.ToWin32ColorRef());
                std::wstring wst = Utf8ToW(r.status_text);
                DrawTextW(hdc, wst.c_str(), -1, &st_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }

            // Column 3: Download rate
            if (geom.table_columns.size() > 3 && geom.table_columns[3].rect.width() > 0) {
                RECT dl_rc = ToWinRect(geom.table_columns[3].rect);
                dl_rc.top = rrc.top;
                dl_rc.bottom = rrc.bottom;
                dl_rc.right -= DesignSystem::ScaleForDpi(8, m.dpi);

                SelectObject(hdc, hFontBody);
                if (r.state == TaskState::kDownloading || r.state == TaskState::kProbing) {
                    SetTextColor(hdc, pal.accent_primary.ToWin32ColorRef());
                    std::wstring wspd = Utf8ToW(r.speed_text);
                    DrawTextW(hdc, wspd.c_str(), -1, &dl_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                } else {
                    SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
                    DrawTextW(hdc, L"—", -1, &dl_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                }
            }

            // Column 4: Upload rate
            if (geom.table_columns.size() > 4 && geom.table_columns[4].rect.width() > 0) {
                RECT ul_rc = ToWinRect(geom.table_columns[4].rect);
                ul_rc.top = rrc.top;
                ul_rc.bottom = rrc.bottom;
                const bool ul_is_outer = (geom.table_columns.size() <= 5 || geom.table_columns[5].rect.width() <= 0);
                ul_rc.right -= DesignSystem::ScaleForDpi(ul_is_outer ? 16 : 8, m.dpi);
                SelectObject(hdc, hFontBody);
                if (!r.upload_speed_text.empty() && r.upload_speed_text != "0 B/s") {
                    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
                    std::wstring wul = Utf8ToW(r.upload_speed_text);
                    DrawTextW(hdc, wul.c_str(), -1, &ul_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                } else {
                    SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
                    DrawTextW(hdc, L"—", -1, &ul_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                }
            }

            // Column 5: Added Timestamp
            if (geom.table_columns.size() > 5 && geom.table_columns[5].rect.width() > 0) {
                RECT add_time_rc = ToWinRect(geom.table_columns[5].rect);
                add_time_rc.top = rrc.top;
                add_time_rc.bottom = rrc.bottom;
                add_time_rc.right -= DesignSystem::ScaleForDpi(16, m.dpi);
                SelectObject(hdc, hFontBody);
                SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
                std::wstring wadd = Utf8ToW(r.added_time_text);
                DrawTextW(hdc, wadd.c_str(), -1, &add_time_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
        }
    }

    // 3.4 Custom Slim Fluent Scrollbar
    if (all_tasks.size() > visible_count) {
        int track_top = geom.table_body_rect.top + 4;
        int track_bottom = geom.table_body_rect.bottom - 4;
        int track_h = std::max(10, track_bottom - track_top);
        size_t max_offset = all_tasks.size() - visible_count;
        int thumb_h = std::max(24, static_cast<int>(track_h * visible_count / all_tasks.size()));
        int thumb_travel = track_h - thumb_h;
        int thumb_y = track_top + (thumb_travel > 0 ? static_cast<int>(thumb_travel * std::min(view.GetScrollOffset(), max_offset) / max_offset) : 0);

        RECT thumb_rc{geom.table_body_rect.right - 11, thumb_y, geom.table_body_rect.right - 5, thumb_y + thumb_h};
        bool active = (ctx && (ctx->is_dragging_vscroll || ctx->is_vscroll_hovered));
        ColorRgba thumb_col = active ? ColorRgba{255, 255, 255, 80} : ColorRgba{255, 255, 255, 38};
        DrawRoundedBoxGdi(hdc, thumb_rc, 3, thumb_col, thumb_col, 0);
    }

    // 3.5 Selected Download Details Inspector
    if (geom.details_panel_rect.height() > 0) {
        PaintDetailsPanelWin32(hdc, view, geom.details_panel_rect, hFontBody, hFontBold, hFontSmall);
    }

    // 4. Status Bar / Footer Glass Surface (Permanently removed)
    if (geom.status_bar_rect.height() > 0) {
    RECT status_rc = ToWinRect(geom.status_bar_rect);
    DrawGlassPanelGdi(hdc, status_rc, 0, pal.glass_surface, pal.glass_border, 1, false);

    RECT off_rc = ToWinRect(geom.status_toggle_off_rect);
    bool limiter = view.IsSpeedLimiterActive();
    ColorRgba off_bg = limiter ? pal.accent_primary : pal.surface_primary;
    DrawRoundedBoxGdi(hdc, off_rc, 10, off_bg, pal.border_subtle, 1);
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, limiter ? RGB(255, 255, 255) : pal.text_secondary.ToWin32ColorRef());
    DrawTextW(hdc, limiter ? L"ON" : L"OFF", -1, &off_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    int timer_r = DesignSystem::ScaleForDpi(8, m.dpi);
    int timer_cx = off_rc.right + DesignSystem::ScaleForDpi(20, m.dpi);
    int timer_cy = (status_rc.top + status_rc.bottom) / 2;
    DrawTimerClockGdi(hdc, timer_cx, timer_cy, timer_r, pal.text_secondary);

    auto sb = view.BuildStatusBar();

    // 4a. Download Speed
    SelectObject(hdc, hFontBody);
    RECT dl_stat_rc{timer_cx + timer_r * 2 + 8, status_rc.top, timer_cx + timer_r * 2 + DesignSystem::ScaleForDpi(110, m.dpi), status_rc.bottom};
    SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
    std::wstring wstat_dl = std::wstring(L"\x2913 ") + Utf8ToW(sb.formatted_total_speed);
    DrawTextW(hdc, wstat_dl.c_str(), -1, &dl_stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4b. Upload Speed
    RECT ul_stat_rc{dl_stat_rc.right + 10, status_rc.top, dl_stat_rc.right + DesignSystem::ScaleForDpi(100, m.dpi), status_rc.bottom};
    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
    std::wstring wstat_ul = std::wstring(L"\x2912 ") + Utf8ToW(sb.formatted_total_upload_speed);
    DrawTextW(hdc, wstat_ul.c_str(), -1, &ul_stat_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4c. Active & Queued Counts
    RECT counts_rc{ul_stat_rc.right + 12, status_rc.top, ul_stat_rc.right + DesignSystem::ScaleForDpi(145, m.dpi), status_rc.bottom};
    SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
    std::wstring wcounts = L"Active: " + std::to_wstring(sb.active_downloads) + L" | Queued: " + std::to_wstring(sb.remaining_queued);
    DrawTextW(hdc, wcounts.c_str(), -1, &counts_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int stat_r = DesignSystem::ScaleForDpi(4, m.dpi);

    // 4d. Browser Status Indicator & Label
    RECT brw_rc{counts_rc.right + 12, status_rc.top, counts_rc.right + DesignSystem::ScaleForDpi(125, m.dpi), status_rc.bottom};
    bool browser_active = sb.browser_ipc_listening && sb.browser_capture_enabled;
    ColorRgba brw_dot_color = browser_active ? ColorRgba(0x22, 0xC5, 0x5E) : (sb.browser_ipc_listening ? ColorRgba(0x3B, 0x82, 0xF6) : pal.text_muted);
    HPEN hBrwPen = CreatePen(PS_SOLID, 1, brw_dot_color.ToWin32ColorRef());
    HBRUSH hBrwBrush = CreateSolidBrush(brw_dot_color.ToWin32ColorRef());
    HGDIOBJ op_brw = SelectObject(hdc, hBrwPen);
    HGDIOBJ ob_brw = SelectObject(hdc, hBrwBrush);
    Ellipse(hdc, brw_rc.left, timer_cy - stat_r, brw_rc.left + stat_r * 2, timer_cy + stat_r);
    SelectObject(hdc, ob_brw);
    SelectObject(hdc, op_brw);
    DeleteObject(hBrwBrush);
    DeleteObject(hBrwPen);

    RECT brw_txt_rc{brw_rc.left + stat_r * 2 + 6, status_rc.top, brw_rc.right, status_rc.bottom};
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, browser_active ? pal.text_primary.ToWin32ColorRef() : pal.text_secondary.ToWin32ColorRef());
    std::wstring wbrw = browser_active ? L"Browser: Active" : (sb.browser_ipc_listening ? L"Browser: Connected" : L"Browser: Standby");
    DrawTextW(hdc, wbrw.c_str(), -1, &brw_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4e. Stream Workers Status Indicator & Label
    RECT wrk_rc{brw_rc.right + 12, status_rc.top, brw_rc.right + DesignSystem::ScaleForDpi(110, m.dpi), status_rc.bottom};
    ColorRgba wrk_dot_color = sb.workers_ready ? ColorRgba(0x22, 0xC5, 0x5E) : pal.accent_primary;
    HPEN hWrkPen = CreatePen(PS_SOLID, 1, wrk_dot_color.ToWin32ColorRef());
    HBRUSH hWrkBrush = CreateSolidBrush(wrk_dot_color.ToWin32ColorRef());
    HGDIOBJ op_wrk = SelectObject(hdc, hWrkPen);
    HGDIOBJ ob_wrk = SelectObject(hdc, hWrkBrush);
    Ellipse(hdc, wrk_rc.left, timer_cy - stat_r, wrk_rc.left + stat_r * 2, timer_cy + stat_r);
    SelectObject(hdc, ob_wrk);
    SelectObject(hdc, op_wrk);
    DeleteObject(hWrkBrush);
    DeleteObject(hWrkPen);

    RECT wrk_txt_rc{wrk_rc.left + stat_r * 2 + 6, status_rc.top, wrk_rc.right, status_rc.bottom};
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, sb.workers_ready ? pal.text_primary.ToWin32ColorRef() : pal.accent_primary.ToWin32ColorRef());
    std::wstring wwrk = sb.workers_ready ? L"Workers: Ready" : L"Workers: Missing";
    DrawTextW(hdc, wwrk.c_str(), -1, &wrk_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4f. Priority / Limiter State Indicator & Label
    RECT pri_rc{wrk_rc.right + 12, status_rc.top, wrk_rc.right + DesignSystem::ScaleForDpi(90, m.dpi), status_rc.bottom};
    ColorRgba dot_color = limiter ? pal.accent_primary : pal.surface_elevated;
    HPEN hDotPen = CreatePen(PS_SOLID, 1, dot_color.ToWin32ColorRef());
    HBRUSH hDotBrush = CreateSolidBrush(dot_color.ToWin32ColorRef());
    HGDIOBJ op = SelectObject(hdc, hDotPen);
    HGDIOBJ ob = SelectObject(hdc, hDotBrush);
    Ellipse(hdc, pri_rc.left, timer_cy - stat_r, pri_rc.left + stat_r * 2, timer_cy + stat_r);
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(hDotBrush);
    DeleteObject(hDotPen);

    RECT pri_txt_rc{pri_rc.left + stat_r * 2 + 6, status_rc.top, pri_rc.right, status_rc.bottom};
    SelectObject(hdc, hFontSmallBold);
    SetTextColor(hdc, limiter ? pal.text_primary.ToWin32ColorRef() : pal.text_secondary.ToWin32ColorRef());
    std::wstring wlimit = limiter ? L"LIMIT ON" : L"NORMAL";
    DrawTextW(hdc, wlimit.c_str(), -1, &pri_txt_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    // 4g. Active or Selected Filename (Right-aligned)
    if (!rows.empty()) {
        std::string right_fn;
        std::string sel_id = view.GetSelectedTaskId();
        if (!sel_id.empty()) {
            for (const auto& r : rows) {
                if (r.task_id == sel_id) {
                    right_fn = r.full_filename_tooltip;
                    break;
                }
            }
        }
        if (right_fn.empty()) {
            for (const auto& r : rows) {
                if (r.state == TaskState::kDownloading) {
                    right_fn = r.full_filename_tooltip;
                    break;
                }
            }
        }
        if (right_fn.empty()) {
            right_fn = rows[0].full_filename_tooltip;
        }

        int min_margin = DesignSystem::ScaleForDpi(24, m.dpi);
        int right_end = w - DesignSystem::ScaleForDpi(16, m.dpi);
        int left_start = pri_rc.right + min_margin;
        int avail_fn_w = right_end - left_start;
        if (avail_fn_w >= DesignSystem::ScaleForDpi(140, m.dpi)) {
            RECT rfn_rc{left_start, status_rc.top, right_end, status_rc.bottom};
            SelectObject(hdc, hFontBody);
            SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
            std::wstring wrfn = Utf8ToW(right_fn);
            DrawTextW(hdc, wrfn.c_str(), -1, &rfn_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }
    }
    }

    // 5. Settings Center Overlay (when active)
    if (view.IsSettingsCenterOpen()) {
        PaintSettingsCenterOverlayWin32(hdc, view, client_rc, hFontBody, hFontBold);
    }

    if (ctx && !ctx->conflict_dialog_task_id.empty()) {
        const ColorRgba scrim = (pal.mode == ThemeMode::kLight)
            ? ColorRgba{0x65, 0x6B, 0x73, 96} : ColorRgba{6, 6, 8, 210};
        FillRectSolid(hdc, client_rc, scrim);
        auto l = ComputeConflictDialogLayout(w, h, m.dpi);
        const ColorRgba modal_bg = pal.surface_primary;
        const ColorRgba modal_border = pal.border_subtle;
        const ColorRgba accent = pal.accent_primary;
        DrawRoundedBoxGdi(hdc, l.card, DesignSystem::ScaleForDpi(12, m.dpi), modal_bg, modal_border, 1);
        SelectObject(hdc, hFontBold);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        RECT title{l.card.left + DesignSystem::ScaleForDpi(18, m.dpi), l.card.top + DesignSystem::ScaleForDpi(12, m.dpi),
                   l.close.left - DesignSystem::ScaleForDpi(6, m.dpi), l.card.top + DesignSystem::ScaleForDpi(34, m.dpi)};
        DrawTextW(hdc, L"Warning: file exists already", -1, &title, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        DrawTextW(hdc, L"×", 1, &l.close, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, hFontSmall);
        SetTextColor(hdc, pal.text_primary.ToWin32ColorRef());
        DrawTextW(hdc, ctx->conflict_dialog_path.c_str(), static_cast<int>(ctx->conflict_dialog_path.size()), &l.path,
                  DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
        const int cb_sz = DesignSystem::ScaleForDpi(16, m.dpi);
        const int cb_top = l.checkbox.top + ((l.checkbox.bottom - l.checkbox.top - cb_sz) / 2);
        RECT check_box{l.checkbox.left, cb_top, l.checkbox.left + cb_sz, cb_top + cb_sz};
        DrawRoundedBoxGdi(hdc, check_box, DesignSystem::ScaleForDpi(4, m.dpi), modal_bg, modal_border, 1);
        if (ctx->conflict_dialog_remember) {
            HPEN pen = CreatePen(PS_SOLID, DesignSystem::ScaleForDpi(2, m.dpi), accent.ToWin32ColorRef());
            HGDIOBJ old_pen = SelectObject(hdc, pen);
            MoveToEx(hdc, check_box.left + DesignSystem::ScaleForDpi(3, m.dpi), check_box.top + DesignSystem::ScaleForDpi(8, m.dpi), nullptr);
            LineTo(hdc, check_box.left + DesignSystem::ScaleForDpi(6, m.dpi), check_box.top + DesignSystem::ScaleForDpi(11, m.dpi));
            LineTo(hdc, check_box.left + DesignSystem::ScaleForDpi(13, m.dpi), check_box.top + DesignSystem::ScaleForDpi(4, m.dpi));
            SelectObject(hdc, old_pen); DeleteObject(pen);
        }
        RECT check_label{check_box.right + DesignSystem::ScaleForDpi(8, m.dpi), l.checkbox.top,
                         l.checkbox.right, l.checkbox.bottom};
        SetTextColor(hdc, pal.text_secondary.ToWin32ColorRef());
        DrawTextW(hdc, L"Remember my choice for all downloads", -1, &check_label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        auto draw_button = [&](const RECT& rc, const wchar_t* label, ColorRgba bg, ColorRgba border, COLORREF text) {
            DrawRoundedBoxGdi(hdc, rc, DesignSystem::ScaleForDpi(6, m.dpi), bg, border, 1);
            SetTextColor(hdc, text);
            DrawTextW(hdc, label, -1, const_cast<RECT*>(&rc), DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        };
        SelectObject(hdc, hFontSmallBold);
        std::wstring rename = (ctx->conflict_dialog_processing && ctx->conflict_dialog_processing_choice == ConflictPolicy::kAutoRename)
            ? L"Applying..." : L"Rename";
        draw_button(l.rename, rename.c_str(), accent, accent, pal.text_primary.ToWin32ColorRef());
        const bool ovw_allowed = ctx->pending_conflicts.empty() || ctx->pending_conflicts.front().report.overwrite_allowed;
        std::wstring overwrite_lbl = (ctx->conflict_dialog_processing && ctx->conflict_dialog_processing_choice == ConflictPolicy::kOverwrite)
            ? L"Applying..." : L"Overwrite";
        draw_button(l.overwrite, overwrite_lbl.c_str(),
                    ovw_allowed ? pal.surface_elevated : pal.surface_primary,
                    modal_border,
                    ovw_allowed ? pal.text_primary.ToWin32ColorRef() : pal.text_muted.ToWin32ColorRef());
        draw_button(l.abort, L"Abort", pal.surface_elevated, modal_border, pal.text_primary.ToWin32ColorRef());
        if (!ctx->conflict_validation_error.empty()) {
            SelectObject(hdc, hFontSmall);
            SetTextColor(hdc, RGB(239, 68, 68));
            RECT err_rc{l.card.left + DesignSystem::ScaleForDpi(18, m.dpi), l.rename_edit.top - DesignSystem::ScaleForDpi(16, m.dpi),
                        l.card.right - DesignSystem::ScaleForDpi(18, m.dpi), l.rename_edit.top - DesignSystem::ScaleForDpi(2, m.dpi)};
            std::wstring werr = Utf8ToW(ctx->conflict_validation_error);
            DrawTextW(hdc, werr.c_str(), static_cast<int>(werr.size()), &err_rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        if (ctx->hConflictRenameEdit && IsWindow(ctx->hConflictRenameEdit)) {
            MoveWindow(ctx->hConflictRenameEdit, l.rename_edit.left, l.rename_edit.top,
                       l.rename_edit.right - l.rename_edit.left, l.rename_edit.bottom - l.rename_edit.top, FALSE);
            ShowWindow(ctx->hConflictRenameEdit, SW_SHOW);
        }
    }

    SelectObject(hdc, old_font);
    DeleteObject(hFontTitle);
    DeleteObject(hFontBody);
    DeleteObject(hFontBold);
    DeleteObject(hFontSmall);
    DeleteObject(hFontSmallBold);

    SetLayout(hdc, 0);
    BitBlt(hdc_screen, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, old_bm);
    DeleteObject(hbm);
    DeleteDC(hdc);
    EndPaint(hwnd, &ps);
}

static void InitWin32Theme(HWND hwnd, const ThemePalette& palette) {
    ApplyWindowGlassBackdropWin32(hwnd, palette, false /* is_transient_dialog */);
    ApplyNativeWindowTheme(hwnd, palette);
    HMODULE hUxTheme = GetUxThemeModule();
    if (hUxTheme) {
        using fnFlushMenuThemes = void(WINAPI*)();
        auto pfnFlushMenuThemes = reinterpret_cast<fnFlushMenuThemes>(GetProcAddress(hUxTheme, MAKEINTRESOURCEA(136)));
        if (pfnFlushMenuThemes) {
            pfnFlushMenuThemes();
        }
    }
}

LRESULT CALLBACK SearchEditSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<Win32WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ctx || !ctx->orig_search_proc) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    LRESULT res = CallWindowProcW(ctx->orig_search_proc, hwnd, msg, wParam, lParam);
    if (msg == WM_PAINT && GetWindowTextLengthW(hwnd) == 0 && ctx->view) {
        HDC hdc = GetDC(hwnd);
        if (hdc) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const auto& pal = ctx->view->GetPalette();
            HFONT hFont = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
            HGDIOBJ old_f = hFont ? SelectObject(hdc, hFont) : nullptr;
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, pal.text_muted.ToWin32ColorRef());
            const wchar_t* cue = (rc.right - rc.left < DesignSystem::ScaleForDpi(150, ctx->view->GetMetrics().dpi))
                ? L"Search..."
                : L"Search downloads...";
            DrawTextW(hdc, cue, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            if (old_f) SelectObject(hdc, old_f);
            ReleaseDC(hwnd, hdc);
        }
    } else if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS) {
        InvalidateRect(hwnd, nullptr, FALSE);
    }
    return res;
}

static void SyncSearchEditVisibility(Win32WindowContext* ctx) {
    if (!ctx || !ctx->view || !ctx->hSearchEdit) return;
    const bool should_hide = ctx->view->IsSettingsCenterOpen() || !ctx->conflict_dialog_task_id.empty();
    const bool is_visible = (IsWindowVisible(ctx->hSearchEdit) != FALSE);
    if (should_hide && is_visible) {
        ShowWindow(ctx->hSearchEdit, SW_HIDE);
        EnableWindow(ctx->hSearchEdit, FALSE);
    } else if (!should_hide && !is_visible) {
        ShowWindow(ctx->hSearchEdit, SW_SHOW);
        EnableWindow(ctx->hSearchEdit, TRUE);
    }
}

static void SyncSearchEditLayout(Win32WindowContext* ctx) {
    if (!ctx || !ctx->view || !ctx->hSearchEdit) return;
    SyncSearchEditVisibility(ctx);
    auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
    const int dpi = ctx->view->GetMetrics().dpi;
    const int edit_pad = DesignSystem::ScaleForDpi(32, dpi);
    const int outer_pad = DesignSystem::ScaleForDpi(14, dpi);
    const int edit_h = DesignSystem::ScaleForDpi(22, dpi);
    const int edit_y = geom.search_box_rect.top + (geom.search_box_rect.height() - edit_h) / 2;
    const int edit_x = geom.search_box_rect.left + edit_pad;
    const int edit_w = std::max(10, geom.search_box_rect.width() - edit_pad - outer_pad);
    MoveWindow(ctx->hSearchEdit, edit_x, edit_y, edit_w, edit_h, TRUE);
}

void ExecuteContextCommandWin32(HWND hwnd, Win32WindowContext* ctx, int cmd, std::optional<TaskModel> sel_task) {
    if (!ctx || !ctx->view || !ctx->controller) return;
    const bool has_sel = sel_task.has_value();
    const std::string sel = has_sel ? sel_task->task_id : "";
            if (cmd == IDM_CTX_START) ctx->view->ActionStartSelected();
            else if (cmd == IDM_CTX_PAUSE) ctx->view->ActionPauseSelected();
            else if (cmd == IDM_CTX_RESUME) ctx->view->ActionResumeSelected();
            else if (cmd == IDM_CTX_RETRY) ctx->view->ActionRetrySelected();
            else if (cmd == IDM_CTX_CANCEL) ctx->view->ActionCancelSelected();
            else if (cmd == IDM_CTX_OPEN_FILE) ctx->view->ActionOpenCompletedFile(false);
            else if (cmd == IDM_CTX_OPEN_FOLDER) ctx->view->ActionOpenDestinationFolder(false);
            else if (cmd == IDM_CTX_PRIORITY_HIGH || cmd == IDM_CTX_PRIORITY_NORMAL || cmd == IDM_CTX_PRIORITY_LOW) {
                if (has_sel) {
                    TaskPriority new_prio = TaskPriority::kNormal;
                    if (cmd == IDM_CTX_PRIORITY_HIGH) new_prio = TaskPriority::kHigh;
                    else if (cmd == IDM_CTX_PRIORITY_LOW) new_prio = TaskPriority::kLow;
                    auto r = ctx->controller->SetTaskPriority(sel, new_prio);
                    if (r.ok()) {
                        ctx->view->SetFeedbackMessage("Priority set to " + TaskPriorityDisplayName(new_prio));
                    } else {
                        MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Set Priority Failed", MB_OK | MB_ICONWARNING);
                    }
                }
            }
            else if (cmd == IDM_CTX_RENAME) {
                if (has_sel) {
                    std::wstring new_name;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Rename Download", L"Enter new file name:", Utf8ToW(sel_task->file_name), new_name)) {
                        std::string clean_name = PathSanitizer::SanitizeFileName(WToUtf8(new_name));
                        if (!clean_name.empty()) {
                            std::filesystem::path old_p = std::filesystem::path(sel_task->save_dir) / sel_task->file_name;
                            std::filesystem::path new_p = std::filesystem::path(sel_task->save_dir) / clean_name;
                            std::error_code ec;
                            if (std::filesystem::exists(old_p, ec)) {
                                std::filesystem::rename(old_p, new_p, ec);
                            }
                            sel_task->file_name = clean_name;
                            ctx->controller->UpdateTask(*sel_task);
                            ctx->view->SetFeedbackMessage("Renamed download to " + clean_name);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_MOVE_TO) {
                if (has_sel) {
                    std::string chosen_dir = sel_task->save_dir;
                    if (SelectFolderWithDialogWin32(hwnd, chosen_dir) && chosen_dir != sel_task->save_dir) {
                        std::filesystem::path old_p = std::filesystem::path(sel_task->save_dir) / sel_task->file_name;
                        std::filesystem::path new_p = std::filesystem::path(chosen_dir) / sel_task->file_name;
                        std::error_code ec;
                        if (std::filesystem::exists(old_p, ec)) {
                            std::filesystem::rename(old_p, new_p, ec);
                        }
                        sel_task->save_dir = chosen_dir;
                        ctx->controller->UpdateTask(*sel_task);
                        ctx->view->SetFeedbackMessage("Moved download to " + chosen_dir);
                    }
                }
            } else if (cmd == IDM_CTX_COPY_URL) {
                if (has_sel) {
                    CopyTextToClipboardWin32(hwnd, sel_task->url);
                    ctx->view->SetFeedbackMessage("Copied download URL to clipboard");
                }
            } else if (cmd == IDM_CTX_REFRESH_URL) {
                if (has_sel) {
                    std::wstring new_url_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Refresh / Change URL", L"Enter updated download URL (e.g. refreshed signed link):", Utf8ToW(sel_task->url), new_url_w)) {
                        std::string new_u = WToUtf8(new_url_w);
                        while (!new_u.empty() && (new_u.front() == ' ' || new_u.front() == '\t')) new_u.erase(0, 1);
                        while (!new_u.empty() && (new_u.back() == ' ' || new_u.back() == '\t')) new_u.pop_back();
                        if (!new_u.empty()) {
                            auto r = ctx->controller->RefreshTaskUrl(sel, new_u);
                            if (r.ok()) {
                                ctx->view->SetFeedbackMessage("Updated download URL");
                            } else {
                                MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Failed to Update URL", MB_OK | MB_ICONERROR);
                            }
                        }
                    }
                }
            } else if (cmd == IDM_CTX_ADD_MIRROR) {
                if (has_sel) {
                    std::wstring mirror_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Add Mirror URL", L"Enter alternate download mirror URL:", L"", mirror_w)) {
                        std::string mir = WToUtf8(mirror_w);
                        while (!mir.empty() && (mir.front() == ' ' || mir.front() == '\t')) mir.erase(0, 1);
                        while (!mir.empty() && (mir.back() == ' ' || mir.back() == '\t')) mir.pop_back();
                        if (!mir.empty()) {
                            auto r = ctx->controller->AddAlternateUrl(sel, mir);
                            if (r.ok()) {
                                ctx->view->SetFeedbackMessage("Added alternate mirror URL");
                            } else {
                                MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Failed to Add Mirror", MB_OK | MB_ICONERROR);
                            }
                        }
                    }
                }
            } else if (cmd == IDM_CTX_SPEED_LIMIT) {
                if (has_sel) {
                    uint64_t cur_limit = sel_task->speed_limit_bps;
                    std::wstring init_val = (cur_limit > 0) ? std::to_wstring(cur_limit / 1024) : L"0";
                    std::wstring val_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Set Download Speed Limit", L"Enter speed limit in KB/s (0 = Unlimited):", init_val, val_w)) {
                        std::string val_str = WToUtf8(val_w);
                        try {
                            uint64_t kbps = std::stoull(val_str);
                            uint64_t bps = kbps * 1024ULL;
                            auto r = ctx->controller->SetTaskSpeedLimit(sel, bps);
                            if (r.ok()) {
                                ctx->view->SetFeedbackMessage((bps == 0) ? "Task speed limit set to Unlimited" : ("Task speed limit set to " + DesktopView::FormatSpeed(bps)));
                            } else {
                                MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Speed Limit Failed", MB_OK | MB_ICONWARNING);
                            }
                        } catch (...) {
                            MessageBoxW(hwnd, L"Invalid speed limit number entered.", L"Invalid Input", MB_OK | MB_ICONWARNING);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_SCHEDULE) {
                if (has_sel) {
                    std::wstring init_t = sel_task->schedule_time_local.empty() ? L"23:00" : Utf8ToW(sel_task->schedule_time_local);
                    std::wstring time_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Schedule Download", L"Enter scheduled start time (24-hour HH:MM format, e.g. 23:00):", init_t, time_w)) {
                        std::string t_str = WToUtf8(time_w);
                        auto r = ctx->controller->SetTaskSchedule(sel, t_str, "");
                        if (r.ok()) {
                            ctx->view->SetFeedbackMessage("Scheduled download for " + t_str);
                        } else {
                            MessageBoxW(hwnd, Utf8ToW(r.error->message).c_str(), L"Schedule Failed", MB_OK | MB_ICONWARNING);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_FILE_INTEGRITY) {
                if (has_sel) {
                    std::filesystem::path fp = std::filesystem::path(sel_task->save_dir) / sel_task->file_name;
                    std::error_code ec;
                    if (!std::filesystem::exists(fp, ec)) {
                        MessageBoxW(hwnd, L"Completed file does not exist on disk.", L"File Missing", MB_OK | MB_ICONWARNING);
                    } else {
                        auto sha_res = CryptoUtils::ComputeFileSha256Hex(fp);
                        if (sha_res.ok()) {
                            std::string hash = *sha_res.value;
                            std::wstring msg_text = L"File: " + Utf8ToW(sel_task->file_name) + L"\n"
                                                  + L"SHA-256 Checksum:\n" + Utf8ToW(hash) + L"\n\n"
                                                  + L"Copy checksum to clipboard?";
                            if (MessageBoxW(hwnd, msg_text.c_str(), L"File Integrity (SHA-256)", MB_YESNO | MB_ICONINFORMATION) == IDYES) {
                                CopyTextToClipboardWin32(hwnd, hash);
                                ctx->view->SetFeedbackMessage("Copied SHA-256 hash to clipboard");
                            }
                        } else {
                            MessageBoxW(hwnd, Utf8ToW(sha_res.error->message).c_str(), L"Checksum Computation Error", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_REMOVE_LIST) {
                ctx->view->ActionRemoveSelectedFromList();
            } else if (cmd == IDM_CTX_DELETE_DISK) {
                ShowDeleteConfirmationDialogWin32(hwnd, *ctx->view, *ctx->controller);
            } else if (cmd == IDM_CTX_ANTIVIRUS) {
                if (!sel.empty()) {
                    auto rep = ctx->controller->ScanTaskFile(sel);
                    if (rep.ok()) {
                        std::wstring alert_info;
                        UINT icon = MB_ICONINFORMATION;
                        if (rep.value->status == security::ScanStatus::kClean) {
                            alert_info = L"Antivirus scan completed: Verified clean. No threats were found.";
                            icon = MB_ICONINFORMATION;
                        } else if (rep.value->status == security::ScanStatus::kThreatDetected) {
                            alert_info = L"SECURITY WARNING: A threat was detected by " +
                                std::wstring(rep.value->scanner_name.begin(), rep.value->scanner_name.end()) +
                                L"!\nAutomatic execution of this file is blocked.";
                            icon = MB_ICONSTOP;
                        } else if (rep.value->status == security::ScanStatus::kScannerError) {
                            alert_info = L"Antivirus scan failed: The scanner encountered an error during execution.";
                            icon = MB_ICONWARNING;
                        } else if (rep.value->status == security::ScanStatus::kUnavailable) {
                            alert_info = L"Antivirus scanner is unavailable on this system.";
                            icon = MB_ICONWARNING;
                        } else {
                            alert_info = L"Antivirus scan was skipped or not performed.";
                            icon = MB_ICONWARNING;
                        }
                        MessageBoxW(hwnd, alert_info.c_str(), L"Antivirus Scan Result", MB_OK | icon);
                    } else {
                        std::wstring err_msg = L"Failed to initiate antivirus scan: " +
                            std::wstring(rep.error->message.begin(), rep.error->message.end());
                        MessageBoxW(hwnd, err_msg.c_str(), L"Antivirus Scan Error", MB_OK | MB_ICONERROR);
                    }
                }
            } else if (cmd == IDM_CTX_ZIP_PREVIEW) {
                if (!sel.empty()) {
                    auto t = ctx->controller->GetTask(sel);
                    if (t.has_value()) {
                        std::filesystem::path fp = std::filesystem::path(t->save_dir) / t->file_name;
                        auto preview_res = ctx->controller->PreviewZipArchive(fp);
                        if (preview_res.ok()) {
                            std::wstring preview_info = L"Archive: " + fp.filename().wstring() + L"\n" +
                                                        L"Files: " + std::to_wstring(preview_res.value->total_files) + L"\n" +
                                                        L"Folders: " + std::to_wstring(preview_res.value->total_folders) + L"\n" +
                                                        L"Total Uncompressed: " + std::to_wstring(preview_res.value->total_uncompressed_bytes / 1024) + L" KB\n" +
                                                        (preview_res.value->is_potential_zip_bomb ? L"[ALERT: High compression ratio (Zip Bomb Risk)]\n" : L"") +
                                                        (preview_res.value->has_zip_slip_risk ? L"[ALERT: Suspicious paths (Zip Slip Risk)]\n" : L"");
                            MessageBoxW(hwnd, preview_info.c_str(), L"ZIP Archive Preview", MB_OK | MB_ICONINFORMATION);
                        } else {
                            MessageBoxW(hwnd, L"Could not read ZIP Central Directory (file may not be a valid ZIP archive).", L"ZIP Preview Error", MB_OK | MB_ICONEXCLAMATION);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_EXPORT_TASKS) {
                auto exp_str = ctx->controller->ExportTasksData(storage::ExportFormat::kJson);
                if (exp_str.ok()) {
                    std::filesystem::path exp_path = PathSanitizer::GetDefaultDownloadDir() / "radium_tasks_export.json";
                    std::ofstream ofs(exp_path);
                    if (ofs) {
                        ofs << exp_str.value.value();
                        std::wstring export_info = L"Tasks successfully exported to:\n" + exp_path.wstring();
                        MessageBoxW(hwnd, export_info.c_str(), L"Export Tasks", MB_OK | MB_ICONINFORMATION);
                    }
                }
            } else if (cmd == IDM_CTX_OPEN_TORRENT) {
                wchar_t szFile[MAX_PATH] = L"";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
                ofn.lpstrFilter = L"Torrent Files (*.torrent)\0*.torrent\0All Files (*.*)\0*.*\0\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::string tor_path = WToUtf8(szFile);
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(tor_path);
                    if (parsed.ok()) {
                        auto tinfo = std::move(*parsed.value);
                        std::string default_dir = ctx->controller->GetSettings().default_download_dir;
                        bool start_torrent = true;
                        if (ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo, tor_path, default_dir,
                                                      start_torrent, &default_dir, &start_torrent)) {
                            TaskModel t;
                            t.task_type = TaskType::kTorrent;
                            t.url = tor_path;
                            t.file_name = tinfo.name.empty() ? PathSanitizer::PathToUtf8(std::filesystem::path(tor_path).filename()) : tinfo.name;
                            t.save_dir = default_dir;
                            t.total_bytes = tinfo.selected_bytes();
                            for (const auto& file : tinfo.files) {
                                t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                            }
                            auto res = ctx->controller->AddTask(std::move(t), start_torrent, false);
                            if (res.ok()) {
                                ctx->view->SelectTask(res.value->task_id);
                                ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                            }
                        }
                    } else {
                        std::wstring err = L"Failed to parse torrent file:\n" + Utf8ToW(parsed.error->message);
                        MessageBoxW(hwnd, err.c_str(), L"Invalid Torrent", MB_OK | MB_ICONERROR);
                    }
                }
            } else if (cmd == IDM_CTX_TORRENT_SWARM) {
                if (has_sel) {
                    auto swarm = ctx->controller->GetTorrentSwarmDetails(sel);
                    if (swarm.ok() && swarm.value) {
                        std::wostringstream wss;
                        wss << L"Active Peers: " << swarm.value->active_peer_count
                            << L"\nSeeds: " << swarm.value->total_seeds
                            << L" | Leechers: " << swarm.value->total_leechers
                            << L"\nSequential Mode: " << (swarm.value->sequential_mode ? L"ON" : L"OFF")
                            << L"\nTrackers (" << swarm.value->trackers.size() << L"):";
                        for (size_t i = 0; i < (std::min<size_t>)(5, swarm.value->trackers.size()); ++i) {
                            wss << L"\n - " << Utf8ToW(swarm.value->trackers[i].url)
                                << L" [" << Utf8ToW(swarm.value->trackers[i].status) << L"]";
                        }
                        MessageBoxW(hwnd, wss.str().c_str(), L"Torrent Swarm Details", MB_OK | MB_ICONINFORMATION);
                    } else {
                        MessageBoxW(hwnd, L"Torrent task is not currently active in the swarm.", L"Torrent Swarm", MB_OK | MB_ICONINFORMATION);
                    }
                }
            } else if (cmd == IDM_CTX_TORRENT_ADD_TRACKER) {
                if (has_sel) {
                    std::wstring tr_w;
                    if (ShowSimpleInputDialogWin32(hwnd, L"Add Torrent Tracker", L"Enter tracker URL (http/udp):", L"", tr_w)) {
                        std::string tr_url = WToUtf8(tr_w);
                        if (!tr_url.empty()) {
                            auto res = ctx->controller->AddTrackersToTask(sel, {tr_url});
                            if (!res.ok() && res.error) {
                                MessageBoxW(hwnd, Utf8ToW(res.error->message).c_str(), L"Add Tracker", MB_OK | MB_ICONWARNING);
                            }
                        }
                    }
                }
            } else if (cmd == IDM_CTX_TORRENT_SEQUENTIAL) {
                if (has_sel) {
                    auto swarm = ctx->controller->GetTorrentSwarmDetails(sel);
                    bool next_seq = (swarm.ok() && swarm.value) ? !swarm.value->sequential_mode : true;
                    auto res = ctx->controller->SetTorrentSequentialMode(sel, next_seq);
                    if (res.ok()) {
                        MessageBoxW(hwnd, next_seq ? L"Sequential torrent mode enabled." : L"Sequential torrent mode disabled.", L"Sequential Mode", MB_OK | MB_ICONINFORMATION);
                    } else if (res.error) {
                        MessageBoxW(hwnd, Utf8ToW(res.error->message).c_str(), L"Sequential Mode", MB_OK | MB_ICONWARNING);
                    }
                }
            } else if (cmd == IDM_CTX_CREATE_TORRENT) {
                wchar_t srcFile[MAX_PATH] = L"";
                OPENFILENAMEW ofnSrc{};
                ofnSrc.lStructSize = sizeof(ofnSrc);
                ofnSrc.hwndOwner = hwnd;
                ofnSrc.lpstrFile = srcFile;
                ofnSrc.nMaxFile = MAX_PATH;
                ofnSrc.lpstrFilter = L"All Files (*.*)\0*.*\0";
                ofnSrc.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                if (GetOpenFileNameW(&ofnSrc)) {
                    wchar_t outFile[MAX_PATH] = L"output.torrent";
                    OPENFILENAMEW ofnOut{};
                    ofnOut.lStructSize = sizeof(ofnOut);
                    ofnOut.hwndOwner = hwnd;
                    ofnOut.lpstrFile = outFile;
                    ofnOut.nMaxFile = MAX_PATH;
                    ofnOut.lpstrFilter = L"Torrent Files (*.torrent)\0*.torrent\0";
                    ofnOut.lpstrDefExt = L"torrent";
                    ofnOut.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
                    if (GetSaveFileNameW(&ofnOut)) {
                        auto res = ctx->controller->CreateTorrentFile(std::filesystem::path(srcFile), std::filesystem::path(outFile));
                        if (res.ok()) {
                            MessageBoxW(hwnd, L"Torrent file created successfully.", L"Create Torrent", MB_OK | MB_ICONINFORMATION);
                        } else if (res.error) {
                            MessageBoxW(hwnd, Utf8ToW(res.error->message).c_str(), L"Create Torrent", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_SITE_GRABBER) {
                std::string start_url = PromptTextModalWin32(hwnd, L"Site Grabber", L"Start URL (http/https):", "", ctx->view->GetPalette());
                if (!start_url.empty()) {
                    crawler::SiteGrabberConfig scfg;
                    scfg.start_url = start_url;
                    scfg.max_depth = 2;
                    scfg.max_pages = 50;
                    std::atomic<bool> cancel_crawl{false};
                    auto crawl_res = ctx->view->ActionLaunchSiteGrabber(scfg, cancel_crawl);
                    if (crawl_res.ok() && crawl_res.value) {
                        auto groups = crawler::SiteGrabber::GroupGrabbedItems(*crawl_res.value);
                        std::vector<std::string> batch_urls;
                        for (const auto& grp : groups) {
                            for (const auto& item : grp.items) {
                                batch_urls.push_back(item.url);
                            }
                        }
                        if (!batch_urls.empty()) {
                            ctx->controller->AddBatchTasks(batch_urls, start_url, {}, {});
                        }
                        std::wstring info_msg = L"Site Grabber found " + std::to_wstring(crawl_res.value->size()) +
                                                L" item(s) across " + std::to_wstring(groups.size()) + L" group(s) (including split-volume packages).";
                        MessageBoxW(hwnd, info_msg.c_str(), L"Site Grabber", MB_OK | MB_ICONINFORMATION);
                    } else if (crawl_res.error) {
                        MessageBoxW(hwnd, Utf8ToW(crawl_res.error->message).c_str(), L"Site Grabber Error", MB_OK | MB_ICONWARNING);
                    }
                }
            } else if (cmd == IDM_CTX_BATCH_URL) {
                std::string tmpl = PromptTextModalWin32(hwnd, L"Batch URL Template", L"URL Template (e.g. https://host/file_[01-05].zip or /{a,b}/):", "", ctx->view->GetPalette());
                if (!tmpl.empty()) {
                    auto urls = crawler::SiteGrabber::ExpandBatchUrlTemplate(tmpl);
                    if (!urls.empty()) {
                        ctx->controller->AddBatchTasks(urls, "", {}, {});
                    }
                    std::wstring info_msg = L"Queued " + std::to_wstring(urls.size()) + L" URL(s) from template.";
                    MessageBoxW(hwnd, info_msg.c_str(), L"Batch URL Expander", MB_OK | MB_ICONINFORMATION);
                }
            } else if (cmd == IDM_CTX_QUEUE_MANAGER) {
                auto queues = ctx->controller->ListQueues();
                std::wostringstream wss;
                wss << L"Configured Queues (" << queues.size() << L"):\n";
                for (const auto& q : queues) {
                    wss << L" - " << Utf8ToW(q.name) << L" [" << Utf8ToW(q.queue_id)
                        << L"] | Max Concurrent: " << q.max_concurrent_tasks
                        << L" | Schedule: " << (q.schedule_enabled ? L"ON" : L"OFF");
                    if (!q.start_time_local.empty() || !q.stop_time_local.empty()) {
                        wss << L" (" << Utf8ToW(q.start_time_local) << L" - " << Utf8ToW(q.stop_time_local) << L")";
                    }
                    wss << L"\n";
                }
                wss << L"\nEnter a new Queue ID to create/update (or Cancel):";
                std::string qname = PromptTextModalWin32(hwnd, L"Multi-Queue Manager", wss.str(), "", ctx->view->GetPalette());
                if (!qname.empty()) {
                    QueueConfig qc;
                    qc.queue_id = PathSanitizer::SanitizeFileName(qname);
                    qc.name = qname;
                    qc.max_concurrent_tasks = 2;
                    ctx->controller->UpsertQueue(qc);
                    MessageBoxW(hwnd, L"Queue saved.", L"Multi-Queue Manager", MB_OK | MB_ICONINFORMATION);
                }
            } else if (cmd == IDM_CTX_DROP_TARGET) {
                if (ctx->hDropTargetWnd && IsWindow(ctx->hDropTargetWnd)) {
                    ShowWindow(ctx->hDropTargetWnd, IsWindowVisible(ctx->hDropTargetWnd) ? SW_HIDE : SW_SHOW);
                } else {
                    ctx->hDropTargetWnd = CreateWindowExW(
                        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
                        L"STATIC", L"Radium Drop Target\n(Drop URL or .torrent)",
                        WS_POPUP | WS_BORDER | WS_VISIBLE | SS_CENTER,
                        120, 120, 180, 56, hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
                    if (ctx->hDropTargetWnd) {
                        platform::WindowsIntegrations::RegisterWindowDropTarget(ctx->hDropTargetWnd, [hwnd, ctx](const std::string& dropped) {
                            TaskModel t;
                            t.url = dropped;
                            ctx->controller->AddTask(std::move(t), true);
                            InvalidateRect(hwnd, nullptr, FALSE);
                        });
                    }
                }
            } else if (cmd == IDM_CTX_TRAY_PROGRESS) {
                auto tasks = ctx->controller->ListTasks();
                size_t active_n = 0;
                uint64_t total_bps = 0;
                std::wostringstream wss;
                for (const auto& t : tasks) {
                    if (t.state == TaskState::kDownloading || t.state == TaskState::kProbing || t.state == TaskState::kMuxing) {
                        ++active_n;
                        total_bps += t.speed_bps;
                        wss << L" - " << Utf8ToW(t.file_name) << L" (" << t.downloaded_bytes << L"/" << t.total_bytes << L" B)\n";
                    }
                }
                std::wstring hdr = L"Active Transfers: " + std::to_wstring(active_n) + L" (" + std::to_wstring(total_bps / 1024) + L" KiB/s)\n";
                MessageBoxW(hwnd, (hdr + wss.str()).c_str(), L"Radium Transfer Panel", MB_OK | MB_ICONINFORMATION);
            } else if (cmd == IDM_CTX_EXPORT_EF2) {
                wchar_t szFile[MAX_PATH] = L"radium_tasks.ef2";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"IDM Export File (*.ef2)\0*.ef2\0All Files (*.*)\0*.*\0";
                ofn.lpstrDefExt = L"ef2";
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameW(&ofn)) {
                    auto tasks = ctx->controller->ListTasks();
                    auto res = storage::TaskImportExport::ExportTasksToFile(tasks, std::filesystem::path(szFile), storage::ExportFormat::kEf2, false);
                    MessageBoxW(hwnd, res.ok() ? L"Exported .ef2 successfully." : Utf8ToW(res.error->message).c_str(), L"Export .ef2", MB_OK | (res.ok() ? MB_ICONINFORMATION : MB_ICONERROR));
                }
            } else if (cmd == IDM_CTX_IMPORT_EF2) {
                wchar_t szFile[MAX_PATH] = L"";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"IDM Export File (*.ef2)\0*.ef2\0All Files (*.*)\0*.*\0";
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::ifstream ifs(std::filesystem::path(szFile), std::ios::binary);
                    if (ifs) {
                        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                        auto imp = storage::TaskImportExport::ImportFromEf2(content);
                        if (imp.ok() && imp.value) {
                            std::vector<std::string> urls;
                            urls.reserve(imp.value->size());
                            for (const auto& t : *imp.value) urls.push_back(t.url);
                            if (!urls.empty()) ctx->controller->AddBatchTasks(urls, "", {}, {});
                            std::wstring info_msg = L"Imported " + std::to_wstring(imp.value->size()) + L" task(s) from .ef2.";
                            MessageBoxW(hwnd, info_msg.c_str(), L"Import .ef2", MB_OK | MB_ICONINFORMATION);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_IMPORT_TASKS) {
                wchar_t szFile[MAX_PATH] = L"";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = sizeof(szFile) / sizeof(szFile[0]);
                ofn.lpstrFilter = L"Task Files (*.json;*.csv;*.txt)\0*.json;*.csv;*.txt\0JSON Files (*.json)\0*.json\0CSV / Text Files (*.csv;*.txt)\0*.csv;*.txt\0All Files (*.*)\0*.*\0";
                ofn.nFilterIndex = 1;
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) {
                    std::filesystem::path imp_path(szFile);
                    std::ifstream ifs(imp_path, std::ios::binary);
                    if (ifs) {
                        std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
                        storage::ExportFormat fmt = (imp_path.extension() == ".json") ? storage::ExportFormat::kJson : storage::ExportFormat::kCsv;
                        auto imp_res = ctx->controller->ImportTasksData(content, fmt);
                        if (imp_res.ok()) {
                            std::wstring ok_msg = L"Successfully imported " + std::to_wstring(*imp_res.value) + L" task(s).";
                            MessageBoxW(hwnd, ok_msg.c_str(), L"Import Tasks", MB_OK | MB_ICONINFORMATION);
                        } else {
                            std::wstring err = L"Failed to import tasks: " + Utf8ToW(imp_res.error->message);
                            MessageBoxW(hwnd, err.c_str(), L"Import Error", MB_OK | MB_ICONERROR);
                        }
                    }
                }
            } else if (cmd == IDM_CTX_SETTINGS) ctx->view->OpenSettingsCenter(SettingsCategory::kGeneral);
            else if (cmd == IDM_CTX_BROWSER_STAT) ctx->view->OpenSettingsCenter(SettingsCategory::kBrowserIntegration);
            else if (cmd == IDM_CTX_STARTUP_LOG) ShowStartupLogDialogWin32(hwnd, *ctx->controller, ctx->view->GetPalette());
            else if (cmd == IDM_CTX_ABOUT) ctx->view->OpenSettingsCenter(SettingsCategory::kAboutAndDiagnostics);
            InvalidateRect(hwnd, nullptr, FALSE);
}

void ShowGlobalToolsMenuWin32(HWND hwnd, Win32WindowContext* ctx, POINT screen_pt) {
    if (!ctx || !ctx->view) return;
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SITE_GRABBER, L"Site Grabber && Link Crawler...");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_BATCH_URL, L"Add Batch URL Template ([01-10], {a,b})...");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_QUEUE_MANAGER, L"Multi-Queue && Schedule Manager...");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_DROP_TARGET, L"Show Floating Drop Target");
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_TRAY_PROGRESS, L"Show Active Transfer Summary");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

    HMENU hImportExport = CreatePopupMenu();
    AppendMenuW(hImportExport, MF_STRING, IDM_CTX_IMPORT_TASKS, L"Import Tasks from File...");
    AppendMenuW(hImportExport, MF_STRING, IDM_CTX_EXPORT_TASKS, L"Export Tasks (JSON / CSV)...");
    AppendMenuW(hImportExport, MF_STRING, IDM_CTX_EXPORT_EF2, L"Export Tasks to IDM (.ef2)...");
    AppendMenuW(hImportExport, MF_STRING, IDM_CTX_IMPORT_EF2, L"Import Tasks from IDM (.ef2)...");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hImportExport), L"Import / Export Tasks");

    HMENU hTorrentTools = CreatePopupMenu();
    AppendMenuW(hTorrentTools, MF_STRING, IDM_CTX_OPEN_TORRENT, L"Open Torrent File...");
    AppendMenuW(hTorrentTools, MF_STRING, IDM_CTX_CREATE_TORRENT, L"Create Torrent File...");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTorrentTools), L"Torrent Tools");

    HMENU hDiag = CreatePopupMenu();
    AppendMenuW(hDiag, MF_STRING, IDM_CTX_BROWSER_STAT, L"Browser Integration Diagnostics...");
    if (security::IsCurrentUserLocalAdmin()) {
        AppendMenuW(hDiag, MF_STRING, IDM_CTX_STARTUP_LOG, L"Download Startup Log (Admin)...");
    }
    AppendMenuW(hDiag, MF_STRING, IDM_CTX_ABOUT, L"About Radium Download Manager...");
    AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hDiag), L"Diagnostics && Logs");

    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_CTX_SETTINGS, L"Settings && Configuration (Ctrl+,)...");

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, screen_pt.x, screen_pt.y, 0, hwnd, nullptr);
    DestroyMenu(hMenu);
    if (cmd != 0) {
        ExecuteContextCommandWin32(hwnd, ctx, cmd, std::nullopt);
        InvalidateRect(hwnd, nullptr, FALSE);
    }
}

LRESULT CALLBACK RadiumWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<Win32WindowContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<Win32WindowContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx || !ctx->view) {
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    switch (msg) {
        case WM_CREATE: {
            if (g_msgTaskbarButtonCreated == 0) {
                g_msgTaskbarButtonCreated = RegisterWindowMessageW(L"TaskbarButtonCreated");
            }
            InitWin32Theme(hwnd, ctx->view->GetPalette());
            DragAcceptFiles(hwnd, TRUE);
            HICON hAppIcon = CreateRadiumAppIcon();
            if (hAppIcon) {
                SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(hAppIcon));
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(hAppIcon));
            }
            // Reset taskbar cache upon creation/re-creation
            ctx->taskbar_info_initialized = false;
            ctx->last_taskbar_info = {};

            // Initialize OLE/COM for ITaskbarList3 and IDropTarget (apartment-threaded, UI thread)
            HRESULT hr_ole = OleInitialize(nullptr);
            ctx->ole_initialized = SUCCEEDED(hr_ole);
            HRESULT hr_com = ctx->ole_initialized ? S_OK : CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            ctx->com_initialized = SUCCEEDED(hr_com) || (hr_com == RPC_E_CHANGED_MODE);
            if (ctx->com_initialized) {
                platform::WindowsIntegrations::RegisterWindowDropTarget(hwnd, [hwnd, ctx](const std::string& dropped) {
                    if (dropped.rfind("http://", 0) == 0 || dropped.rfind("https://", 0) == 0 ||
                        dropped.rfind("ftp://", 0) == 0 || dropped.rfind("ftps://", 0) == 0 ||
                        dropped.rfind("magnet:?", 0) == 0) {
                        TaskModel t;
                        t.url = dropped;
                        ctx->controller->AddTask(std::move(t), true);
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                });
                ITaskbarList3* pTbl = nullptr;
                HRESULT hr_tbl = CoCreateInstance(
                    CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                    IID_ITaskbarList3, reinterpret_cast<void**>(&pTbl));
                if (SUCCEEDED(hr_tbl) && pTbl) {
                    if (SUCCEEDED(pTbl->HrInit())) {
                        ctx->pTaskbarList = pTbl;
                    } else {
                        pTbl->Release();
                    }
                }
            }
            auto geom = ctx->view->ComputeLayoutGeometry(0);
            int edit_pad = DesignSystem::ScaleForDpi(32, ctx->view->GetMetrics().dpi);
            int edit_h = DesignSystem::ScaleForDpi(22, ctx->view->GetMetrics().dpi);
            int edit_y = geom.search_box_rect.top + (geom.search_box_rect.height() - edit_h) / 2;
            ctx->hSearchEdit = CreateWindowExW(
                0, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                geom.search_box_rect.left + edit_pad, edit_y,
                std::max(10, geom.search_box_rect.width() - edit_pad - 14), edit_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SEARCH_EDIT)),
                GetModuleHandleW(nullptr), nullptr);
            if (ctx->hSearchEdit) {
                ApplyNativeEditTheme(ctx->hSearchEdit, false);
            }
            if (ctx->controller->GetSettings().monitor_clipboard) {
                ctx->clipboard_listener_added = (AddClipboardFormatListener(hwnd) != FALSE);
            }
            ctx->hotkey_registered = (RegisterHotKey(hwnd, IDHOT_SHOW_RADIUM, MOD_CONTROL | MOD_ALT, 'R') != FALSE);
            int edit_font_sz = DesignSystem::ScaleForDpi(15, ctx->view->GetMetrics().dpi);
            ctx->hEditFont = CreateFontW(
                -edit_font_sz, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            SendMessageW(ctx->hSearchEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hEditFont), TRUE);
            SendMessageW(ctx->hSearchEdit, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"Search downloads..."));
            if (ctx->hSearchEdit) {
                SetWindowLongPtrW(ctx->hSearchEdit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
                ctx->orig_search_proc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(ctx->hSearchEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SearchEditSubclassProc)));
                SyncSearchEditLayout(ctx);
            }
            return 0;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC hdcEdit = reinterpret_cast<HDC>(wParam);
            HWND hCtl = reinterpret_cast<HWND>(lParam);
            if (hCtl == ctx->hSearchEdit || hCtl == ctx->hConflictRenameEdit) {
                const auto& palette = ctx->view->GetPalette();
                SetTextColor(hdcEdit, palette.text_primary.ToWin32ColorRef());
                ColorRgba bg_col = palette.surface_elevated;
                SetBkColor(hdcEdit, bg_col.ToWin32ColorRef());
                static HBRUSH hSearchEditBrush = nullptr;
                static COLORREF lastBrushColor = CLR_INVALID;
                COLORREF cr = bg_col.ToWin32ColorRef();
                if (!hSearchEditBrush || lastBrushColor != cr) {
                    if (hSearchEditBrush) DeleteObject(hSearchEditBrush);
                    hSearchEditBrush = CreateSolidBrush(cr);
                    lastBrushColor = cr;
                }
                return reinterpret_cast<LRESULT>(hSearchEditBrush);
            }
            break;
        }
        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_SEARCH_EDIT && HIWORD(wParam) == EN_CHANGE) {
                wchar_t buf[512]{};
                GetWindowTextW(ctx->hSearchEdit, buf, 511);
                if (ctx->view->IsSettingsCenterOpen()) {
                    ctx->view->SetSettingsSearchQuery(WToUtf8(buf));
                } else {
                    ctx->view->SetSearchQuery(WToUtf8(buf));
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lParam) == HTCLIENT) {
                POINT pt{};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if (ctx->view->IsResizingColumn() || ctx->view->HitTestColumnDivider(pt.x, pt.y, ctx->view->GetScrollOffset()) >= 0) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
            }
            break;
        }
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = ctx->view->GetMetrics().min_window_width;
            mmi->ptMinTrackSize.y = ctx->view->GetMetrics().min_window_height;
            return 0;
        }
        case WM_DPICHANGED: {
            UINT new_dpi = HIWORD(wParam);
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ctx->view->SetWindowDimensions(
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                new_dpi);
            SetWindowPos(
                hwnd, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            SyncSearchEditLayout(ctx);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_SHOWWINDOW: {
            bool shown = (wParam != FALSE);
            ctx->view->SetWindowVisibility(shown, IsIconic(hwnd) != FALSE);
            return 0;
        }
        case WM_SIZE: {
            if (wParam == SIZE_MINIMIZED) {
                ctx->view->SetWindowVisibility(true, true);
                return 0;
            }
            ctx->view->SetWindowVisibility(IsWindowVisible(hwnd) != FALSE, false);
            UINT dpi = GetDpiForWindow(hwnd);
            ctx->view->SetWindowDimensions(LOWORD(lParam), HIWORD(lParam), dpi > 0 ? dpi : 96);
            auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
            if (ctx->hThemeTooltip) {
                TOOLINFOW tool{};
                tool.cbSize = sizeof(tool);
                tool.hwnd = hwnd;
                tool.uId = 1;
                tool.rect = ToWinRect(geom.theme_button_rect);
                SendMessageW(ctx->hThemeTooltip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
            }
            SyncSearchEditLayout(ctx);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (ctx->view->IsResizingColumn()) {
                ctx->view->UpdateColumnResize(x);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ctx->is_dragging_vscroll) {
                auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
                auto all_tasks = ctx->view->GetFilteredAndSortedTasks();
                int row_h_calc = std::max(1, geom.task_row_height);
                size_t visible_count = static_cast<size_t>(std::max(1, geom.table_body_rect.height() / row_h_calc));
                if (all_tasks.size() > visible_count) {
                    size_t max_offset = all_tasks.size() - visible_count;
                    int track_top = geom.table_body_rect.top + 4;
                    int track_bottom = geom.table_body_rect.bottom - 4;
                    int track_h = std::max(10, track_bottom - track_top);
                    int thumb_h = std::max(24, static_cast<int>(track_h * visible_count / all_tasks.size()));
                    int thumb_travel = track_h - thumb_h;
                    if (thumb_travel > 0) {
                        int dy = y - ctx->vscroll_drag_start_y;
                        int offset_delta = (dy * static_cast<int>(max_offset)) / thumb_travel;
                        int new_offset = std::clamp(static_cast<int>(ctx->vscroll_drag_start_offset) + offset_delta, 0, static_cast<int>(max_offset));
                        ctx->view->SetScrollOffset(static_cast<size_t>(new_offset));
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                }
                return 0;
            }

            auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
            bool on_scroll = (x >= geom.table_body_rect.right - 16 && x <= geom.table_body_rect.right && y >= geom.table_body_rect.top && y <= geom.table_body_rect.bottom);
            if (on_scroll != ctx->is_vscroll_hovered) {
                ctx->is_vscroll_hovered = on_scroll;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (!ctx->mouse_tracking_active) {
                TRACKMOUSEEVENT tme{sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&tme);
                ctx->mouse_tracking_active = true;
            }

            int old_row = ctx->view->GetHoveredRowIndex();
            int old_tb = ctx->view->GetHoveredToolbarIndex();
            int old_sb = ctx->view->GetHoveredSidebarIndex();

            size_t scroll_offset = ctx->view->GetScrollOffset();
            ctx->view->HandleMouseMove(x, y, scroll_offset);

            int new_row = ctx->view->GetHoveredRowIndex();
            int new_tb = ctx->view->GetHoveredToolbarIndex();
            int new_sb = ctx->view->GetHoveredSidebarIndex();

            if (old_row != new_row) {
                if (old_row >= 0) {
                    RECT rc = ToWinRect(ctx->view->GetRowRect(static_cast<size_t>(old_row)));
                    RedrawWindow(hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                }
                if (new_row >= 0) {
                    RECT rc = ToWinRect(ctx->view->GetRowRect(static_cast<size_t>(new_row)));
                    RedrawWindow(hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
                }
            }

            if (old_tb != new_tb || old_sb != new_sb || geom.theme_button_rect.Contains(x, y) || geom.tools_button_rect.Contains(x, y)) {
                RECT tb_rc = ToWinRect(geom.toolbar_rect);
                InvalidateRect(hwnd, &tb_rc, FALSE);
            }

            return 0;
        }
        case WM_MOUSELEAVE: {
            ctx->mouse_tracking_active = false;
            int old_row = ctx->view->GetHoveredRowIndex();
            int old_tb = ctx->view->GetHoveredToolbarIndex();
            int old_sb = ctx->view->GetHoveredSidebarIndex();

            ctx->view->ClearHoverState();

            if (old_row >= 0) {
                RECT rc = ToWinRect(ctx->view->GetRowRect(static_cast<size_t>(old_row)));
                RedrawWindow(hwnd, &rc, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
            }
            if (old_tb >= 0 || old_sb >= 0 || ctx->view) {
                auto geom = ctx->view->ComputeLayoutGeometry(ctx->view->GetScrollOffset());
                RECT tb_rc = ToWinRect(geom.toolbar_rect);
                InvalidateRect(hwnd, &tb_rc, FALSE);
            }
            if (ctx->is_vscroll_hovered) {
                ctx->is_vscroll_hovered = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (ctx->view->IsResizingColumn()) {
                ReleaseCapture();
                ctx->view->EndColumnResize();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ctx->is_dragging_vscroll) {
                ctx->is_dragging_vscroll = false;
                ReleaseCapture();
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            // Clear toolbar press state for button release animation
            if (ctx->view->GetAnimationState().pressed_toolbar_idx != -1) {
                ctx->view->GetAnimationState().pressed_toolbar_idx = -1;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (ctx->language_button_pressed) {
                ctx->language_button_pressed = false;
                const int x = GET_X_LPARAM(lParam);
                const int y = GET_Y_LPARAM(lParam);
                const size_t scroll_offset = ctx->view->GetScrollOffset();
                const auto hit = ctx->view->HitTestPoint(x, y, scroll_offset);
                if (hit.type == HitTargetType::kSidebarLanguageButton) {
                    const auto geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                    const auto& languages = LocalizationManager::Languages();
                    const std::string selected_locale = ctx->controller->GetSettings().language;
                    HMENU language_menu = CreatePopupMenu();
                    if (language_menu) {
                        AppendMenuW(language_menu, MF_STRING | MF_DISABLED | MF_GRAYED, 100, L"Choose a language");
                        AppendMenuW(language_menu, MF_SEPARATOR, 0, nullptr);
                        for (size_t i = 0; i < languages.size(); ++i) {
                            const auto& option = languages[i];
                            const UINT id = static_cast<UINT>(i + 1);
                            const UINT flags = MF_STRING | (selected_locale == option.locale ? MF_CHECKED : 0);
                            const std::wstring native_name = Utf8ToW(option.native_name);
                            AppendMenuW(language_menu, flags, id, native_name.c_str());
                        }

                        RECT language_rect = ToWinRect(geom.sidebar_language_rect);
                        POINT menu_anchor{language_rect.left, language_rect.top - 6};
                        ClientToScreen(hwnd, &menu_anchor);
                        const int choice = TrackPopupMenu(
                            language_menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                            menu_anchor.x, menu_anchor.y, 0, hwnd, nullptr);
                        DestroyMenu(language_menu);

                        if (choice >= 1 && static_cast<size_t>(choice - 1) < languages.size()) {
                            ctx->view->SetDraftSettingByKey("language", languages[static_cast<size_t>(choice - 1)].locale);
                            const auto saved = ctx->view->ApplySettingsChanges();
                            if (saved.ok()) {
                                ApplyNativeWindowTheme(hwnd, ctx->view->GetPalette());
                                SyncSearchEditLayout(ctx);
                            } else {
                                const std::wstring error = Utf8ToW(saved.error ? saved.error->message : "Could not save language preference.");
                                MessageBoxW(hwnd, error.c_str(), L"Language", MB_OK | MB_ICONWARNING);
                            }
                        }
                    }
                    InvalidateRect(hwnd, nullptr, TRUE);
                }
                return 0;
            }
            break;
        }
        case WM_MOUSEWHEEL: {
            short zDelta = GET_WHEEL_DELTA_WPARAM(wParam);
            if (ctx->view->IsSettingsCenterOpen()) {
                int delta_px = (zDelta > 0) ? -48 : 48;
                ctx->view->ScrollSettingsBy(delta_px);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int rows_to_scroll = (zDelta > 0) ? -3 : 3;
            ctx->view->ScrollBy(rows_to_scroll);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_VSCROLL: {
            return 0;
        }
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK: {
            int x = GET_X_LPARAM(lParam);
            int y = GET_Y_LPARAM(lParam);
            if (!ctx->conflict_dialog_task_id.empty()) {
                if (ctx->conflict_dialog_processing) {
                    return 0;
                }
                RECT cr{}; GetClientRect(hwnd, &cr);
                auto lay = ComputeConflictDialogLayout(cr.right, cr.bottom, GetDpiForWindow(hwnd));
                auto inside = [x, y](const RECT& r) { return x >= r.left && x <= r.right && y >= r.top && y <= r.bottom; };
                if (inside(lay.close) || inside(lay.abort)) ResolveConflictDialog(*ctx, ConflictPolicy::kFailIfExists);
                else if (inside(lay.rename)) ResolveConflictDialog(*ctx, ConflictPolicy::kAutoRename);
                else if (inside(lay.overwrite)) ResolveConflictDialog(*ctx, ConflictPolicy::kOverwrite);
                else if (inside(lay.checkbox)) ctx->conflict_dialog_remember = !ctx->conflict_dialog_remember;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            size_t scroll_offset = ctx->view->GetScrollOffset();

            // Custom Slim Scrollbar Hit Test
            auto geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
            const bool on_scrollbar = (x >= geom.table_body_rect.right - 16 && x <= geom.table_body_rect.right && y >= geom.table_body_rect.top && y <= geom.table_body_rect.bottom);
            if (on_scrollbar) {
                auto all_tasks = ctx->view->GetFilteredAndSortedTasks();
                int row_h_calc = std::max(1, geom.task_row_height);
                size_t visible_count = static_cast<size_t>(std::max(1, geom.table_body_rect.height() / row_h_calc));
                if (all_tasks.size() > visible_count) {
                    size_t max_offset = all_tasks.size() - visible_count;
                    int track_top = geom.table_body_rect.top + 4;
                    int track_bottom = geom.table_body_rect.bottom - 4;
                    int track_h = std::max(10, track_bottom - track_top);
                    int thumb_h = std::max(24, static_cast<int>(track_h * visible_count / all_tasks.size()));
                    int thumb_travel = track_h - thumb_h;
                    int thumb_y = track_top + (thumb_travel > 0 ? static_cast<int>(thumb_travel * std::min(scroll_offset, max_offset) / max_offset) : 0);

                    if (y >= thumb_y && y <= thumb_y + thumb_h) {
                        ctx->is_dragging_vscroll = true;
                        ctx->vscroll_drag_start_y = y;
                        ctx->vscroll_drag_start_offset = scroll_offset;
                        SetCapture(hwnd);
                        return 0;
                    } else if (y < thumb_y) {
                        ctx->view->ScrollBy(-static_cast<int>(visible_count));
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    } else {
                        ctx->view->ScrollBy(static_cast<int>(visible_count));
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                }
            }

            int div_idx = ctx->view->HitTestColumnDivider(x, y, scroll_offset);
            if (div_idx >= 0 && msg == WM_LBUTTONDOWN) {
                SetCapture(hwnd);
                ctx->view->StartColumnResize(div_idx, x);
                return 0;
            }

            auto hit = ctx->view->HitTestPoint(x, y, scroll_offset);
            if (hit.type == HitTargetType::kSidebarLanguageButton) {
                if (msg == WM_LBUTTONDOWN) {
                    ctx->language_button_pressed = true;
                }
                return 0;
            }
            if (hit.type == HitTargetType::kThemeToggleButton) {
                auto changed = ctx->view->ToggleTheme();
                if (changed.ok()) ApplyNativeWindowTheme(hwnd, ctx->view->GetPalette());
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (hit.type == HitTargetType::kToolsMenuButton && msg == WM_LBUTTONDOWN) {
                geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                POINT pt{geom.tools_button_rect.left, geom.tools_button_rect.bottom + 2};
                ClientToScreen(hwnd, &pt);
                ShowGlobalToolsMenuWin32(hwnd, ctx, pt);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            // Track pressed toolbar button for press animation feedback
            if (msg == WM_LBUTTONDOWN) {
                int new_pressed = -1;
                if (hit.type == HitTargetType::kToolbarButton) new_pressed = hit.index;
                else if (hit.type == HitTargetType::kAddDownloadButton) new_pressed = 0;
                else if (hit.type == HitTargetType::kThemeToggleButton) new_pressed = -2;
                ctx->view->GetAnimationState().pressed_toolbar_idx = new_pressed;
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            if (ctx->view->IsSettingsCenterOpen()) {
                if (hit.type == HitTargetType::kSettingsItemControl) {
                    if (hit.task_id == "default_download_dir") {
                        std::string chosen_dir = ctx->view->GetDraftSettings().default_download_dir;
                        if (SelectFolderWithDialogWin32(hwnd, chosen_dir)) {
                            ctx->view->SetDraftSettingByKey("default_download_dir", chosen_dir);
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    auto sv = ctx->view->BuildSettingsCenterView();
                    const SettingItemView* clicked_itm = nullptr;
                    for (const auto& itm : sv.visible_items) {
                        if (itm.key == hit.task_id) {
                            clicked_itm = &itm;
                            break;
                        }
                    }
                    if (clicked_itm && hit.task_id == "proxy_server") {
                        std::wstring proxy_value;
                        if (ShowSimpleInputDialogWin32(hwnd, L"Manual Proxy Server",
                            L"Enter the HTTP proxy host and port (for example 127.0.0.1:8080):",
                            Utf8ToW(ctx->view->GetDraftSettings().proxy_server), proxy_value)) {
                            ctx->view->SetDraftSettingByKey("proxy_server", WToUtf8(proxy_value));
                        }
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (clicked_itm && clicked_itm->control_type == SettingControlType::kActionButton &&
                        hit.task_id == "admin_startup_log") {
                        ShowStartupLogDialogWin32(hwnd, *ctx->controller, ctx->view->GetPalette());
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (clicked_itm && clicked_itm->control_type == SettingControlType::kActionButton &&
                        hit.task_id == "check_for_updates") {
                        if (!ctx->view->IsUpdateCheckInProgress()) {
                            ctx->view->SetUpdateCheckInProgress(true);
                            StartGitHubUpdateCheck(hwnd);
                            InvalidateRect(hwnd, nullptr, FALSE);
                        }
                        return 0;
                    }
                    if (clicked_itm && clicked_itm->control_type == SettingControlType::kActionButton &&
                        hit.task_id == "download_browser_extension") {
                        ShowExtensionSetupDialogWin32(hwnd, *ctx->controller, ctx->view->GetPalette(), false /* is_first_launch */);
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    if (clicked_itm && clicked_itm->control_type == SettingControlType::kActionButton &&
                        hit.task_id == "open_developer_github") {
                        ShellExecuteW(hwnd, L"open", L"https://github.com/alve33", nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                    if (clicked_itm && clicked_itm->control_type == SettingControlType::kActionButton &&
                        hit.task_id == "open_github_repo") {
                        ShellExecuteW(hwnd, L"open", L"https://github.com/alve33/Radium-Download-Manager", nullptr, nullptr, SW_SHOWNORMAL);
                        return 0;
                    }
                    if (clicked_itm && clicked_itm->control_type == SettingControlType::kDropdown) {
                        RECT ctrl_rc = ToWinRect(clicked_itm->control_rect);
                        POINT pt{ctrl_rc.left, ctrl_rc.bottom + 2};
                        ClientToScreen(hwnd, &pt);
                        HMENU hMenu = CreatePopupMenu();
                        if (hit.task_id == "theme") {
                            const auto& current_theme = ctx->view->GetDraftSettings().theme;
                            AppendMenuW(hMenu, MF_STRING | (current_theme == "system" ? MF_CHECKED : 0), 1, L"System (follow Windows)");
                            AppendMenuW(hMenu, MF_STRING | (current_theme == "light" ? MF_CHECKED : 0), 2, L"Light");
                            AppendMenuW(hMenu, MF_STRING | (current_theme == "dark" || current_theme == "coral_dark" ? MF_CHECKED : 0), 3, L"Dark");
                            int choice = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (choice == 1) ctx->view->SetDraftSettingByKey("theme", "system");
                            else if (choice == 2) ctx->view->SetDraftSettingByKey("theme", "light");
                            else if (choice == 3) ctx->view->SetDraftSettingByKey("theme", "dark");
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "conflict_policy") {
                            auto pol = ctx->view->GetDraftSettings().conflict_policy;
                            AppendMenuW(hMenu, MF_STRING | (pol == ConflictPolicy::kAutoRename ? MF_CHECKED : 0), 1, L"Auto-Rename (Numbered Copy)");
                            AppendMenuW(hMenu, MF_STRING | (pol == ConflictPolicy::kOverwrite ? MF_CHECKED : 0), 2, L"Overwrite Existing File");
                            AppendMenuW(hMenu, MF_STRING | (pol == ConflictPolicy::kFailIfExists ? MF_CHECKED : 0), 3, L"Skip if Exists");
                            AppendMenuW(hMenu, MF_STRING | (pol == ConflictPolicy::kPrompt ? MF_CHECKED : 0), 4, L"Ask When File Exists");
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c == 1) ctx->view->SetDraftSettingByKey("conflict_policy", "auto_rename");
                            else if (c == 2) ctx->view->SetDraftSettingByKey("conflict_policy", "overwrite");
                            else if (c == 3) ctx->view->SetDraftSettingByKey("conflict_policy", "fail_if_exists");
                            else if (c == 4) ctx->view->SetDraftSettingByKey("conflict_policy", "prompt");
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "proxy_mode") {
                            auto pm = ctx->view->GetDraftSettings().proxy_mode;
                            AppendMenuW(hMenu, MF_STRING | (pm == "direct" ? MF_CHECKED : 0), 1, L"Direct (No Proxy)");
                            AppendMenuW(hMenu, MF_STRING | (pm == "system" ? MF_CHECKED : 0), 2, L"Use Windows System Proxy");
                            AppendMenuW(hMenu, MF_STRING | (pm == "manual" ? MF_CHECKED : 0), 3, L"Manual HTTP/SOCKS Proxy");
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c == 1) ctx->view->SetDraftSettingByKey("proxy_mode", "direct");
                            else if (c == 2) ctx->view->SetDraftSettingByKey("proxy_mode", "system");
                            else if (c == 3) {
                                auto current = ctx->view->GetDraftSettings().proxy_server;
                                std::wstring proxy_value;
                                if (ShowSimpleInputDialogWin32(hwnd, L"Manual Proxy Server",
                                    L"Enter the HTTP proxy host and port (for example 127.0.0.1:8080):",
                                    Utf8ToW(current), proxy_value)) {
                                    auto saved_proxy = ctx->view->SetDraftSettingByKey("proxy_server", WToUtf8(proxy_value));
                                    if (saved_proxy.ok() && !proxy_value.empty()) ctx->view->SetDraftSettingByKey("proxy_mode", "manual");
                                }
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "default_connections_per_task") {
                            auto conns = ctx->view->GetDraftSettings().default_connections_per_task;
                            const uint32_t presets[] = {1, 2, 4, 8, 16, 24, 32};
                            for (size_t i = 0; i < 7; ++i) {
                                std::wstring lbl = std::to_wstring(presets[i]) + L" Connections" + (presets[i] == 8 ? L" (Recommended)" : L"");
                                AppendMenuW(hMenu, MF_STRING | (conns == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(10 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 10 && c < 17) {
                                ctx->view->SetDraftSettingByKey("default_connections_per_task", std::to_string(presets[c - 10]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "max_concurrent_tasks") {
                            auto mc = ctx->view->GetDraftSettings().max_concurrent_tasks;
                            const uint32_t presets[] = {1, 2, 3, 4, 5, 6, 8, 10, 12, 16};
                            for (size_t i = 0; i < 10; ++i) {
                                std::wstring lbl = std::to_wstring(presets[i]) + L" Tasks" + (presets[i] == 3 ? L" (Default)" : (presets[i] == 6 ? L" (Recommended)" : L""));
                                AppendMenuW(hMenu, MF_STRING | (mc == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(20 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 20 && c < 30) {
                                ctx->view->SetDraftSettingByKey("max_concurrent_tasks", std::to_string(presets[c - 20]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "max_auto_retries") {
                            auto retries = ctx->view->GetDraftSettings().max_auto_retries;
                            const uint32_t presets[] = {0, 1, 2, 3, 5, 8, 10, 15, 20, 30, 50};
                            for (size_t i = 0; i < 11; ++i) {
                                std::wstring lbl = (presets[i] == 0) ? L"0 (No Retries)" : (std::to_wstring(presets[i]) + L" Retries" + (presets[i] == 5 ? L" (Default)" : L""));
                                AppendMenuW(hMenu, MF_STRING | (retries == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(80 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 80 && c < 91) {
                                ctx->view->SetDraftSettingByKey("max_auto_retries", std::to_string(presets[c - 80]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "retry_interval_sec") {
                            auto sec = ctx->view->GetDraftSettings().retry_interval_sec;
                            const uint32_t presets[] = {5, 10, 15, 30, 60, 120};
                            for (size_t i = 0; i < 6; ++i) {
                                std::wstring lbl = std::to_wstring(presets[i]) + L" Seconds" + (presets[i] == 15 ? L" (Default)" : L"");
                                AppendMenuW(hMenu, MF_STRING | (sec == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(100 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 100 && c < 106) {
                                ctx->view->SetDraftSettingByKey("retry_interval_sec", std::to_string(presets[c - 100]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "min_split_bytes") {
                            auto msb = ctx->view->GetDraftSettings().min_split_bytes;
                            const uint64_t presets[] = {65536ULL, 131072ULL, 262144ULL, 524288ULL, 1048576ULL, 2097152ULL, 5242880ULL};
                            const wchar_t* lbls[] = {L"64 KiB", L"128 KiB (Default)", L"256 KiB", L"512 KiB", L"1 MiB", L"2 MiB", L"5 MiB"};
                            for (size_t i = 0; i < 7; ++i) {
                                AppendMenuW(hMenu, MF_STRING | (msb == presets[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(110 + i), lbls[i]);
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 110 && c < 117) {
                                ctx->view->SetDraftSettingByKey("min_split_bytes", std::to_string(presets[c - 110]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "speed_limit_bps") {
                            auto sp = ctx->view->GetDraftSettings().speed_limit_bps;
                            struct SpDef { uint64_t bps; const wchar_t* lbl; };
                            const SpDef sp_presets[] = {
                                {0ULL, L"Unlimited"},
                                {1048576ULL, L"1 MB/s (8 Mbps)"},
                                {5242880ULL, L"5 MB/s (40 Mbps)"},
                                {10485760ULL, L"10 MB/s (80 Mbps)"},
                                {26214400ULL, L"25 MB/s (200 Mbps)"},
                                {52428800ULL, L"50 MB/s (400 Mbps)"}
                            };
                            for (size_t i = 0; i < 6; ++i) {
                                AppendMenuW(hMenu, MF_STRING | (sp == sp_presets[i].bps ? MF_CHECKED : 0), static_cast<UINT_PTR>(30 + i), sp_presets[i].lbl);
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 30 && c < 36) {
                                ctx->view->SetDraftSettingByKey("speed_limit_bps", std::to_string(sp_presets[c - 30].bps));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "stall_timeout_ms") {
                            auto st = ctx->view->GetDraftSettings().stall_timeout_ms;
                            const int timeouts[] = {1500, 3000, 5000, 10000, 15000};
                            for (size_t i = 0; i < 5; ++i) {
                                std::wstring lbl = std::to_wstring(timeouts[i] / 1000) + L" Seconds" + (timeouts[i] == 3000 ? L" (Default)" : L"");
                                AppendMenuW(hMenu, MF_STRING | (st == timeouts[i] ? MF_CHECKED : 0), static_cast<UINT_PTR>(40 + i), lbl.c_str());
                            }
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c >= 40 && c < 45) {
                                ctx->view->SetDraftSettingByKey("stall_timeout_ms", std::to_string(timeouts[c - 40]));
                            }
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else if (hit.task_id == "queue_completion_action") {
                            auto qa = ctx->view->GetDraftSettings().queue_completion_action;
                            AppendMenuW(hMenu, MF_STRING | (qa == "NONE" ? MF_CHECKED : 0), 51, L"None (Do Nothing)");
                            AppendMenuW(hMenu, MF_STRING | (qa == "SLEEP" ? MF_CHECKED : 0), 52, L"Put PC to Sleep");
                            AppendMenuW(hMenu, MF_STRING | (qa == "SHUTDOWN" ? MF_CHECKED : 0), 53, L"Shutdown PC");
                            int c = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                            DestroyMenu(hMenu);
                            if (c == 51) ctx->view->SetDraftSettingByKey("queue_completion_action", "NONE");
                            else if (c == 52) ctx->view->SetDraftSettingByKey("queue_completion_action", "SLEEP");
                            else if (c == 53) ctx->view->SetDraftSettingByKey("queue_completion_action", "SHUTDOWN");
                            InvalidateRect(hwnd, nullptr, FALSE);
                            return 0;
                        } else {
                            DestroyMenu(hMenu);
                        }
                    }
                }
                if (hit.type == HitTargetType::kSettingsRestoreDefaultsButton) {
                    if (MessageBoxW(hwnd, L"Restore all Radium settings to their factory defaults?", L"Confirm Restore Defaults", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                        ctx->view->ActionRestoreDefaultSettings();
                    }
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                ctx->view->HandleMouseClick(x, y, msg == WM_LBUTTONDBLCLK, scroll_offset);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.type == HitTargetType::kSortDropdown) {
                geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                POINT pt{geom.sort_dropdown_rect.left, geom.sort_dropdown_rect.bottom + 2};
                ClientToScreen(hwnd, &pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kCreatedAt && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 101, L"Newest (Default)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kCreatedAt && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 102, L"Oldest");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kFileName && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 103, L"Name (A-Z)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kFileName && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 104, L"Name (Z-A)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kSize && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 105, L"Size (Largest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kSize && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 106, L"Size (Smallest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kStatus ? MF_CHECKED : 0), 107, L"Status");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kSpeed ? MF_CHECKED : 0), 108, L"Speed (Fastest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kProgress && !ctx->view->IsSortAscending() ? MF_CHECKED : 0), 109, L"Progress (Highest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kProgress && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 110, L"Progress (Lowest)");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSortColumn() == SortColumn::kEta && ctx->view->IsSortAscending() ? MF_CHECKED : 0), 111, L"ETA (Shortest)");
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == 101) ctx->view->SetSort(SortColumn::kCreatedAt, false);
                else if (cmd == 102) ctx->view->SetSort(SortColumn::kCreatedAt, true);
                else if (cmd == 103) ctx->view->SetSort(SortColumn::kFileName, true);
                else if (cmd == 104) ctx->view->SetSort(SortColumn::kFileName, false);
                else if (cmd == 105) ctx->view->SetSort(SortColumn::kSize, false);
                else if (cmd == 106) ctx->view->SetSort(SortColumn::kSize, true);
                else if (cmd == 107) ctx->view->SetSort(SortColumn::kStatus, true);
                else if (cmd == 108) ctx->view->SetSort(SortColumn::kSpeed, false);
                else if (cmd == 109) ctx->view->SetSort(SortColumn::kProgress, false);
                else if (cmd == 110) ctx->view->SetSort(SortColumn::kProgress, true);
                else if (cmd == 111) ctx->view->SetSort(SortColumn::kEta, true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.type == HitTargetType::kFilterDropdown) {
                geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                POINT pt{geom.filter_dropdown_rect.left, geom.filter_dropdown_rect.bottom + 2};
                ClientToScreen(hwnd, &pt);
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kAllDownloads ? MF_CHECKED : 0), 201, L"All Files");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kInProgress ? MF_CHECKED : 0), 202, L"In Progress");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kCompleted ? MF_CHECKED : 0), 203, L"Completed");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kQueued ? MF_CHECKED : 0), 204, L"Queued");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kFailed ? MF_CHECKED : 0), 207, L"Failed / Canceled");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kVideoDownloads ? MF_CHECKED : 0), 205, L"Video Downloads");
                AppendMenuW(hMenu, MF_STRING | (ctx->view->GetSelectedCategory() == SidebarCategory::kScheduled ? MF_CHECKED : 0), 206, L"Scheduled");
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == 201) ctx->view->SelectCategory(SidebarCategory::kAllDownloads);
                else if (cmd == 202) ctx->view->SelectCategory(SidebarCategory::kInProgress);
                else if (cmd == 203) ctx->view->SelectCategory(SidebarCategory::kCompleted);
                else if (cmd == 204) ctx->view->SelectCategory(SidebarCategory::kQueued);
                else if (cmd == 205) ctx->view->SelectCategory(SidebarCategory::kVideoDownloads);
                else if (cmd == 206) ctx->view->SelectCategory(SidebarCategory::kScheduled);
                else if (cmd == 207) ctx->view->SelectCategory(SidebarCategory::kFailed);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (hit.type == HitTargetType::kSearchBox && ctx->hSearchEdit) {
                SetFocus(ctx->hSearchEdit);
                SendMessageW(ctx->hSearchEdit, EM_SETSEL, 0, -1);
                return 0;
            }

            ctx->view->HandleMouseClick(x, y, msg == WM_LBUTTONDBLCLK, scroll_offset);

            if (hit.type == HitTargetType::kToolbarButton || hit.type == HitTargetType::kAddDownloadButton) {
                geom = ctx->view->ComputeLayoutGeometry(scroll_offset);
                if (hit.type == HitTargetType::kAddDownloadButton) {
                    ShowAddUrlQuickDialogWin32(hwnd, *ctx->view, *ctx->controller);
                } else if (hit.index >= 0 && static_cast<size_t>(hit.index) < geom.toolbar_buttons.size()) {
                    auto aid = geom.toolbar_buttons[static_cast<size_t>(hit.index)].action_id;
                    if (aid == ToolbarActionId::kAddUrl) {
                        ShowAddUrlQuickDialogWin32(hwnd, *ctx->view, *ctx->controller);
                    }
                }
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_CONTEXTMENU: {
            if (ctx->view->IsSettingsCenterOpen()) return 0;
            POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const bool from_keyboard = (pt.x == -1 && pt.y == -1);
            if (!from_keyboard) {
                POINT client_pt = pt;
                ScreenToClient(hwnd, &client_pt);
                size_t scroll_offset = ctx->view->GetScrollOffset();
                auto hit = ctx->view->HitTestPoint(client_pt.x, client_pt.y, scroll_offset);
                if (hit.type == HitTargetType::kTaskRow || hit.type == HitTargetType::kTaskRowActionButton) {
                    ctx->view->SelectTask(hit.task_id);
                } else {
                    ShowGlobalToolsMenuWin32(hwnd, ctx, pt);
                    return 0;
                }
            }

            std::string sel = ctx->view->GetSelectedTaskId();
            auto sel_task = sel.empty() ? std::nullopt : ctx->controller->GetTask(sel);
            if (!sel_task.has_value()) {
                if (from_keyboard) {
                    RECT wr{};
                    GetWindowRect(hwnd, &wr);
                    pt = {wr.left + 80, wr.top + 80};
                }
                ShowGlobalToolsMenuWin32(hwnd, ctx, pt);
                return 0;
            }
            if (from_keyboard) {
                RECT wr{};
                GetWindowRect(hwnd, &wr);
                pt = {wr.left + 140, wr.top + 140};
            }

            auto tb = ctx->view->ComputeToolbarState();
            bool is_completed = (sel_task->state == TaskState::kCompleted);

            HMENU hMenu = CreatePopupMenu();

            // 1. Primary Open Actions
            AppendMenuW(hMenu, tb.can_open_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_OPEN_FILE, L"Open File (Enter)");
            AppendMenuW(hMenu, tb.can_open_folder ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_OPEN_FOLDER, L"Show in Folder");

            // 1b. Priority Submenu
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            HMENU hPriorityMenu = CreatePopupMenu();
            AppendMenuW(hPriorityMenu, MF_STRING | (sel_task->priority == TaskPriority::kHigh ? MF_CHECKED : 0), IDM_CTX_PRIORITY_HIGH, L"High");
            AppendMenuW(hPriorityMenu, MF_STRING | (sel_task->priority == TaskPriority::kNormal ? MF_CHECKED : 0), IDM_CTX_PRIORITY_NORMAL, L"Normal");
            AppendMenuW(hPriorityMenu, MF_STRING | (sel_task->priority == TaskPriority::kLow ? MF_CHECKED : 0), IDM_CTX_PRIORITY_LOW, L"Low");
            AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hPriorityMenu), L"Set priority");

            // 2. State-Relevant Transfer Controls
            if (tb.can_start || tb.can_resume || tb.can_pause || tb.can_retry || tb.can_cancel) {
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                if (tb.can_start)  AppendMenuW(hMenu, MF_STRING, IDM_CTX_START, L"Start Download");
                if (tb.can_resume) AppendMenuW(hMenu, MF_STRING, IDM_CTX_RESUME, L"Resume Download");
                if (tb.can_pause)  AppendMenuW(hMenu, MF_STRING, IDM_CTX_PAUSE, L"Pause Download");
                if (tb.can_retry)  AppendMenuW(hMenu, MF_STRING, IDM_CTX_RETRY, L"Retry Download (F5)");
                if (tb.can_cancel) AppendMenuW(hMenu, MF_STRING, IDM_CTX_CANCEL, L"Cancel Download");
            }

            // 3. File & URL Actions
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_RENAME, L"Rename File...");
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_MOVE_TO, L"Move To Folder...");
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_COPY_URL, L"Copy URL to Clipboard");
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_REFRESH_URL, L"Change / Refresh URL...");
            AppendMenuW(hMenu, MF_STRING, IDM_CTX_ADD_MIRROR, L"Add Mirror URL...");

            HMENU hSchedMenu = CreatePopupMenu();
            AppendMenuW(hSchedMenu, MF_STRING, IDM_CTX_SCHEDULE, L"Schedule Download...");
            AppendMenuW(hSchedMenu, MF_STRING, IDM_CTX_SPEED_LIMIT, L"Set Speed Limit...");
            AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hSchedMenu), L"Schedule && Speed Limit");

            // 4. More Tools Submenu
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            HMENU hToolsSub = CreatePopupMenu();
            AppendMenuW(hToolsSub, is_completed ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_FILE_INTEGRITY, L"File Integrity (SHA-256)...");
            AppendMenuW(hToolsSub, tb.can_open_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_ANTIVIRUS, L"Scan with Antivirus...");
            std::string lower_name = ToLower(sel_task->file_name);
            bool is_zip = (lower_name.size() >= 4 && lower_name.compare(lower_name.size() - 4, 4, ".zip") == 0);
            if (is_zip) {
                AppendMenuW(hToolsSub, tb.can_open_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_ZIP_PREVIEW, L"Preview Archive Contents (ZIP)...");
            }
            AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hToolsSub), L"More Tools");

            // 5. Conditional Torrent Submenu (only for BitTorrent tasks)
            std::string lower_url = ToLower(sel_task->url);
            bool is_torrent = (sel_task->task_type == TaskType::kTorrent ||
                               lower_url.rfind("magnet:?", 0) == 0 ||
                               lower_url.find(".torrent") != std::string::npos ||
                               (lower_name.size() >= 8 && lower_name.compare(lower_name.size() - 8, 8, ".torrent") == 0));
            if (is_torrent) {
                HMENU hTorrentSub = CreatePopupMenu();
                AppendMenuW(hTorrentSub, MF_STRING, IDM_CTX_TORRENT_SWARM, L"Torrent Swarm && Trackers...");
                AppendMenuW(hTorrentSub, MF_STRING, IDM_CTX_TORRENT_ADD_TRACKER, L"Add Torrent Tracker...");
                AppendMenuW(hTorrentSub, MF_STRING, IDM_CTX_TORRENT_SEQUENTIAL, L"Toggle Sequential Torrent Mode");
                AppendMenuW(hMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(hTorrentSub), L"Torrent");
            }

            // 6. Removal
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(hMenu, tb.can_remove ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_REMOVE_LIST, L"Remove from List (Delete)");
            AppendMenuW(hMenu, tb.can_delete_file ? MF_STRING : MF_STRING | MF_GRAYED, IDM_CTX_DELETE_DISK, L"Delete File... (Shift+Delete)");

            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(hMenu);
            if (cmd != 0) {
                ExecuteContextCommandWin32(hwnd, ctx, cmd, sel_task);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_DROPFILES: {
            HDROP hDrop = reinterpret_cast<HDROP>(wParam);
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                wchar_t filePath[MAX_PATH];
                if (DragQueryFileW(hDrop, i, filePath, MAX_PATH) > 0) {
                    std::filesystem::path p(filePath);
                    std::string ext = p.extension().string();
                    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                    if (ext == ".torrent") {
                        // Route dropped torrents directly to dedicated Add Torrent dialog
                        std::string tor_path = WToUtf8(filePath);
                        auto parsed = torrent::TorrentParser::ParseTorrentFile(tor_path);
                        if (parsed.ok()) {
                            auto tinfo = std::move(*parsed.value);
                            std::string default_dir = ctx->controller->GetSettings().default_download_dir;
                            bool start_torrent = true;
                            if (ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo, tor_path, default_dir,
                                                          start_torrent, &default_dir, &start_torrent)) {
                                TaskModel t;
                                t.task_type = TaskType::kTorrent;
                                t.url = tor_path;
                                t.file_name = tinfo.name.empty() ? PathSanitizer::PathToUtf8(p.filename()) : tinfo.name;
                                t.save_dir = default_dir;
                                t.total_bytes = tinfo.selected_bytes();
                                for (const auto& file : tinfo.files) {
                                    t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                    t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                                }
                                auto res = ctx->controller->AddTask(std::move(t), start_torrent, false);
                                if (res.ok()) {
                                    ctx->view->SelectTask(res.value->task_id);
                                    ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                                }
                            }
                        } else {
                            std::wstring err = L"Failed to parse torrent file:\n" + Utf8ToW(parsed.error->message);
                            MessageBoxW(hwnd, err.c_str(), L"Invalid Torrent", MB_OK | MB_ICONERROR);
                        }
                    } else if (ext == ".txt" || ext == ".csv" || ext == ".json") {
                        std::ifstream ifs(p);
                        if (ifs) {
                            std::stringstream ss_file;
                            ss_file << ifs.rdbuf();
                            std::string content = ss_file.str();
                            if (ext == ".json") {
                                ctx->controller->ImportTasksData(content, storage::ExportFormat::kJson);
                            } else if (ext == ".csv") {
                                ctx->controller->ImportTasksData(content, storage::ExportFormat::kCsv);
                            } else {
                                auto urls_res = storage::TaskImportExport::ImportFromUrlList(content);
                                if (urls_res.ok()) {
                                    for (auto& task_item : urls_res.value.value()) {
                                        ctx->controller->AddTask(std::move(task_item), true, false);
                                    }
                                }
                            }
                        }
                    } else {
                        std::string target_str = WToUtf8(filePath);
                        ShowNewDownloadDialogWin32(hwnd, *ctx->view, *ctx->controller, target_str, PathSanitizer::PathToUtf8(p.filename()));
                    }
                }
            }
            DragFinish(hDrop);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_RADIUM_SHOW_CONFLICT: {
            if (lParam != 0) {
                std::unique_ptr<Win32WindowContext::PendingConflictItem> incoming(
                    reinterpret_cast<Win32WindowContext::PendingConflictItem*>(lParam));
                EnqueueConflictPrompt(*ctx, std::move(*incoming));
            } else {
                RefreshConflictDialog(*ctx);
            }
            if (!IsWindowVisible(hwnd)) {
                ShowWindow(hwnd, SW_SHOW);
            }
            if (IsIconic(hwnd)) {
                ShowWindow(hwnd, SW_RESTORE);
            }
            ctx->view->SetWindowVisibility(true, false);
            SetForegroundWindow(hwnd);
            BringWindowToTop(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_RADIUM_CONFLICT_COMMIT_COMPLETE: {
            std::unique_ptr<ConflictCommitPayload> payload(reinterpret_cast<ConflictCommitPayload*>(lParam));
            ctx->conflict_dialog_processing = false;
            ctx->conflict_dialog_processing_choice = ConflictPolicy::kPrompt;
            if (!payload) {
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!payload->settings_warning.empty()) {
                std::wstring message = L"This download choice will be applied, but Radium could not remember it: " + Utf8ToW(payload->settings_warning);
                MessageBoxW(hwnd, message.c_str(), L"Could not save preference", MB_OK | MB_ICONWARNING);
            }
            if (payload->success) {
                if (!ctx->pending_conflicts.empty()) {
                    const auto& front = ctx->pending_conflicts.front();
                    if (ctx->ipc_server && !front.draft_task.task_id.empty()) {
                        ctx->ipc_server->ResolvePendingOffer(front.draft_task.task_id);
                    }
                    ctx->pending_conflicts.pop_front();
                }
                ctx->conflict_dialog_task_id.clear();
                ctx->conflict_dialog_path.clear();
                ctx->conflict_dialog_remember = false;
                ctx->conflict_validation_error.clear();
                HideConflictRenameEdit(*ctx);
                if (!ctx->pending_conflicts.empty()) {
                    ActivatePendingConflict(*ctx, ctx->pending_conflicts.front());
                }
                if (!payload->task_id.empty()) {
                    ctx->view->SelectTask(payload->task_id);
                    ctx->view->ScrollTaskIntoView(payload->task_id, true);
                }
            } else {
                ctx->conflict_validation_error = payload->error_message.empty()
                    ? "Could not apply conflict resolution for the selected destination."
                    : payload->error_message;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_RADIUM_TASK_NOTIFICATION: {
            std::unique_ptr<TaskNotificationPayload> notice(reinterpret_cast<TaskNotificationPayload*>(lParam));
            if (notice && ctx->tray_added) {
                if (!notice->success && !notice->task_id.empty() &&
                    !ctx->notified_failure_task_ids.insert(notice->task_id).second) {
                    return 0;
                }
                NOTIFYICONDATAW nid{};
                nid.cbSize = sizeof(NOTIFYICONDATAW);
                nid.hWnd = hwnd;
                nid.uID = 1;
                nid.uFlags = NIF_INFO;
                nid.dwInfoFlags = (notice->success ? NIIF_INFO : NIIF_ERROR) | NIIF_RESPECT_QUIET_TIME;
                wcsncpy_s(nid.szInfoTitle, notice->title.c_str(), _TRUNCATE);
                wcsncpy_s(nid.szInfo, notice->message.c_str(), _TRUNCATE);
                Shell_NotifyIconW(NIM_MODIFY, &nid);
            }
            return 0;
        }
        case WM_RADIUM_UPDATE_CHECK_COMPLETE: {
            if (ctx && ctx->view) {
                ctx->view->SetUpdateCheckInProgress(false);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            std::unique_ptr<UpdateCheckPayload> update(reinterpret_cast<UpdateCheckPayload*>(lParam));
            if (!update) return 0;
            const int choice = MessageBoxW(hwnd, update->message.c_str(), update->title.c_str(),
                update->available && update->matching_installer ? MB_YESNO | MB_ICONINFORMATION : MB_OK | MB_ICONINFORMATION);
            const std::wstring release_prefix = Utf8ToW(std::string(radium::config::UpdateAndReleaseConfig::kReleasePrefixUrl));
            if (update->available && update->matching_installer && choice == IDYES &&
                !update->release_url.empty() && update->release_url.rfind(release_prefix, 0) == 0) {
                ShellExecuteW(hwnd, L"open", update->release_url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            return 0;
        }
        case WM_RADIUM_CHECK_FIRST_LAUNCH_ONBOARDING: {
            if (ctx && ctx->controller && ctx->view &&
                ctx->view->IsFirstLaunchOnboardingEligible() &&
                ctx->pending_conflicts.empty() &&
                ctx->controller->ShouldShowExtensionOnboarding()) {
                ctx->view->SetFirstLaunchOnboardingEligible(false);
                ShowExtensionSetupDialogWin32(hwnd, *ctx->controller, ctx->view->GetPalette(), true /* is_first_launch */);
            }
            return 0;
        }
        case WM_RADIUM_SHOW_NEW_DOWNLOAD: {
            if (ctx && ctx->view) {
                ctx->view->SetFirstLaunchOnboardingEligible(false);
            }
            auto* offer = reinterpret_cast<OfferNotificationData*>(lParam);
            if (offer) {
                RADIUM_LOG_INFO("UI", "DOWNLOAD_OFFER_RECEIVED: taskId=" + offer->task_id + " filename=" + offer->file_name);

                // Immediately bring the full Radium application to the foreground
                if (!IsWindowVisible(hwnd)) {
                    ShowWindow(hwnd, SW_SHOW);
                }
                if (IsIconic(hwnd)) {
                    ShowWindow(hwnd, SW_RESTORE);
                }
                ctx->view->SetWindowVisibility(true, false);
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(hwnd);
                BringWindowToTop(hwnd);
                PostMessageW(hwnd, WM_SYSCOMMAND, SC_RESTORE, 0);

                if (IsRemoteTorrentOffer(*offer)) {
                    OfferNotificationData torrent_offer = *offer;
                    if (ctx->ipc_server && !offer->task_id.empty()) {
                    ctx->ipc_server->ResolvePendingOffer(offer->task_id);
                }
                delete offer;
                    const std::string torrent_task_id = torrent_offer.task_id;
                    try {
                        std::thread([hwnd, torrent_offer = std::move(torrent_offer)]() mutable {
                            auto* payload = new TorrentFetchPayload;
                            payload->offer = std::move(torrent_offer);
                            try {
                                auto fetched = FetchTorrentMetainfo(payload->offer);
                                if (fetched.ok()) {
                                    payload->metainfo_path = std::move(*fetched.value);
                                } else {
                                    payload->error_message = fetched.error->message;
                                }
                            } catch (...) {
                                payload->error_message = "Unexpected error while retrieving torrent metadata";
                            }
                            if (!PostMessageW(hwnd, WM_RADIUM_TORRENT_FETCH_COMPLETE, 0,
                                              reinterpret_cast<LPARAM>(payload))) {
                                std::error_code ignored;
                                if (!payload->metainfo_path.empty()) {
                                    std::filesystem::remove_all(std::filesystem::path(payload->metainfo_path).parent_path(), ignored);
                                }
                                delete payload;
                            }
                        }).detach();
                    } catch (...) {
                        ctx->controller->CancelTask(torrent_task_id);
                        ctx->controller->RemoveTask(torrent_task_id, false);
                        MessageBoxW(hwnd, L"Could not start torrent metadata retrieval.", L"Torrent Error", MB_OK | MB_ICONERROR);
                    }
                    return 0;
                }

                bool is_offer_torrent = (offer->url.rfind(".torrent") != std::string::npos);
                std::error_code tor_ec;
                if (is_offer_torrent && std::filesystem::exists(offer->url, tor_ec)) {
                    auto parsed = torrent::TorrentParser::ParseTorrentFile(offer->url);
                    if (parsed.ok()) {
                        auto tinfo = std::move(*parsed.value);
                        std::string target_dir = offer->save_dir.empty()
                            ? (ctx->controller->GetSettings().last_download_dir.empty()
                                ? ctx->controller->GetSettings().default_download_dir
                                : ctx->controller->GetSettings().last_download_dir)
                            : offer->save_dir;
                        auto tor_report = ctx->controller->InspectIncomingTorrent(tinfo, target_dir);
                        if (tor_report.has_conflict() &&
                            ctx->controller->GetSettings().conflict_policy == ConflictPolicy::kPrompt) {
                            Win32WindowContext::PendingConflictItem item;
                            item.draft_task = offer->draft_task;
                            item.draft_task.task_type = TaskType::kTorrent;
                            item.draft_task.file_name = tinfo.name.empty() ? offer->file_name : tinfo.name;
                            item.draft_task.save_dir = target_dir;
                            item.torrent_info = std::move(tinfo);
                            item.report = std::move(tor_report);
                            item.start_immediately = true;
                            EnqueueConflictPrompt(*ctx, std::move(item));
                            InvalidateRect(hwnd, nullptr, FALSE);
                        } else if (ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo, offer->url, target_dir, true, &target_dir)) {
                            auto post_report = ctx->controller->InspectIncomingTorrent(tinfo, target_dir);
                            if (post_report.has_conflict() &&
                                ctx->controller->GetSettings().conflict_policy == ConflictPolicy::kPrompt) {
                                Win32WindowContext::PendingConflictItem item;
                                item.draft_task = offer->draft_task;
                                item.draft_task.task_type = TaskType::kTorrent;
                                item.draft_task.file_name = tinfo.name.empty() ? offer->file_name : tinfo.name;
                                item.draft_task.save_dir = target_dir;
                                item.torrent_info = std::move(tinfo);
                                item.report = std::move(post_report);
                                item.start_immediately = true;
                                EnqueueConflictPrompt(*ctx, std::move(item));
                                InvalidateRect(hwnd, nullptr, FALSE);
                            } else {
                                TaskModel t = offer->draft_task;
                                t.task_type = TaskType::kTorrent;
                                t.url = offer->url;
                                t.file_name = tinfo.name.empty() ? offer->file_name : tinfo.name;
                                t.save_dir = target_dir;
                                t.total_bytes = tinfo.selected_bytes();
                                for (const auto& file : tinfo.files) {
                                    t.torrent_file_paths.push_back(file.relative_path.generic_string());
                                    t.torrent_file_selection.push_back(file.selected ? 1 : 0);
                                }
                                auto res = ctx->controller->AddTask(std::move(t), true, false);
                                if (res.ok()) {
                                    ctx->view->SelectTask(res.value->task_id);
                                    ctx->view->ScrollTaskIntoView(res.value->task_id, true);
                                }
                            }
                        }
                    } else {
                        ShowNewDownloadDialogWin32(
                            hwnd, *ctx->view, *ctx->controller, offer->url, offer->file_name, offer->save_dir,
                            static_cast<int64_t>(offer->file_size), "", offer->draft_task);
                    }
                } else {
                    auto report = offer->has_precomputed_conflict
                        ? offer->precomputed_conflict
                        : ctx->controller->InspectIncomingDownload(offer->draft_task, offer->speculative_tx_id);
                    if (report.kind == DownloadController::IncomingConflictKind::kActiveOrQueuedSameDownload &&
                        report.matched_task.has_value()) {
                        ctx->view->SelectTask(report.matched_task->task_id);
                        ctx->view->ScrollTaskIntoView(report.matched_task->task_id, true);
                    } else if (report.has_conflict() &&
                               ctx->controller->GetSettings().conflict_policy == ConflictPolicy::kPrompt) {
                        Win32WindowContext::PendingConflictItem item;
                        item.draft_task = offer->draft_task;
                        item.draft_task.file_name = report.resolved_file_name;
                        item.draft_task.save_dir = report.resolved_save_dir;
                        item.report = std::move(report);
                        item.start_immediately = true;
                        EnqueueConflictPrompt(*ctx, std::move(item));
                        InvalidateRect(hwnd, nullptr, FALSE);
                    } else {
                        int64_t initial_sz = static_cast<int64_t>(offer->file_size);
                        ShowNewDownloadDialogWin32(
                            hwnd,
                            *ctx->view,
                            *ctx->controller,
                            offer->url,
                            offer->file_name,
                            offer->save_dir,
                            initial_sz,
                            "",
                            offer->draft_task,
                            offer->speculative_tx_id);
                    }
                }
                if (ctx->ipc_server && !offer->task_id.empty()) {
                    ctx->ipc_server->ResolvePendingOffer(offer->task_id);
                }
                delete offer;
            }
            return 0;
        }
        case WM_RADIUM_TORRENT_FETCH_COMPLETE: {
            std::unique_ptr<TorrentFetchPayload> payload(reinterpret_cast<TorrentFetchPayload*>(lParam));
            if (!payload) return 0;

            auto remove_metainfo = [&]() {
                if (payload->metainfo_path.empty()) return;
                std::error_code ignored;
                std::filesystem::remove_all(std::filesystem::path(payload->metainfo_path).parent_path(), ignored);
            };
            auto remove_offer_task = [&]() {
                ctx->controller->CancelTask(payload->offer.task_id);
                ctx->controller->RemoveTask(payload->offer.task_id, false);
            };
            if (!payload->error_message.empty()) {
                remove_offer_task();
                std::wstring error = L"Could not retrieve the torrent file:\n" + Utf8ToW(payload->error_message);
                MessageBoxW(hwnd, error.c_str(), L"Torrent Download Error", MB_OK | MB_ICONERROR);
                return 0;
            }

            auto parsed = torrent::TorrentParser::ParseTorrentFile(payload->metainfo_path);
            if (!parsed.ok()) {
                remove_offer_task();
                remove_metainfo();
                std::wstring error = L"The downloaded file is not valid torrent metadata:\n" + Utf8ToW(parsed.error->message);
                MessageBoxW(hwnd, error.c_str(), L"Invalid Torrent", MB_OK | MB_ICONERROR);
                return 0;
            }
            auto tinfo = std::move(*parsed.value);
            std::string target_dir = payload->offer.save_dir.empty()
                ? (ctx->controller->GetSettings().last_download_dir.empty()
                    ? ctx->controller->GetSettings().default_download_dir
                    : ctx->controller->GetSettings().last_download_dir)
                : payload->offer.save_dir;
            auto tor_report = ctx->controller->InspectIncomingTorrent(tinfo, target_dir);
            if (tor_report.has_conflict() &&
                ctx->controller->GetSettings().conflict_policy == ConflictPolicy::kPrompt) {
                Win32WindowContext::PendingConflictItem item;
                item.draft_task = payload->offer.draft_task;
                item.draft_task.task_type = TaskType::kTorrent;
                item.draft_task.url = payload->metainfo_path;
                item.draft_task.file_name = tinfo.name.empty() ? payload->offer.file_name : tinfo.name;
                item.draft_task.save_dir = target_dir;
                item.draft_task.part_path = std::filesystem::path(payload->metainfo_path).parent_path().string();
                item.draft_task.headers.clear();
                item.draft_task.auth_headers.clear();
                item.torrent_info = std::move(tinfo);
                item.torrent_metainfo_path = payload->metainfo_path;
                item.report = std::move(tor_report);
                item.start_immediately = true;
                EnqueueConflictPrompt(*ctx, std::move(item));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            bool start_torrent = true;
            std::string selected_category;
            if (!ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, tinfo,
                                          payload->metainfo_path, target_dir, start_torrent,
                                          &target_dir, &start_torrent, &selected_category)) {
                remove_offer_task();
                remove_metainfo();
                return 0;
            }

            TaskModel task = payload->offer.draft_task;
            task.task_type = TaskType::kTorrent;
            task.url = payload->metainfo_path;
            task.file_name = tinfo.name.empty() ? payload->offer.file_name : tinfo.name;
            task.category = selected_category.empty() ? "Torrents" : selected_category;
            task.save_dir = target_dir;
            task.part_path = std::filesystem::path(payload->metainfo_path).parent_path().string();
            task.total_bytes = tinfo.selected_bytes();
            task.downloaded_bytes = 0;
            task.headers.clear();
            task.auth_headers.clear();
            task.torrent_file_paths.clear();
            task.torrent_file_selection.clear();
            for (const auto& file : tinfo.files) {
                task.torrent_file_paths.push_back(file.relative_path.generic_string());
                task.torrent_file_selection.push_back(file.selected ? 1 : 0);
            }
            auto post_report = ctx->controller->InspectIncomingTorrent(tinfo, target_dir);
            if (post_report.has_conflict() &&
                ctx->controller->GetSettings().conflict_policy == ConflictPolicy::kPrompt) {
                Win32WindowContext::PendingConflictItem item;
                item.draft_task = std::move(task);
                item.torrent_info = std::move(tinfo);
                item.torrent_metainfo_path = payload->metainfo_path;
                item.report = std::move(post_report);
                item.start_immediately = start_torrent;
                EnqueueConflictPrompt(*ctx, std::move(item));
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            auto added = ctx->controller->AddTask(std::move(task), start_torrent, false);
            if (!added.ok()) {
                remove_metainfo();
                MessageBoxW(hwnd, Utf8ToW("Could not add torrent: " + added.error->message).c_str(),
                            L"Torrent Error", MB_OK | MB_ICONERROR);
                return 0;
            }
            ctx->view->SelectTask(added.value->task_id);
            ctx->view->ScrollTaskIntoView(added.value->task_id, true);
            return 0;
        }
        case WM_RADIUM_SELECT_TORRENT_METADATA: {
            auto* request = reinterpret_cast<TorrentMetadataSelectionRequest*>(lParam);
            if (request && request->info) {
                request->accepted = ShowAddTorrentDialogWin32(hwnd, *ctx->view, *ctx->controller, *request->info);
            }
            return 0;
        }
        case WM_TIMER: {
            if (wParam == IDT_UI_REFRESH) {
                RefreshConflictDialog(*ctx);
                // Check for newly added or started downloads and auto-scroll into view
                ctx->view->CheckAndAutoScrollNewOrStartedDownloads();
                // Sync Windows taskbar progress indicator
                SyncTaskbarProgress(hwnd, ctx->pTaskbarList, *ctx->view, ctx);
                bool has_active = false;
                if (ctx->controller) {
                    for (const auto& t : ctx->controller->ListTasks()) {
                        if (t.state == TaskState::kDownloading || t.state == TaskState::kProbing ||
                            t.state == TaskState::kQueued) {
                            has_active = true;
                            break;
                        }
                    }
                }
                if (has_active || ctx->was_active_last_tick || !ctx->conflict_dialog_task_id.empty() ||
                    (ctx->view && ctx->view->IsUpdateCheckInProgress() && ctx->view->IsSettingsCenterOpen())) {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                ctx->was_active_last_tick = has_active;
            }
            return 0;
        }
        case WM_GETOBJECT: {
            if (static_cast<long>(lParam) == UiaRootObjectId) {
                if (!ctx->pUiaProvider) {
                    ctx->pUiaProvider = new RadiumTaskListUiaProvider(hwnd, ctx->view);
                }
                return UiaReturnRawElementProvider(hwnd, wParam, lParam, ctx->pUiaProvider);
            }
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_KEYDOWN: {
            bool ctrl_down = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            bool shift_down = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wParam == VK_ESCAPE && !ctx->conflict_dialog_task_id.empty()) {
                ResolveConflictDialog(*ctx, ConflictPolicy::kFailIfExists);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (wParam == VK_ESCAPE && ctx->view->IsSettingsCenterOpen()) {
                ctx->view->CloseSettingsCenter(true);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (ctrl_down && (wParam == 'K' || wParam == 'F')) {
                if (ctx->hSearchEdit) {
                    SetFocus(ctx->hSearchEdit);
                    SendMessageW(ctx->hSearchEdit, EM_SETSEL, 0, -1);
                }
                return 0;
            } else if (ctrl_down && wParam == 'N') {
                ShowAddUrlQuickDialogWin32(hwnd, *ctx->view, *ctx->controller);
            } else if (ctrl_down && wParam == VK_OEM_COMMA) {
                ctx->view->OpenSettingsCenter(SettingsCategory::kGeneral);
            } else if (wParam == VK_DOWN) ctx->view->HandleKeyShortcut("VK_DOWN");
            else if (wParam == VK_UP) ctx->view->HandleKeyShortcut("VK_UP");
            else if (wParam == VK_HOME) ctx->view->HandleKeyShortcut("VK_HOME");
            else if (wParam == VK_END) ctx->view->HandleKeyShortcut("VK_END");
            else if (wParam == VK_SPACE) ctx->view->HandleKeyShortcut("VK_SPACE");
            else if (wParam == VK_DELETE) {
                if (shift_down) {
                    ctx->view->HandleKeyShortcut("Shift+Delete");
                } else {
                    ctx->view->HandleKeyShortcut("VK_DELETE");
                }
            } else if (wParam == VK_F5) ctx->view->HandleKeyShortcut("VK_F5");
            else if (wParam == VK_RETURN) ctx->view->ActionOpenCompletedFile(false);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        case WM_RADIUM_TRAYICON: {
            UINT tray_msg = LOWORD(lParam);
            if (tray_msg == WM_CONTEXTMENU || tray_msg == WM_RBUTTONUP) {
                POINT pt{};
                if (tray_msg == WM_CONTEXTMENU && (LOWORD(wParam) != 0 || HIWORD(wParam) != 0)) {
                    pt.x = GET_X_LPARAM(wParam);
                    pt.y = GET_Y_LPARAM(wParam);
                } else {
                    GetCursorPos(&pt);
                }
                HMENU hMenu = CreatePopupMenu();
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_SHOW, L"Show Radium");
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HIDE, L"Hide to Tray");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_PAUSE_ALL, L"Pause All Downloads");
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_RESUME_ALL, L"Resume All Downloads");
                AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, L"Exit Radium");
                SetForegroundWindow(hwnd);
                int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd, nullptr);
                DestroyMenu(hMenu);
                if (cmd == IDM_TRAY_SHOW) {
                    ShowWindow(hwnd, SW_RESTORE);
                    ctx->view->SetWindowVisibility(true, false);
                } else if (cmd == IDM_TRAY_HIDE) {
                    ShowWindow(hwnd, SW_HIDE);
                    ctx->view->SetWindowVisibility(false, false);
                } else if (cmd == IDM_TRAY_PAUSE_ALL) ctx->controller->PauseAllTasks();
                else if (cmd == IDM_TRAY_RESUME_ALL) ctx->controller->ResumeAllTasks();
                else if (cmd == IDM_TRAY_EXIT) DestroyWindow(hwnd);
            } else if (tray_msg == WM_LBUTTONDBLCLK || tray_msg == NIN_SELECT || tray_msg == NIN_KEYSELECT) {
                ShowWindow(hwnd, SW_RESTORE);
                ctx->view->SetWindowVisibility(true, false);
                SetForegroundWindow(hwnd);
            }
            return 0;
        }
        case WM_POWERBROADCAST: {
            if (wParam == PBT_APMSUSPEND) {
                if (ctx && ctx->controller) {
                    ctx->controller->OnSystemSuspend();
                }
                return TRUE;
            } else if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) {
                if (ctx && ctx->controller) {
                    ctx->controller->OnSystemResume();
                }
                return TRUE;
            }
            break;
        }
        case WM_SETTINGCHANGE:
        case WM_THEMECHANGED: {
            if (ctx && ctx->view && ctx->controller->GetSettings().theme == "system") {
                ctx->view->RefreshSystemTheme(IsWindowsAppThemeLight());
                ApplyNativeWindowTheme(hwnd, ctx->view->GetPalette());
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            break;
        }
        case WM_QUERYENDSESSION: {
            return TRUE;
        }
        case WM_ENDSESSION: {
            if (wParam == TRUE && ctx && ctx->controller) {
                ctx->controller->Shutdown();
            }
            return 0;
        }
        case WM_CLOSE: {
            if (ctx->controller->GetSettings().close_to_tray && ctx->tray_added) {
                ShowWindow(hwnd, SW_HIDE);
                ctx->view->SetWindowVisibility(false, false);
                return 0;
            }
            DestroyWindow(hwnd);
            return 0;
        }
        case WM_PAINT: {
            SyncSearchEditVisibility(ctx);
            PaintMainWindow(hwnd, *ctx->view);
            return 0;
        }
        case WM_CLIPBOARDUPDATE: {
            if (ctx && ctx->controller && ctx->controller->GetSettings().monitor_clipboard) {
                if (OpenClipboard(hwnd)) {
                    HANDLE hData = GetClipboardData(CF_UNICODETEXT);
                    if (hData) {
                        const wchar_t* ptr = static_cast<const wchar_t*>(GlobalLock(hData));
                        if (ptr) {
                            int needed = WideCharToMultiByte(CP_UTF8, 0, ptr, -1, nullptr, 0, nullptr, nullptr);
                            std::string text;
                            if (needed > 1) {
                                text.resize(static_cast<size_t>(needed - 1));
                                WideCharToMultiByte(CP_UTF8, 0, ptr, -1, text.data(), needed - 1, nullptr, nullptr);
                            }
                            GlobalUnlock(hData);
                            while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
                            if ((text.rfind("http://", 0) == 0 || text.rfind("https://", 0) == 0 ||
                                 text.rfind("ftp://", 0) == 0 || text.rfind("magnet:?", 0) == 0) &&
                                text != ctx->last_clipboard_url) {
                                ctx->last_clipboard_url = text;
                                ShowNewDownloadDialogWin32(hwnd, *ctx->view, *ctx->controller, text);
                            }
                        }
                    }
                    CloseClipboard();
                }
            }
            return 0;
        }
        case WM_HOTKEY: {
            if (wParam == IDHOT_SHOW_RADIUM) {
                ShowWindow(hwnd, SW_RESTORE);
                SetForegroundWindow(hwnd);
            }
            return 0;
        }
        case WM_DESTROY: {
            if (ctx->hEditFont) {
                DeleteObject(ctx->hEditFont);
                ctx->hEditFont = nullptr;
            }
            if (ctx->pUiaProvider) {
                ctx->pUiaProvider->Release();
                ctx->pUiaProvider = nullptr;
            }
            if (ctx->clipboard_listener_added) {
                RemoveClipboardFormatListener(hwnd);
                ctx->clipboard_listener_added = false;
            }
            if (ctx->hotkey_registered) {
                UnregisterHotKey(hwnd, IDHOT_SHOW_RADIUM);
                ctx->hotkey_registered = false;
            }
            if (ctx->hDropTargetWnd && IsWindow(ctx->hDropTargetWnd)) {
                platform::WindowsIntegrations::RevokeWindowDropTarget(ctx->hDropTargetWnd);
                DestroyWindow(ctx->hDropTargetWnd);
                ctx->hDropTargetWnd = nullptr;
            }
            platform::WindowsIntegrations::RevokeWindowDropTarget(hwnd);
            KillTimer(hwnd, IDT_UI_REFRESH);
            // Clear taskbar progress and release COM
            if (ctx->pTaskbarList) {
                ctx->pTaskbarList->SetProgressState(hwnd, TBPF_NOPROGRESS);
                ctx->pTaskbarList->Release();
                ctx->pTaskbarList = nullptr;
            }
            ctx->taskbar_info_initialized = false;
            ctx->last_taskbar_info = {};
            if (ctx->ole_initialized) {
                OleUninitialize();
                ctx->ole_initialized = false;
                ctx->com_initialized = false;
            } else if (ctx->com_initialized) {
                CoUninitialize();
                ctx->com_initialized = false;
            }
            if (ctx->tray_added) {
                Shell_NotifyIconW(NIM_DELETE, &ctx->nid);
                ctx->tray_added = false;
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    if (g_msgTaskbarButtonCreated != 0 && msg == g_msgTaskbarButtonCreated) {
        // Explorer restarted or recreated taskbar buttons - re-acquire ITaskbarList3 and reset cached state
        if (ctx) {
            ctx->taskbar_info_initialized = false;
            ctx->last_taskbar_info = {};
            if (ctx->pTaskbarList) {
                ctx->pTaskbarList->Release();
                ctx->pTaskbarList = nullptr;
            }
            if (!ctx->com_initialized) {
                HRESULT hr_com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                ctx->com_initialized = SUCCEEDED(hr_com) || (hr_com == RPC_E_CHANGED_MODE);
            }
            ITaskbarList3* pTbl = nullptr;
            HRESULT hr_tbl = CoCreateInstance(
                CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER,
                IID_ITaskbarList3, reinterpret_cast<void**>(&pTbl));
            if (SUCCEEDED(hr_tbl) && pTbl) {
                if (SUCCEEDED(pTbl->HrInit())) {
                    ctx->pTaskbarList = pTbl;
                } else {
                    pTbl->Release();
                }
            }
            if (ctx->pTaskbarList && ctx->view) {
                SyncTaskbarProgress(hwnd, ctx->pTaskbarList, *ctx->view, ctx);
            }
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int DesktopView::RunWin32MessageLoop() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icex{sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icex);

    ULONG_PTR gdiplusToken = 0;
    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    HICON hAppIcon = CreateRadiumAppIcon();
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = RadiumWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = hAppIcon ? hAppIcon : LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = hAppIcon;
    wc.lpszClassName = L"Radium_MainWindowClass";
    RegisterClassExW(&wc);

    Win32WindowContext ctx{this, &m_controller, nullptr, nullptr, {}, false};
    ctx.ipc_server = m_ipc_server;
    RADIUM_LOG_INFO("UI", "Calling CreateWindowExW for Radium_MainWindowClass");
    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        L"Radium Download Manager",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT,
        m_window_width, m_window_height,
        nullptr, nullptr, hInst, &ctx);
    if (!hwnd) {
        DWORD err = GetLastError();
        RADIUM_LOG_INFO("UI", "ERROR: CreateWindowExW failed with err=" + std::to_string(err));
        return 1;
    }
    RADIUM_LOG_INFO("UI", "CreateWindowExW succeeded: hwnd=" + std::to_string(reinterpret_cast<uintptr_t>(hwnd)));
    ctx.hwnd = hwnd;

    ctx.hThemeTooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        hwnd, nullptr, hInst, nullptr);
    if (ctx.hThemeTooltip) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = hwnd;
        tool.uId = 1;
        tool.lpszText = const_cast<LPWSTR>(L"Switch between Light and Dark appearance");
        const auto geometry = ComputeLayoutGeometry(GetScrollOffset());
        tool.rect = ToWinRect(geometry.theme_button_rect);
        SendMessageW(ctx.hThemeTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }

    ApplyNativeWindowTheme(hwnd, GetPalette());

    ctx.nid.cbSize = sizeof(NOTIFYICONDATAW);
    ctx.nid.hWnd = hwnd;
    ctx.nid.uID = 1;
    ctx.nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    ctx.nid.uCallbackMessage = WM_RADIUM_TRAYICON;
    ctx.nid.hIcon = wc.hIcon;
    wcsncpy_s(ctx.nid.szTip, L"Radium Download Manager", _TRUNCATE);
    ctx.tray_added = (Shell_NotifyIconW(NIM_ADD, &ctx.nid) != FALSE);
    if (ctx.tray_added) {
        NOTIFYICONDATAW nid_ver{};
        nid_ver.cbSize = sizeof(NOTIFYICONDATAW);
        nid_ver.hWnd = hwnd;
        nid_ver.uID = 1;
        nid_ver.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid_ver);
    }

    SetWindowHwnd(hwnd);
    m_controller.SetTaskNotificationCallback([hwnd](const DownloadController::TaskNotification& notice) {
        if (!IsWindow(hwnd)) return;
        auto* payload = new TaskNotificationPayload;
        payload->task_id = notice.task_id;
        payload->success = notice.success;
        payload->title = notice.success ? L"Download complete" : L"Download failed";
        const std::string body = notice.file_name + (notice.success ? " finished downloading" : ": " + notice.message);
        payload->message = Utf8ToW(body);
        if (!PostMessageW(hwnd, WM_RADIUM_TASK_NOTIFICATION, 0, reinterpret_cast<LPARAM>(payload))) delete payload;
    });
    m_controller.SetTorrentMetadataSelectionCallback(
        [hwnd](torrent::TorrentInfo& info) {
            TorrentMetadataSelectionRequest request{&info, false};
            if (!IsWindow(hwnd)) {
                return Result<void>::Err("ERR_TORRENT_SELECTION_UNAVAILABLE", "The torrent file-selection window is unavailable");
            }
            SendMessageW(hwnd, WM_RADIUM_SELECT_TORRENT_METADATA, 0, reinterpret_cast<LPARAM>(&request));
            return request.accepted
                ? Result<void>::Ok()
                : Result<void>::Err("ERR_TORRENT_SELECTION_CANCELED", "Torrent file selection canceled");
        });
    if (m_ipc_server) {
        m_ipc_server->SetSpeculativeDecisionCallback(
            [hwnd](const std::string& tx_id, const TaskModel& draft, const DownloadController::IncomingConflictReport& conflict) {
                auto* offer = new OfferNotificationData(draft);
                offer->speculative_tx_id = tx_id;
                offer->has_precomputed_conflict = true;
                offer->precomputed_conflict = conflict;
                if (!PostMessageW(hwnd, WM_RADIUM_SHOW_NEW_DOWNLOAD, 0, reinterpret_cast<LPARAM>(offer))) {
                    delete offer;
                }
            });
        m_ipc_server->SetOfferNotificationCallback(
            [hwnd, this](const TaskModel& task) {
                // Browser file offers are held as pending drafts until the UI resolves any
                // file conflict or confirms them; `.torrent` offers fetch metainfo first.
                auto* offer = new OfferNotificationData(task);
                PostMessageW(hwnd, WM_RADIUM_SHOW_NEW_DOWNLOAD, 0, reinterpret_cast<LPARAM>(offer));
            });
    }

    SetTimer(hwnd, IDT_UI_REFRESH, 200, nullptr);
    bool start_min = !m_start_visible;
    SetWindowVisibility(!start_min, start_min);
    ShowWindow(hwnd, start_min ? SW_HIDE : SW_SHOWNORMAL);
    if (!start_min) {
        SetForegroundWindow(hwnd);
        if (m_first_launch_onboarding_eligible) {
            PostMessageW(hwnd, WM_RADIUM_CHECK_FIRST_LAUNCH_ONBOARDING, 0, 0);
        }
    }
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return static_cast<int>(msg.wParam);
}
#endif

int DesktopView::RunApplicationLoop(bool headless_exit_immediately) {
    SelectFirstRow();
#ifdef _WIN32
    if (!headless_exit_immediately) {
        return RunWin32MessageLoop();
    }
#endif
    if (headless_exit_immediately) {
        return 0;
    }
    return 0;
}

} // namespace radium::ui
