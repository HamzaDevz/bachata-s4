// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool Requires16BitSharedAtomic(const IR::Inst& inst) {
    // Nothing yet
    return false;
}

static bool Requires64BitSharedAtomic(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::SharedAtomicIAdd64:
    case IR::Opcode::SharedAtomicISub64:
    case IR::Opcode::SharedAtomicSMin64:
    case IR::Opcode::SharedAtomicUMin64:
    case IR::Opcode::SharedAtomicSMax64:
    case IR::Opcode::SharedAtomicUMax64:
    case IR::Opcode::SharedAtomicInc64:
    case IR::Opcode::SharedAtomicDec64:
    case IR::Opcode::SharedAtomicAnd64:
    case IR::Opcode::SharedAtomicOr64:
    case IR::Opcode::SharedAtomicXor64:
        return true;
    default:
        return false;
    }
}

static bool IsNon32BitSharedLoadStore(const IR::Inst& inst) {
    switch (inst.GetOpcode()) {
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::WriteSharedU16:
    case IR::Opcode::WriteSharedU64:
        return true;
    default:
        return false;
    }
}

IR::Type CalculateSpecialSharedAtomicTypes(IR::Program& program) {
    IR::Type extra_atomic_types{IR::Type::Void};
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (Requires16BitSharedAtomic(inst)) {
                extra_atomic_types |= IR::Type::U16;
            }
            if (Requires64BitSharedAtomic(inst)) {
                extra_atomic_types |= IR::Type::U64;
            }
        }
    }
    return extra_atomic_types;
}

// Simplifies down U16 and U64 shared memory operations to U32 when atomics of the same type are
// not used. U64 operations are split even when aliasing is supported: a shader that clears two
// dwords with one U64 store and then reads them as U32 must not depend on the driver keeping
// the aliased views ordered, or it reads stale counters and can loop for billions of iterations.
// U16 stores stay narrow when aliasing is supported, as a read-modify-write of their dword would
// race with the lane storing its other half.
void SharedMemorySimplifyPass(IR::Program& program, const Profile& profile) {
    if (program.info.stage != Stage::Compute) {
        return;
    }
    const bool split_u16 = !profile.supports_workgroup_explicit_memory_layout;

    const auto atomic_types = CalculateSpecialSharedAtomicTypes(program);
    if (True(atomic_types & IR::Type::U16) && True(atomic_types & IR::Type::U64)) {
        // If both other atomic types are used, there is nothing to do.
        return;
    }

    // Iterate through shared load/store U16/U64 instructions, replacing with
    // equivalent U32 ops when the types are not needed for atomics.
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (!IsNon32BitSharedLoadStore(inst)) {
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::U32 offset{inst.Arg(0)};
            if (split_u16 && False(atomic_types & IR::Type::U16)) {
                switch (inst.GetOpcode()) {
                case IR::Opcode::LoadSharedU16: {
                    const IR::U32 dword_offset{ir.BitwiseAnd(offset, ir.Imm32(~3U))};
                    const IR::U32 dword_value{ir.LoadShared(32, false, dword_offset)};
                    const IR::U32 bit_offset{
                        ir.IMul(ir.BitwiseAnd(offset, ir.Imm32(2U)), ir.Imm32(8U))};
                    const IR::U32 value{ir.BitFieldExtract(dword_value, bit_offset, ir.Imm32(16U))};
                    inst.ReplaceUsesWithAndRemove(ir.UConvert(16, value));
                    continue;
                }
                case IR::Opcode::WriteSharedU16: {
                    const IR::U32 value{ir.UConvert(32, IR::U16{inst.Arg(1)})};
                    const IR::U32 bit_offset{
                        ir.IMul(ir.BitwiseAnd(offset, ir.Imm32(2U)), ir.Imm32(8U))};
                    const IR::U32 dword_offset{ir.BitwiseAnd(offset, ir.Imm32(~3U))};
                    const IR::U32 dword_value{
                        ir.LoadShared(32, false, ir.BitwiseAnd(offset, dword_offset))};
                    const IR::U32 new_dword_value{
                        ir.BitFieldInsert(dword_value, value, bit_offset, ir.Imm32(16U))};
                    ir.WriteShared(32, new_dword_value, dword_offset);
                    inst.Invalidate();
                    continue;
                }
                default:
                    break;
                }
            }
            if (False(atomic_types & IR::Type::U64)) {
                switch (inst.GetOpcode()) {
                case IR::Opcode::LoadSharedU64: {
                    const IR::U32 value0{ir.LoadShared(32, false, offset)};
                    const IR::U32 value1{ir.LoadShared(32, false, ir.IAdd(offset, ir.Imm32(4U)))};
                    const IR::Value value{ir.PackUint2x32(ir.CompositeConstruct(value0, value1))};
                    inst.ReplaceUsesWithAndRemove(value);
                    continue;
                }
                case IR::Opcode::WriteSharedU64: {
                    const IR::Value value{ir.UnpackUint2x32(IR::U64{inst.Arg(1)})};
                    const IR::U32 value0{ir.CompositeExtract(value, 0)};
                    const IR::U32 value1{ir.CompositeExtract(value, 1)};
                    ir.WriteShared(32, value0, offset);
                    ir.WriteShared(32, value1, ir.IAdd(offset, ir.Imm32(4U)));
                    inst.Invalidate();
                    continue;
                }
                default:
                    break;
                }
            }
        }
    }
}

} // namespace Shader::Optimization
