// SPDX-License-Identifier: Apache-2.0
// Radium Download Manager — Windows Administrator Privilege Check Helpers
// Implements secure, non-elevating check to verify if the current user/process
// belongs to the local Administrators group.
#pragma once

#include <windows.h>

namespace radium::security {

/**
 * Checks whether the current user is a member of the local Administrators group.
 * Works under standard UAC tokens (medium integrity) without requiring elevation.
 */
inline bool IsCurrentUserLocalAdmin() {
    // 1. Direct token membership check (covers elevated processes, SYSTEM, and non-UAC admins)
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
    PSID admin_group = nullptr;
    BOOL is_member = FALSE;

    if (AllocateAndInitializeSid(
            &nt_authority, 2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_member);
        FreeSid(admin_group);
        if (is_member) {
            return true;
        }
    }

    // 2. If UAC is active and process is filtered/unelevated, check linked elevation token or token groups
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }

    // Check TokenElevationType
    TOKEN_ELEVATION_TYPE elevation_type = TokenElevationTypeDefault;
    DWORD return_length = 0;
    if (GetTokenInformation(token, TokenElevationType, &elevation_type, sizeof(elevation_type), &return_length)) {
        if (elevation_type == TokenElevationTypeLimited) {
            // Under UAC limited token, query the linked (unfiltered admin) token
            TOKEN_LINKED_TOKEN linked_token{};
            if (GetTokenInformation(token, TokenLinkedToken, &linked_token, sizeof(linked_token), &return_length)) {
                if (linked_token.LinkedToken) {
                    PSID admin_sid = nullptr;
                    if (AllocateAndInitializeSid(
                            &nt_authority, 2,
                            SECURITY_BUILTIN_DOMAIN_RID,
                            DOMAIN_ALIAS_RID_ADMINS,
                            0, 0, 0, 0, 0, 0,
                            &admin_sid)) {
                        BOOL in_linked_admin = FALSE;
                        CheckTokenMembership(linked_token.LinkedToken, admin_sid, &in_linked_admin);
                        FreeSid(admin_sid);
                        CloseHandle(linked_token.LinkedToken);
                        if (in_linked_admin) {
                            CloseHandle(token);
                            return true;
                        }
                    }
                    CloseHandle(linked_token.LinkedToken);
                }
            }
        }
    }

    // 3. Fallback: inspect group SIDs in current token directly
    DWORD info_size = 0;
    GetTokenInformation(token, TokenGroups, nullptr, 0, &info_size);
    if (info_size > 0) {
        std::vector<BYTE> buffer(info_size);
        if (GetTokenInformation(token, TokenGroups, buffer.data(), info_size, &info_size)) {
            auto* groups = reinterpret_cast<TOKEN_GROUPS*>(buffer.data());
            PSID admin_sid = nullptr;
            if (AllocateAndInitializeSid(
                    &nt_authority, 2,
                    SECURITY_BUILTIN_DOMAIN_RID,
                    DOMAIN_ALIAS_RID_ADMINS,
                    0, 0, 0, 0, 0, 0,
                    &admin_sid)) {
                for (DWORD i = 0; i < groups->GroupCount; ++i) {
                    if (EqualSid(groups->Groups[i].Sid, admin_sid)) {
                        // Even if SE_GROUP_USE_FOR_DENY_ONLY (UAC filtered token), the user is an admin!
                        FreeSid(admin_sid);
                        CloseHandle(token);
                        return true;
                    }
                }
                FreeSid(admin_sid);
            }
        }
    }

    CloseHandle(token);
    return false;
}

} // namespace radium::security
