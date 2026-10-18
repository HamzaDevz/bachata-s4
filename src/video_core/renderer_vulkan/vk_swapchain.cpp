// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <limits>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/emulator_settings.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain.h"

#ifdef ENABLE_BACHATA_RUNTIME
#include <bit>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <thread>
#include <dlfcn.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xcb/xcb.h>
#include "common/thread.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#endif

namespace Vulkan {

static constexpr vk::SurfaceFormatKHR SURFACE_FORMAT_HDR = {
    .format = vk::Format::eA2B10G10R10UnormPack32,
    .colorSpace = vk::ColorSpaceKHR::eHdr10St2084EXT,
};

#ifdef ENABLE_BACHATA_RUNTIME
// The runtime's Vulkan X11 WSI is software only: each frame was read back from uncached memory
// and streamed to the embedded X server over its socket, which cost about a third of a core on
// each side at 30 fps. Instead every image gets a dma-buf the X server maps as a DRI3 pixmap, so
// presenting is one GPU copy plus a CopyArea request.
struct Swapchain::HostPresent {
#define XCB_FN(name) decltype(&::name) name = nullptr
    XCB_FN(xcb_connect);
    XCB_FN(xcb_connection_has_error);
    XCB_FN(xcb_disconnect);
    XCB_FN(xcb_generate_id);
    XCB_FN(xcb_get_extension_data);
    XCB_FN(xcb_get_geometry);
    XCB_FN(xcb_get_geometry_reply);
    XCB_FN(xcb_create_gc);
    XCB_FN(xcb_copy_area);
    XCB_FN(xcb_free_pixmap);
    XCB_FN(xcb_get_input_focus);
    XCB_FN(xcb_get_input_focus_reply);
    XCB_FN(xcb_request_check);
    XCB_FN(xcb_poll_for_event);
    XCB_FN(xcb_flush);
#undef XCB_FN
    // libxcb-dri3, whose headers the build does not have.
    xcb_void_cookie_t (*xcb_dri3_pixmap_from_buffer_checked)(
        xcb_connection_t* conn, xcb_pixmap_t pixmap, xcb_drawable_t drawable, u32 size, u16 width,
        u16 height, u16 stride, u8 depth, u8 bpp, s32 pixmap_fd) = nullptr;
    xcb_extension_t* xcb_dri3_id = nullptr;

    struct Slot {
        vk::Image image;
        vk::DeviceMemory image_memory;
        vk::Buffer buffer;
        vk::DeviceMemory buffer_memory;
        vk::CommandBuffer cmdbuf;
        vk::Fence fence;
        xcb_pixmap_t pixmap = 0;
        int dmabuf = -1;
        bool busy = false;
    };

    xcb_connection_t* conn = nullptr;
    xcb_window_t window = 0;
    xcb_gcontext_t gc = 0;
    u8 depth = 24;
    u16 width = 0;
    u16 height = 0;
    vk::CommandPool command_pool;
    std::vector<Slot> slots;

    // The worker waits for each GPU copy and hands the frame to the X server, so the presenter
    // never blocks on the GPU, as with the WSI queue thread this replaces.
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<u32> queue;
    bool stop = false;
    std::thread worker;

    void Run(vk::Device device) {
        Common::SetCurrentThreadName("shadPS4:HostPresent");
        std::unique_lock lk{mutex};
        while (true) {
            cv.wait(lk, [this] { return stop || !queue.empty(); });
            if (queue.empty()) {
                return;
            }
            Slot& slot = slots[queue.front()];
            queue.pop_front();
            lk.unlock();
            vk::Result result = device.waitForFences(slot.fence, true, ~u64{0});
            if (result == vk::Result::eSuccess) {
                result = device.resetFences(slot.fence);
            }
            if (result == vk::Result::eSuccess) {
                // GPU writes bypass the CPU caches: drop stale lines of this buffer before the X
                // server reads it. Invalidation works on physical lines, so it covers the X
                // server's own mapping too.
                dma_buf_sync sync{.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
                ioctl(slot.dmabuf, DMA_BUF_IOCTL_SYNC, &sync);
                xcb_copy_area(conn, slot.pixmap, window, gc, 0, 0, 0, 0, width, height);
                // The reply comes after the copy: the GPU may then overwrite the buffer.
                std::free(xcb_get_input_focus_reply(conn, xcb_get_input_focus(conn), nullptr));
                while (auto* event = xcb_poll_for_event(conn)) {
                    std::free(event);
                }
            } else {
                LOG_CRITICAL(Render_Vulkan, "Host presentation copy failed: {}",
                             vk::to_string(result));
            }
            lk.lock();
            slot.busy = false;
            cv.notify_all();
        }
    }

    void WaitIdle() {
        std::unique_lock lk{mutex};
        cv.wait(lk, [this] { return std::ranges::none_of(slots, &Slot::busy); });
    }
};
#else
struct Swapchain::HostPresent {};
#endif

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_)
    : instance{instance_}, window{window_}, surface{CreateSurface(instance.GetInstance(), window)} {
    std::fprintf(stderr, "BACHATA_SWAPCHAIN_FORMAT_ENTER\n");
    FindPresentFormat();
    std::fprintf(stderr, "BACHATA_SWAPCHAIN_FORMAT_READY\n");
    FindPresentMode();
    std::fprintf(stderr, "BACHATA_SWAPCHAIN_MODE_READY\n");
    InitHostPresent();

    Create(window.GetWidth(), window.GetHeight());
    std::fprintf(stderr, "BACHATA_SWAPCHAIN_READY\n");
    std::fprintf(stderr, "BACHATA_IMGUI_ENTER\n");
    ImGui::Core::Initialize(instance, window, image_count, surface_format.format);
    std::fprintf(stderr, "BACHATA_IMGUI_READY\n");
}

#ifdef ENABLE_BACHATA_RUNTIME
void Swapchain::InitHostPresent() {
    const auto info = window.GetWindowInfo();
    const char* setting = std::getenv("BACHATA_HOST_PRESENT");
    if ((setting && setting[0] == '0') || info.type != Frontend::WindowSystemType::X11 ||
        !info.render_surface || !instance.IsDmaBufExportSupported() ||
        surface_format.format != vk::Format::eB8G8R8A8Unorm) {
        return;
    }

    void* xcb = dlopen("libxcb.so.1", RTLD_NOW);
    void* dri3 = dlopen("libxcb-dri3.so.0", RTLD_NOW);
    if (!xcb || !dri3) {
        LOG_WARNING(Render_Vulkan, "Host presentation: {}", dlerror());
        return;
    }
    auto hp = std::make_unique<HostPresent>();
    bool loaded = true;
    const auto load = [&loaded](void* lib, auto& symbol, const char* name) {
        symbol = reinterpret_cast<std::remove_reference_t<decltype(symbol)>>(dlsym(lib, name));
        loaded &= symbol != nullptr;
    };
#define XCB_LOAD(lib, name) load(lib, hp->name, #name)
    XCB_LOAD(xcb, xcb_connect);
    XCB_LOAD(xcb, xcb_connection_has_error);
    XCB_LOAD(xcb, xcb_disconnect);
    XCB_LOAD(xcb, xcb_generate_id);
    XCB_LOAD(xcb, xcb_get_extension_data);
    XCB_LOAD(xcb, xcb_get_geometry);
    XCB_LOAD(xcb, xcb_get_geometry_reply);
    XCB_LOAD(xcb, xcb_create_gc);
    XCB_LOAD(xcb, xcb_copy_area);
    XCB_LOAD(xcb, xcb_free_pixmap);
    XCB_LOAD(xcb, xcb_get_input_focus);
    XCB_LOAD(xcb, xcb_get_input_focus_reply);
    XCB_LOAD(xcb, xcb_request_check);
    XCB_LOAD(xcb, xcb_poll_for_event);
    XCB_LOAD(xcb, xcb_flush);
    XCB_LOAD(dri3, xcb_dri3_pixmap_from_buffer_checked);
    XCB_LOAD(dri3, xcb_dri3_id);
#undef XCB_LOAD
    if (!loaded) {
        LOG_WARNING(Render_Vulkan, "Host presentation: missing xcb symbols");
        return;
    }

    // A connection of our own: SDL keeps using its Xlib display from its event thread.
    hp->conn = hp->xcb_connect(nullptr, nullptr);
    if (hp->xcb_connection_has_error(hp->conn)) {
        hp->xcb_disconnect(hp->conn);
        LOG_WARNING(Render_Vulkan, "Host presentation: cannot connect to the X server");
        return;
    }
    hp->window = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(info.render_surface));
    const auto* dri3_ext = hp->xcb_get_extension_data(hp->conn, hp->xcb_dri3_id);
    auto* geometry = hp->xcb_get_geometry_reply(
        hp->conn, hp->xcb_get_geometry(hp->conn, hp->window), nullptr);
    if (!dri3_ext || !dri3_ext->present || !geometry) {
        std::free(geometry);
        hp->xcb_disconnect(hp->conn);
        LOG_WARNING(Render_Vulkan, "Host presentation: the X server has no DRI3");
        return;
    }
    hp->depth = geometry->depth;
    std::free(geometry);

    hp->gc = hp->xcb_generate_id(hp->conn);
    const u32 no_exposures = 0;
    hp->xcb_create_gc(hp->conn, hp->gc, hp->window, XCB_GC_GRAPHICS_EXPOSURES, &no_exposures);

    auto [pool_result, pool] = instance.GetDevice().createCommandPool(vk::CommandPoolCreateInfo{
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    });
    if (pool_result != vk::Result::eSuccess) {
        hp->xcb_disconnect(hp->conn);
        return;
    }
    hp->command_pool = pool;
    hp->worker = std::thread{&HostPresent::Run, hp.get(), instance.GetDevice()};
    host_present = std::move(hp);
    supports_hdr = false;
    LOG_INFO(Render_Vulkan, "Host presentation through DRI3 pixmaps, window depth {}",
             host_present->depth);
}

bool Swapchain::CreateHostImages() {
    auto& hp = *host_present;
    const vk::Device device = instance.GetDevice();
    const u32 stride = extent.width * 4;
    const u32 size = stride * extent.height;
    if (stride > std::numeric_limits<u16>::max() ||
        extent.height > std::numeric_limits<u16>::max()) {
        return false;
    }
    hp.width = static_cast<u16>(extent.width);
    hp.height = static_cast<u16>(extent.height);

    const auto memory_props = instance.GetPhysicalDevice().getMemoryProperties();
    const auto find_memory_type = [&](u32 type_bits, vk::MemoryPropertyFlags wanted) {
        for (u32 i = 0; i < memory_props.memoryTypeCount; ++i) {
            if ((type_bits & (1u << i)) &&
                (memory_props.memoryTypes[i].propertyFlags & wanted) == wanted) {
                return i;
            }
        }
        return static_cast<u32>(std::countr_zero(type_bits));
    };

    auto [cmdbuf_result, cmdbufs] = device.allocateCommandBuffers(vk::CommandBufferAllocateInfo{
        .commandPool = hp.command_pool,
        .level = vk::CommandBufferLevel::ePrimary,
        .commandBufferCount = image_count,
    });
    if (cmdbuf_result != vk::Result::eSuccess) {
        return false;
    }

    constexpr auto dma_buf = vk::ExternalMemoryHandleTypeFlagBits::eDmaBufEXT;
    constexpr vk::ImageSubresourceRange color_range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    for (u32 i = 0; i < image_count; ++i) {
        auto& slot = hp.slots.emplace_back();
        slot.cmdbuf = cmdbufs[i];

        auto [image_result, image] = device.createImage(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = surface_format.format,
            .extent = {extent.width, extent.height, 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eColorAttachment |
                     vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        if (image_result != vk::Result::eSuccess) {
            return false;
        }
        slot.image = image;
        const auto image_reqs = device.getImageMemoryRequirements(image);
        auto [image_memory_result, image_memory] = device.allocateMemory(vk::MemoryAllocateInfo{
            .allocationSize = image_reqs.size,
            .memoryTypeIndex = find_memory_type(image_reqs.memoryTypeBits,
                                                vk::MemoryPropertyFlagBits::eDeviceLocal),
        });
        if (image_memory_result != vk::Result::eSuccess) {
            return false;
        }
        slot.image_memory = image_memory;
        if (device.bindImageMemory(image, image_memory, 0) != vk::Result::eSuccess) {
            return false;
        }

        const vk::ExternalMemoryBufferCreateInfo external_info{.handleTypes = dma_buf};
        auto [buffer_result, buffer] = device.createBuffer(vk::BufferCreateInfo{
            .pNext = &external_info,
            .size = size,
            .usage = vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        if (buffer_result != vk::Result::eSuccess) {
            return false;
        }
        slot.buffer = buffer;
        const auto buffer_reqs = device.getBufferMemoryRequirements(buffer);
        const vk::MemoryDedicatedAllocateInfo dedicated_info{.buffer = buffer};
        const vk::ExportMemoryAllocateInfo export_info{
            .pNext = &dedicated_info,
            .handleTypes = dma_buf,
        };
        auto [buffer_memory_result, buffer_memory] = device.allocateMemory(vk::MemoryAllocateInfo{
            .pNext = &export_info,
            .allocationSize = buffer_reqs.size,
            .memoryTypeIndex = find_memory_type(buffer_reqs.memoryTypeBits, {}),
        });
        if (buffer_memory_result != vk::Result::eSuccess) {
            return false;
        }
        slot.buffer_memory = buffer_memory;
        if (device.bindBufferMemory(buffer, buffer_memory, 0) != vk::Result::eSuccess) {
            return false;
        }
        auto [fd_result, fd] = device.getMemoryFdKHR(vk::MemoryGetFdInfoKHR{
            .memory = buffer_memory,
            .handleType = dma_buf,
        });
        if (fd_result != vk::Result::eSuccess) {
            return false;
        }
        slot.dmabuf = fd;

        // xcb closes the descriptor it sends, the X server maps it read-only.
        slot.pixmap = hp.xcb_generate_id(hp.conn);
        const auto cookie = hp.xcb_dri3_pixmap_from_buffer_checked(
            hp.conn, slot.pixmap, hp.window, size, static_cast<u16>(extent.width),
            static_cast<u16>(extent.height), static_cast<u16>(stride), hp.depth, 32, dup(fd));
        if (auto* error = hp.xcb_request_check(hp.conn, cookie)) {
            std::free(error);
            slot.pixmap = 0;
            return false;
        }

        auto [fence_result, fence] = device.createFence(vk::FenceCreateInfo{});
        if (fence_result != vk::Result::eSuccess) {
            return false;
        }
        slot.fence = fence;

        // The same commands copy every frame of this image, so record them once.
        const vk::CommandBuffer cmdbuf = slot.cmdbuf;
        if (cmdbuf.begin(vk::CommandBufferBeginInfo{}) != vk::Result::eSuccess) {
            return false;
        }
        const vk::ImageMemoryBarrier to_transfer{
            .srcAccessMask = vk::AccessFlagBits::eNone,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::ePresentSrcKHR,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, to_transfer);
        cmdbuf.copyImageToBuffer(image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                                 vk::BufferImageCopy{
                                     .imageSubresource =
                                         {
                                             .aspectMask = vk::ImageAspectFlagBits::eColor,
                                             .layerCount = 1,
                                         },
                                     .imageExtent = {extent.width, extent.height, 1},
                                 });
        const vk::BufferMemoryBarrier to_host{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eHostRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .size = VK_WHOLE_SIZE,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eHost, {}, {}, to_host, {});
        if (cmdbuf.end() != vk::Result::eSuccess) {
            return false;
        }
        images.push_back(image);
    }
    hp.xcb_flush(hp.conn);
    return true;
}

void Swapchain::DestroyHostImages() {
    auto& hp = *host_present;
    const vk::Device device = instance.GetDevice();
    hp.WaitIdle();
    for (auto& slot : hp.slots) {
        if (slot.pixmap) {
            hp.xcb_free_pixmap(hp.conn, slot.pixmap);
        }
        if (slot.dmabuf >= 0) {
            close(slot.dmabuf);
        }
        if (slot.cmdbuf) {
            device.freeCommandBuffers(hp.command_pool, slot.cmdbuf);
        }
        device.destroyFence(slot.fence);
        device.destroyBuffer(slot.buffer);
        device.freeMemory(slot.buffer_memory);
        device.destroyImage(slot.image);
        device.freeMemory(slot.image_memory);
    }
    if (!hp.slots.empty()) {
        hp.xcb_flush(hp.conn);
    }
    hp.slots.clear();
    images.clear();
}

void Swapchain::DisableHostPresent() {
    DestroyHostImages();
    {
        std::scoped_lock lk{host_present->mutex};
        host_present->stop = true;
    }
    host_present->cv.notify_all();
    host_present->worker.join();
    instance.GetDevice().destroyCommandPool(host_present->command_pool);
    host_present->xcb_disconnect(host_present->conn);
    host_present.reset();
}

void Swapchain::AcquireHostImage() {
    auto& hp = *host_present;
    image_index = frame_index;
    // Neither the previous GPU copy of this image nor the X server's read of its buffer may
    // still be running when the presenter draws into it again.
    std::unique_lock lk{hp.mutex};
    hp.cv.wait(lk, [&] { return !hp.slots[image_index].busy; });
}

bool Swapchain::PresentHost() {
    auto& hp = *host_present;
    auto& slot = hp.slots[image_index];

    const vk::PipelineStageFlags wait_stage = vk::PipelineStageFlagBits::eTransfer;
    const vk::SubmitInfo submit_info{
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &slot.cmdbuf,
    };
    vk::Result result;
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        result = instance.GetGraphicsQueue().submit(submit_info, slot.fence);
    }
    frame_index = (frame_index + 1) % image_count;
    if (result != vk::Result::eSuccess) {
        LOG_CRITICAL(Render_Vulkan, "Host presentation copy failed: {}", vk::to_string(result));
        needs_recreation = true;
        return false;
    }
    {
        std::scoped_lock lk{hp.mutex};
        slot.busy = true;
        hp.queue.push_back(image_index);
    }
    hp.cv.notify_all();
    return true;
}
#else
void Swapchain::InitHostPresent() {}

void Swapchain::AcquireHostImage() {}

bool Swapchain::CreateHostImages() {
    return false;
}

void Swapchain::DestroyHostImages() {}

void Swapchain::DisableHostPresent() {
    host_present.reset();
}

bool Swapchain::PresentHost() {
    return false;
}
#endif

Swapchain::~Swapchain() {
    Destroy();
    if (host_present) {
        DisableHostPresent();
    }
    instance.GetInstance().destroySurfaceKHR(surface);
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;

    Destroy();

    SetSurfaceProperties();

    if (host_present) {
        if (CreateHostImages()) {
            SetupImages();
            RefreshSemaphores();
            return;
        }
        LOG_WARNING(Render_Vulkan, "Host presentation failed, using the Vulkan swapchain");
        DisableHostPresent();
    }

    const std::array queue_family_indices = {
        instance.GetGraphicsQueueFamilyIndex(),
        instance.GetPresentQueueFamilyIndex(),
    };

    const bool exclusive = queue_family_indices[0] == queue_family_indices[1];
    const u32 queue_family_indices_count = exclusive ? 1u : 2u;
    const vk::SharingMode sharing_mode =
        exclusive ? vk::SharingMode::eExclusive : vk::SharingMode::eConcurrent;
    const auto format = needs_hdr ? SURFACE_FORMAT_HDR : surface_format;
    const vk::SwapchainCreateInfoKHR swapchain_info = {
        .surface = surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = vk::ImageUsageFlagBits::eColorAttachment |
                      vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .imageSharingMode = sharing_mode,
        .queueFamilyIndexCount = queue_family_indices_count,
        .pQueueFamilyIndices = queue_family_indices.data(),
        .preTransform = transform,
        .compositeAlpha = composite_alpha,
        .presentMode = present_mode,
        .clipped = true,
        .oldSwapchain = nullptr,
    };

    auto [swapchain_result, chain] = instance.GetDevice().createSwapchainKHR(swapchain_info);
    ASSERT_MSG(swapchain_result == vk::Result::eSuccess, "Failed to create swapchain: {}",
               vk::to_string(swapchain_result));
    swapchain = chain;

    SetupImages();
    RefreshSemaphores();
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the swapchain: width={} height={} HDR={}", width_, height_,
              needs_hdr);
    Create(width_, height_);
}

void Swapchain::SetHDR(bool hdr) {
    if (needs_hdr == hdr) {
        return;
    }

    auto result = instance.GetDevice().waitIdle();
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(ImGui, "Failed to wait for Vulkan device idle on mode change: {}",
                    vk::to_string(result));
    }

    needs_hdr = hdr;
    Recreate(width, height);
    ImGui::Core::OnSurfaceFormatChange(needs_hdr ? SURFACE_FORMAT_HDR.format
                                                 : surface_format.format);
}

bool Swapchain::AcquireNextImage() {
    if (host_present) {
        AcquireHostImage();
        return true;
    }
    vk::Device device = instance.GetDevice();
    vk::Result result =
        device.acquireNextImageKHR(swapchain, std::numeric_limits<u64>::max(),
                                   image_acquired[frame_index], VK_NULL_HANDLE, &image_index);

    switch (result) {
    case vk::Result::eSuccess:
        break;
    case vk::Result::eSuboptimalKHR:
    case vk::Result::eErrorSurfaceLostKHR:
    case vk::Result::eErrorOutOfDateKHR:
    case vk::Result::eErrorUnknown:
        needs_recreation = true;
        break;
    case vk::Result::eErrorDeviceLost:
        // Mali/Vortek can report device-lost after heavy first-frame GPU work. Do not
        // UNREACHABLE (exit 133); mark for recreation and let Present skip the frame so
        // session logs flush and the Android layer can observe a clean stop.
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned ErrorDeviceLost");
        {
            // Client-side gate; server dumps alloc map via present_sync / GpuTrack.
            static std::atomic<bool> device_lost_snapshot_logged{false};
            if (!device_lost_snapshot_logged.exchange(true)) {
                LOG_CRITICAL(Render_Vulkan,
                             "DEVICE_LOST_SNAPSHOT where=AcquireNextImage frame_index={} "
                             "image_count={} image_index={} (server dump: Bachata.Vortek.GpuTrack)",
                             frame_index, image_count, image_index);
            }
        }
        needs_recreation = true;
        break;
    default:
        LOG_CRITICAL(Render_Vulkan, "Swapchain acquire returned unknown result {}",
                     vk::to_string(result));
        UNREACHABLE();
        break;
    }

    return !needs_recreation;
}

bool Swapchain::Present() {
    if (host_present) {
        return PresentHost();
    }

    const vk::PresentInfoKHR present_info = {
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &present_ready[image_index],
        .swapchainCount = 1,
        .pSwapchains = &swapchain,
        .pImageIndices = &image_index,
    };

    auto result = instance.GetPresentQueue().presentKHR(present_info);
    if (result == vk::Result::eErrorOutOfDateKHR || result == vk::Result::eSuboptimalKHR ||
        result == vk::Result::eErrorDeviceLost || result == vk::Result::eErrorSurfaceLostKHR) {
        if (result == vk::Result::eErrorDeviceLost) {
            LOG_CRITICAL(Render_Vulkan, "Swapchain present returned ErrorDeviceLost");
            static std::atomic<bool> device_lost_snapshot_logged{false};
            if (!device_lost_snapshot_logged.exchange(true)) {
                LOG_CRITICAL(Render_Vulkan,
                             "DEVICE_LOST_SNAPSHOT where=QueuePresent frame_index={} "
                             "image_count={} image_index={} (server dump: Bachata.Vortek.GpuTrack)",
                             frame_index, image_count, image_index);
            }
        }
        needs_recreation = true;
    } else {
        ASSERT_MSG(result == vk::Result::eSuccess, "Swapchain presentation failed: {}",
                   vk::to_string(result));
    }

    frame_index = (frame_index + 1) % image_count;

    return !needs_recreation;
}

void Swapchain::FindPresentFormat() {
    const auto [formats_result, formats] =
        instance.GetPhysicalDevice().getSurfaceFormatsKHR(surface);
    ASSERT_MSG(formats_result == vk::Result::eSuccess, "Failed to query surface formats: {}",
               vk::to_string(formats_result));

    // Check if the device supports HDR formats. Here we care of Rec.2020 PQ only as it is expected
    // game output. Other variants as e.g. linear Rec.2020 will require additional color space
    // rotation
    supports_hdr =
        std::find_if(formats.begin(), formats.end(), [](const vk::SurfaceFormatKHR& format) {
            return format == SURFACE_FORMAT_HDR;
        }) != formats.end();
    // Also make sure that user allowed us to use HDR
    supports_hdr &= EmulatorSettings.IsHdrAllowed();

    // If there is a single undefined surface format, the device doesn't care, so we'll just use
    // RGBA sRGB.
    if (formats[0].format == vk::Format::eUndefined) {
        surface_format.format = vk::Format::eR8G8B8A8Unorm;
        surface_format.colorSpace = vk::ColorSpaceKHR::eSrgbNonlinear;
        return;
    }

    // Try to find a suitable format.
    for (const vk::SurfaceFormatKHR& sformat : formats) {
        vk::Format format = sformat.format;
        if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm) {
            continue;
        }

        surface_format.format = format;
        surface_format.colorSpace = sformat.colorSpace;
        return;
    }

    UNREACHABLE_MSG("Unable to find required swapchain format!");
}

void Swapchain::FindPresentMode() {
    const auto [modes_result, modes] =
        instance.GetPhysicalDevice().getSurfacePresentModesKHR(surface);
    if (modes_result != vk::Result::eSuccess) {
        LOG_ERROR(Render, "Failed to query available present modes, falling back to Fifo as "
                          "guaranteed supported option.");
        present_mode = vk::PresentModeKHR::eFifo;
        return;
    }

    const auto requested_mode = EmulatorSettings.GetPresentMode();
    if (requested_mode == "Mailbox") {
        present_mode = vk::PresentModeKHR::eMailbox;
    } else if (requested_mode == "Fifo") {
        present_mode = vk::PresentModeKHR::eFifo;
    } else if (requested_mode == "Immediate") {
        present_mode = vk::PresentModeKHR::eImmediate;
    } else {
        LOG_ERROR(Render_Vulkan, "Unknown present mode {}, defaulting to Mailbox.",
                  EmulatorSettings.GetPresentMode());
        present_mode = vk::PresentModeKHR::eMailbox;
    }

    if (std::ranges::find(modes, present_mode) == modes.cend()) {
        // FIFO is guaranteed to be supported by the Vulkan spec.
        constexpr auto fallback = vk::PresentModeKHR::eFifo;
        LOG_WARNING(Render, "Requested present mode {} is not supported, falling back to {}.",
                    vk::to_string(present_mode), vk::to_string(fallback));
        present_mode = fallback;
    }
}

void Swapchain::SetSurfaceProperties() {
    const auto [capabilities_result, capabilities] =
        instance.GetPhysicalDevice().getSurfaceCapabilitiesKHR(surface);
    ASSERT_MSG(capabilities_result == vk::Result::eSuccess,
               "Failed to query surface capabilities: {}", vk::to_string(capabilities_result));

    extent = capabilities.currentExtent;
    if (capabilities.currentExtent.width == std::numeric_limits<u32>::max()) {
        extent.width = std::max(capabilities.minImageExtent.width,
                                std::min(capabilities.maxImageExtent.width, width));
        extent.height = std::max(capabilities.minImageExtent.height,
                                 std::min(capabilities.maxImageExtent.height, height));
    }

    // Select number of images in swap chain, we prefer one buffer in the background to work on
    image_count = capabilities.minImageCount + 1;
    if (capabilities.maxImageCount > 0) {
        image_count = std::min(image_count, capabilities.maxImageCount);
    }

    // Prefer identity transform if possible
    transform = vk::SurfaceTransformFlagBitsKHR::eIdentity;
    if (!(capabilities.supportedTransforms & transform)) {
        transform = capabilities.currentTransform;
    }

    // Opaque is not supported everywhere.
    composite_alpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
    if (!(capabilities.supportedCompositeAlpha & vk::CompositeAlphaFlagBitsKHR::eOpaque)) {
        composite_alpha = vk::CompositeAlphaFlagBitsKHR::eInherit;
    }
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }

    for (auto& image_view : images_view) {
        device.destroyImageView(image_view);
    }
    images_view.clear();

    if (host_present) {
        DestroyHostImages();
    }
    if (swapchain) {
        device.destroySwapchainKHR(swapchain);
        swapchain = nullptr;
    }

    for (const auto& sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto& sem : present_ready) {
        device.destroySemaphore(sem);
    }

    image_acquired.clear();
    present_ready.clear();
}

void Swapchain::RefreshSemaphores() {
    const vk::Device device = instance.GetDevice();
    image_acquired.resize(image_count);
    present_ready.resize(image_count);

    for (vk::Semaphore& semaphore : image_acquired) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create image acquired semaphore: {}",
                   vk::to_string(semaphore_result));
        semaphore = sem;
    }
    for (vk::Semaphore& semaphore : present_ready) {
        auto [semaphore_result, sem] = device.createSemaphore({});
        ASSERT_MSG(semaphore_result == vk::Result::eSuccess,
                   "Failed to create present ready semaphore: {}", vk::to_string(semaphore_result));
        semaphore = sem;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, image_acquired[i], "Swapchain Semaphore: image_acquired {}", i);
        SetObjectName(device, present_ready[i], "Swapchain Semaphore: present_ready {}", i);
    }
}

void Swapchain::SetupImages() {
    vk::Device device = instance.GetDevice();
    if (!host_present) {
        auto [images_result, imgs] = device.getSwapchainImagesKHR(swapchain);
        ASSERT_MSG(images_result == vk::Result::eSuccess, "Failed to create swapchain images: {}",
                   vk::to_string(images_result));
        images = std::move(imgs);
    }
    image_count = static_cast<u32>(images.size());
    images_view.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        if (images_view[i]) {
            device.destroyImageView(images_view[i]);
        }
        auto [im_view_result, im_view] = device.createImageView(vk::ImageViewCreateInfo{
            .image = images[i],
            .viewType = vk::ImageViewType::e2D,
            .format = needs_hdr ? SURFACE_FORMAT_HDR.format : surface_format.format,
            .subresourceRange =
                {
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .levelCount = 1,
                    .layerCount = 1,
                },
        });
        ASSERT_MSG(im_view_result == vk::Result::eSuccess, "Failed to create image view: {}",
                   vk::to_string(im_view_result));
        images_view[i] = im_view;
    }

    for (u32 i = 0; i < image_count; ++i) {
        SetObjectName(device, images[i], "Swapchain Image {}", i);
        SetObjectName(device, images_view[i], "Swapchain ImageView {}", i);
    }
}

} // namespace Vulkan
