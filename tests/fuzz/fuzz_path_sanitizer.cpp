// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/path_sanitizer.hpp"
#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data || size > 65536) return 0;
    std::string s(reinterpret_cast<const char*>(data), size);
    auto clean = openidm::PathSanitizer::SanitizeFileName(s);
    auto cd = openidm::PathSanitizer::ExtractFromContentDisposition(s);
    (void)clean;
    (void)cd;
    return 0;
}
