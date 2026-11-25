// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Admin-Only Download Startup Log Dialog Implementation
#include "ui/startup_log_dialog.hpp"
#include "common/startup_diagnostic.hpp"
#include "security/admin_check.hpp"
#include "ui/design_system.hpp"
#include <algorithm>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <fstream>
#include <iomanip>
#include <sstream>

#ifdef _WIN32

namespace radium::ui {

namespace {

inline std::wstring Utf8ToW(std::string_view sv) {
    if (sv.empty()) return {};
    int count = MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring out(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, sv.data(), static_cast<int>(sv.size()), out.data(), count);
    return out;
}

constexpr int kDlgStartupRefreshBtn = 9101;
constexpr int kDlgStartupClearBtn   = 9102;
constexpr int kDlgStartupExportJson = 9103;
constexpr int kDlgStartupExportCsv  = 9104;
constexpr int kDlgStartupCloseBtn   = 9105;
constexpr int kDlgStartupReportList = 9106;
constexpr int kDlgStartupEventList  = 9107;

struct StartupLogDialogContext {
    HWND hwnd{nullptr};
    HWND hReportList{nullptr};
    HWND hEventList{nullptr};
    HWND hRefreshBtn{nullptr};
    HWND hClearBtn{nullptr};
    HWND hExportJsonBtn{nullptr};
    HWND hExportCsvBtn{nullptr};
    HWND hCloseBtn{nullptr};

    HFONT hFontHeading{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontMono{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hHeaderBrush{nullptr};

    uint32_t dpi{96};
    DownloadController* controller{nullptr};
    ThemePalette palette{DesignSystem::GetRedesignPalette()};
    std::vector<diagnostics::StartupReport> reports;
    int selected_report_idx{-1};

    void RefreshReports() {
        reports = diagnostics::StartupDiagnosticRecorder::Instance().GetReports();
        selected_report_idx = reports.empty() ? -1 : static_cast<int>(reports.size() - 1);
        PopulateReportList();
        PopulateEventList();
    }

    void PopulateReportList() {
        if (!hReportList) return;
        SendMessageW(hReportList, LVM_DELETEALLITEMS, 0, 0);

        for (size_t i = 0; i < reports.size(); ++i) {
            const auto& rep = reports[i];
            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = static_cast<int>(i);
            lvi.lParam = static_cast<LPARAM>(i);

            // Col 0: File Name / ID
            std::wstring wname = Utf8ToW(rep.filename + " [" + rep.download_id + "]");
            lvi.pszText = const_cast<LPWSTR>(wname.c_str());
            SendMessageW(hReportList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvi));

            // Col 1: Type
            std::wstring wtype = Utf8ToW(rep.download_type);
            ListView_SetItemText(hReportList, static_cast<int>(i), 1, const_cast<LPWSTR>(wtype.c_str()));

            // Col 2: Startup Time
            std::wostringstream ss_st;
            ss_st << std::fixed << std::setprecision(1) << rep.startup_time_ms << L" ms";
            std::wstring wst = ss_st.str();
            ListView_SetItemText(hReportList, static_cast<int>(i), 2, const_cast<LPWSTR>(wst.c_str()));

            // Col 3: Transfer Speed
            std::wostringstream ss_spd;
            if (rep.avg_speed_mbps > 0.0) {
                ss_spd << std::fixed << std::setprecision(2) << rep.avg_speed_mbps << L" MB/s";
            } else {
                ss_spd << L"—";
            }
            std::wstring wspd = ss_spd.str();
            ListView_SetItemText(hReportList, static_cast<int>(i), 3, const_cast<LPWSTR>(wspd.c_str()));

            // Col 4: Status
            std::wstring wstatus = Utf8ToW(rep.status);
            ListView_SetItemText(hReportList, static_cast<int>(i), 4, const_cast<LPWSTR>(wstatus.c_str()));
        }

        if (selected_report_idx >= 0 && selected_report_idx < static_cast<int>(reports.size())) {
            ListView_SetItemState(hReportList, selected_report_idx, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(hReportList, selected_report_idx, FALSE);
        }
    }

    void PopulateEventList() {
        if (!hEventList) return;
        SendMessageW(hEventList, LVM_DELETEALLITEMS, 0, 0);

        if (selected_report_idx < 0 || selected_report_idx >= static_cast<int>(reports.size())) {
            return;
        }

        auto rep = reports[selected_report_idx];
        std::stable_sort(rep.events.begin(), rep.events.end(), [](const diagnostics::StartupEvent& a, const diagnostics::StartupEvent& b) {
            return a.elapsed_us < b.elapsed_us;
        });

        uint64_t prev_elapsed = 0;
        for (size_t i = 0; i < rep.events.size(); ++i) {
            auto& ev = rep.events[i];
            if (i == 0) {
                ev.duration_us = ev.elapsed_us;
            } else {
                ev.duration_us = (ev.elapsed_us >= prev_elapsed) ? (ev.elapsed_us - prev_elapsed) : 0;
            }
            prev_elapsed = ev.elapsed_us;

            LVITEMW lvi{};
            lvi.mask = LVIF_TEXT;
            lvi.iItem = static_cast<int>(i);

            // Col 0: Event Name
            std::wstring wev = Utf8ToW(ev.event_name);
            lvi.pszText = const_cast<LPWSTR>(wev.c_str());
            SendMessageW(hEventList, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&lvi));

            // Col 1: Elapsed
            std::wostringstream ss_el;
            ss_el << std::fixed << std::setprecision(2) << (static_cast<double>(ev.elapsed_us) / 1000.0) << L" ms";
            std::wstring wel = ss_el.str();
            ListView_SetItemText(hEventList, static_cast<int>(i), 1, const_cast<LPWSTR>(wel.c_str()));

            // Col 2: Delta
            std::wostringstream ss_dur;
            ss_dur << L"+" << std::fixed << std::setprecision(2) << (static_cast<double>(ev.duration_us) / 1000.0) << L" ms";
            std::wstring wdur = ss_dur.str();
            ListView_SetItemText(hEventList, static_cast<int>(i), 2, const_cast<LPWSTR>(wdur.c_str()));

            // Col 3: Details
            std::wstring wdet = Utf8ToW(ev.detail);
            ListView_SetItemText(hEventList, static_cast<int>(i), 3, const_cast<LPWSTR>(wdet.c_str()));
        }
    }
};

LRESULT CALLBACK StartupLogDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<StartupLogDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        ctx = static_cast<StartupLogDialogContext*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    if (!ctx) return DefWindowProcW(hwnd, msg, wParam, lParam);

    switch (msg) {
        case WM_CREATE: {
            ctx->hwnd = hwnd;
            ctx->dpi = GetDpiForWindow(hwnd);
            if (ctx->dpi == 0) ctx->dpi = 96;

            ctx->hFontHeading = CreateFontW(-DesignSystem::ScaleForDpi(16, ctx->dpi), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontBody = CreateFontW(-DesignSystem::ScaleForDpi(13, ctx->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
            ctx->hFontMono = CreateFontW(-DesignSystem::ScaleForDpi(12, ctx->dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");

            ctx->hBgBrush = CreateSolidBrush(ctx->palette.app_background.ToWin32ColorRef());
            ctx->hHeaderBrush = CreateSolidBrush(ctx->palette.surface_primary.ToWin32ColorRef());

            HINSTANCE hInst = GetModuleHandleW(nullptr);
            int margin = DesignSystem::ScaleForDpi(16, ctx->dpi);
            int btn_h = DesignSystem::ScaleForDpi(30, ctx->dpi);
            int btn_w = DesignSystem::ScaleForDpi(110, ctx->dpi);

            // Left: Download Sessions List
            int left_w = DesignSystem::ScaleForDpi(360, ctx->dpi);
            int top_y = DesignSystem::ScaleForDpi(50, ctx->dpi);
            int bot_margin = DesignSystem::ScaleForDpi(55, ctx->dpi);

            RECT client_rc{};
            GetClientRect(hwnd, &client_rc);
            int list_h = client_rc.bottom - top_y - bot_margin;

            ctx->hReportList = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                margin, top_y, left_w, list_h, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDlgStartupReportList)), hInst, nullptr);

            ListView_SetExtendedListViewStyle(ctx->hReportList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            SendMessageW(ctx->hReportList, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            ListView_SetBkColor(ctx->hReportList, ctx->palette.surface_elevated.ToWin32ColorRef());
            ListView_SetTextBkColor(ctx->hReportList, ctx->palette.surface_elevated.ToWin32ColorRef());
            ListView_SetTextColor(ctx->hReportList, ctx->palette.text_primary.ToWin32ColorRef());

            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.cx = DesignSystem::ScaleForDpi(140, ctx->dpi);
            col.pszText = const_cast<LPWSTR>(L"Download / Task");
            ListView_InsertColumn(ctx->hReportList, 0, &col);

            col.cx = DesignSystem::ScaleForDpi(55, ctx->dpi);
            col.pszText = const_cast<LPWSTR>(L"Type");
            ListView_InsertColumn(ctx->hReportList, 1, &col);

            col.cx = DesignSystem::ScaleForDpi(75, ctx->dpi);
            col.pszText = const_cast<LPWSTR>(L"Startup");
            ListView_InsertColumn(ctx->hReportList, 2, &col);

            col.cx = DesignSystem::ScaleForDpi(75, ctx->dpi);
            col.pszText = const_cast<LPWSTR>(L"Speed");
            ListView_InsertColumn(ctx->hReportList, 3, &col);

            col.cx = DesignSystem::ScaleForDpi(65, ctx->dpi);
            col.pszText = const_cast<LPWSTR>(L"Status");
            ListView_InsertColumn(ctx->hReportList, 4, &col);

            // Right: Event Lifecycle Breakdown Table
            int right_x = margin + left_w + DesignSystem::ScaleForDpi(16, ctx->dpi);
            int right_w = client_rc.right - right_x - margin;

            ctx->hEventList = CreateWindowExW(
                WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                right_x, top_y, right_w, list_h, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDlgStartupEventList)), hInst, nullptr);

            ListView_SetExtendedListViewStyle(ctx->hEventList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);
            SendMessageW(ctx->hEventList, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontMono), TRUE);
            ListView_SetBkColor(ctx->hEventList, ctx->palette.surface_elevated.ToWin32ColorRef());
            ListView_SetTextBkColor(ctx->hEventList, ctx->palette.surface_elevated.ToWin32ColorRef());
            ListView_SetTextColor(ctx->hEventList, ctx->palette.text_primary.ToWin32ColorRef());

            LVCOLUMNW ev_col{};
            ev_col.mask = LVCF_TEXT | LVCF_WIDTH;
            ev_col.cx = DesignSystem::ScaleForDpi(220, ctx->dpi);
            ev_col.pszText = const_cast<LPWSTR>(L"Lifecycle Stage / Network Event");
            ListView_InsertColumn(ctx->hEventList, 0, &ev_col);

            ev_col.cx = DesignSystem::ScaleForDpi(90, ctx->dpi);
            ev_col.pszText = const_cast<LPWSTR>(L"Elapsed");
            ListView_InsertColumn(ctx->hEventList, 1, &ev_col);

            ev_col.cx = DesignSystem::ScaleForDpi(85, ctx->dpi);
            ev_col.pszText = const_cast<LPWSTR>(L"Delta");
            ListView_InsertColumn(ctx->hEventList, 2, &ev_col);

            ev_col.cx = DesignSystem::ScaleForDpi(320, ctx->dpi);
            ev_col.pszText = const_cast<LPWSTR>(L"Diagnostics / Details");
            ListView_InsertColumn(ctx->hEventList, 3, &ev_col);

            // Bottom Buttons
            int btn_y = client_rc.bottom - bot_margin + DesignSystem::ScaleForDpi(12, ctx->dpi);
            ctx->hRefreshBtn = CreateWindowExW(0, L"BUTTON", L"Refresh", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                margin, btn_y, btn_w, btn_h, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDlgStartupRefreshBtn)), hInst, nullptr);

            ctx->hClearBtn = CreateWindowExW(0, L"BUTTON", L"Clear Logs", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                margin + btn_w + DesignSystem::ScaleForDpi(10, ctx->dpi), btn_y, btn_w, btn_h, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDlgStartupClearBtn)), hInst, nullptr);

            ctx->hExportJsonBtn = CreateWindowExW(0, L"BUTTON", L"Export JSON", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                margin + (btn_w + DesignSystem::ScaleForDpi(10, ctx->dpi)) * 2, btn_y, btn_w, btn_h, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDlgStartupExportJson)), hInst, nullptr);

            ctx->hExportCsvBtn = CreateWindowExW(0, L"BUTTON", L"Export CSV", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
                margin + (btn_w + DesignSystem::ScaleForDpi(10, ctx->dpi)) * 3, btn_y, btn_w, btn_h, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDlgStartupExportCsv)), hInst, nullptr);

            ctx->hCloseBtn = CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                client_rc.right - margin - btn_w, btn_y, btn_w, btn_h, hwnd,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kDlgStartupCloseBtn)), hInst, nullptr);

            SendMessageW(ctx->hRefreshBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hClearBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hExportJsonBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hExportCsvBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);
            SendMessageW(ctx->hCloseBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);

            ctx->RefreshReports();
            return 0;
        }

        case WM_NOTIFY: {
            auto* pnm = reinterpret_cast<NMHDR*>(lParam);
            if (pnm->idFrom == kDlgStartupReportList && (pnm->code == LVN_ITEMCHANGED || pnm->code == NM_CLICK)) {
                int sel = ListView_GetNextItem(ctx->hReportList, -1, LVNI_SELECTED);
                if (sel >= 0 && sel < static_cast<int>(ctx->reports.size())) {
                    ctx->selected_report_idx = sel;
                    ctx->PopulateEventList();
                }
            }
            return 0;
        }

        case WM_COMMAND: {
            int id = LOWORD(wParam);
            int code = HIWORD(wParam);
            if (id == kDlgStartupCloseBtn || (id == IDCANCEL && code == 0)) {
                EndDialog(hwnd, 0);
                DestroyWindow(hwnd);
                return 0;
            } else if (id == kDlgStartupRefreshBtn) {
                ctx->RefreshReports();
                return 0;
            } else if (id == kDlgStartupClearBtn) {
                if (MessageBoxW(hwnd, L"Are you sure you want to clear all startup diagnostic lifecycle reports?",
                                L"Clear Startup Logs", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    diagnostics::StartupDiagnosticRecorder::Instance().ClearReports();
                    ctx->RefreshReports();
                }
                return 0;
            } else if (id == kDlgStartupExportJson) {
                wchar_t path[MAX_PATH] = L"radium_startup_log.json";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"JSON Files (*.json)\0*.json\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameW(&ofn)) {
                    std::string json_data = diagnostics::StartupDiagnosticRecorder::Instance().ExportToJson();
                    std::ofstream out(std::filesystem::path(ofn.lpstrFile), std::ios::binary);
                    if (out.is_open()) {
                        out << json_data;
                        MessageBoxW(hwnd, L"Startup performance report exported successfully to JSON.",
                                    L"Export Completed", MB_OK | MB_ICONINFORMATION);
                    }
                }
                return 0;
            } else if (id == kDlgStartupExportCsv) {
                wchar_t path[MAX_PATH] = L"radium_startup_log.csv";
                OPENFILENAMEW ofn{};
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFilter = L"CSV Files (*.csv)\0*.csv\0All Files (*.*)\0*.*\0";
                ofn.lpstrFile = path;
                ofn.nMaxFile = MAX_PATH;
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
                if (GetSaveFileNameW(&ofn)) {
                    std::string csv_data = diagnostics::StartupDiagnosticRecorder::Instance().ExportToCsv();
                    std::ofstream out(std::filesystem::path(ofn.lpstrFile), std::ios::binary);
                    if (out.is_open()) {
                        out << csv_data;
                        MessageBoxW(hwnd, L"Startup performance report exported successfully to CSV.",
                                    L"Export Completed", MB_OK | MB_ICONINFORMATION);
                    }
                }
                return 0;
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT client_rc;
            GetClientRect(hwnd, &client_rc);
            FillRect(hdc, &client_rc, ctx->hBgBrush);

            // Title Banner
            RECT header_rc = client_rc;
            header_rc.bottom = DesignSystem::ScaleForDpi(42, ctx->dpi);
            FillRect(hdc, &header_rc, ctx->hHeaderBrush);

            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, ctx->palette.text_primary.ToWin32ColorRef());
            SelectObject(hdc, ctx->hFontHeading);

            RECT title_rc = header_rc;
            title_rc.left += DesignSystem::ScaleForDpi(16, ctx->dpi);
            DrawTextW(hdc, L"Administrator Download Startup Log & Performance Diagnostic", -1, &title_rc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            // Admin badge in header
            RECT badge_rc = header_rc;
            badge_rc.right -= DesignSystem::ScaleForDpi(16, ctx->dpi);
            SetTextColor(hdc, RGB(0x56, 0xC4, 0x82)); // Accent green
            SelectObject(hdc, ctx->hFontBody);
            DrawTextW(hdc, L"✓ Admin Access Verified", -1, &badge_rc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY: {
            if (ctx->hFontHeading) DeleteObject(ctx->hFontHeading);
            if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
            if (ctx->hFontMono) DeleteObject(ctx->hFontMono);
            if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            if (ctx->hHeaderBrush) DeleteObject(ctx->hHeaderBrush);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowStartupLogDialogWin32(HWND hwnd_parent, DownloadController& controller, const ThemePalette& palette) {
    // 1. Strict Administrator Verification Check (Enforces user privilege policy)
    if (!security::IsCurrentUserLocalAdmin()) {
        MessageBoxW(
            hwnd_parent,
            L"Access Denied: The Download Startup Log and Performance Diagnostic is restricted strictly to local administrators.\n\n"
            L"Your Windows account does not belong to the local Administrators group.",
            L"Administrator Access Required",
            MB_OK | MB_ICONSTOP);
        return;
    }

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = StartupLogDlgProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Radium_StartupLogDialogClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    StartupLogDialogContext ctx;
    ctx.controller = &controller;
    ctx.palette = palette;

    UINT dpi = 96;
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        dpi = GetDpiForWindow(hwnd_parent);
    }
    if (dpi == 0) dpi = 96;

    RECT prc{};
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        GetWindowRect(hwnd_parent, &prc);
    } else {
        prc.left = 100; prc.top = 100; prc.right = 1100; prc.bottom = 750;
    }

    int dlg_w = DesignSystem::ScaleForDpi(1100, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(650, dpi);
    int dlg_x = prc.left + (prc.right - prc.left - dlg_w) / 2;
    int dlg_y = prc.top + (prc.bottom - prc.top - dlg_h) / 2;

    HWND hDlg = CreateWindowExW(
        (hwnd_parent ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT,
        L"Radium_StartupLogDialogClass",
        L"Download Startup Log & Performance Report (Administrator)",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        hwnd_parent, nullptr, hInst, &ctx);

    if (hwnd_parent) EnableWindow(hwnd_parent, FALSE);

    MSG msg;
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    if (hwnd_parent) {
        EnableWindow(hwnd_parent, TRUE);
        SetForegroundWindow(hwnd_parent);
    }
}

} // namespace radium::ui

#endif
