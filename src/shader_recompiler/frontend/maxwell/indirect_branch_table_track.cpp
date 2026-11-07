// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <optional>

#include "common/common_types.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/maxwell/decode.h"
#include "shader_recompiler/frontend/maxwell/indirect_branch_table_track.h"
#include "shader_recompiler/frontend/maxwell/opcodes.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/load_constant.h"

namespace Shader::Maxwell {
namespace {
union Encoding {
    u64 raw;
    BitField<0, 8, IR::Reg> dest_reg;
    BitField<8, 8, IR::Reg> src_reg;
    BitField<20, 19, u64> immediate;
    BitField<56, 1, u64> is_negative;
    BitField<20, 24, s64> brx_offset;
};

template <typename Reader, typename Callable>
std::optional<u64> Track(Reader& env, Location block_begin, Location& pos, Callable&& func) {
    while (pos >= block_begin) {
        const u64 insn{env.ReadInstruction(pos.Offset())};
        --pos;
        if (func(insn, Decode(insn))) {
            return insn;
        }
    }
    return std::nullopt;
}

template <typename Reader>
std::optional<u64> TrackLDC(Reader& env, Location block_begin, Location& pos, IR::Reg brx_reg) {
    return Track(env, block_begin, pos, [brx_reg](u64 insn, Opcode opcode) {
        const LDC::Encoding ldc{insn};
        return opcode == Opcode::LDC && ldc.dest_reg == brx_reg && ldc.size == LDC::Size::B32 &&
               ldc.mode == LDC::Mode::Default;
    });
}

template <typename Reader>
std::optional<u64> TrackSHL(Reader& env, Location block_begin, Location& pos, IR::Reg ldc_reg) {
    return Track(env, block_begin, pos, [ldc_reg](u64 insn, Opcode opcode) {
        const Encoding shl{insn};
        return opcode == Opcode::SHL_imm && shl.dest_reg == ldc_reg;
    });
}

template <typename Reader>
std::optional<u64> TrackIMNMX(Reader& env, Location block_begin, Location& pos, IR::Reg shl_reg) {
    return Track(env, block_begin, pos, [shl_reg](u64 insn, Opcode opcode) {
        const Encoding imnmx{insn};
        return opcode == Opcode::IMNMX_imm && imnmx.dest_reg == shl_reg;
    });
}
template <typename Reader>
std::optional<IndirectBranchTableInfo> TrackTable(Reader& env, Location brx_pos,
                                                  Location block_begin) {
    const u64 brx_insn{env.ReadInstruction(brx_pos.Offset())};
    const Opcode brx_opcode{Decode(brx_insn)};
    if (brx_opcode != Opcode::BRX && brx_opcode != Opcode::JMX) {
        throw LogicError("Tracked instruction is not BRX or JMX");
    }
    const IR::Reg brx_reg{Encoding{brx_insn}.src_reg};
    const s32 brx_offset{static_cast<s32>(Encoding{brx_insn}.brx_offset)};

    Location pos{brx_pos};
    const std::optional<u64> ldc_insn{TrackLDC(env, block_begin, pos, brx_reg)};
    if (!ldc_insn) {
        return std::nullopt;
    }
    const LDC::Encoding ldc{*ldc_insn};
    const u32 cbuf_index{static_cast<u32>(ldc.index)};
    const u32 cbuf_offset{static_cast<u32>(static_cast<s32>(ldc.offset.Value()))};
    const IR::Reg ldc_reg{ldc.src_reg};

    const std::optional<u64> shl_insn{TrackSHL(env, block_begin, pos, ldc_reg)};
    if (!shl_insn) {
        return std::nullopt;
    }
    const Encoding shl{*shl_insn};
    const IR::Reg shl_reg{shl.src_reg};

    const std::optional<u64> imnmx_insn{TrackIMNMX(env, block_begin, pos, shl_reg)};
    if (!imnmx_insn) {
        return std::nullopt;
    }
    const Encoding imnmx{*imnmx_insn};
    if (imnmx.is_negative != 0) {
        return std::nullopt;
    }
    const u32 imnmx_immediate{static_cast<u32>(imnmx.immediate.Value())};
    return IndirectBranchTableInfo{
        .cbuf_index = cbuf_index,
        .cbuf_offset = cbuf_offset,
        .num_entries = imnmx_immediate + 1,
        .branch_offset = brx_offset,
        .branch_reg = brx_reg,
    };
}

struct CodeReader {
    std::span<const u64> code;

    u64 ReadInstruction(u32 address) const {
        if (address % sizeof(u64) != 0 || address / sizeof(u64) >= code.size()) {
            throw LogicError("Indirect branch tracker instruction out of bounds");
        }
        return code[address / sizeof(u64)];
    }
};
} // Anonymous namespace

std::optional<IndirectBranchTableInfo> TrackIndirectBranchTable(Environment& env, Location brx_pos,
                                                                Location block_begin) {
    return TrackTable(env, brx_pos, block_begin);
}

std::optional<IndirectBranchTableInfo> TrackIndirectBranchTable(std::span<const u64> code,
                                                                Location brx_pos,
                                                                Location block_begin) {
    CodeReader reader{code};
    return TrackTable(reader, brx_pos, block_begin);
}

} // namespace Shader::Maxwell
