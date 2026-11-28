// SPDX-FileCopyrightText: Copyright 2026 gow3_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/alignment.h"
#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

// gow3 28 Nov 2026 (Uncharted 3 chapter 2 "device lost" as young Sully appears; Uncharted 2 has
// shaders of the same kind): the PS4's GPU checks every LDS (shared memory) access of a compute
// shader against the workgroup's LDS allocation. A read past it gives 0 and a write past it is
// dropped, and some of the games' compute shaders rely on that (a cloth solver adds lds[i] and
// lds[i + half] for every thread, so its last threads read past the end and expect zeros). Vulkan
// gives no such guarantee: on GeForce and Intel the reads land in other data or outside the
// workgroup's memory, and the device can be lost.
//
// This pass gives the compute shaders the PS4's rule. For every shared-memory access whose address
// is not a constant known to be in range, the address is checked against the allocation: an access
// out of range goes to a spare 8 bytes just after the allocation (so a write there changes nothing
// the shader reads), and a load or an atomic out of range returns 0. Accesses that are in range
// behave exactly as before. The allocation grows by those 8 bytes; the wave64 ballot scratch of
// lower_wave64_pass.cpp goes after them (Info::lds_bounds_spare).
//
// GOW3_LDS_BOUNDS=0: off. GOW3_LDS_BOUNDS=<hex hashes, comma separated>: only those shaders.
// Unset: every compute shader with shared memory. A shader whose allocation leaves no room for the
// spare bytes under the device's limit is left as it is (logged).

constexpr u32 SpareBytes = 8;

namespace {

struct BoundsSwitch {
    bool off = false;
    std::vector<u64> only;   // empty: every compute shader
};

const BoundsSwitch& Switch() {
    static const BoundsSwitch s = [] {
        BoundsSwitch r;
        const char* env = std::getenv("GOW3_LDS_BOUNDS");
        if (!env || !*env || std::strcmp(env, "1") == 0) {
            return r;
        }
        if (std::strcmp(env, "0") == 0) {
            r.off = true;
            return r;
        }
        std::string text{env};
        size_t pos = 0;
        while (pos <= text.size()) {
            const size_t end = std::min(text.find(',', pos), text.size());
            const std::string item = text.substr(pos, end - pos);
            if (!item.empty()) {
                r.only.push_back(std::strtoull(item.c_str(), nullptr, 16));
            }
            pos = end + 1;
        }
        return r;
    }();
    return s;
}

} // namespace

// the bytes a shared-memory access of this opcode touches; 0 for every other opcode (also used by
// uniform_barrier_pass.cpp)
u32 SharedAccessBytes(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::LoadSharedU16:
    case IR::Opcode::WriteSharedU16:
        return 2;
    case IR::Opcode::LoadSharedU64:
    case IR::Opcode::WriteSharedU64:
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
    case IR::Opcode::SharedAtomicCmpSwap64:
        return 8;
    case IR::Opcode::LoadSharedU32:
    case IR::Opcode::WriteSharedU32:
    case IR::Opcode::SharedAtomicIAdd32:
    case IR::Opcode::SharedAtomicISub32:
    case IR::Opcode::SharedAtomicSMin32:
    case IR::Opcode::SharedAtomicUMin32:
    case IR::Opcode::SharedAtomicSMax32:
    case IR::Opcode::SharedAtomicUMax32:
    case IR::Opcode::SharedAtomicInc32:
    case IR::Opcode::SharedAtomicDec32:
    case IR::Opcode::SharedAtomicAnd32:
    case IR::Opcode::SharedAtomicOr32:
    case IR::Opcode::SharedAtomicXor32:
    case IR::Opcode::SharedAtomicCmpSwap32:
        return 4;
    default:
        return 0;
    }
}

namespace {

bool IsWrite(IR::Opcode op) {
    return op == IR::Opcode::WriteSharedU16 || op == IR::Opcode::WriteSharedU32 ||
           op == IR::Opcode::WriteSharedU64;
}

} // namespace

bool LdsBounds(u64 pgm_hash) {
    const BoundsSwitch& s = Switch();
    if (s.off) {
        return false;
    }
    if (s.only.empty()) {
        return true;
    }
    for (const u64 h : s.only) {
        if (h == pgm_hash) {
            return true;
        }
    }
    return false;
}

void LdsBoundsPass(IR::Program& program, const RuntimeInfo& runtime_info, const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || !LdsBounds(program.info.pgm_hash)) {
        return;
    }
    const u32 size = runtime_info.hw.cs.shared_memory_size;
    if (size == 0) {
        return;
    }
    std::vector<IR::Inst*> accesses;
    for (IR::Block* const block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const u32 bytes = SharedAccessBytes(inst.GetOpcode());
            if (bytes == 0 || inst.Flags<bool>()) {   // not shared memory, or GDS
                continue;
            }
            const IR::Value address = inst.Arg(0);
            if (address.IsImmediate() && u64(address.U32()) + bytes <= size) {
                continue;   // a constant address in range
            }
            accesses.push_back(&inst);
        }
    }
    if (accesses.empty()) {
        return;
    }
    const u32 spare = Common::AlignUp(size, sizeof(u64));
    if (spare + SpareBytes > profile.max_shared_memory_size) {
        LOG_WARNING(Render_Recompiler,
                    "cs {:#x}: {} bytes of LDS leave no room for the bounds check: unchecked",
                    program.info.pgm_hash, size);
        return;
    }
    u32 constant_past_end = 0;   // accesses at a constant address past the allocation
    for (IR::Inst* inst : accesses) {
        IR::Block& block = *inst->GetParent();
        const auto it = IR::Block::InstructionList::s_iterator_to(*inst);
        IR::IREmitter ir{block, it};
        const IR::Opcode op = inst->GetOpcode();
        const u32 bytes = SharedAccessBytes(op);
        const IR::U32 address{inst->Arg(0)};
        if (address.IsImmediate()) {
            constant_past_end++;
        }
        // in range: address + bytes <= size, written so that it cannot wrap
        const IR::U1 in_range =
            size >= bytes ? ir.ILessThanEqual(address, ir.Imm32(size - bytes), false) : ir.Imm1(false);
        const IR::U32 checked{ir.Select(in_range, address, ir.Imm32(spare))};
        if (IsWrite(op)) {
            inst->SetArg(0, checked);
            continue;
        }
        // a load or an atomic: the same access on the checked address, its result 0 when out of range
        const auto copy = block.PrependNewInst(it, *inst);
        copy->SetArg(0, checked);
        const IR::Value result{&*copy};
        IR::Value zero_or_result;
        switch (bytes) {
        case 2: {
            const IR::U32 wide{ir.UConvert(32, IR::U16{result})};
            zero_or_result = ir.UConvert(16, IR::U32{ir.Select(in_range, wide, ir.Imm32(0))});
            break;
        }
        case 8:
            zero_or_result = ir.Select(in_range, result, ir.Imm64(u64{0}));
            break;
        default:
            zero_or_result = ir.Select(in_range, result, ir.Imm32(0));
            break;
        }
        inst->ReplaceUsesWithAndRemove(zero_or_result);
    }
    program.info.lds_bounds_spare = SpareBytes;
    program.info.shared_memory_scratch_size =
        std::max(program.info.shared_memory_scratch_size, spare + SpareBytes - size);
    LOG_INFO(Render_Recompiler, "cs {:#x}: {} LDS accesses checked against {} bytes{}",
             program.info.pgm_hash, accesses.size(), size,
             constant_past_end ? " (some at a constant address past the end)" : "");
}

} // namespace Shader::Optimization
