// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Secure Win32 Process Execution Utility
#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include "common/types.hpp"

namespace radium::process {

#ifdef _WIN32

/**
 * Encodes an argument vector into a single Windows command line according to
 * Microsoft CommandLineToArgvW parsing rules.
 * Handles spaces, empty strings, embedded quotes, and trailing backslashes.
 */
inline std::wstring BuildWindowsCommandLine(const std::vector<std::wstring>& args) {
    std::wstring out;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) out += L' ';
        const std::wstring& arg = args[i];

        // An empty argument must be enclosed in quotes
        if (arg.empty()) {
            out += L"\"\"";
            continue;
        }

        // Check if quoting is necessary
        bool needs_quotes = (arg.find_first_of(L" \t\n\v\"") != std::wstring::npos);
        if (!needs_quotes) {
            out += arg;
            continue;
        }

        out += L'\"';
        size_t backslashes = 0;
        for (wchar_t c : arg) {
            if (c == L'\\') {
                backslashes++;
            } else if (c == L'\"') {
                // 2N + 1 backslashes followed by quote
                out.append(backslashes * 2 + 1, L'\\');
                out += L'\"';
                backslashes = 0;
            } else {
                // N backslashes not followed by quote
                if (backslashes > 0) {
                    out.append(backslashes, L'\\');
                    backslashes = 0;
                }
                out += c;
            }
        }
        // 2N backslashes before closing quote
        if (backslashes > 0) {
            out.append(backslashes * 2, L'\\');
        }
        out += L'\"';
    }
    return out;
}

/**
 * Spawns a process directly using CreateProcessW without shell interpretation.
 * Optionally captures stdout and stderr via an inherited pipe.
 */
inline radium::Result<std::pair<int, std::string>> RunProcessCaptureOutput(
    const std::filesystem::path& executable_path,
    const std::vector<std::wstring>& args,
    const std::filesystem::path& working_dir = {},
    int timeout_ms = 30000) {

    if (!std::filesystem::exists(executable_path)) {
        return radium::Result<std::pair<int, std::string>>::Err(
            "ERR_EXEC_NOT_FOUND", "Executable not found: " + executable_path.string());
    }

    HANDLE hStdOutRead = nullptr, hStdOutWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0)) {
        return radium::Result<std::pair<int, std::string>>::Err(
            "ERR_PIPE", "Failed to create pipe for process output");
    }
    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hStdOutWrite;
    si.hStdError = hStdOutWrite;

    PROCESS_INFORMATION pi{};
    std::wstring cmd_line = BuildWindowsCommandLine(args);
    std::vector<wchar_t> cmd_buf(cmd_line.begin(), cmd_line.end());
    cmd_buf.push_back(L'\0');

    std::wstring wexe = executable_path.wstring();
    LPCWSTR app = wexe.c_str();
    LPCWSTR work_dir = working_dir.empty() ? nullptr : working_dir.c_str();

    BOOL ok = CreateProcessW(
        app, cmd_buf.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, work_dir, &si, &pi);

    CloseHandle(hStdOutWrite);

    if (!ok) {
        CloseHandle(hStdOutRead);
        return radium::Result<std::pair<int, std::string>>::Err(
            "ERR_PROCESS_START", "CreateProcessW failed for: " + executable_path.string());
    }

    std::string output;
    char buffer[4096];
    DWORD bytesRead = 0;
    auto start_time = std::chrono::steady_clock::now();

    while (true) {
        DWORD bytes_avail = 0;
        if (PeekNamedPipe(hStdOutRead, nullptr, 0, nullptr, &bytes_avail, nullptr) && bytes_avail > 0) {
            DWORD to_read = (std::min)(bytes_avail, static_cast<DWORD>(sizeof(buffer) - 1));
            if (ReadFile(hStdOutRead, buffer, to_read, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output.append(buffer, bytesRead);
            }
        }

        DWORD wait_res = WaitForSingleObject(pi.hProcess, 50);
        if (wait_res == WAIT_OBJECT_0) {
            while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, nullptr) && bytesRead > 0) {
                buffer[bytesRead] = '\0';
                output.append(buffer, bytesRead);
            }
            break;
        }

        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start_time).count();
            if (elapsed > timeout_ms) {
                TerminateProcess(pi.hProcess, static_cast<UINT>(-1));
                WaitForSingleObject(pi.hProcess, 1000);
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                CloseHandle(hStdOutRead);
                return radium::Result<std::pair<int, std::string>>::Err(
                    "ERR_PROCESS_TIMEOUT", "Process timed out after " + std::to_string(timeout_ms) + " ms");
            }
        }
    }

    DWORD exit_code = 0;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdOutRead);

    return radium::Result<std::pair<int, std::string>>::Ok(
        std::make_pair(static_cast<int>(exit_code), std::move(output)));
}

#endif // _WIN32

} // namespace radium::process
