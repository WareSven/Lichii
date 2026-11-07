// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stop_token>
#include <thread>
#include <vector>

namespace VideoCommon::ShaderWarm {

inline void WaitForWarmup(std::vector<std::jthread>& compute, std::vector<std::jthread>& graphics,
                          std::stop_token stop) {
    const std::stop_callback cancel{stop, [&] {
                                        for (auto& thread : compute) {
                                            thread.request_stop();
                                        }
                                        for (auto& thread : graphics) {
                                            thread.request_stop();
                                        }
                                    }};
    // The vectors stay intact until the callback is destroyed. Driver calls already in flight
    // finish normally; cancellation stops the next entry, not vkCreateGraphicsPipelines itself.
    for (auto& thread : compute) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    for (auto& thread : graphics) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

} // namespace VideoCommon::ShaderWarm
