// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_torrent/bencode.hpp"
#include <cstdint>
#include <cstddef>
#include <string_view>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data || size > 1024 * 1024) return 0;
    std::string_view sv(reinterpret_cast<const char*>(data), size);
    auto res = radium::torrent::BencodeParser::Decode(sv);
    (void)res;
    return 0;
}
