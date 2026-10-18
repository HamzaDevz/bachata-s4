// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <queue>
#include <utility>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class MasterSemaphore {
public:
    explicit MasterSemaphore(const Instance& instance_);
    ~MasterSemaphore();

    [[nodiscard]] u64 CurrentTick() const noexcept {
        return current_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] u64 KnownGpuTick() const noexcept {
        return gpu_tick.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool IsFree(u64 tick) const noexcept {
        return KnownGpuTick() >= tick;
    }

    [[nodiscard]] u64 NextTick() noexcept {
        return current_tick.fetch_add(1, std::memory_order_release);
    }

    [[nodiscard]] vk::Semaphore Handle() const noexcept {
        return semaphore.get();
    }

    /// Refresh the known GPU tick
    void Refresh();

    /// Waits for a tick to be hit on the GPU
    void Wait(u64 tick);

    /// Remembers the shaders a submission runs, logged if the device is lost before it completes.
    void RecordWork(u64 tick, std::vector<u64>&& shader_hashes);

protected:
    /// Logs the shaders of the submissions the GPU did not complete.
    void LogWorkInFlight();

    const Instance& instance;
    vk::UniqueSemaphore semaphore;    ///< Timeline semaphore.
    std::atomic<u64> gpu_tick{0};     ///< Current known GPU tick.
    std::atomic<u64> current_tick{1}; ///< Current logical tick.
    std::mutex work_mutex;
    std::array<std::pair<u64, std::vector<u64>>, 16> work_in_flight{};
};

} // namespace Vulkan
