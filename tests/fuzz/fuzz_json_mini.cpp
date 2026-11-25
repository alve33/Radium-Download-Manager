// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/json_mini.hpp"
#include <cstdint>
#include <cstddef>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data || size > 1024 * 1024) return 0;
    std::string s(reinterpret_cast<const char*>(data), size);
    try {
        auto v = radium::json::Parse(s);
        (void)v.dump();
    } catch (...) {
    }
    return 0;
}
