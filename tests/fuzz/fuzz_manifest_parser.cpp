// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine_stream/video_extractor.hpp"
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (!data || size > 1024 * 1024) return 0;
    std::string text(reinterpret_cast<const char*>(data), size);
    auto hls = radium::engine_stream::VideoExtractionManager::ParseHlsMasterPlaylist(
        text, "https://cdn.example.com/master.m3u8");
    (void)hls;
    std::vector<uint8_t> bytes(data, data + size);
    auto idx = radium::engine_stream::VideoExtractionManager::ParseContainerIndex(bytes);
    (void)idx;
    return 0;
}
