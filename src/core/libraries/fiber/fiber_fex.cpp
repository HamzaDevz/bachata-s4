// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "core/libraries/fiber/fiber.h"
#include "core/libraries/libs.h"

namespace Libraries::Fiber {

#include "core/libraries/fiber/fiber_guest.inc"

// Under FEX the fiber library is guest code (fiber_guest.c): switching fibers swaps guest stacks
// and registers, out of reach of a native HLE call.
void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    const auto add = [sym](const char* nid, u32 offset) {
        Core::Loader::SymbolResolver sr{};
        sr.name = nid;
        sr.library = "libSceFiber";
        sr.library_version = 1;
        sr.module = "libSceFiber";
        sr.type = Core::Loader::SymbolType::Function;
        sym->AddGuestFunction(sr, FiberGuestCode, offset);
    };
    add("hVYD7Ou2pCQ", FiberGuest_sceFiberInitialize);
    add("7+OJIpko9RY", FiberGuest_sceFiberInitializeImpl); // _sceFiberInitializeWithInternalOptionImpl
    add("asjUJJ+aa8s", FiberGuest_sceFiberOptParamInitialize);
    add("JeNX5F-NzQU", FiberGuest_sceFiberFinalize);
    add("a0LLrZWac0M", FiberGuest_sceFiberRun);
    add("PFT2S-tJ7Uk", FiberGuest_sceFiberSwitch);
    add("p+zLIOg27zU", FiberGuest_sceFiberGetSelf);
    add("B0ZX2hx9DMw", FiberGuest_sceFiberReturnToThread);
    add("avfGJ94g36Q", FiberGuest_sceFiberRunImpl);    // _sceFiberAttachContextAndRun
    add("ZqhZFuzKT6U", FiberGuest_sceFiberSwitchImpl); // _sceFiberAttachContextAndSwitch
    add("uq2Y5BFz0PE", FiberGuest_sceFiberGetInfo);
    add("Lcqty+QNWFc", FiberGuest_sceFiberStartContextSizeCheck);
    add("Kj4nXMpnM8Y", FiberGuest_sceFiberStopContextSizeCheck);
    add("JzyT91ucGDc", FiberGuest_sceFiberRename);
    add("0dy4JtMUcMQ", FiberGuest_sceFiberGetThreadFramePointerAddress);
}

} // namespace Libraries::Fiber
