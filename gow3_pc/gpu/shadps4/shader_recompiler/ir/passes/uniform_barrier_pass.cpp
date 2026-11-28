// SPDX-FileCopyrightText: Copyright 2026 gow3_pc
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cstdlib>
#include <cstring>
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

// gow3 28 Nov 2026 (Uncharted 3 chapters 2 and 6, "device lost" on Intel graphics): a small compute
// shader of the game (cs f8fa9165, a running sum over an image column) waits at a workgroup barrier
// inside an `if` that only some of its threads enter. The PS4 counts whole waves at a barrier, so
// it does not matter there, and GeForce and Radeon drivers cope too; Intel's driver waits for the
// threads that never arrive, and Windows resets the card. Vulkan requires a barrier to be reached
// by every invocation of the workgroup.
//
// This pass lets every thread run such an `if`'s body, with the same result: the condition the
// structure branches on becomes true, the shared-memory writes (and atomics) of the threads whose
// real condition is false go to the spare bytes after the LDS allocation (the same place the LDS
// bounds check uses), and each value the `if` merges is select(condition, the body's value, the
// value from before). Only bodies of plain blocks whose only effects are shared-memory writes and
// barriers, with no lane-level operation (ballots, lane reads, quad broadcasts, exec), are changed;
// anything else is logged and left as it is.
//
// On Intel's drivers (Windows and Mesa) for the listed shader. GOW3_DIVERGENT_BARRIERS=0: off;
// =1: on with any driver; =all: on with any driver, for every compute shader with such an `if`.

constexpr u64 ListedShaders[] = {0xf8fa9165};

namespace {

enum class Mode { Driver, Off, Any, All };

Mode BarrierMode() {
    static const Mode mode = [] {
        const char* env = std::getenv("GOW3_DIVERGENT_BARRIERS");
        if (!env || !*env) return Mode::Driver;
        if (std::strcmp(env, "0") == 0) return Mode::Off;
        if (std::strcmp(env, "all") == 0) return Mode::All;
        return Mode::Any;
    }();
    return mode;
}

bool IsLaneOp(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::QuadBroadcast:
    case IR::Opcode::ReadFirstLane:
    case IR::Opcode::ReadLane:
    case IR::Opcode::WriteLane:
    case IR::Opcode::Ballot:
    case IR::Opcode::BallotFindLsb:
    case IR::Opcode::InverseBallot:
    case IR::Opcode::GroupAny:
    case IR::Opcode::GetExec:
    case IR::Opcode::SetExec:
    case IR::Opcode::GetScc:
    case IR::Opcode::SetScc:
    case IR::Opcode::GetVcc:
    case IR::Opcode::SetVcc:
    case IR::Opcode::GetVccLo:
    case IR::Opcode::SetVccLo:
    case IR::Opcode::GetVccHi:
    case IR::Opcode::SetVccHi:
        return true;
    default:
        return false;
    }
}

bool IsBarrier(IR::Opcode op) {
    return op == IR::Opcode::Barrier || op == IR::Opcode::WorkgroupMemoryBarrier ||
           op == IR::Opcode::DeviceMemoryBarrier;
}

bool IsSharedLoad(IR::Opcode op) {
    return op == IR::Opcode::LoadSharedU16 || op == IR::Opcode::LoadSharedU32 ||
           op == IR::Opcode::LoadSharedU64;
}

bool Selectable(IR::Type type) {
    return type == IR::Type::U1 || type == IR::Type::U32 || type == IR::Type::F32 ||
           type == IR::Type::U64;
}

struct Candidate {
    size_t node;                     // the If node
    std::vector<IR::Block*> body;    // its blocks
    std::vector<IR::Inst*> writes;   // shared writes and atomics in it
};

// why the `if` at `index` cannot be changed, or nullptr when it can (and the candidate is filled)
const char* Inspect(const IR::Program& program, size_t index, Candidate& c) {
    const IR::AbstractSyntaxList& list = program.syntax_list;
    const auto& node = list[index].data.if_node;
    if (node.cond.IsImmediate()) return "constant condition";
    size_t end = index + 1;
    for (; end < list.size(); ++end) {
        const auto& n = list[end];
        if (n.type == IR::AbstractSyntaxNode::Type::EndIf && n.data.end_if.merge == node.merge) break;
        if (n.type != IR::AbstractSyntaxNode::Type::Block) return "control flow in the body";
        c.body.push_back(n.data.block);
    }
    if (end == list.size() || c.body.empty()) return "no plain body";
    bool barrier = false;
    for (IR::Block* block : c.body) {
        for (IR::Inst& inst : block->Instructions()) {
            const IR::Opcode op = inst.GetOpcode();
            if (IsBarrier(op)) {
                barrier |= op == IR::Opcode::Barrier;
                continue;
            }
            if (IsLaneOp(op)) return "a lane-level operation in the body";
            if (SharedAccessBytes(op) != 0) {
                if (inst.Flags<bool>()) return "GDS access in the body";
                if (!IsSharedLoad(op)) c.writes.push_back(&inst);
                continue;
            }
            if (op == IR::Opcode::Phi) return "a merge inside the body";
            if (inst.MayHaveSideEffects()) return "an effect other than shared writes in the body";
        }
    }
    if (!barrier) return "no barrier";
    for (IR::Inst& inst : node.merge->Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::Phi) break;
        if (inst.NumArgs() != 2 || !Selectable(inst.Type())) return "a merged value it cannot select";
        int from_body = 0;
        for (size_t i = 0; i < 2; ++i) {
            from_body += std::ranges::find(c.body, inst.PhiBlock(i)) != c.body.end();
        }
        if (from_body != 1) return "a merged value not from the body and the block before";
    }
    c.node = index;
    return nullptr;
}

} // namespace

bool& UniformBarrierGpu() {
    static bool on = false;
    return on;
}

bool UniformBarriers(u64 pgm_hash) {
    const Mode mode = BarrierMode();
    if (mode == Mode::Off || (mode == Mode::Driver && !UniformBarrierGpu())) return false;
    return mode == Mode::All || std::ranges::find(ListedShaders, pgm_hash) != std::end(ListedShaders);
}

void UniformBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info, const Profile& profile) {
    if (program.info.hw_stage != HwStage::Compute || !UniformBarriers(program.info.pgm_hash)) {
        return;
    }
    const u32 size = runtime_info.hw.cs.shared_memory_size;
    const u32 spare = Common::AlignUp(size, sizeof(u64));
    std::vector<Candidate> found;
    for (size_t i = 0; i < program.syntax_list.size(); ++i) {
        if (program.syntax_list[i].type != IR::AbstractSyntaxNode::Type::If) continue;
        Candidate c;
        if (const char* why = Inspect(program, i, c)) {
            if (std::strcmp(why, "no barrier") != 0 && std::strcmp(why, "constant condition") != 0 &&
                std::strcmp(why, "control flow in the body") != 0) {
                LOG_WARNING(Render_Recompiler, "cs {:#x}: an if with a barrier left as it is: {}",
                            program.info.pgm_hash, why);
            }
            continue;
        }
        if (!c.writes.empty() && (size == 0 || spare + 8 > profile.max_shared_memory_size)) {
            LOG_WARNING(Render_Recompiler, "cs {:#x}: no room for the spare LDS bytes: if left as it is",
                        program.info.pgm_hash);
            continue;
        }
        found.push_back(std::move(c));
    }
    for (Candidate& c : found) {
        auto& node = program.syntax_list[c.node].data.if_node;
        const IR::U1 cond = node.cond;
        // the writes of threads whose condition is false go to the spare bytes
        for (IR::Inst* write : c.writes) {
            IR::IREmitter ir{*write->GetParent(), IR::Block::InstructionList::s_iterator_to(*write)};
            write->SetArg(0, ir.Select(cond, IR::U32{write->Arg(0)}, ir.Imm32(spare)));
        }
        // the merged values: the body's for threads whose condition is true, the earlier one else
        IR::Block& merge = *node.merge;
        auto after_phis = merge.Instructions().begin();
        while (after_phis != merge.Instructions().end() && after_phis->GetOpcode() == IR::Opcode::Phi) {
            ++after_phis;
        }
        std::vector<IR::Inst*> phis;
        for (auto it = merge.Instructions().begin(); it != after_phis; ++it) phis.push_back(&*it);
        for (IR::Inst* phi : phis) {
            const size_t before = std::ranges::find(c.body, phi->PhiBlock(0)) != c.body.end() ? 1 : 0;
            const IR::Value earlier = phi->Arg(before);
            const std::vector<IR::Use> uses(phi->Uses().begin(), phi->Uses().end());
            IR::IREmitter ir{merge, after_phis};
            const IR::Value chosen = ir.Select(cond, IR::Value{phi}, earlier);
            for (const IR::Use& use : uses) use.user->SetArg(use.operand, chosen);
        }
        node.cond = IR::U1{IR::Value{true}};   // every thread runs the body and reaches its barriers
    }
    if (found.empty()) return;
    if (std::ranges::any_of(found, [](const Candidate& c) { return !c.writes.empty(); })) {
        program.info.lds_bounds_spare = 8;
        program.info.shared_memory_scratch_size =
            std::max(program.info.shared_memory_scratch_size, spare + 8 - size);
    }
    LOG_INFO(Render_Recompiler, "cs {:#x}: {} if(s) with a barrier run by every thread",
             program.info.pgm_hash, found.size());
}

} // namespace Shader::Optimization
