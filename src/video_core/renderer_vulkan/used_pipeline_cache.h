// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <span>
#include <unordered_map>

#include "common/common_types.h"

namespace Vulkan {

using UsedPipelineAges = std::unordered_map<u64, u8>;

class UsedPipelineCheckpoint {
public:
    using Clock = std::chrono::steady_clock;

    explicit UsedPipelineCheckpoint(Clock::time_point now = Clock::now())
        : next_attempt{now + Interval} {}

    void MarkDirty() {
        dirty = true;
    }

    bool CanSave(size_t used_count) const {
        // A boot to the menu should not replace the previous session's selection.
        return dirty && used_count >= 64;
    }

    bool IsDue(Clock::time_point now, size_t used_count) const {
        return CanSave(used_count) && now >= next_attempt;
    }

    void CompleteAttempt(Clock::time_point now, bool success) {
        next_attempt = now + Interval;
        if (success) {
            dirty = false;
        }
    }

private:
    static constexpr auto Interval = std::chrono::seconds{15};
    Clock::time_point next_attempt;
    bool dirty{};
};

UsedPipelineAges MakeUsedPipelineSnapshot(const UsedPipelineAges& boot, std::span<const u64> used,
                                          u8 max_age);
UsedPipelineAges LoadUsedPipelines(const std::filesystem::path& path, u8 max_age);
bool WriteUsedPipelineSnapshot(std::ostream& stream, const UsedPipelineAges& snapshot);
using PipelineFileReplace = bool (*)(const std::filesystem::path&, const std::filesystem::path&);
bool WritePipelineFileAtomically(const std::filesystem::path& path,
                                 const std::function<bool(std::ostream&)>& write,
                                 PipelineFileReplace replace = nullptr);
bool SaveUsedPipelineSnapshot(const std::filesystem::path& path, const UsedPipelineAges& snapshot);

} // namespace Vulkan
