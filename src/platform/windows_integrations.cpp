// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Windows OS & Workflow Integrations
#include "platform/windows_integrations.hpp"

#include <algorithm>
#include <cctype>
#include <vector>

#ifdef _WIN32
#include <ole2.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace radium::platform {

namespace {

std::string UrlDecodeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out.push_back(' ');
        } else if (s[i] == '%' && i + 2 < s.size() &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out.push_back(static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

#ifdef _WIN32
std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring w(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), needed);
    return w;
}

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int needed = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string s(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), needed, nullptr, nullptr);
    return s;
}

std::optional<uint32_t> ReadRegDword(HKEY root, const wchar_t* subkey, const wchar_t* value_name) {
    DWORD val = 0;
    DWORD sz = sizeof(val);
    DWORD type = 0;
    LSTATUS st = RegGetValueW(root, subkey, value_name, RRF_RT_REG_DWORD, &type, &val, &sz);
    if (st == ERROR_SUCCESS) return static_cast<uint32_t>(val);
    return std::nullopt;
}

bool WriteRegString(HKEY root, const std::wstring& subkey, const wchar_t* value_name, const std::wstring& value) {
    HKEY hkey = nullptr;
    if (RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &hkey, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    LSTATUS st = RegSetValueExW(hkey, value_name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes);
    RegCloseKey(hkey);
    return st == ERROR_SUCCESS;
}

std::wstring GetCurrentExePathW() {
    wchar_t buf[MAX_PATH]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (n > 0) ? std::wstring(buf, n) : L"";
}

class RadiumDropTarget final : public IDropTarget {
public:
    explicit RadiumDropTarget(std::function<void(const std::string&)> on_drop)
        : m_ref_count(1), m_on_drop(std::move(on_drop)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&m_ref_count));
    }

    ULONG STDMETHODCALLTYPE Release() override {
        LONG next = InterlockedDecrement(&m_ref_count);
        if (next == 0) delete this;
        return static_cast<ULONG>(next);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        m_can_accept = CanAccept(pDataObj);
        if (pdwEffect) *pdwEffect = m_can_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* pdwEffect) override {
        if (pdwEffect) *pdwEffect = m_can_accept ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE DragLeave() override {
        m_can_accept = false;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE Drop(IDataObject* pDataObj, DWORD, POINTL, DWORD* pdwEffect) override {
        m_can_accept = false;
        if (!pDataObj) {
            if (pdwEffect) *pdwEffect = DROPEFFECT_NONE;
            return E_INVALIDARG;
        }
        std::string dropped = ExtractDroppedItem(pDataObj);
        if (!dropped.empty() && m_on_drop) {
            m_on_drop(dropped);
            if (pdwEffect) *pdwEffect = DROPEFFECT_COPY;
            return S_OK;
        }
        if (pdwEffect) *pdwEffect = DROPEFFECT_NONE;
        return S_OK;
    }

private:
    static bool CanAccept(IDataObject* pDataObj) {
        if (!pDataObj) return false;
        static const CLIPFORMAT cf_url_w = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocatorW"));
        CLIPFORMAT formats[] = { cf_url_w, CF_UNICODETEXT, CF_HDROP };
        for (CLIPFORMAT cf : formats) {
            FORMATETC fmt{ cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            if (pDataObj->QueryGetData(&fmt) == S_OK) return true;
        }
        return false;
    }

    static std::string ExtractDroppedItem(IDataObject* pDataObj) {
        static const CLIPFORMAT cf_url_w = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocatorW"));
        const CLIPFORMAT text_formats[] = { cf_url_w, static_cast<CLIPFORMAT>(CF_UNICODETEXT) };
        for (CLIPFORMAT cf : text_formats) {
            FORMATETC fmt{ cf, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM stg{};
            if (pDataObj->GetData(&fmt, &stg) == S_OK) {
                std::string out;
                if (stg.hGlobal) {
                    const wchar_t* ptr = static_cast<const wchar_t*>(GlobalLock(stg.hGlobal));
                    if (ptr) {
                        out = ToUtf8(std::wstring(ptr));
                        GlobalUnlock(stg.hGlobal);
                    }
                }
                ReleaseStgMedium(&stg);
                while (!out.empty() && (out.back() == '\r' || out.back() == '\n' || out.back() == ' ')) out.pop_back();
                if (!out.empty()) return out;
            }
        }
        FORMATETC hdrop_fmt{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM hdrop_stg{};
        if (pDataObj->GetData(&hdrop_fmt, &hdrop_stg) == S_OK) {
            std::string out;
            if (hdrop_stg.hGlobal) {
                HDROP hd = static_cast<HDROP>(GlobalLock(hdrop_stg.hGlobal));
                if (hd) {
                    wchar_t path[MAX_PATH]{};
                    if (DragQueryFileW(hd, 0, path, MAX_PATH) > 0) {
                        out = ToUtf8(std::wstring(path));
                    }
                    GlobalUnlock(hdrop_stg.hGlobal);
                }
            }
            ReleaseStgMedium(&hdrop_stg);
            return out;
        }
        return {};
    }

    LONG m_ref_count{1};
    bool m_can_accept{false};
    std::function<void(const std::string&)> m_on_drop;
};
#endif

} // namespace

bool WindowsIntegrations::KeepsZoneInformation(
    std::optional<uint32_t> machine_policy,
    std::optional<uint32_t> user_policy) {
    if (machine_policy.has_value()) return *machine_policy != 1;
    if (user_policy.has_value()) return *user_policy != 1;
    return true;
}

bool WindowsIntegrations::WriteZoneIdentifier(
    const std::filesystem::path& file_path,
    const std::string& url,
    const std::string& referrer,
    bool user_option_enabled) {
    if (!user_option_enabled || file_path.empty()) return false;
#ifdef _WIN32
    constexpr const wchar_t* kAttachPolicy = L"Software\\Microsoft\\Windows\\CurrentVersion\\Policies\\Attachments";
    auto mach = ReadRegDword(HKEY_LOCAL_MACHINE, kAttachPolicy, L"SaveZoneInformation");
    auto user = ReadRegDword(HKEY_CURRENT_USER, kAttachPolicy, L"SaveZoneInformation");
    if (!KeepsZoneInformation(mach, user)) return false;

    std::wstring ads_path = file_path.wstring() + L":Zone.Identifier";
    HANDLE hFile = CreateFileW(
        ads_path.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    std::string content = "[ZoneTransfer]\r\nZoneId=3\r\n";
    if (!referrer.empty() && referrer.find('\r') == std::string::npos && referrer.find('\n') == std::string::npos) {
        content += "ReferrerUrl=" + referrer + "\r\n";
    }
    if (!url.empty() && url.find('\r') == std::string::npos && url.find('\n') == std::string::npos) {
        content += "HostUrl=" + url + "\r\n";
    }
    DWORD written = 0;
    BOOL ok = WriteFile(hFile, content.data(), static_cast<DWORD>(content.size()), &written, nullptr);
    CloseHandle(hFile);
    return ok != FALSE && written == content.size();
#else
    (void)url;
    (void)referrer;
    return false;
#endif
}

void WindowsIntegrations::SetDownloadKeepAlive(bool active) {
#ifdef _WIN32
    if (active) {
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED);
    } else {
        SetThreadExecutionState(ES_CONTINUOUS);
    }
#else
    (void)active;
#endif
}

bool WindowsIntegrations::SetRunAtLogin(bool enable) {
#ifdef _WIN32
    constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    constexpr const wchar_t* kValName = L"RadiumDownloadManager";
    if (!enable) {
        HKEY hkey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &hkey) != ERROR_SUCCESS) {
            return true;
        }
        RegDeleteValueW(hkey, kValName);
        RegCloseKey(hkey);
        return true;
    }
    std::wstring exe = GetCurrentExePathW();
    if (exe.empty()) return false;
    return WriteRegString(HKEY_CURRENT_USER, kRunKey, kValName, L"\"" + exe + L"\" --minimized");
#else
    (void)enable;
    return false;
#endif
}

bool WindowsIntegrations::IsRunAtLoginEnabled() {
#ifdef _WIN32
    constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    DWORD type = 0;
    DWORD sz = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"RadiumDownloadManager", RRF_RT_REG_SZ, &type, nullptr, &sz) == ERROR_SUCCESS;
#else
    return false;
#endif
}

bool WindowsIntegrations::RegisterUrlScheme(const std::string& scheme) {
#ifdef _WIN32
    std::wstring exe = GetCurrentExePathW();
    if (exe.empty() || scheme.empty()) return false;
    std::wstring wscheme = ToWide(scheme);
    std::wstring root = L"Software\\Classes\\" + wscheme;
    if (!WriteRegString(HKEY_CURRENT_USER, root, nullptr, L"URL:Radium Download Manager Protocol")) return false;
    if (!WriteRegString(HKEY_CURRENT_USER, root, L"URL Protocol", L"")) return false;
    return WriteRegString(HKEY_CURRENT_USER, root + L"\\shell\\open\\command", nullptr, L"\"" + exe + L"\" \"%1\"");
#else
    (void)scheme;
    return false;
#endif
}

bool WindowsIntegrations::UnregisterUrlScheme(const std::string& scheme) {
#ifdef _WIN32
    std::wstring root = L"Software\\Classes\\" + ToWide(scheme);
    RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str());
    return true;
#else
    (void)scheme;
    return false;
#endif
}

bool WindowsIntegrations::RegisterTorrentAssociation() {
#ifdef _WIN32
    std::wstring exe = GetCurrentExePathW();
    if (exe.empty()) return false;
    const std::wstring prog_id = L"Radium.torrent";
    const std::wstring expected_cmd = L"\"" + exe + L"\" \"%1\"";

    auto read_default = [](const std::wstring& subkey) -> std::wstring {
        HKEY hKey = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) {
            return {};
        }
        wchar_t buf[1024]{};
        DWORD sz = sizeof(buf);
        DWORD type = 0;
        LONG rc = RegQueryValueExW(hKey, nullptr, nullptr, &type, reinterpret_cast<LPBYTE>(buf), &sz);
        RegCloseKey(hKey);
        if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return {};
        return std::wstring(buf);
    };

    if (read_default(L"Software\\Classes\\.torrent") == prog_id &&
        read_default(L"Software\\Classes\\" + prog_id + L"\\shell\\open\\command") == expected_cmd) {
        return true;
    }

    WriteRegString(HKEY_CURRENT_USER, L"Software\\Classes\\" + prog_id, nullptr, L"BitTorrent Metainfo File");
    WriteRegString(HKEY_CURRENT_USER, L"Software\\Classes\\" + prog_id + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    WriteRegString(HKEY_CURRENT_USER, L"Software\\Classes\\" + prog_id + L"\\shell\\open\\command", nullptr, expected_cmd);
    WriteRegString(HKEY_CURRENT_USER, L"Software\\Classes\\.torrent", nullptr, prog_id);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return true;
#else
    return false;
#endif
}

WindowsIntegrations::LaunchUri WindowsIntegrations::ParseLaunchUri(const std::string& raw) {
    LaunchUri out;
    if (raw.rfind("radium://", 0) != 0) return out;
    out.valid = true;
    std::string rest = raw.substr(9);
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest == "wake") {
        out.wake_only = true;
        return out;
    }
    size_t q = rest.find('?');
    std::string query = (q == std::string::npos) ? "" : rest.substr(q + 1);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        std::string kv = (amp == std::string::npos) ? query.substr(pos) : query.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == "url") {
            out.url = UrlDecodeStr(kv.substr(eq + 1));
            break;
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

void WindowsIntegrations::ExecutePostCompletionAction(
    PostCompletionAction action,
    const std::filesystem::path& target_path) {
#ifdef _WIN32
    if (action == PostCompletionAction::kOpenFile && !target_path.empty()) {
        ShellExecuteW(nullptr, L"open", target_path.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (action == PostCompletionAction::kOpenFolder && !target_path.empty()) {
        std::wstring args = L"/select,\"" + target_path.wstring() + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    } else if (action == PostCompletionAction::kRestartSystem) {
        HANDLE hToken = nullptr;
        if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
            TOKEN_PRIVILEGES tkp{};
            if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tkp.Privileges[0].Luid)) {
                tkp.PrivilegeCount = 1;
                tkp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
                AdjustTokenPrivileges(hToken, FALSE, &tkp, 0, nullptr, nullptr);
            }
            CloseHandle(hToken);
        }
        ExitWindowsEx(EWX_REBOOT | EWX_FORCEIFHUNG, SHTDN_REASON_MAJOR_APPLICATION);
    }
#else
    (void)action;
    (void)target_path;
#endif
}

#ifdef _WIN32
bool WindowsIntegrations::RegisterWindowDropTarget(HWND hwnd, std::function<void(const std::string&)> on_drop) {
    if (!hwnd) return false;
    auto* target = new RadiumDropTarget(std::move(on_drop));
    HRESULT hr = RegisterDragDrop(hwnd, target);
    target->Release(); // RegisterDragDrop holds its own AddRef on success
    return SUCCEEDED(hr);
}

void WindowsIntegrations::RevokeWindowDropTarget(HWND hwnd) {
    if (hwnd) {
        RevokeDragDrop(hwnd);
    }
}
#endif

} // namespace radium::platform
