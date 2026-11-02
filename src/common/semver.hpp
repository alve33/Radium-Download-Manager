// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openidm {

struct SemanticVersion {
    uint64_t major{0}, minor{0}, patch{0};
    std::vector<std::string> prerelease;
};

inline std::optional<SemanticVersion> ParseSemanticVersion(std::string_view input) {
    if (!input.empty() && (input.front() == 'v' || input.front() == 'V')) input.remove_prefix(1);
    const auto plus = input.find('+');
    if (plus != std::string_view::npos) input = input.substr(0, plus);
    const auto dash = input.find('-');
    const auto core = input.substr(0, dash);
    SemanticVersion result;
    uint64_t* fields[] = {&result.major, &result.minor, &result.patch};
    size_t start = 0;
    for (size_t i = 0; i < 3; ++i) {
        size_t end = core.find('.', start);
        if (end == std::string_view::npos) end = core.size();
        if (end == start || (end - start > 1 && core[start] == '0')) return std::nullopt;
        uint64_t value = 0;
        for (size_t j = start; j < end; ++j) {
            const char c = core[j];
            if (c < '0' || c > '9' || value > (UINT64_MAX - static_cast<uint64_t>(c - '0')) / 10) return std::nullopt;
            value = value * 10 + static_cast<uint64_t>(c - '0');
        }
        *fields[i] = value;
        if (i < 2 && end == core.size()) return std::nullopt;
        start = end + 1;
    }
    if (start != core.size() + 1) return std::nullopt;
    if (dash != std::string_view::npos) {
        auto pre = input.substr(dash + 1);
        if (pre.empty()) return std::nullopt;
        start = 0;
        while (start <= pre.size()) {
            size_t end = pre.find('.', start);
            if (end == std::string_view::npos) end = pre.size();
            if (end == start) return std::nullopt;
            std::string id(pre.substr(start, end - start));
            bool numeric = true;
            for (char c : id) {
                if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-')) return std::nullopt;
                numeric = numeric && c >= '0' && c <= '9';
            }
            if (numeric && id.size() > 1 && id.front() == '0') return std::nullopt;
            result.prerelease.push_back(std::move(id));
            if (end == pre.size()) break;
            start = end + 1;
        }
    }
    return result;
}

inline std::optional<int> CompareSemanticVersions(std::string_view lhs, std::string_view rhs) {
    auto a = ParseSemanticVersion(lhs), b = ParseSemanticVersion(rhs);
    if (!a || !b) return std::nullopt;
    const uint64_t av[] = {a->major, a->minor, a->patch};
    const uint64_t bv[] = {b->major, b->minor, b->patch};
    for (size_t i = 0; i < 3; ++i) if (av[i] != bv[i]) return av[i] < bv[i] ? -1 : 1;
    if (a->prerelease.empty() != b->prerelease.empty()) return a->prerelease.empty() ? 1 : -1;
    for (size_t i = 0; i < a->prerelease.size() && i < b->prerelease.size(); ++i) {
        const auto& x = a->prerelease[i]; const auto& y = b->prerelease[i];
        if (x == y) continue;
        const bool xn = !x.empty() && x.find_first_not_of("0123456789") == std::string::npos;
        const bool yn = !y.empty() && y.find_first_not_of("0123456789") == std::string::npos;
        if (xn && yn) {
            if (x.size() != y.size()) return x.size() < y.size() ? -1 : 1;
            return x < y ? -1 : 1;
        }
        if (xn != yn) return xn ? -1 : 1;
        return x < y ? -1 : 1;
    }
    if (a->prerelease.size() == b->prerelease.size()) return 0;
    return a->prerelease.size() < b->prerelease.size() ? -1 : 1;
}

} // namespace openidm
