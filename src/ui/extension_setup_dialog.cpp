// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Browser Extension Setup & Folder Instructions Dialogs (Win32)
#include "ui/extension_setup_dialog.hpp"

#include "common/path_sanitizer.hpp"

#ifdef _WIN32
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <atomic>
#include <memory>
#include <thread>

namespace openidm::ui {

namespace {

constexpr UINT WM_EXT_PROGRESS = WM_APP + 210;
constexpr UINT WM_EXT_DONE     = WM_APP + 211;

constexpr int IDC_EXT_PRIMARY_BTN  = 8101;
constexpr int IDC_EXT_LATER_BTN    = 8102;
constexpr int IDC_EXT_PROGRESS     = 8103;

constexpr int IDC_INST_PATH_EDIT   = 8201;
constexpr int IDC_INST_OPEN_BTN    = 8202;
constexpr int IDC_INST_COPY_BTN    = 8203;
constexpr int IDC_INST_DONE_BTN    = 8204;

std::wstring Utf8ToW(std::string_view s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring w(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

void ApplyDarkTitleBar(HWND hwnd, const ThemePalette& palette) {
    BOOL dark = (palette.mode == ThemeMode::kDark) ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
}

HFONT CreateUiFont(int px, int weight) {
    return CreateFontW(
        -px, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
}

void DrawOwnerButton(const DRAWITEMSTRUCT* dis, const ThemePalette& pal, bool is_primary, UINT dpi) {
    if (!dis) return;
    HDC hdc = dis->hDC;
    RECT rc = dis->rcItem;
    const bool disabled = (dis->itemState & ODS_DISABLED) != 0;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    const bool focused = (dis->itemState & ODS_FOCUS) != 0;

    ColorRgba bg = is_primary ? pal.accent_primary : pal.surface_elevated;
    ColorRgba border = is_primary ? pal.accent_primary : pal.border_subtle;
    ColorRgba fg = is_primary ? ColorRgba{255, 255, 255, 255} : pal.text_primary;

    if (disabled) {
        bg = pal.surface_primary;
        border = pal.border_subtle;
        fg = pal.text_muted;
    } else if (pressed) {
        bg = is_primary ? pal.accent_hover : pal.surface_pressed;
    } else if (focused) {
        border = pal.border_focus;
    }

    HBRUSH hBgFill = CreateSolidBrush(pal.app_background.ToWin32ColorRef());
    FillRect(hdc, &rc, hBgFill);
    DeleteObject(hBgFill);

    HBRUSH hBrush = CreateSolidBrush(bg.ToWin32ColorRef());
    HPEN hPen = CreatePen(PS_SOLID, 1, border.ToWin32ColorRef());
    HGDIOBJ oldB = SelectObject(hdc, hBrush);
    HGDIOBJ oldP = SelectObject(hdc, hPen);
    int r = DesignSystem::ScaleForDpi(6, dpi);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, r, r);
    SelectObject(hdc, oldB);
    SelectObject(hdc, oldP);
    DeleteObject(hBrush);
    DeleteObject(hPen);

    wchar_t text[128]{};
    GetWindowTextW(dis->hwndItem, text, 128);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, fg.ToWin32ColorRef());
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

struct ExtProgressPayload {
    int percent{0};
    std::string status;
};

struct ExtDonePayload {
    radium::core::ExtensionInstallResult result;
};

struct SetupDialogContext {
    DownloadController* controller{nullptr};
    ThemePalette palette;
    bool is_first_launch{true};
    bool downloading{false};
    bool finished_success{false};
    bool has_error{false};
    int progress_pct{0};
    std::wstring status_text;
    std::wstring error_text;
    radium::core::ExtensionInstallResult install_result;

    HWND hPrimaryBtn{nullptr};
    HWND hLaterBtn{nullptr};
    HWND hProgress{nullptr};
    HFONT hFontTitle{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontCaption{nullptr};
    HBRUSH hBgBrush{nullptr};
    UINT dpi{96};
    std::shared_ptr<std::atomic<bool>> alive_flag;
};

void StartDownloadWorker(HWND hwnd, SetupDialogContext* ctx) {
    if (!ctx || ctx->downloading) return;
    ctx->downloading = true;
    ctx->has_error = false;
    ctx->error_text.clear();
    ctx->progress_pct = 5;
    ctx->status_text = L"Connecting to GitHub...";

    SetWindowTextW(ctx->hPrimaryBtn, L"Downloading...");
    EnableWindow(ctx->hPrimaryBtn, FALSE);
    EnableWindow(ctx->hLaterBtn, FALSE);
    ShowWindow(ctx->hProgress, SW_SHOW);
    SendMessageW(ctx->hProgress, PBM_SETPOS, 5, 0);
    InvalidateRect(hwnd, nullptr, FALSE);

    const std::filesystem::path app_data_dir =
        ctx->controller->GetConfig().state_db_path.parent_path();
    auto alive = ctx->alive_flag;

    std::thread([hwnd, app_data_dir, alive]() {
        auto res = radium::core::ExtensionInstaller::DownloadAndInstall(
            app_data_dir,
            [hwnd, alive](int pct, const std::string& msg) {
                if (!alive->load()) return;
                auto* p = new ExtProgressPayload{pct, msg};
                if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_EXT_PROGRESS, 0, reinterpret_cast<LPARAM>(p))) {
                    delete p;
                }
            });
        if (!alive->load()) return;
        auto* done = new ExtDonePayload{std::move(res)};
        if (!IsWindow(hwnd) || !PostMessageW(hwnd, WM_EXT_DONE, 0, reinterpret_cast<LPARAM>(done))) {
            delete done;
        }
    }).detach();
}

LRESULT CALLBACK SetupDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<SetupDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<SetupDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            ctx->dpi = GetDpiForWindow(hwnd);
            if (ctx->dpi == 0) ctx->dpi = 96;
            ctx->hFontTitle = CreateUiFont(DesignSystem::ScaleForDpi(18, ctx->dpi), FW_SEMIBOLD);
            ctx->hFontBody = CreateUiFont(DesignSystem::ScaleForDpi(14, ctx->dpi), FW_NORMAL);
            ctx->hFontBold = CreateUiFont(DesignSystem::ScaleForDpi(13, ctx->dpi), FW_SEMIBOLD);
            ctx->hFontCaption = CreateUiFont(DesignSystem::ScaleForDpi(12, ctx->dpi), FW_NORMAL);
            ctx->hBgBrush = CreateSolidBrush(ctx->palette.app_background.ToWin32ColorRef());

            RECT rc{};
            GetClientRect(hwnd, &rc);
            int pad = DesignSystem::ScaleForDpi(24, ctx->dpi);
            int btn_h = DesignSystem::ScaleForDpi(34, ctx->dpi);
            int btn_y = rc.bottom - pad - btn_h;
            int btn_w1 = DesignSystem::ScaleForDpi(164, ctx->dpi);
            int btn_w2 = DesignSystem::ScaleForDpi(100, ctx->dpi);
            int gap = DesignSystem::ScaleForDpi(12, ctx->dpi);

            ctx->hProgress = CreateWindowExW(
                0, PROGRESS_CLASSW, nullptr,
                WS_CHILD | PBS_SMOOTH,
                pad, btn_y - DesignSystem::ScaleForDpi(44, ctx->dpi),
                rc.right - 2 * pad, DesignSystem::ScaleForDpi(12, ctx->dpi),
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXT_PROGRESS)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ctx->hProgress, PBM_SETRANGE, 0, MAKELPARAM(0, 100));

            ctx->hPrimaryBtn = CreateWindowExW(
                0, L"BUTTON", L"Download Extension",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                rc.right - pad - btn_w1, btn_y, btn_w1, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXT_PRIMARY_BTN)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ctx->hPrimaryBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

            ctx->hLaterBtn = CreateWindowExW(
                0, L"BUTTON", ctx->is_first_launch ? L"Later" : L"Cancel",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                rc.right - pad - btn_w1 - gap - btn_w2, btn_y, btn_w2, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EXT_LATER_BTN)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ctx->hLaterBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

            if (!ctx->is_first_launch) {
                PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_EXT_PRIMARY_BTN, BN_CLICKED), 0);
            }
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            SetBkMode(hdc, TRANSPARENT);
            int pad = DesignSystem::ScaleForDpi(24, ctx->dpi);
            int y = pad;

            SelectObject(hdc, ctx->hFontTitle);
            SetTextColor(hdc, ctx->palette.text_primary.ToWin32ColorRef());
            RECT title_rc{pad, y, rc.right - pad, y + DesignSystem::ScaleForDpi(30, ctx->dpi)};
            DrawTextW(hdc,
                ctx->is_first_launch ? L"Set up your browser extension" : L"Download or Update Browser Extension",
                -1, &title_rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += DesignSystem::ScaleForDpi(38, ctx->dpi);

            SelectObject(hdc, ctx->hFontBody);
            SetTextColor(hdc, ctx->palette.text_secondary.ToWin32ColorRef());
            RECT body_rc{pad, y, rc.right - pad, y + DesignSystem::ScaleForDpi(48, ctx->dpi)};
            DrawTextW(hdc,
                L"Install the Radium browser extension to detect browser downloads and send them directly to Radium.",
                -1, &body_rc, DT_LEFT | DT_WORDBREAK);
            y += DesignSystem::ScaleForDpi(52, ctx->dpi);

            SelectObject(hdc, ctx->hFontCaption);
            SetTextColor(hdc, ctx->palette.text_muted.ToWin32ColorRef());
            RECT note_rc{pad, y, rc.right - pad, y + DesignSystem::ScaleForDpi(36, ctx->dpi)};
            DrawTextW(hdc,
                L"Source: github.com/alve33/Radium-Download-Manager (Chromium & Firefox MV3). Administrator permissions are not required.",
                -1, &note_rc, DT_LEFT | DT_WORDBREAK);
            y += DesignSystem::ScaleForDpi(42, ctx->dpi);

            if (!ctx->status_text.empty() && !ctx->has_error) {
                SelectObject(hdc, ctx->hFontCaption);
                SetTextColor(hdc, ctx->palette.text_secondary.ToWin32ColorRef());
                RECT st_rc{pad, y, rc.right - pad, y + DesignSystem::ScaleForDpi(24, ctx->dpi)};
                DrawTextW(hdc, ctx->status_text.c_str(), -1, &st_rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            if (ctx->has_error && !ctx->error_text.empty()) {
                SelectObject(hdc, ctx->hFontCaption);
                SetTextColor(hdc, ctx->palette.status_danger.ToWin32ColorRef());
                RECT err_rc{pad, y, rc.right - pad, y + DesignSystem::ScaleForDpi(42, ctx->dpi)};
                DrawTextW(hdc, ctx->error_text.c_str(), -1, &err_rc, DT_LEFT | DT_WORDBREAK);
            }

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && (dis->CtlID == IDC_EXT_PRIMARY_BTN || dis->CtlID == IDC_EXT_LATER_BTN)) {
                DrawOwnerButton(dis, ctx->palette, dis->CtlID == IDC_EXT_PRIMARY_BTN, ctx->dpi);
                return TRUE;
            }
            break;
        }
        case WM_EXT_PROGRESS: {
            std::unique_ptr<ExtProgressPayload> payload(reinterpret_cast<ExtProgressPayload*>(lParam));
            if (ctx && payload) {
                ctx->progress_pct = payload->percent;
                ctx->status_text = Utf8ToW(payload->status);
                SendMessageW(ctx->hProgress, PBM_SETPOS, static_cast<WPARAM>(ctx->progress_pct), 0);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }
        case WM_EXT_DONE: {
            std::unique_ptr<ExtDonePayload> payload(reinterpret_cast<ExtDonePayload*>(lParam));
            if (ctx && payload) {
                ctx->downloading = false;
                if (payload->result.success) {
                    ctx->finished_success = true;
                    ctx->install_result = std::move(payload->result);
                    if (ctx->controller) {
                        ctx->controller->RecordInstalledExtension(
                            ctx->install_result.version,
                            ctx->install_result.commit_sha,
                            ctx->install_result.install_dir.string());
                    }
                    DestroyWindow(hwnd);
                } else {
                    ctx->has_error = true;
                    ctx->error_text = Utf8ToW(payload->result.error_message);
                    ctx->status_text.clear();
                    ShowWindow(ctx->hProgress, SW_HIDE);
                    SetWindowTextW(ctx->hPrimaryBtn, L"Retry Download");
                    SetWindowTextW(ctx->hLaterBtn, ctx->is_first_launch ? L"Later" : L"Close");
                    EnableWindow(ctx->hPrimaryBtn, TRUE);
                    EnableWindow(ctx->hLaterBtn, TRUE);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_EXT_PRIMARY_BTN && !ctx->downloading) {
                StartDownloadWorker(hwnd, ctx);
                return 0;
            }
            if (id == IDC_EXT_LATER_BTN && !ctx->downloading) {
                if (ctx && ctx->is_first_launch && ctx->controller) {
                    ctx->controller->MarkExtensionOnboardingDisplayed("later");
                }
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            if (ctx && ctx->downloading) return 0; // wait for worker to finish or cancel
            if (ctx && ctx->is_first_launch && ctx->controller) {
                ctx->controller->MarkExtensionOnboardingDisplayed("closed");
            }
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (ctx) {
                if (ctx->alive_flag) ctx->alive_flag->store(false);
                if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
                if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
                if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
                if (ctx->hFontCaption) DeleteObject(ctx->hFontCaption);
                if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct InstalledDialogContext {
    std::filesystem::path install_dir;
    std::wstring version_w;
    std::wstring nmh_summary_w;
    bool nmh_ok{false};
    ThemePalette palette;
    std::wstring feedback_text;

    HWND hPathEdit{nullptr};
    HWND hOpenBtn{nullptr};
    HWND hCopyBtn{nullptr};
    HWND hDoneBtn{nullptr};
    HFONT hFontTitle{nullptr};
    HFONT hFontBody{nullptr};
    HFONT hFontBold{nullptr};
    HFONT hFontCaption{nullptr};
    HBRUSH hBgBrush{nullptr};
    HBRUSH hSurfaceBrush{nullptr};
    UINT dpi{96};
};

void CopyWideTextToClipboard(HWND hwnd, const std::wstring& text) {
    if (!OpenClipboard(hwnd)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* ptr = GlobalLock(hMem);
        if (ptr) {
            memcpy(ptr, text.c_str(), bytes);
            GlobalUnlock(hMem);
            SetClipboardData(CF_UNICODETEXT, hMem);
        } else {
            GlobalFree(hMem);
        }
    }
    CloseClipboard();
}

LRESULT CALLBACK InstalledDialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ctx = reinterpret_cast<InstalledDialogContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<InstalledDialogContext*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));

            ctx->dpi = GetDpiForWindow(hwnd);
            if (ctx->dpi == 0) ctx->dpi = 96;
            ctx->hFontTitle = CreateUiFont(DesignSystem::ScaleForDpi(18, ctx->dpi), FW_SEMIBOLD);
            ctx->hFontBody = CreateUiFont(DesignSystem::ScaleForDpi(13, ctx->dpi), FW_NORMAL);
            ctx->hFontBold = CreateUiFont(DesignSystem::ScaleForDpi(13, ctx->dpi), FW_SEMIBOLD);
            ctx->hFontCaption = CreateUiFont(DesignSystem::ScaleForDpi(12, ctx->dpi), FW_NORMAL);
            ctx->hBgBrush = CreateSolidBrush(ctx->palette.app_background.ToWin32ColorRef());
            ctx->hSurfaceBrush = CreateSolidBrush(ctx->palette.surface_elevated.ToWin32ColorRef());

            RECT rc{};
            GetClientRect(hwnd, &rc);
            int pad = DesignSystem::ScaleForDpi(24, ctx->dpi);
            int edit_y = DesignSystem::ScaleForDpi(118, ctx->dpi);
            int edit_h = DesignSystem::ScaleForDpi(28, ctx->dpi);

            ctx->hPathEdit = CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", ctx->install_dir.wstring().c_str(),
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_READONLY,
                pad, edit_y, rc.right - 2 * pad, edit_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INST_PATH_EDIT)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ctx->hPathEdit, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBody), TRUE);

            int row_btn_y = edit_y + edit_h + DesignSystem::ScaleForDpi(10, ctx->dpi);
            int btn_h = DesignSystem::ScaleForDpi(32, ctx->dpi);
            int open_w = DesignSystem::ScaleForDpi(126, ctx->dpi);
            int copy_w = DesignSystem::ScaleForDpi(114, ctx->dpi);
            int gap = DesignSystem::ScaleForDpi(10, ctx->dpi);

            ctx->hOpenBtn = CreateWindowExW(
                0, L"BUTTON", L"Open Folder",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                pad, row_btn_y, open_w, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INST_OPEN_BTN)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ctx->hOpenBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

            ctx->hCopyBtn = CreateWindowExW(
                0, L"BUTTON", L"Copy Path",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                pad + open_w + gap, row_btn_y, copy_w, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INST_COPY_BTN)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ctx->hCopyBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);

            int done_w = DesignSystem::ScaleForDpi(104, ctx->dpi);
            int done_y = rc.bottom - pad - btn_h;
            ctx->hDoneBtn = CreateWindowExW(
                0, L"BUTTON", L"Done",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                rc.right - pad - done_w, done_y, done_w, btn_h,
                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INST_DONE_BTN)),
                GetModuleHandleW(nullptr), nullptr);
            SendMessageW(ctx->hDoneBtn, WM_SETFONT, reinterpret_cast<WPARAM>(ctx->hFontBold), TRUE);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, ctx->palette.text_primary.ToWin32ColorRef());
            SetBkColor(hdc, ctx->palette.surface_elevated.ToWin32ColorRef());
            return reinterpret_cast<LRESULT>(ctx->hSurfaceBrush);
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, ctx->hBgBrush);

            SetBkMode(hdc, TRANSPARENT);
            int pad = DesignSystem::ScaleForDpi(24, ctx->dpi);
            int y = pad;

            SelectObject(hdc, ctx->hFontTitle);
            SetTextColor(hdc, ctx->palette.text_primary.ToWin32ColorRef());
            std::wstring title = L"Browser Extension Downloaded (v" + ctx->version_w + L")";
            RECT title_rc{pad, y, rc.right - pad, y + DesignSystem::ScaleForDpi(28, ctx->dpi)};
            DrawTextW(hdc, title.c_str(), -1, &title_rc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += DesignSystem::ScaleForDpi(34, ctx->dpi);

            SelectObject(hdc, ctx->hFontBody);
            SetTextColor(hdc, ctx->palette.text_secondary.ToWin32ColorRef());
            RECT sub_rc{pad, y, rc.right - pad, y + DesignSystem::ScaleForDpi(36, ctx->dpi)};
            DrawTextW(hdc,
                L"Extension files were downloaded to your computer. Complete setup by loading the unpacked extension in your browser:",
                -1, &sub_rc, DT_LEFT | DT_WORDBREAK);

            if (!ctx->feedback_text.empty()) {
                SelectObject(hdc, ctx->hFontCaption);
                SetTextColor(hdc, ctx->palette.accent_primary.ToWin32ColorRef());
                RECT fb_rc{pad + DesignSystem::ScaleForDpi(264, ctx->dpi),
                           DesignSystem::ScaleForDpi(164, ctx->dpi),
                           rc.right - pad,
                           DesignSystem::ScaleForDpi(188, ctx->dpi)};
                DrawTextW(hdc, ctx->feedback_text.c_str(), -1, &fb_rc, DT_LEFT | DT_SINGLELINE);
            }

            int inst_y = DesignSystem::ScaleForDpi(204, ctx->dpi);
            SelectObject(hdc, ctx->hFontBold);
            SetTextColor(hdc, ctx->palette.text_primary.ToWin32ColorRef());
            RECT hdr_rc{pad, inst_y, rc.right - pad, inst_y + DesignSystem::ScaleForDpi(22, ctx->dpi)};
            DrawTextW(hdc, L"How to install in your browser:", -1, &hdr_rc, DT_LEFT | DT_SINGLELINE);
            inst_y += DesignSystem::ScaleForDpi(26, ctx->dpi);

            SelectObject(hdc, ctx->hFontCaption);
            SetTextColor(hdc, ctx->palette.text_secondary.ToWin32ColorRef());
            RECT steps_rc{pad, inst_y, rc.right - pad, inst_y + DesignSystem::ScaleForDpi(118, ctx->dpi)};
            DrawTextW(hdc,
                L"Chrome / Edge / Brave (Chromium):\n"
                L"  1. Open chrome://extensions (or edge://extensions, brave://extensions).\n"
                L"  2. Enable Developer mode (top-right toggle).\n"
                L"  3. Click \"Load unpacked\" and select the folder path above.\n\n"
                L"Mozilla Firefox:\n"
                L"  Open about:debugging#/runtime/this-firefox, click \"Load Temporary Add-on...\", and select manifest.firefox.json.",
                -1, &steps_rc, DT_LEFT | DT_WORDBREAK);

            int nmh_y = inst_y + DesignSystem::ScaleForDpi(126, ctx->dpi);
            SelectObject(hdc, ctx->hFontCaption);
            SetTextColor(hdc, ctx->nmh_ok
                ? ctx->palette.text_secondary.ToWin32ColorRef()
                : ctx->palette.status_warning.ToWin32ColorRef());
            RECT nmh_rc{pad, nmh_y, rc.right - pad, nmh_y + DesignSystem::ScaleForDpi(30, ctx->dpi)};
            DrawTextW(hdc, ctx->nmh_summary_w.c_str(), -1, &nmh_rc, DT_LEFT | DT_WORDBREAK);

            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (dis && (dis->CtlID == IDC_INST_OPEN_BTN || dis->CtlID == IDC_INST_COPY_BTN || dis->CtlID == IDC_INST_DONE_BTN)) {
                DrawOwnerButton(dis, ctx->palette, dis->CtlID == IDC_INST_DONE_BTN, ctx->dpi);
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_INST_OPEN_BTN) {
                ShellExecuteW(hwnd, L"open", ctx->install_dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                return 0;
            }
            if (id == IDC_INST_COPY_BTN) {
                CopyWideTextToClipboard(hwnd, ctx->install_dir.wstring());
                ctx->feedback_text = L"Path copied to clipboard.";
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (id == IDC_INST_DONE_BTN) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            if (ctx) {
                if (ctx->hFontTitle) DeleteObject(ctx->hFontTitle);
                if (ctx->hFontBody) DeleteObject(ctx->hFontBody);
                if (ctx->hFontBold) DeleteObject(ctx->hFontBold);
                if (ctx->hFontCaption) DeleteObject(ctx->hFontCaption);
                if (ctx->hBgBrush) DeleteObject(ctx->hBgBrush);
                if (ctx->hSurfaceBrush) DeleteObject(ctx->hSurfaceBrush);
            }
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RunModalLoop(HWND hwnd_parent, HWND hDlg) {
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        EnableWindow(hwnd_parent, FALSE);
    }
    SetForegroundWindow(hDlg);

    MSG msg{};
    while (IsWindow(hDlg) && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            SendMessageW(hDlg, WM_CLOSE, 0, 0);
            continue;
        }
        if (!IsDialogMessageW(hDlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (msg.message == WM_QUIT) {
        PostQuitMessage(static_cast<int>(msg.wParam));
    }
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        EnableWindow(hwnd_parent, TRUE);
        SetForegroundWindow(hwnd_parent);
    }
}

} // namespace

void ShowExtensionSetupDialogWin32(
    HWND hwnd_parent,
    DownloadController& controller,
    const ThemePalette& palette,
    bool is_first_launch) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SetupDialogProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Radium_ExtensionSetupDialogClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    if (is_first_launch) {
        // Persist immediately so crash/close/failed download never re-triggers first-launch onboarding.
        controller.MarkExtensionOnboardingDisplayed("displayed");
    }

    SetupDialogContext ctx;
    ctx.controller = &controller;
    ctx.palette = palette;
    ctx.is_first_launch = is_first_launch;
    ctx.alive_flag = std::make_shared<std::atomic<bool>>(true);

    UINT dpi = (hwnd_parent && IsWindow(hwnd_parent)) ? GetDpiForWindow(hwnd_parent) : 96;
    if (dpi == 0) dpi = 96;
    RECT prc{};
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        GetWindowRect(hwnd_parent, &prc);
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &prc, 0);
    }
    int dlg_w = DesignSystem::ScaleForDpi(540, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(280, dpi);
    int dlg_x = prc.left + (prc.right - prc.left - dlg_w) / 2;
    int dlg_y = prc.top + (prc.bottom - prc.top - dlg_h) / 2;

    HWND hDlg = CreateWindowExW(
        (hwnd_parent ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT,
        L"Radium_ExtensionSetupDialogClass",
        is_first_launch ? L"Browser Extension Setup" : L"Download Browser Extension",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        hwnd_parent, nullptr, hInst, &ctx);
    if (!hDlg) return;

    ApplyDarkTitleBar(hDlg, palette);
    RunModalLoop(hwnd_parent, hDlg);

    if (ctx.finished_success) {
        const auto exe_dir = PathSanitizer::GetExecutableDir(nullptr);
        const auto app_data_dir = controller.GetConfig().state_db_path.parent_path();
        auto nmh_status = radium::core::ExtensionInstaller::VerifyOrRepairNativeMessaging(exe_dir, app_data_dir);
        ShowExtensionInstalledDialogWin32(
            hwnd_parent,
            ctx.install_result.install_dir,
            ctx.install_result.version,
            nmh_status,
            palette);
    }
}

void ShowExtensionInstalledDialogWin32(
    HWND hwnd_parent,
    const std::filesystem::path& install_dir,
    const std::string& version,
    const radium::core::NativeMessagingStatus& nmh_status,
    const ThemePalette& palette) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = InstalledDialogProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"Radium_ExtensionInstalledDialogClass";
        RegisterClassExW(&wc);
        registered = true;
    }

    InstalledDialogContext ctx;
    ctx.install_dir = install_dir;
    ctx.version_w = Utf8ToW(version.empty() ? "1.0.0" : version);
    ctx.nmh_summary_w = Utf8ToW(nmh_status.summary);
    ctx.nmh_ok = nmh_status.host_executable_found && nmh_status.registry_configured && nmh_status.manifest_valid;
    ctx.palette = palette;

    UINT dpi = (hwnd_parent && IsWindow(hwnd_parent)) ? GetDpiForWindow(hwnd_parent) : 96;
    if (dpi == 0) dpi = 96;
    RECT prc{};
    if (hwnd_parent && IsWindow(hwnd_parent)) {
        GetWindowRect(hwnd_parent, &prc);
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &prc, 0);
    }
    int dlg_w = DesignSystem::ScaleForDpi(620, dpi);
    int dlg_h = DesignSystem::ScaleForDpi(420, dpi);
    int dlg_x = prc.left + (prc.right - prc.left - dlg_w) / 2;
    int dlg_y = prc.top + (prc.bottom - prc.top - dlg_h) / 2;

    HWND hDlg = CreateWindowExW(
        (hwnd_parent ? 0 : WS_EX_APPWINDOW) | WS_EX_CONTROLPARENT,
        L"Radium_ExtensionInstalledDialogClass",
        L"Browser Extension Ready",
        WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        dlg_x, dlg_y, dlg_w, dlg_h,
        hwnd_parent, nullptr, hInst, &ctx);
    if (!hDlg) return;

    ApplyDarkTitleBar(hDlg, palette);
    RunModalLoop(hwnd_parent, hDlg);
}

} // namespace openidm::ui
#endif
