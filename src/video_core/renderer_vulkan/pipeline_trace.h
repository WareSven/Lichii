// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <charconv>
#include <cstdlib>
#include <optional>
#include <string_view>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan {

inline std::optional<u64> ParsePipelineTraceHash(std::string_view text) {
    if (text.size() != 16) {
        return std::nullopt;
    }
    u64 hash{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), hash, 16);
    if (error != std::errc{} || end != text.data() + text.size()) {
        return std::nullopt;
    }
    return hash;
}

// Independent of the general pipeline timing quotas, including synchronous boot builds.
inline bool TracePipeline(u64 hash) {
    static const auto target = []() -> std::optional<u64> {
        const char* value = std::getenv("EDEN_NX2_PIPELINE_TRACE_HASH");
        if (!value) {
            return std::nullopt;
        }
        auto parsed = ParsePipelineTraceHash(value);
        if (!parsed) {
            LOG_WARNING(Render_Vulkan,
                        "EDEN_NX2_PIPELINE_TRACE_HASH requires exactly 16 hexadecimal digits");
        }
        return parsed;
    }();
    if (!target || *target != hash) {
        return false;
    }
    static std::atomic<u32> messages{};
    const u32 index = messages.fetch_add(1, std::memory_order_relaxed);
    if (index == 256) {
        LOG_INFO(Render_Vulkan, "Nx2Perf pipeline_target: key={:016x} trace_limit=256", hash);
    }
    return index < 256;
}

} // namespace Vulkan
