// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// libSceFiber for the FEX guest CPU. A fiber switch swaps the guest stack and callee-saved
// registers, which a native HLE call cannot do, so this port of fiber.cpp is built as x86-64
// code (tools/odin3/build_fiber_guest.sh writes fiber_guest.inc) and runs inside the guest.
// The code is mapped read-only and executable: no writable globals, no external calls.

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef int s32;
typedef signed char s8;

#define ORBIS_OK 0
#define ORBIS_FIBER_ERROR_NULL ((s32)0x80590001)
#define ORBIS_FIBER_ERROR_ALIGNMENT ((s32)0x80590002)
#define ORBIS_FIBER_ERROR_RANGE ((s32)0x80590003)
#define ORBIS_FIBER_ERROR_INVALID ((s32)0x80590004)
#define ORBIS_FIBER_ERROR_PERMISSION ((s32)0x80590005)
#define ORBIS_FIBER_ERROR_STATE ((s32)0x80590006)

#define ORBIS_FIBER_MAX_NAME_LENGTH 31
#define ORBIS_FIBER_CONTEXT_MINIMUM_SIZE 512
#define FW_350 0x3500000u

enum { FiberRun = 1, FiberIdle = 2, FiberTerminated = 3 };
enum { FlagContextSizeCheck = 0x10, FlagSetFpuRegs = 0x100 };

#define kFiberSignature0 0xdef1649cu
#define kFiberSignature1 0xb37592a0u
#define kFiberOptSignature 0xbb40e64du
#define kFiberStackSignature 0x7149f2ca7149f2caull

typedef void (*OrbisFiberEntry)(u64 arg_on_initialize, u64 arg_on_run);

typedef struct OrbisFiber OrbisFiber;

// Same layouts as fiber.h; the assembly below addresses these fields by offset.
typedef struct {
    u64 rax, rcx, rdx, rbx, rsp, rbp, r8, r9, r10, r11, r12, r13, r14, r15;
    u16 fpucw;
    u32 mxcsr;
    OrbisFiber* current_fiber;
    OrbisFiber* prev_fiber;
    u64 arg_on_run_to;
    u64 arg_on_return;
    u64 return_val;
} OrbisFiberContext;

typedef struct {
    OrbisFiberEntry entry;
    u64 arg_on_initialize;
    u64 arg_on_run_to;
    void* stack_addr;
    u32* state;
    u16 fpucw;
    s8 pad[2];
    u32 mxcsr;
} OrbisFiberData;

struct OrbisFiber {
    u32 magic_start;
    u32 state;
    OrbisFiberEntry entry;
    u64 arg_on_initialize;
    void* addr_context;
    u64 size_context;
    char name[ORBIS_FIBER_MAX_NAME_LENGTH + 1];
    OrbisFiberContext* context;
    u32 flags;
    void* context_start;
    void* context_end;
    u32 magic_end;
};

typedef struct {
    u64 size;
    OrbisFiberEntry entry;
    u64 arg_on_initialize;
    void* addr_context;
    u64 size_context;
    char name[ORBIS_FIBER_MAX_NAME_LENGTH + 1];
    u64 size_context_margin;
    u8 pad[48];
} OrbisFiberInfo;

typedef struct {
    u32 magic;
} OrbisFiberOptParam;

_Static_assert(__builtin_offsetof(OrbisFiberContext, current_fiber) == 0x78, "context layout");
_Static_assert(__builtin_offsetof(OrbisFiberData, mxcsr) == 0x2c, "data layout");
_Static_assert(sizeof(OrbisFiberInfo) == 128, "info layout");

__attribute__((returns_twice)) s32 _sceFiberSetJmp(OrbisFiberContext* ctx);
__attribute__((noreturn)) void _sceFiberLongJmp(OrbisFiberContext* ctx);
__attribute__((noreturn)) void _sceFiberSwitchEntry(OrbisFiberData* data, int set_fpu);

__asm__(".text\n"
        "_sceFiberSetJmp:\n"
        "    movq %rax, 0x0(%rdi)\n"
        "    movq (%rsp), %rdx\n"
        "    movq %rdx, 0x10(%rdi)\n"
        "    movq %rcx, 0x08(%rdi)\n"
        "    movq %rbx, 0x18(%rdi)\n"
        "    movq %rsp, 0x20(%rdi)\n"
        "    movq %rbp, 0x28(%rdi)\n"
        "    movq %r8,  0x30(%rdi)\n"
        "    movq %r9,  0x38(%rdi)\n"
        "    movq %r10, 0x40(%rdi)\n"
        "    movq %r11, 0x48(%rdi)\n"
        "    movq %r12, 0x50(%rdi)\n"
        "    movq %r13, 0x58(%rdi)\n"
        "    movq %r14, 0x60(%rdi)\n"
        "    movq %r15, 0x68(%rdi)\n"
        "    fnstcw  0x70(%rdi)\n"
        "    stmxcsr 0x72(%rdi)\n"
        "    xor %eax, %eax\n"
        "    ret\n"
        "\n"
        "_sceFiberLongJmp:\n"
        // MXCSR = (MXCSR & 0x3f) ^ (ctx->mxcsr & ~0x3f)
        "    stmxcsr -0x4(%rsp)\n"
        "    movl 0x72(%rdi), %eax\n"
        "    andl $0xffffffc0, %eax\n"
        "    movl -0x4(%rsp), %ecx\n"
        "    andl $0x3f, %ecx\n"
        "    xorl %eax, %ecx\n"
        "    movl %ecx, -0x4(%rsp)\n"
        "    ldmxcsr -0x4(%rsp)\n"
        "    movq 0x00(%rdi), %rax\n"
        "    movq 0x08(%rdi), %rcx\n"
        "    movq 0x10(%rdi), %rdx\n"
        "    movq 0x18(%rdi), %rbx\n"
        "    movq 0x20(%rdi), %rsp\n"
        "    movq 0x28(%rdi), %rbp\n"
        "    movq 0x30(%rdi), %r8\n"
        "    movq 0x38(%rdi), %r9\n"
        "    movq 0x40(%rdi), %r10\n"
        "    movq 0x48(%rdi), %r11\n"
        "    movq 0x50(%rdi), %r12\n"
        "    movq 0x58(%rdi), %r13\n"
        "    movq 0x60(%rdi), %r14\n"
        "    movq 0x68(%rdi), %r15\n"
        "    fldcw 0x70(%rdi)\n"
        // Make the jump and return 1
        "    movq %rdx, 0x00(%rsp)\n"
        "    movl $0x1, %eax\n"
        "    ret\n"
        "\n"
        "_sceFiberSwitchEntry:\n"
        "    mov %rdi, %r11\n"
        "    movq 0x18(%r11), %rsp\n"
        "    xorl %ebp, %ebp\n"
        "    movq 0x20(%r11), %r10\n"
        // Set previous fiber state to Idle
        "    test %r10, %r10\n"
        "    jz 1f\n"
        "    movl $2, (%r10)\n"
        "1:\n"
        "    test %esi, %esi\n"
        "    jz 2f\n"
        "    ldmxcsr 0x2c(%r11)\n"
        "    fldcw 0x28(%r11)\n"
        "2:\n"
        "    movq 0x08(%r11), %rdi\n"
        "    movq 0x10(%r11), %rsi\n"
        "    movq 0x00(%r11), %r11\n"
        "    xorl %eax, %eax\n"
        "    xorl %ebx, %ebx\n"
        "    xorl %ecx, %ecx\n"
        "    xorl %edx, %edx\n"
        "    xorq %r8, %r8\n"
        "    xorq %r9, %r9\n"
        "    xorq %r10, %r10\n"
        "    xorq %r12, %r12\n"
        "    xorq %r13, %r13\n"
        "    xorq %r14, %r14\n"
        "    xorq %r15, %r15\n"
        "    pxor %mm0, %mm0\n"
        "    pxor %mm1, %mm1\n"
        "    pxor %mm2, %mm2\n"
        "    pxor %mm3, %mm3\n"
        "    pxor %mm4, %mm4\n"
        "    pxor %mm5, %mm5\n"
        "    pxor %mm6, %mm6\n"
        "    pxor %mm7, %mm7\n"
        "    emms\n"
        "    vzeroall\n"
        // entry(arg_on_initialize, arg_on_run_to)
        "    call *%r11\n"
        // The fiber entry returned: _sceFiberForceQuit(1)
        "    movl $1, %edi\n"
        "    call _sceFiberForceQuit\n"
        "    ud2\n");

// clang may lower struct copies and zeroing to these even when freestanding.
void* memset(void* dst, int value, u64 size) {
    u8* d = (u8*)dst;
    while (size--) {
        *d++ = (u8)value;
    }
    return dst;
}

void* memcpy(void* dst, const void* src, u64 size) {
    u8* d = (u8*)dst;
    const u8* s = (const u8*)src;
    while (size--) {
        *d++ = *s++;
    }
    return dst;
}

static void CopyName(char* dst, const char* src) {
    u32 i = 0;
    for (; i < ORBIS_FIBER_MAX_NAME_LENGTH && src[i]; ++i) {
        dst[i] = src[i];
    }
    for (; i < ORBIS_FIBER_MAX_NAME_LENGTH; ++i) {
        dst[i] = 0;
    }
}

static OrbisFiberContext** FiberContextSlot(void) {
    // fs:0 is Core::Tcb::tcb_self; tcb_fiber follows tcb_dtv and tcb_thread.
    u64 tcb;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tcb));
    return (OrbisFiberContext**)(tcb + 0x18);
}

static OrbisFiberContext* GetFiberContext(void) {
    return *FiberContextSlot();
}

static int ClaimIdle(OrbisFiber* fiber, u32 new_state) {
    u32 expected = FiberIdle;
    return __atomic_compare_exchange_n(&fiber->state, &expected, new_state, 0, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}

static int ValidFiber(const OrbisFiber* fiber) {
    return fiber->magic_start == kFiberSignature0 && fiber->magic_end == kFiberSignature1;
}

__attribute__((noreturn, used)) void _sceFiberForceQuit(u64 ret) {
    OrbisFiberContext* g_ctx = GetFiberContext();
    g_ctx->return_val = ret;
    _sceFiberLongJmp(g_ctx);
}

static void CheckStackOverflow(OrbisFiberContext* ctx) {
    u64* stack_base = (u64*)ctx->current_fiber->addr_context;
    if (stack_base && *stack_base != kFiberStackSignature) {
        __builtin_trap();
    }
}

static void FillContextCheck(OrbisFiber* fiber) {
    u64* stack_ptr = (u64*)fiber->context_start + 1;
    u64* stack_end = (u64*)fiber->context_end;
    while (stack_ptr < stack_end) {
        *stack_ptr++ = 0xdeadbeefdeadbeefull;
    }
}

static s32 AttachContext(OrbisFiber* fiber, void* addr_context, u64 size_context) {
    if (size_context && size_context < ORBIS_FIBER_CONTEXT_MINIMUM_SIZE) {
        return ORBIS_FIBER_ERROR_RANGE;
    }
    if (size_context & 15) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    if (!addr_context || !size_context) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    if (fiber->addr_context) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    fiber->addr_context = addr_context;
    fiber->size_context = size_context;
    fiber->context_start = addr_context;
    fiber->context_end = (u8*)addr_context + size_context;
    *(u64*)addr_context = kFiberStackSignature;
    if (fiber->flags & FlagContextSizeCheck) {
        FillContextCheck(fiber);
    }
    return ORBIS_OK;
}

__attribute__((noreturn)) static void StartFiber(OrbisFiber* fiber, u64 arg_on_run_to,
                                                 void* stack_addr, u32* prev_state) {
    OrbisFiberData data = {0};
    data.entry = fiber->entry;
    data.arg_on_initialize = fiber->arg_on_initialize;
    data.arg_on_run_to = arg_on_run_to;
    data.stack_addr = stack_addr;
    data.state = prev_state;
    if (fiber->flags & FlagSetFpuRegs) {
        data.fpucw = 0x037f;
        data.mxcsr = 0x9fc0;
        _sceFiberSwitchEntry(&data, 1);
    }
    _sceFiberSwitchEntry(&data, 0);
}

__attribute__((noreturn)) static void SwitchToFiber(OrbisFiber* fiber, u64 arg_on_run_to,
                                                    OrbisFiberContext* ctx) {
    OrbisFiberContext* fiber_ctx = fiber->context;
    if (fiber_ctx) {
        ctx->arg_on_run_to = arg_on_run_to;
        _sceFiberLongJmp(fiber_ctx);
    }
    u32* prev_state = 0;
    if (ctx->prev_fiber) {
        prev_state = &ctx->prev_fiber->state;
        ctx->prev_fiber = 0;
    }
    StartFiber(fiber, arg_on_run_to, (u8*)fiber->addr_context + fiber->size_context, prev_state);
}

__attribute__((noreturn)) static void Switch(OrbisFiber* cur_fiber, OrbisFiber* fiber,
                                             u64 arg_on_run_to, OrbisFiberContext* ctx) {
    ctx->prev_fiber = cur_fiber;
    ctx->current_fiber = fiber;
    if (fiber->addr_context == 0) {
        ctx->prev_fiber = 0;
        StartFiber(fiber, arg_on_run_to, (void*)(ctx->rsp & ~15ull), &cur_fiber->state);
    }
    SwitchToFiber(fiber, arg_on_run_to, ctx);
}

s32 sceFiberInitializeImpl(OrbisFiber* fiber, const char* name, OrbisFiberEntry entry,
                           u64 arg_on_initialize, void* addr_context, u64 size_context,
                           const OrbisFiberOptParam* opt_param, u32 flags, u32 build_ver) {
    if (!fiber || !name || !entry) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7 || (u64)addr_context & 15) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (opt_param && (u64)opt_param & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (size_context && size_context < ORBIS_FIBER_CONTEXT_MINIMUM_SIZE) {
        return ORBIS_FIBER_ERROR_RANGE;
    }
    if (size_context & 15) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    if (!addr_context != !size_context) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    if (opt_param && opt_param->magic != kFiberOptSignature) {
        return ORBIS_FIBER_ERROR_INVALID;
    }

    u32 user_flags = flags;
    if (build_ver >= FW_350) {
        user_flags |= FlagSetFpuRegs;
    }

    CopyName(fiber->name, name);
    fiber->entry = entry;
    fiber->arg_on_initialize = arg_on_initialize;
    fiber->addr_context = addr_context;
    fiber->size_context = size_context;
    fiber->context = 0;
    fiber->flags = user_flags;
    fiber->magic_start = kFiberSignature0;
    fiber->magic_end = kFiberSignature1;

    if (addr_context) {
        fiber->context_start = addr_context;
        fiber->context_end = (u8*)addr_context + size_context;
        *(u64*)addr_context = kFiberStackSignature;
        if (flags & FlagContextSizeCheck) {
            FillContextCheck(fiber);
        }
    }

    __atomic_store_n(&fiber->state, FiberIdle, __ATOMIC_SEQ_CST);
    return ORBIS_OK;
}

s32 sceFiberInitialize(OrbisFiber* fiber, const char* name, OrbisFiberEntry entry,
                       u64 arg_on_initialize, void* addr_context, u64 size_context,
                       const OrbisFiberOptParam* opt_param, u32 build_ver) {
    return sceFiberInitializeImpl(fiber, name, entry, arg_on_initialize, addr_context,
                                  size_context, opt_param, 0, build_ver);
}

s32 sceFiberOptParamInitialize(OrbisFiberOptParam* opt_param) {
    if (!opt_param) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)opt_param & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    opt_param->magic = kFiberOptSignature;
    return ORBIS_OK;
}

s32 sceFiberFinalize(OrbisFiber* fiber) {
    if (!fiber) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (!ValidFiber(fiber)) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    if (!ClaimIdle(fiber, FiberTerminated)) {
        return ORBIS_FIBER_ERROR_STATE;
    }
    return ORBIS_OK;
}

s32 sceFiberRunImpl(OrbisFiber* fiber, void* addr_context, u64 size_context, u64 arg_on_run_to,
                    u64* arg_on_return) {
    if (!fiber) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7 || (u64)addr_context & 15) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (!ValidFiber(fiber)) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    OrbisFiberContext** slot = FiberContextSlot();
    if (*slot) {
        return ORBIS_FIBER_ERROR_PERMISSION;
    }
    if (addr_context || size_context) {
        const s32 res = AttachContext(fiber, addr_context, size_context);
        if (res < 0) {
            return res;
        }
    }
    if (!ClaimIdle(fiber, FiberRun)) {
        return ORBIS_FIBER_ERROR_STATE;
    }

    OrbisFiberContext ctx = {0};
    ctx.current_fiber = fiber;
    *slot = &ctx;

    if (!_sceFiberSetJmp(&ctx)) {
        if (fiber->addr_context) {
            SwitchToFiber(fiber, arg_on_run_to, &ctx);
        }
        StartFiber(fiber, arg_on_run_to, (void*)(ctx.rsp & ~15ull), 0);
    }

    OrbisFiber* cur_fiber = ctx.current_fiber;
    ctx.current_fiber = 0;
    __atomic_store_n(&cur_fiber->state, FiberIdle, __ATOMIC_SEQ_CST);
    if (ctx.return_val != 0) {
        // The fiber entry function returned.
        __builtin_trap();
    }
    if (arg_on_return) {
        *arg_on_return = ctx.arg_on_return;
    }
    *FiberContextSlot() = 0;
    return ORBIS_OK;
}

s32 sceFiberRun(OrbisFiber* fiber, u64 arg_on_run_to, u64* arg_on_return) {
    return sceFiberRunImpl(fiber, 0, 0, arg_on_run_to, arg_on_return);
}

s32 sceFiberSwitchImpl(OrbisFiber* fiber, void* addr_context, u64 size_context, u64 arg_on_run_to,
                       u64* arg_on_run) {
    if (!fiber) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7 || (u64)addr_context & 15) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (!ValidFiber(fiber)) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    OrbisFiberContext* g_ctx = GetFiberContext();
    if (!g_ctx) {
        return ORBIS_FIBER_ERROR_PERMISSION;
    }
    if (addr_context || size_context) {
        const s32 res = AttachContext(fiber, addr_context, size_context);
        if (res < 0) {
            return res;
        }
    }
    if (!ClaimIdle(fiber, FiberRun)) {
        return ORBIS_FIBER_ERROR_STATE;
    }

    OrbisFiber* cur_fiber = g_ctx->current_fiber;
    if (cur_fiber->addr_context == 0) {
        Switch(cur_fiber, fiber, arg_on_run_to, g_ctx);
    }

    OrbisFiberContext ctx = {0};
    if (!_sceFiberSetJmp(&ctx)) {
        cur_fiber->context = &ctx;
        CheckStackOverflow(g_ctx);
        Switch(cur_fiber, fiber, arg_on_run_to, g_ctx);
    }

    g_ctx = GetFiberContext();
    if (g_ctx->prev_fiber) {
        __atomic_store_n(&g_ctx->prev_fiber->state, FiberIdle, __ATOMIC_SEQ_CST);
        g_ctx->prev_fiber = 0;
    }
    if (arg_on_run) {
        *arg_on_run = g_ctx->arg_on_run_to;
    }
    return ORBIS_OK;
}

s32 sceFiberSwitch(OrbisFiber* fiber, u64 arg_on_run_to, u64* arg_on_run) {
    return sceFiberSwitchImpl(fiber, 0, 0, arg_on_run_to, arg_on_run);
}

s32 sceFiberGetSelf(OrbisFiber** fiber) {
    if (!fiber) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    OrbisFiberContext* g_ctx = GetFiberContext();
    if (!g_ctx) {
        return ORBIS_FIBER_ERROR_PERMISSION;
    }
    *fiber = g_ctx->current_fiber;
    return ORBIS_OK;
}

s32 sceFiberReturnToThread(u64 arg_on_return, u64* arg_on_run) {
    OrbisFiberContext* g_ctx = GetFiberContext();
    if (!g_ctx) {
        return ORBIS_FIBER_ERROR_PERMISSION;
    }
    OrbisFiber* cur_fiber = g_ctx->current_fiber;
    OrbisFiberContext ctx = {0};
    if (cur_fiber->addr_context) {
        if (_sceFiberSetJmp(&ctx)) {
            g_ctx = GetFiberContext();
            if (g_ctx->prev_fiber) {
                __atomic_store_n(&g_ctx->prev_fiber->state, FiberIdle, __ATOMIC_SEQ_CST);
                g_ctx->prev_fiber = 0;
            }
            if (arg_on_run) {
                *arg_on_run = g_ctx->arg_on_run_to;
            }
            return ORBIS_OK;
        }
        cur_fiber->context = &ctx;
        CheckStackOverflow(g_ctx);
    }
    g_ctx->arg_on_return = arg_on_return;
    _sceFiberLongJmp(g_ctx);
}

s32 sceFiberGetInfo(OrbisFiber* fiber, OrbisFiberInfo* fiber_info) {
    if (!fiber || !fiber_info) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7 || (u64)fiber_info & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (fiber_info->size != sizeof(OrbisFiberInfo)) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    if (!ValidFiber(fiber)) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    fiber_info->entry = fiber->entry;
    fiber_info->arg_on_initialize = fiber->arg_on_initialize;
    fiber_info->addr_context = fiber->addr_context;
    fiber_info->size_context = fiber->size_context;
    CopyName(fiber_info->name, fiber->name);
    fiber_info->size_context_margin = -1ull;
    if (fiber->flags & FlagContextSizeCheck && fiber->addr_context) {
        u64 stack_margin = 0;
        u64* stack_start = (u64*)fiber->context_start;
        u64* stack_end = (u64*)fiber->context_end;
        if (*stack_start == kFiberStackSignature) {
            u64* stack_ptr = stack_start + 1;
            while (stack_ptr < stack_end && *stack_ptr == 0xdeadbeefdeadbeefull) {
                stack_ptr++;
            }
            stack_margin = (u64)stack_ptr - (u64)(stack_start + 1);
        }
        fiber_info->size_context_margin = stack_margin;
    }
    return ORBIS_OK;
}

// Context size checks are a debugging aid that needs writable global state: accepted, not done.
s32 sceFiberStartContextSizeCheck(u32 flags) {
    return flags != 0 ? ORBIS_FIBER_ERROR_INVALID : ORBIS_OK;
}

s32 sceFiberStopContextSizeCheck(void) {
    return ORBIS_OK;
}

s32 sceFiberRename(OrbisFiber* fiber, const char* name) {
    if (!fiber || !name) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    if ((u64)fiber & 7) {
        return ORBIS_FIBER_ERROR_ALIGNMENT;
    }
    if (!ValidFiber(fiber)) {
        return ORBIS_FIBER_ERROR_INVALID;
    }
    CopyName(fiber->name, name);
    return ORBIS_OK;
}

s32 sceFiberGetThreadFramePointerAddress(u64* addr_frame_pointer) {
    if (!addr_frame_pointer) {
        return ORBIS_FIBER_ERROR_NULL;
    }
    OrbisFiberContext* g_ctx = GetFiberContext();
    if (!g_ctx) {
        return ORBIS_FIBER_ERROR_PERMISSION;
    }
    *addr_frame_pointer = g_ctx->rbp;
    return ORBIS_OK;
}
