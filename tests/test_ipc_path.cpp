// SPDX-License-Identifier: Apache-2.0
// Dedicated Unit Test for IPC Path Resolution and Environment Variable Precedence (RDM-02)

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <iostream>
#include <string>
#include <cstdlib>
#include <cassert>

#include "common/ipc_path.hpp"

#ifdef _WIN32
static void SetEnvVar(const char* name, const char* value) {
    if (value && value[0] != '\0') {
        std::string assign = std::string(name) + "=" + value;
        _putenv(assign.c_str());
    } else {
        std::string clear = std::string(name) + "=";
        _putenv(clear.c_str());
    }
}
#else
static void SetEnvVar(const char* name, const char* value) {
    if (value && value[0] != '\0') {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}
#endif

#define TEST_ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            std::cerr << "FAILED: " << msg << " (" << #cond << ") at line " << __LINE__ << std::endl; \
            return false; \
        } \
    } while (0)

bool TestIpcPathPrecedenceAndFallbacks() {
    // Save original environment
    const char* orig_radium = std::getenv("RADIUM_IPC_SOCKET");
    const char* orig_flux = std::getenv("FLUX_IPC_SOCKET");
    const char* orig_openidm = std::getenv("OPENIDM_IPC_SOCKET");

    std::string s_orig_radium = orig_radium ? orig_radium : "";
    std::string s_orig_flux = orig_flux ? orig_flux : "";
    std::string s_orig_openidm = orig_openidm ? orig_openidm : "";

    auto cleanup = [&]() {
        SetEnvVar("RADIUM_IPC_SOCKET", s_orig_radium.c_str());
        SetEnvVar("FLUX_IPC_SOCKET", s_orig_flux.c_str());
        SetEnvVar("OPENIDM_IPC_SOCKET", s_orig_openidm.c_str());
    };

    // Case 1: All three set -> RADIUM_IPC_SOCKET takes strict precedence
    SetEnvVar("RADIUM_IPC_SOCKET", "\\\\.\\pipe\\Radium_Primary_Test");
    SetEnvVar("FLUX_IPC_SOCKET", "\\\\.\\pipe\\Flux_Secondary_Test");
    SetEnvVar("OPENIDM_IPC_SOCKET", "\\\\.\\pipe\\OpenIDM_Tertiary_Test");

    TEST_ASSERT(openidm::ipc::GetDefaultIpcSocketPath() == "\\\\.\\pipe\\Radium_Primary_Test",
                "RADIUM_IPC_SOCKET takes strict precedence when all three env vars are set");
    TEST_ASSERT(radium::ipc::GetDefaultIpcSocketPath() == "\\\\.\\pipe\\Radium_Primary_Test",
                "radium::ipc namespace alias functions identically");

    // Case 2: RADIUM_IPC_SOCKET empty string -> FLUX_IPC_SOCKET fallback takes precedence
    SetEnvVar("RADIUM_IPC_SOCKET", "");
    TEST_ASSERT(openidm::ipc::GetDefaultIpcSocketPath() == "\\\\.\\pipe\\Flux_Secondary_Test",
                "FLUX_IPC_SOCKET fallback selected when RADIUM_IPC_SOCKET is empty");

    // Case 3: RADIUM_IPC_SOCKET and FLUX_IPC_SOCKET empty -> OPENIDM_IPC_SOCKET fallback
    SetEnvVar("FLUX_IPC_SOCKET", "");
    TEST_ASSERT(openidm::ipc::GetDefaultIpcSocketPath() == "\\\\.\\pipe\\OpenIDM_Tertiary_Test",
                "OPENIDM_IPC_SOCKET fallback selected when RADIUM and FLUX are empty");

    // Case 4: All env vars empty -> System default named pipe / Unix domain socket
    SetEnvVar("OPENIDM_IPC_SOCKET", "");
    std::string default_path = openidm::ipc::GetDefaultIpcSocketPath();
    TEST_ASSERT(!default_path.empty(), "Default socket path is non-empty");

#ifdef _WIN32
    TEST_ASSERT(default_path.rfind("\\\\.\\pipe\\Radium_IPC_", 0) == 0,
                "Windows default pipe starts with \\\\.\\pipe\\Radium_IPC_");
#else
    TEST_ASSERT(default_path.rfind("/tmp/radium_ipc_", 0) == 0,
                "POSIX default socket starts with /tmp/radium_ipc_");
    TEST_ASSERT(default_path.rfind(".sock") == default_path.length() - 5,
                "POSIX default socket ends with .sock");
#endif

    // Restore environment
    cleanup();
    return true;
}

int main() {
    std::cout << "====================================================\n";
    std::cout << " Radium IPC Path Helper Unit Tests (RDM-02)\n";
    std::cout << "====================================================\n";

    bool ok = TestIpcPathPrecedenceAndFallbacks();
    if (ok) {
        std::cout << "[PASS] All IPC socket path precedence and fallback tests passed successfully.\n";
        return 0;
    } else {
        std::cerr << "[FAIL] IPC socket path precedence test failed!\n";
        return 1;
    }
}
