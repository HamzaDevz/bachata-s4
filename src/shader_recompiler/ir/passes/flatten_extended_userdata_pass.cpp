// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/container/flat_map.hpp>
#include <xbyak/xbyak.h>
#include <xbyak/xbyak_util.h>
#include "common/arch.h"
#include "common/decoder.h"
#include "common/io_file.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/signal_context.h"
#include "core/emulator_settings.h"
#include "core/signals.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/opcodes.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/srt_gvn_table.h"
#include "shader_recompiler/ir/value.h"

#if defined(ARCH_ARM64) && defined(__linux__)
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <unistd.h>
#endif

#ifdef ARCH_X86_64

using namespace Xbyak::util;

static Xbyak::CodeGenerator g_srt_codegen(32_MB);
static const u8* g_srt_codegen_start = nullptr;

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    const auto func_addr = (PFN_SrtWalker)g_srt_codegen.getCurr();
    g_srt_codegen.db(ptr, size);
    g_srt_codegen.ready();
    return func_addr;
}

} // namespace Shader

namespace {

static void DumpSrtProgram(const Shader::Info& info, const u8* code, size_t codesize) {
    using namespace Common::FS;

    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}_{:#018x}.srtprogram.txt", info.stage, info.pgm_hash);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create, FileType::TextFile};

    u64 address = reinterpret_cast<u64>(code);
    u64 code_end = address + codesize;
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = ZYAN_STATUS_SUCCESS;
    while (address < code_end && ZYAN_SUCCESS(Common::Decoder::Instance()->decodeInstruction(
                                     instruction, operands, reinterpret_cast<void*>(address)))) {
        std::string s =
            Common::Decoder::Instance()->disassembleInst(instruction, operands, address);
        s += "\n";
        file.WriteString(s);
        address += instruction.length;
    }
}

static bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    // Only handle if the fault address is within the SRT code range
    const u8* code_start = g_srt_codegen_start;
    const u8* code_end = code_start + g_srt_codegen.getSize();
    const void* code = Common::GetRip(context);
    if (code < code_start || code >= code_end) {
        return false; // Not in SRT code range
    }

    // Patch instruction to zero register
    ZydisDecodedInstruction instruction;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    ZyanStatus status = Common::Decoder::Instance()->decodeInstruction(instruction, operands,
                                                                       const_cast<void*>(code), 15);

    ASSERT(ZYAN_SUCCESS(status) && instruction.mnemonic == ZYDIS_MNEMONIC_MOV &&
           operands[0].type == ZYDIS_OPERAND_TYPE_REGISTER &&
           operands[1].type == ZYDIS_OPERAND_TYPE_MEMORY);

    size_t len = instruction.length;
    const size_t patch_size = 3;
    u8* code_patch = const_cast<u8*>(reinterpret_cast<const u8*>(code));

    // We can only encounter rdi or r10d as the first operand in a
    // fault memory access for SRT walker.
    switch (operands[0].reg.value) {
    case ZYDIS_REGISTER_RDI:
        // mov rdi, [rdi + (off_dw << 2)] -> xor rdi, rdi
        code_patch[0] = 0x48;
        code_patch[1] = 0x31;
        code_patch[2] = 0xFF;
        break;
    case ZYDIS_REGISTER_R10D:
        // mov r10d, [rdi + (off_dw << 2)] -> xor r10d, r10d
        code_patch[0] = 0x45;
        code_patch[1] = 0x31;
        code_patch[2] = 0xD2;
        break;
    default:
        UNREACHABLE_MSG("Unsupported register for SRT walker patch");
        return false;
    }

    // Fill nops
    memset(code_patch + patch_size, 0x90, len - patch_size);

    LOG_DEBUG(Render_Recompiler, "Patched SRT walker at {}", code);

    return true;
}

using namespace Shader;

struct PassInfo {
    // map offset to inst
    using PtrUserList = boost::container::flat_map<u32, Shader::IR::Inst*>;

    Optimization::SrtGvnTable gvn_table;
    // keys are GetUserData or ReadConst instructions that are used as pointers
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    // GetUserData instructions corresponding to sgpr_base of SRT roots
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;

    // pick a single inst for a given value number
    std::unordered_map<u32, IR::Inst*> vn_to_inst;

    // Bumped during codegen to assign offsets to readconsts
    u32 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        if (it != pointer_uses.end()) {
            return &it->second;
        }
        return nullptr;
    }

    // Return a single instruction that this instruction is identical to, according
    // to value number
    // The "original" is arbitrary. Here it's the first instruction found for a given value number
    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }
};
} // namespace

namespace Shader::Optimization {

namespace {

static inline void PushPtr(Xbyak::CodeGenerator& c, u32 off_dw) {
    c.push(rdi);
    c.mov(rdi, ptr[rdi + (off_dw << 2)]);
    c.mov(r10, 0xFFFFFFFFFFFFULL);
    c.and_(rdi, r10);
}

static inline void PopPtr(Xbyak::CodeGenerator& c) {
    c.pop(rdi);
};

static void VisitPointer(u32 off_dw, IR::Inst* subtree, PassInfo& pass_info,
                         Xbyak::CodeGenerator& c) {
    PushPtr(c, off_dw);
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    ASSERT(use_list);

    // First copy all the src data from this tree level
    // That way, all data that was contiguous in the guest SRT is also contiguous in the
    // flattened buffer.
    // TODO src and dst are contiguous. Optimize with wider loads/stores
    // TODO if this subtree is dynamically indexed, don't compact it (keep it sparse)
    for (auto [src_off_dw, use] : *use_list) {
        c.mov(r10d, ptr[rdi + (src_off_dw << 2)]);
        c.mov(ptr[rsi + (pass_info.dst_off_dw << 2)], r10d);

        use->SetFlags<u32>(pass_info.dst_off_dw);
        pass_info.dst_off_dw++;
    }

    // Then visit any children used as pointers
    for (const auto [src_off_dw, use] : *use_list) {
        if (pass_info.GetUsesAsPointer(use)) {
            VisitPointer(src_off_dw, use, pass_info, c);
        }
    }

    PopPtr(c);
}

static void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    Xbyak::CodeGenerator& c = g_srt_codegen;

    if (pass_info.srt_roots.empty()) {
        return;
    }

    // Register the signal handler for SRT walker, if not already registered
    if (g_srt_codegen_start == nullptr) {
        g_srt_codegen_start = c.getCurr();
        auto* signals = Core::Signals::Instance();
        // Call after the memory invalidation handler
        constexpr u32 priority = 1;
        signals->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    }

    info.srt_info.walker_func = c.getCurr<PFN_SrtWalker>();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);

    for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
        VisitPointer(static_cast<u32>(sgpr_base), root, pass_info, c);
    }

    c.ret();
    c.ready();

    info.srt_info.walker_func_size =
        c.getCurr() - reinterpret_cast<const u8*>(info.srt_info.walker_func);

    if (EmulatorSettings.IsDumpShaders()) {
        DumpSrtProgram(info, reinterpret_cast<const u8*>(info.srt_info.walker_func),
                       info.srt_info.walker_func_size);
    }

    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

}; // namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    Shader::Info& info = program.info;
    PassInfo pass_info;

    // traverse at end and assign offsets to duplicate readconsts, using
    // vn_to_inst as the source
    boost::container::small_vector<IR::Inst*, 32> all_readconsts;

    for (auto r_it = program.post_order_blocks.rbegin(); r_it != program.post_order_blocks.rend();
         r_it++) {
        IR::Block* block = *r_it;
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() == IR::Opcode::ReadConst) {
                if (!inst.Arg(1).IsImmediate()) {
                    LOG_WARNING(Render_Recompiler, "ReadConst has non-immediate offset");
                    continue;
                }

                all_readconsts.push_back(&inst);
                if (pass_info.DeduplicateInstruction(&inst) != &inst) {
                    // This is a duplicate of a readconst we've already visited
                    continue;
                }

                IR::Inst* ptr_composite = inst.Arg(0).InstRecursive();

                const auto pred = [](IR::Inst* inst) -> std::optional<IR::Inst*> {
                    if (inst->GetOpcode() == IR::Opcode::GetUserData ||
                        inst->GetOpcode() == IR::Opcode::ReadConst) {
                        return inst;
                    }
                    return std::nullopt;
                };
                auto base0 = IR::BreadthFirstSearch(ptr_composite->Arg(0), pred);
                auto base1 = IR::BreadthFirstSearch(ptr_composite->Arg(1), pred);
                ASSERT_MSG(base0 && base1, "ReadConst not from constant memory");

                IR::Inst* ptr_lo = base0.value();
                ptr_lo = pass_info.DeduplicateInstruction(ptr_lo);

                auto ptr_uses_kv =
                    pass_info.pointer_uses.try_emplace(ptr_lo, PassInfo::PtrUserList{});
                PassInfo::PtrUserList& user_list = ptr_uses_kv.first->second;

                user_list[inst.Arg(1).U32()] = &inst;

                if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
                    IR::ScalarReg ud_reg = ptr_lo->Arg(0).ScalarReg();
                    pass_info.srt_roots[ud_reg] = ptr_lo;
                }
            }
        }
    }

    GenerateSrtProgram(info, pass_info);

    // Assign offsets to duplicate readconsts
    for (IR::Inst* readconst : all_readconsts) {
        ASSERT(pass_info.vn_to_inst.contains(pass_info.gvn_table.GetValueNumber(readconst)));
        IR::Inst* original = pass_info.DeduplicateInstruction(readconst);
        readconst->SetFlags<u32>(original->Flags<u32>());
    }

    info.RefreshFlatBuf();
}

} // namespace Shader::Optimization

#elif defined(ARCH_ARM64) && defined(__linux__)

namespace {

constexpr u32 Arm64MovX2X0 = 0xaa0003e2;
constexpr u32 Arm64PushX2 = 0xf81f0fe2;
constexpr u32 Arm64PopX2 = 0xf84107e2;
constexpr u32 Arm64LoadPointer = 0xf8646842;
constexpr u32 Arm64MaskPointer = 0xd340bc42;
constexpr u32 Arm64LoadDataRegisterOffset = 0xb8646843;
constexpr u32 Arm64StoreDataRegisterOffset = 0xb8256823;
constexpr u32 Arm64Ret = 0xd65f03c0;
constexpr size_t MaxSrtCodeRanges = 32768;

struct SrtCodeRange {
    uintptr_t begin{};
    uintptr_t end{};
};

struct SrtCodeMapping {
    u8* data{};
    size_t size{};

    ~SrtCodeMapping() {
        if (data != nullptr) {
            munmap(data, size);
        }
    }
};

std::array<SrtCodeRange, MaxSrtCodeRanges> g_srt_code_ranges{};
std::atomic_size_t g_srt_code_range_count{};
std::mutex g_srt_code_mutex;
std::vector<std::unique_ptr<SrtCodeMapping>> g_srt_code_mappings;
std::once_flag g_srt_signal_once;

bool IsSrtCodeAddress(uintptr_t pc) {
    const size_t count = g_srt_code_range_count.load(std::memory_order_acquire);
    for (size_t index = 0; index < count; ++index) {
        const auto& range = g_srt_code_ranges[index];
        if (pc >= range.begin && pc < range.end) {
            return true;
        }
    }
    return false;
}

bool SrtWalkerSignalHandler(void* context, void* fault_address) {
    const auto pc = reinterpret_cast<uintptr_t>(Common::GetRip(context));
    if (!IsSrtCodeAddress(pc)) {
        return false;
    }

    u32 instruction{};
    std::memcpy(&instruction, reinterpret_cast<const void*>(pc), sizeof(instruction));
    auto* signal_context = static_cast<ucontext_t*>(context);
    if (instruction == Arm64LoadPointer) {
        signal_context->uc_mcontext.regs[2] = 0;
    } else if ((instruction & 0xffc003ffu) == 0xb9400043u ||
               instruction == Arm64LoadDataRegisterOffset) {
        signal_context->uc_mcontext.regs[3] = 0;
    } else {
        return false;
    }
    Common::IncrementRip(context, sizeof(u32));
    return true;
}

bool IsArm64SrtWalker(const u8* ptr, size_t size) {
    if (ptr == nullptr || size < sizeof(u32) * 2 || size % sizeof(u32) != 0) {
        return false;
    }
    u32 first{};
    u32 last{};
    std::memcpy(&first, ptr, sizeof(first));
    std::memcpy(&last, ptr + size - sizeof(last), sizeof(last));
    return first == Arm64MovX2X0 && last == Arm64Ret;
}

class Arm64SrtEmitter {
public:
    void Begin() {
        Emit(Arm64MovX2X0);
    }

    // Saves the pointer in x2 and replaces it with the one at byte offset x4 from it.
    void PushPointer() {
        Emit(Arm64PushX2);
        Emit(Arm64LoadPointer);
        Emit(Arm64MaskPointer);
    }

    void PopPointer() {
        Emit(Arm64PopX2);
    }

    void SetSourceOffset(u32 offset_dw) {
        MoveImmediate(4, static_cast<u64>(offset_dw) * sizeof(u32));
    }

    // x4 = the dword index in w<reg>, zero extended, as a byte offset.
    void SetSourceOffsetFromRegister(u32 reg) {
        Emit(0xd37e7c04u | (reg << 5)); // ubfiz x4, x<reg>, #2, #32
    }

    void CopyDword(u32 source_offset_dw, u32 destination_offset_dw) {
        if (source_offset_dw <= 0xfff) {
            Emit(0xb9400043u | (source_offset_dw << 10));
        } else {
            SetSourceOffset(source_offset_dw);
            Emit(Arm64LoadDataRegisterOffset);
        }
        StoreDword(destination_offset_dw);
    }

    // Copies consecutive dwords from byte offset x4 of the pointer in x2.
    void CopyDwordsAtSourceOffset(u32 count, u32 destination_offset_dw) {
        for (u32 dword = 0; dword < count; ++dword) {
            if (dword != 0) {
                Emit(0x91001084u); // add x4, x4, #4
            }
            Emit(Arm64LoadDataRegisterOffset);
            StoreDword(destination_offset_dw + dword);
        }
    }

    // w<reg> = a flat buffer dword: user data, or a value this walker already copied.
    void LoadFlatDword(u32 reg, u32 offset_dw) {
        if (offset_dw <= 0xfff) {
            Emit(0xb9400020u | (offset_dw << 10) | reg);
        } else {
            MoveImmediate(5, static_cast<u64>(offset_dw) * sizeof(u32));
            Emit(0xb8656820u | reg); // ldr w<reg>, [x1, x5]
        }
    }

    // Writes a buffer descriptor over the memory at the pointer in x2: base address, no stride,
    // num_records bytes.
    void WritePointerWindow(u32 destination_offset_dw, u32 num_records) {
        Emit(0x2a0203e3u); // mov w3, w2
        StoreDword(destination_offset_dw);
        Emit(0xd360fc43u); // lsr x3, x2, #32
        StoreDword(destination_offset_dw + 1);
        MoveImmediate32(3, num_records);
        StoreDword(destination_offset_dw + 2);
        MoveImmediate32(3, 0);
        StoreDword(destination_offset_dw + 3);
    }

    void MoveImmediate32(u32 reg, u32 value) {
        Emit(0x52800000u | reg | ((value & 0xffff) << 5));
        if ((value >> 16) != 0) {
            Emit(0x72a00000u | reg | ((value >> 16) << 5));
        }
    }

    void Emit(u32 instruction) {
        code.push_back(instruction);
    }

    size_t Mark() const {
        return code.size();
    }

    void Rewind(size_t mark) {
        code.resize(mark);
    }

    void End() {
        Emit(Arm64Ret);
    }

    const u8* Data() const {
        return reinterpret_cast<const u8*>(code.data());
    }

    size_t Size() const {
        return code.size() * sizeof(u32);
    }

private:
    void StoreDword(u32 destination_offset_dw) {
        if (destination_offset_dw <= 0xfff) {
            Emit(0xb9000023u | (destination_offset_dw << 10));
        } else {
            MoveImmediate(5, static_cast<u64>(destination_offset_dw) * sizeof(u32));
            Emit(Arm64StoreDataRegisterOffset);
        }
    }

    void MoveImmediate(u32 reg, u64 value) {
        Emit(0xd2800000u | reg | (static_cast<u32>(value & 0xffff) << 5));
        for (u32 halfword = 1; halfword < 4; ++halfword) {
            const u32 part = static_cast<u32>((value >> (halfword * 16)) & 0xffff);
            if (part != 0) {
                Emit(0xf2800000u | reg | (halfword << 21) | (part << 5));
            }
        }
    }

    std::vector<u32> code;
};

using namespace Shader;

struct PassInfo {
    // Offsets are dword indices: immediates first in increasing order, then computed ones in
    // program order.
    struct OffsetCompare {
        bool operator()(const IR::Value& a, const IR::Value& b) const {
            if (a.IsImmediate() != b.IsImmediate()) {
                return a.IsImmediate();
            }
            if (a.IsImmediate()) {
                return a.U32() < b.U32();
            }
            const IR::Inst* a_inst = a.InstRecursive();
            const IR::Inst* b_inst = b.InstRecursive();
            if (a_inst == b_inst) {
                return false;
            }
            const IR::Block* a_block = a_inst->GetParent();
            const IR::Block* b_block = b_inst->GetParent();
            if (a_block != b_block) {
                return std::less<const IR::Block*>{}(a_block, b_block);
            }
            for (auto it = a_block->Instructions().iterator_to(*a_inst);
                 it != a_block->Instructions().end(); ++it) {
                if (&*it == b_inst) {
                    return true;
                }
            }
            return false;
        }
    };

    // A load the walker copies: one dword, or a whole descriptor read from a buffer.
    struct Entry {
        IR::Inst* use;
        u32 num_dwords;
    };

    using PtrUserList = boost::container::flat_multimap<IR::Value, Entry, OffsetCompare>;

    Optimization::SrtGvnTable gvn_table;
    std::unordered_map<IR::Inst*, PtrUserList> pointer_uses;
    boost::container::small_flat_map<IR::ScalarReg, IR::Inst*, 1> srt_roots;
    std::unordered_map<u32, IR::Inst*> vn_to_inst;
    // First flat buffer dword of each copied load.
    std::unordered_map<const IR::Inst*, u32> slots;
    // Pointer of each ReadConst that reads straight from it, and the flat buffer dword of the
    // descriptor the walker writes over a pointer whose loads it cannot all copy.
    std::unordered_map<const IR::Inst*, IR::Inst*> window_pointers;
    std::unordered_map<const IR::Inst*, u32> windows;
    u32 dst_off_dw;

    PtrUserList* GetUsesAsPointer(IR::Inst* inst) {
        auto it = pointer_uses.find(inst);
        return it != pointer_uses.end() ? &it->second : nullptr;
    }

    IR::Inst* DeduplicateInstruction(IR::Inst* inst) {
        auto it = vn_to_inst.try_emplace(gvn_table.GetValueNumber(inst), inst);
        return it.first->second;
    }

    std::optional<u32> FindSlot(IR::Inst* inst) {
        const auto it = slots.find(DeduplicateInstruction(inst));
        return it != slots.end() ? std::optional{it->second} : std::nullopt;
    }

    std::optional<u32> FindWindow(const IR::Inst* inst) const {
        const auto pointer = window_pointers.find(inst);
        if (pointer == window_pointers.end()) {
            return std::nullopt;
        }
        const auto it = windows.find(pointer->second);
        return it != windows.end() ? std::optional{it->second} : std::nullopt;
    }

    bool HasPending(IR::Inst* subtree) {
        for (const auto& [offset, entry] : *GetUsesAsPointer(subtree)) {
            if (!slots.contains(entry.use) ||
                (GetUsesAsPointer(entry.use) != nullptr && HasPending(entry.use))) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    if (!IsArm64SrtWalker(ptr, size)) {
        LOG_WARNING(Render_Recompiler,
                    "Ignoring incompatible cached SRT walker; shader will be recompiled");
        return nullptr;
    }

    std::lock_guard lock{g_srt_code_mutex};
    const size_t range_index = g_srt_code_range_count.load(std::memory_order_relaxed);
    if (range_index >= g_srt_code_ranges.size()) {
        LOG_CRITICAL(Render_Recompiler, "ARM64 SRT walker range table is full");
        std::abort();
    }

    const long page_size_result = sysconf(_SC_PAGESIZE);
    if (page_size_result <= 0) {
        LOG_CRITICAL(Render_Recompiler, "Unable to query host page size for ARM64 SRT walker");
        std::abort();
    }
    const size_t page_size = static_cast<size_t>(page_size_result);
    const size_t mapping_size = (size + page_size - 1) & ~(page_size - 1);
    auto mapping = std::make_unique<SrtCodeMapping>();
    mapping->data = static_cast<u8*>(
        mmap(nullptr, mapping_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mapping->size = mapping_size;
    if (mapping->data == MAP_FAILED) {
        mapping->data = nullptr;
        LOG_CRITICAL(Render_Recompiler, "Unable to allocate ARM64 SRT walker: errno {}", errno);
        std::abort();
    }
    std::memcpy(mapping->data, ptr, size);
    __builtin___clear_cache(reinterpret_cast<char*>(mapping->data),
                            reinterpret_cast<char*>(mapping->data + size));
    if (mprotect(mapping->data, mapping_size, PROT_READ | PROT_EXEC) != 0) {
        LOG_CRITICAL(Render_Recompiler, "Unable to protect ARM64 SRT walker: errno {}", errno);
        std::abort();
    }

    std::call_once(g_srt_signal_once, [] {
        constexpr u32 priority = 1;
        Core::Signals::Instance()->RegisterAccessViolationHandler(SrtWalkerSignalHandler, priority);
    });

    const auto begin = reinterpret_cast<uintptr_t>(mapping->data);
    auto* function = reinterpret_cast<PFN_SrtWalker>(mapping->data);
    g_srt_code_mappings.push_back(std::move(mapping));
    g_srt_code_ranges[range_index] = {begin, begin + size};
    g_srt_code_range_count.store(range_index + 1, std::memory_order_release);
    return function;
}

} // namespace Shader

namespace Shader::Optimization {

namespace {

// Dynamic offsets are computed in w6..w15, one register per level of the expression.
constexpr u32 FirstOffsetRegister = 6;
constexpr u32 LastOffsetRegister = 15;

// Bytes a pointer window covers, clamped at bind time to the mapped memory it starts in.
constexpr u32 PointerWindowBytes = 256 * 1024;

// Emits the computation of a dword offset into w<reg> from user data, flat buffer values that the
// walker copied earlier and scalar arithmetic. Fails on anything else, such as a lane value or a
// loop counter.
bool ComputeOffset(const IR::Value& value, u32 reg, PassInfo& pass_info, Arm64SrtEmitter& emitter) {
    if (reg > LastOffsetRegister) {
        return false;
    }
    if (value.IsImmediate()) {
        emitter.MoveImmediate32(reg, value.U32());
        return true;
    }
    IR::Inst* inst = value.InstRecursive();
    const auto operands = [&] {
        return ComputeOffset(inst->Arg(0), reg, pass_info, emitter) &&
               ComputeOffset(inst->Arg(1), reg + 1, pass_info, emitter);
    };
    const auto binary = [&](u32 instruction) {
        if (!operands()) {
            return false;
        }
        emitter.Emit(instruction | ((reg + 1) << 16) | (reg << 5) | reg);
        return true;
    };
    const auto unsigned_select = [&](u32 condition) {
        if (!operands()) {
            return false;
        }
        emitter.Emit(0x6b00001fu | ((reg + 1) << 16) | (reg << 5)); // cmp
        emitter.Emit(0x1a800000u | ((reg + 1) << 16) | (condition << 12) | (reg << 5) | reg);
        return true;
    };
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetUserData:
        emitter.LoadFlatDword(reg, static_cast<u32>(inst->Arg(0).ScalarReg()));
        return true;
    case IR::Opcode::ReadConst:
    case IR::Opcode::ReadConstBuffer:
        if (const auto slot = pass_info.FindSlot(inst)) {
            emitter.LoadFlatDword(reg, *slot);
            return true;
        }
        return false;
    case IR::Opcode::IAdd32:
        return binary(0x0b000000u);
    case IR::Opcode::ISub32:
        return binary(0x4b000000u);
    case IR::Opcode::IMul32:
        return binary(0x1b007c00u);
    case IR::Opcode::ShiftLeftLogical32:
        return binary(0x1ac02000u);
    case IR::Opcode::ShiftRightLogical32:
        return binary(0x1ac02400u);
    case IR::Opcode::BitwiseAnd32:
        return binary(0x0a000000u);
    case IR::Opcode::BitwiseOr32:
        return binary(0x2a000000u);
    case IR::Opcode::BitwiseXor32:
        return binary(0x4a000000u);
    case IR::Opcode::UMin32:
        return unsigned_select(0x3); // lo
    case IR::Opcode::UMax32:
        return unsigned_select(0x8); // hi
    case IR::Opcode::BitwiseNot32:
        if (!ComputeOffset(inst->Arg(0), reg, pass_info, emitter)) {
            return false;
        }
        emitter.Emit(0x2a2003e0u | (reg << 16) | reg); // mvn
        return true;
    case IR::Opcode::BitFieldUExtract: {
        if (!operands()) {
            return false;
        }
        emitter.Emit(0x1ac02400u | ((reg + 1) << 16) | (reg << 5) | reg); // lsr by the offset
        const IR::Value count = inst->Arg(2);
        if (count.IsImmediate()) {
            if (count.U32() >= 32) {
                return true;
            }
            emitter.MoveImmediate32(reg + 1, (1u << count.U32()) - 1);
        } else {
            if (reg + 2 > LastOffsetRegister ||
                !ComputeOffset(count, reg + 1, pass_info, emitter)) {
                return false;
            }
            emitter.MoveImmediate32(reg + 2, 1);
            emitter.Emit(0x1ac02000u | ((reg + 1) << 16) | ((reg + 2) << 5) | (reg + 1));
            emitter.Emit(0x51000400u | ((reg + 1) << 5) | (reg + 1)); // sub #1
        }
        emitter.Emit(0x0a000000u | ((reg + 1) << 16) | (reg << 5) | reg);
        return true;
    }
    default:
        return false;
    }
}

// x4 = the byte offset of a dword offset.
bool SetSourceOffset(const IR::Value& offset_dw, PassInfo& pass_info, Arm64SrtEmitter& emitter) {
    if (offset_dw.IsImmediate()) {
        emitter.SetSourceOffset(offset_dw.U32());
        return true;
    }
    if (!ComputeOffset(offset_dw, FirstOffsetRegister, pass_info, emitter)) {
        return false;
    }
    emitter.SetSourceOffsetFromRegister(FirstOffsetRegister);
    return true;
}

bool CopyEntry(const IR::Value& offset_dw, const PassInfo::Entry& entry, PassInfo& pass_info,
               Arm64SrtEmitter& emitter) {
    if (offset_dw.IsImmediate()) {
        for (u32 dword = 0; dword < entry.num_dwords; ++dword) {
            emitter.CopyDword(offset_dw.U32() + dword, pass_info.dst_off_dw + dword);
        }
    } else {
        if (!SetSourceOffset(offset_dw, pass_info, emitter)) {
            return false;
        }
        emitter.CopyDwordsAtSourceOffset(entry.num_dwords, pass_info.dst_off_dw);
    }
    pass_info.slots[entry.use] = pass_info.dst_off_dw;
    pass_info.dst_off_dw += entry.num_dwords;
    return true;
}

// Copies the loads of one pointer level whose offsets can be computed at this point of the walker,
// then descends. Code runs in emission order, so an offset only reads values copied before it.
// With add_windows, a level left with loads it cannot copy also gets a buffer descriptor over its
// pointer. Returns whether anything was written; if not, the code emitted for this level is dropped.
bool VisitPointer(const IR::Value& offset_dw, IR::Inst* subtree, PassInfo& pass_info,
                  Arm64SrtEmitter& emitter, bool add_windows) {
    if (!pass_info.HasPending(subtree)) {
        return false;
    }
    const size_t mark = emitter.Mark();
    if (!SetSourceOffset(offset_dw, pass_info, emitter)) {
        emitter.Rewind(mark);
        return false;
    }
    emitter.PushPointer();
    PassInfo::PtrUserList* use_list = pass_info.GetUsesAsPointer(subtree);
    bool copied = false;
    for (const auto& [source_offset_dw, entry] : *use_list) {
        if (pass_info.slots.contains(entry.use)) {
            continue;
        }
        const size_t entry_mark = emitter.Mark();
        if (CopyEntry(source_offset_dw, entry, pass_info, emitter)) {
            copied = true;
        } else {
            emitter.Rewind(entry_mark);
        }
    }
    const auto needs_window = [&](const auto& use) {
        return !pass_info.slots.contains(use.second.use) &&
               pass_info.window_pointers.contains(use.second.use);
    };
    if (add_windows && !pass_info.windows.contains(subtree) &&
        std::ranges::any_of(*use_list, needs_window)) {
        emitter.WritePointerWindow(pass_info.dst_off_dw, PointerWindowBytes);
        pass_info.windows[subtree] = pass_info.dst_off_dw;
        pass_info.dst_off_dw += 4;
        copied = true;
    }
    for (const auto& [source_offset_dw, entry] : *use_list) {
        if (pass_info.GetUsesAsPointer(entry.use) != nullptr) {
            copied |= VisitPointer(source_offset_dw, entry.use, pass_info, emitter, add_windows);
        }
    }
    if (!copied) {
        emitter.Rewind(mark);
        return false;
    }
    emitter.PopPointer();
    return true;
}

void GenerateSrtProgram(Info& info, PassInfo& pass_info) {
    if (pass_info.srt_roots.empty()) {
        return;
    }

    Arm64SrtEmitter emitter;
    emitter.Begin();
    pass_info.dst_off_dw = NUM_USER_DATA_REGS;
    ASSERT(pass_info.dst_off_dw == info.srt_info.flattened_bufsize_dw);
    // The first sweep copies everything at immediate offsets; each further one copies what the
    // values copied so far let it compute, for offsets that depend on loaded data.
    // A last one gives the pointers whose loads still could not be copied a descriptor, so the
    // shader reads them from memory.
    const auto sweep = [&](bool add_windows) {
        bool copied = false;
        for (const auto& [sgpr_base, root] : pass_info.srt_roots) {
            copied |= VisitPointer(IR::Value(static_cast<u32>(sgpr_base)), root, pass_info, emitter,
                                   add_windows);
        }
        return copied;
    };
    constexpr int MaxSweeps = 4;
    for (int count = 0; count < MaxSweeps && sweep(false); ++count) {
    }
    sweep(true);
    emitter.End();

    info.srt_info.walker_func = RegisterWalkerCode(emitter.Data(), emitter.Size());
    ASSERT(info.srt_info.walker_func != nullptr);
    info.srt_info.walker_func_size = emitter.Size();
    info.srt_info.flattened_bufsize_dw = pass_info.dst_off_dw;
}

bool IsPointerSource(const IR::Value& value) {
    const IR::Inst* inst = value.TryInstRecursive();
    return inst != nullptr && (inst->GetOpcode() == IR::Opcode::GetUserData ||
                               inst->GetOpcode() == IR::Opcode::ReadConst ||
                               inst->GetOpcode() == IR::Opcode::ReadConstBuffer);
}

// Folds a byte offset added to a pointer (low dword add, high dword add of its carry) into the
// dword offset of the loads, so the pointer itself is what was loaded.
void SimplifyReadConstAddressAdd(IR::Inst& inst) {
    IR::Inst* addr = inst.Arg(0).InstRecursive();
    IR::Inst* lo = addr->Arg(0).TryInstRecursive();
    IR::Inst* hi = addr->Arg(1).TryInstRecursive();
    if (lo == nullptr || hi == nullptr || lo->GetOpcode() != IR::Opcode::IAdd32 ||
        hi->GetOpcode() != IR::Opcode::IAdd32) {
        return;
    }
    IR::Value base = lo->Arg(0);
    IR::Value add_offset = lo->Arg(1);
    if (!IsPointerSource(base)) {
        std::swap(base, add_offset);
        if (!IsPointerSource(base)) {
            return;
        }
    }
    const IR::Inst* carry = hi->Arg(1).TryInstRecursive();
    if (carry == nullptr || carry->GetOpcode() != IR::Opcode::SelectU32 ||
        !carry->Arg(1).IsImmediate() || !carry->Arg(2).IsImmediate() ||
        carry->Arg(1).U32() != 1 || carry->Arg(2).U32() != 0) {
        return;
    }
    for (const auto& use : addr->Uses()) {
        if (use.user->GetOpcode() != IR::Opcode::ReadConst || use.operand != 0) {
            return;
        }
    }
    addr->SetArg(0, base);
    addr->SetArg(1, hi->Arg(0));
    for (const auto& use : addr->Uses()) {
        IR::Inst* user = use.user;
        IR::IREmitter ir{*user->GetParent(), IR::Block::InstructionList::s_iterator_to(*user)};
        const IR::U32 offset{user->Arg(1)};
        if (offset.IsImmediate() && add_offset.IsImmediate()) {
            user->SetArg(1, ir.Imm32(offset.U32() + (add_offset.U32() >> 2)));
        } else {
            const IR::U32 dword_offset =
                add_offset.IsImmediate()
                    ? ir.Imm32(add_offset.U32() >> 2)
                    : IR::U32{ir.ShiftRightLogical(IR::U32{add_offset}, ir.Imm32(2u))};
            user->SetArg(1, ir.IAdd(offset, dword_offset));
        }
    }
}

// Calls visit on each buffer load an offset is computed from.
template <typename Visit>
void ForEachOffsetLoad(const IR::Value& offset, Visit&& visit) {
    std::vector<IR::Inst*> queue;
    std::vector<IR::Inst*> visited;
    if (!offset.IsImmediate()) {
        queue.push_back(offset.InstRecursive());
    }
    while (!queue.empty()) {
        IR::Inst* inst = queue.back();
        queue.pop_back();
        if (std::ranges::find(visited, inst) != visited.end()) {
            continue;
        }
        visited.push_back(inst);
        switch (inst->GetOpcode()) {
        case IR::Opcode::ReadConstBuffer:
            visit(inst);
            break;
        case IR::Opcode::IAdd32:
        case IR::Opcode::ISub32:
        case IR::Opcode::IMul32:
        case IR::Opcode::ShiftLeftLogical32:
        case IR::Opcode::ShiftRightLogical32:
        case IR::Opcode::BitwiseAnd32:
        case IR::Opcode::BitwiseOr32:
        case IR::Opcode::BitwiseXor32:
        case IR::Opcode::BitwiseNot32:
        case IR::Opcode::UMin32:
        case IR::Opcode::UMax32:
        case IR::Opcode::BitFieldUExtract:
            for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
                if (!inst->Arg(arg).IsImmediate()) {
                    queue.push_back(inst->Arg(arg).InstRecursive());
                }
            }
            break;
        default:
            break;
        }
    }
}

// Buffer loads that give the first dword of a resource descriptor (bindless: s_buffer_load of a T#,
// V# or S# out of a table that a V# points to). Seen through phis, as the resource pass does, and
// through the select that turns sampler anisotropy off for textures without mips.
std::vector<IR::Inst*> FindSharpBufferLoads(const IR::Program& program) {
    std::vector<IR::Inst*> loads;
    std::vector<const IR::Inst*> visited;
    const auto visit = [&](const IR::Value& handle) {
        if (handle.IsImmediate()) {
            return;
        }
        std::vector<IR::Inst*> queue{handle.InstRecursive()};
        while (!queue.empty()) {
            IR::Inst* inst = queue.back();
            queue.pop_back();
            if (std::ranges::find(visited, inst) != visited.end()) {
                continue;
            }
            visited.push_back(inst);
            if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer) {
                loads.push_back(inst);
            } else if (inst->GetOpcode() == IR::Opcode::Phi ||
                       inst->GetOpcode() == IR::Opcode::SelectU32) {
                for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
                    if (!inst->Arg(arg).IsImmediate()) {
                        queue.push_back(inst->Arg(arg).InstRecursive());
                    }
                }
            }
        }
    };
    for (const IR::Block* block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            if (IsImageInstruction(inst)) {
                visit(inst.Arg(0));
                const IR::Inst* sampler = inst.GetOpcode() == IR::Opcode::ImageSampleRaw
                                              ? inst.Arg(1).TryInstRecursive()
                                              : nullptr;
                if (sampler != nullptr &&
                    sampler->GetOpcode() == IR::Opcode::CompositeConstructU32x4 &&
                    !sampler->AreAllArgsImmediates()) {
                    visit(sampler->Arg(0));
                }
            } else if (IsBufferInstruction(inst)) {
                const IR::Inst* vsharp = inst.Arg(0).TryInstRecursive();
                if (vsharp != nullptr && !vsharp->AreAllArgsImmediates()) {
                    visit(vsharp->Arg(0));
                }
            }
        }
    }
    return loads;
}

// A sharp read from a buffer becomes a flat user data read, like the s_load of a pointer table: the
// walker takes the buffer base out of the V# (its first 48 bits) and copies the descriptor.
void ReplaceSharpBufferLoad(IR::Inst* load, u32 slot) {
    IR::Inst* vsharp = load->Arg(0).InstRecursive();
    IR::IREmitter ir{*load->GetParent(), IR::Block::InstructionList::s_iterator_to(*load)};
    const IR::Value base = ir.CompositeConstruct(
        vsharp->Arg(0), ir.BitwiseAnd(IR::U32{vsharp->Arg(1)}, ir.Imm32(0xffffu)));
    const IR::U32 value = ir.ReadConst(base, IR::U32{load->Arg(1)});
    value.Inst()->SetFlags<u32>(slot);
    load->ReplaceUsesWith(value);
}

// A load at an offset only the GPU knows (a lane value made uniform, a loop counter, shared memory)
// reads the memory its pointer points to through the descriptor the walker writes over it.
void ReplaceWithWindowLoad(IR::Inst* load, u32 slot) {
    IR::IREmitter ir{*load->GetParent(), IR::Block::InstructionList::s_iterator_to(*load)};
    std::array<IR::U32, 4> vsharp;
    for (u32 dword = 0; dword < 4; ++dword) {
        vsharp[dword] = ir.ReadConst(load->Arg(0), ir.Imm32(0u));
        vsharp[dword].Inst()->SetFlags<u32>(slot + dword);
    }
    IR::BufferInstInfo buffer_info{};
    buffer_info.pointer_window.Assign(1);
    const IR::U32 value =
        ir.ReadConstBuffer(ir.CompositeConstruct(vsharp[0], vsharp[1], vsharp[2], vsharp[3]),
                           IR::U32{load->Arg(1)}, buffer_info);
    load->ReplaceUsesWith(value);
}

// Text form of an offset or pointer for the log, a star marking a load the walker did not copy.
std::string DescribeValue(const IR::Value& value, const PassInfo& pass_info, int depth = 0) {
    if (value.IsImmediate()) {
        return fmt::format("{:#x}", value.U32());
    }
    const IR::Inst* inst = value.InstRecursive();
    const IR::Opcode opcode = inst->GetOpcode();
    if (opcode == IR::Opcode::GetUserData) {
        return fmt::format("ud{}", static_cast<u32>(inst->Arg(0).ScalarReg()));
    }
    if (depth >= 4) {
        return fmt::format("{}(..)", IR::NameOf(opcode));
    }
    if (opcode == IR::Opcode::ReadConst || opcode == IR::Opcode::ReadConstBuffer) {
        return fmt::format("{}{}[{}]", IR::NameOf(opcode), pass_info.slots.contains(inst) ? "" : "*",
                           DescribeValue(inst->Arg(1), pass_info, depth + 1));
    }
    std::string text = fmt::format("{}(", IR::NameOf(opcode));
    for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
        text += (arg != 0 ? ", " : "") + DescribeValue(inst->Arg(arg), pass_info, depth + 1);
    }
    return text + ")";
}

// The values an offset depends on that the walker cannot read.
std::string DescribeOffsetLeaves(const IR::Value& offset, const PassInfo& pass_info) {
    std::vector<std::string> leaves;
    std::vector<const IR::Inst*> queue;
    std::vector<const IR::Inst*> visited;
    if (!offset.IsImmediate()) {
        queue.push_back(offset.InstRecursive());
    }
    while (!queue.empty()) {
        const IR::Inst* inst = queue.back();
        queue.pop_back();
        if (std::ranges::find(visited, inst) != visited.end()) {
            continue;
        }
        visited.push_back(inst);
        std::string leaf;
        switch (inst->GetOpcode()) {
        case IR::Opcode::GetUserData:
            break;
        case IR::Opcode::ReadConst:
        case IR::Opcode::ReadConstBuffer:
            if (!pass_info.slots.contains(inst)) {
                leaf = DescribeValue(IR::Value{const_cast<IR::Inst*>(inst)}, pass_info, 2);
            }
            break;
        case IR::Opcode::IAdd32:
        case IR::Opcode::ISub32:
        case IR::Opcode::IMul32:
        case IR::Opcode::ShiftLeftLogical32:
        case IR::Opcode::ShiftRightLogical32:
        case IR::Opcode::BitwiseAnd32:
        case IR::Opcode::BitwiseOr32:
        case IR::Opcode::BitwiseXor32:
        case IR::Opcode::BitwiseNot32:
        case IR::Opcode::UMin32:
        case IR::Opcode::UMax32:
        case IR::Opcode::BitFieldUExtract:
            for (size_t arg = 0; arg < inst->NumArgs(); ++arg) {
                if (!inst->Arg(arg).IsImmediate()) {
                    queue.push_back(inst->Arg(arg).InstRecursive());
                }
            }
            break;
        default:
            leaf = DescribeValue(IR::Value{const_cast<IR::Inst*>(inst)}, pass_info, 2);
            break;
        }
        if (!leaf.empty() && std::ranges::find(leaves, leaf) == leaves.end()) {
            leaves.push_back(std::move(leaf));
        }
    }
    std::string text;
    for (const std::string& leaf : leaves) {
        text += (text.empty() ? "" : "; ") + leaf;
    }
    return text;
}

} // namespace

void FlattenExtendedUserdataPass(IR::Program& program) {
    Shader::Info& info = program.info;
    PassInfo pass_info;

    // Every load the walker copies, with its size in dwords: each ReadConst, each sharp read from
    // a buffer (up to eight dwords, read back from consecutive flat slots), and the buffer loads
    // that a pointer or an offset is read from.
    std::vector<std::pair<IR::Inst*, u32>> loads;
    std::vector<IR::Inst*> readconsts;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : *block) {
            if (inst.GetOpcode() == IR::Opcode::ReadConst) {
                SimplifyReadConstAddressAdd(inst);
                readconsts.push_back(&inst);
                loads.emplace_back(&inst, 1);
            }
        }
    }
    const std::vector<IR::Inst*> sharp_loads = FindSharpBufferLoads(program);
    std::vector<IR::Inst*> queued = sharp_loads;
    for (IR::Inst* load : sharp_loads) {
        loads.emplace_back(load, 8);
    }
    const auto queue_buffer_load = [&](IR::Inst* inst) {
        if (inst->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            std::ranges::find(queued, inst) == queued.end()) {
            queued.push_back(inst);
            loads.emplace_back(inst, 1);
        }
    };

    const auto pred = [](IR::Inst* candidate) -> std::optional<IR::Inst*> {
        if (candidate->GetOpcode() == IR::Opcode::GetUserData ||
            candidate->GetOpcode() == IR::Opcode::ReadConst ||
            candidate->GetOpcode() == IR::Opcode::ReadConstBuffer ||
            candidate->GetOpcode() == IR::Opcode::ReadFirstLane) {
            return candidate;
        }
        return std::nullopt;
    };
    for (size_t index = 0; index < loads.size(); ++index) {
        const auto [load, num_dwords] = loads[index];
        const IR::Inst* base = load->Arg(0).InstRecursive();
        if (load->GetOpcode() == IR::Opcode::ReadConstBuffer &&
            base->GetOpcode() != IR::Opcode::CompositeConstructU32x4) {
            continue;
        }
        const auto base0 = IR::BreadthFirstSearch(base->Arg(0), pred);
        const auto base1 = IR::BreadthFirstSearch(base->Arg(1), pred);
        // A pointer that differs per lane cannot be walked ahead of the draw.
        if (!base0 || !base1 || (*base0)->GetOpcode() == IR::Opcode::ReadFirstLane ||
            (*base1)->GetOpcode() == IR::Opcode::ReadFirstLane) {
            LOG_WARNING(Render_Recompiler, "Load of {} is not from constant memory",
                        IR::NameOf(load->GetOpcode()));
            continue;
        }
        queue_buffer_load(*base0);
        ForEachOffsetLoad(load->Arg(1), queue_buffer_load);
        IR::Inst* ptr_lo = pass_info.DeduplicateInstruction(*base0);
        if (load->GetOpcode() == IR::Opcode::ReadConst && base->Arg(0).TryInstRecursive() == *base0) {
            pass_info.window_pointers[load] = ptr_lo;
        }
        if (pass_info.DeduplicateInstruction(load) != load) {
            continue;
        }
        pass_info.pointer_uses[ptr_lo].emplace(load->Arg(1), PassInfo::Entry{load, num_dwords});
        if (ptr_lo->GetOpcode() == IR::Opcode::GetUserData) {
            pass_info.srt_roots[ptr_lo->Arg(0).ScalarReg()] = ptr_lo;
        }
    }

    GenerateSrtProgram(info, pass_info);

    const auto report = [&](const IR::Inst* load) {
        const IR::Inst* base = load->Arg(0).InstRecursive();
        LOG_WARNING(Render_Recompiler, "SRT walker cannot copy {} from {}, offset needs {}",
                    DescribeValue(IR::Value{const_cast<IR::Inst*>(load)}, pass_info, 2),
                    DescribeValue(base->Arg(0), pass_info, 2),
                    DescribeOffsetLeaves(load->Arg(1), pass_info));
    };
    for (IR::Inst* readconst : readconsts) {
        if (const auto slot = pass_info.FindSlot(readconst)) {
            readconst->SetFlags<u32>(*slot);
        } else if (const auto window = pass_info.FindWindow(readconst)) {
            ReplaceWithWindowLoad(readconst, *window);
        } else {
            report(readconst);
        }
    }
    // A sharp the walker cannot reach stays a buffer load, and the resource pass binds a null
    // resource for it.
    for (IR::Inst* load : sharp_loads) {
        if (const auto slot = pass_info.FindSlot(load)) {
            ReplaceSharpBufferLoad(load, *slot);
        } else {
            report(load);
        }
    }
    info.RefreshFlatBuf();
}

} // namespace Shader::Optimization

#else

namespace Shader {

PFN_SrtWalker RegisterWalkerCode(const u8* ptr, size_t size) {
    UNREACHABLE_MSG("RegisterWalkerCode unimplemented for target architecture.");
}

namespace Optimization {

void FlattenExtendedUserdataPass(IR::Program& program) {
    UNREACHABLE_MSG("FlattenExtendedUserdataPass unimplemented for target architecture.");
}

} // namespace Optimization

} // namespace Shader

#endif
