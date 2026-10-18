// SPDX-FileCopyrightText: Copyright 2020 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>
#include <cstdio>
#include <string>
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"

#include "common/assert.h"
#include "common/logging/log.h"

namespace Vulkan {

constexpr u64 WAIT_TIMEOUT = std::numeric_limits<u64>::max();

// Every timeline, so a device lost noticed by one scheduler also names the work of the others.
static std::mutex semaphores_mutex;
static std::vector<MasterSemaphore*> semaphores;

MasterSemaphore::MasterSemaphore(const Instance& instance_) : instance{instance_} {
    std::fprintf(stderr, "BACHATA_MASTER_SEMAPHORE_ENTER\n");
    const vk::StructureChain semaphore_chain = {
        vk::SemaphoreCreateInfo{},
        vk::SemaphoreTypeCreateInfo{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        },
    };
    auto [semaphore_result, sem] =
        instance.GetDevice().createSemaphoreUnique(semaphore_chain.get());
    ASSERT_MSG(semaphore_result == vk::Result::eSuccess, "Failed to create master semaphore: {}",
               vk::to_string(semaphore_result));
    semaphore = std::move(sem);
    std::scoped_lock lk{semaphores_mutex};
    semaphores.push_back(this);
    std::fprintf(stderr, "BACHATA_MASTER_SEMAPHORE_READY\n");
}

MasterSemaphore::~MasterSemaphore() {
    std::scoped_lock lk{semaphores_mutex};
    std::erase(semaphores, this);
}

void MasterSemaphore::Refresh() {
    u64 this_tick{};
    u64 counter{};
    do {
        this_tick = gpu_tick.load(std::memory_order_acquire);
        auto [counter_result, cntr] = instance.GetDevice().getSemaphoreCounterValue(*semaphore);
        if (counter_result != vk::Result::eSuccess) {
            LogWorkInFlight();
        }
        ASSERT_MSG(counter_result == vk::Result::eSuccess,
                   "Failed to get master semaphore value: {}", vk::to_string(counter_result));
        counter = cntr;
        if (counter < this_tick) {
            return;
        }
    } while (!gpu_tick.compare_exchange_weak(this_tick, counter, std::memory_order_release,
                                             std::memory_order_relaxed));
}

void MasterSemaphore::Wait(u64 tick) {
    // No need to wait if the GPU is ahead of the tick
    if (IsFree(tick)) {
        return;
    }
    // Update the GPU tick and try again
    Refresh();
    if (IsFree(tick)) {
        return;
    }

    // If none of the above is hit, fallback to a regular wait
    const vk::SemaphoreWaitInfo wait_info = {
        .semaphoreCount = 1,
        .pSemaphores = &semaphore.get(),
        .pValues = &tick,
    };

    vk::Result result;
    while ((result = instance.GetDevice().waitSemaphores(&wait_info, WAIT_TIMEOUT)) !=
           vk::Result::eSuccess) {
        if (result == vk::Result::eErrorDeviceLost) {
            break;
        }
    }
    Refresh();
}

void MasterSemaphore::RecordWork(u64 tick, std::vector<u64>&& shader_hashes) {
    std::scoped_lock lk{work_mutex};
    work_in_flight[tick % work_in_flight.size()] = {tick, std::move(shader_hashes)};
}

void MasterSemaphore::LogWorkInFlight() {
    std::scoped_lock lk{semaphores_mutex};
    for (MasterSemaphore* const timeline : semaphores) {
        timeline->LogOwnWorkInFlight();
    }
}

void MasterSemaphore::LogOwnWorkInFlight() {
    std::scoped_lock lk{work_mutex};
    const u64 known_tick = KnownGpuTick();
    for (u64 tick = known_tick + 1; tick < CurrentTick() && tick <= known_tick + work_in_flight.size();
         ++tick) {
        const auto& [record_tick, shader_hashes] = work_in_flight[tick % work_in_flight.size()];
        if (record_tick != tick || shader_hashes.empty()) {
            continue;
        }
        std::string text;
        for (const u64 hash : shader_hashes) {
            text += fmt::format(" {:#x}", hash);
        }
        LOG_CRITICAL(Render_Vulkan, "BACHATA_GPU_WORK_IN_FLIGHT tick={} shaders:{}", tick, text);
    }
}

} // namespace Vulkan
