// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"
#include "shader_recompiler/ir_opt/passes.h"

namespace {
using namespace Shader;

class PayloadEnvironment final : public Environment {
public:
    explicit PayloadEnvironment(Stage shader_stage) {
        stage = shader_stage;
    }
    u64 ReadInstruction(u32) override {
        throw LogicError("Unexpected instruction read");
    }
    u32 ReadCbufValue(u32, u32) override {
        throw LogicError("Unexpected cbuf read");
    }
    TextureType ReadTextureType(u32) override {
        return TextureType::Color2D;
    }
    TexturePixelFormat ReadTexturePixelFormat(u32) override {
        return TexturePixelFormat::R32_UINT;
    }
    bool IsTexturePixelFormatInteger(u32) override {
        return true;
    }
    u32 ReadViewportTransformState() override {
        return 1;
    }
    u32 TextureBoundBuffer() const override {
        return 0;
    }
    u32 LocalMemorySize() const override {
        return 0;
    }
    u32 SharedMemorySize() const override {
        return 0;
    }
    std::array<u32, 3> WorkgroupSize() const override {
        return {32, 1, 1};
    }
    bool HasHLEMacroState() const override {
        return false;
    }
    std::optional<ReplaceConstant> GetReplaceConstBuffer(u32, u32) override {
        return std::nullopt;
    }
    void Dump(u64, u64) override {}
};

enum class Read { None, Zero, FoldedZero, Dead, Nonzero, Dynamic };

std::vector<u32> Emit(Stage stage, Read read, u32 payload_words) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    PayloadEnvironment env{stage};
    program.stage = stage;
    program.workgroup_size = {32, 1, 1};
    program.mesh.payload_words = payload_words;
    program.mesh.max_vertices = 3;
    program.mesh.max_primitives = 1;
    program.mesh.topology = 4;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    IR::IREmitter ir{block};
    if (read != Read::None) {
        const IR::U32 address = read == Read::Dynamic ? ir.WorkgroupIdX()
                                : read == Read::FoldedZero
                                    ? IR::U32{ir.IAdd(ir.Imm32(1), ir.Imm32(~0U))}
                                : read == Read::Zero ? ir.Imm32(0)
                                                     : ir.Imm32(16);
        const IR::U32 value{ir.IsbeRead(address)};
        if (read != Read::Dead) {
            ir.IsbeWriteAttribute(ir.Imm32(0), value);
        }
    }
    Optimization::ConstantPropagationPass(env, program);
    Optimization::DeadCodeEliminationPass(program);
    Optimization::CollectShaderInfoPass(env, program);
    program.info.is_task = stage == Stage::Task;
    program.info.is_mesh = stage == Stage::Mesh;
    Profile profile{};
    profile.supported_spirv = 0x00010400;
    auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    if (const char* directory = std::getenv("EDEN_MESH_PAYLOAD_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        const auto name = std::to_string(static_cast<int>(stage)) + "-" +
                          std::to_string(static_cast<int>(read)) + "-" +
                          std::to_string(payload_words) + ".spv";
        std::ofstream file{std::filesystem::path{directory} / name, std::ios::binary};
        file.write(reinterpret_cast<const char*>(code.data()),
                   static_cast<std::streamsize>(code.size() * sizeof(u32)));
        REQUIRE(file.good());
    }
    return code;
}

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, spv::Op opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 count{code[offset] >> 16};
        REQUIRE(count > 0);
        REQUIRE(offset + count <= code.size());
        if ((code[offset] & 0xffff) == static_cast<u32>(opcode)) {
            result.emplace_back(code.begin() + offset + 1, code.begin() + offset + count);
        }
        offset += count;
    }
    return result;
}

void CheckPayload(const std::vector<u32>& code, u32 expected_words) {
    constexpr u32 payload_storage = static_cast<u32>(spv::StorageClass::TaskPayloadWorkgroupEXT);
    const auto pointers{Instructions(code, spv::Op::OpTypePointer)};
    const auto variables{Instructions(code, spv::Op::OpVariable)};
    std::vector<u32> payload_ids;
    for (const auto& variable : variables) {
        if (variable[2] == payload_storage) {
            payload_ids.push_back(variable[1]);
            REQUIRE(expected_words != 0);
            const auto pointer =
                std::ranges::find_if(pointers, [&](const auto& p) { return p[0] == variable[0]; });
            REQUIRE(pointer != pointers.end());
            const auto arrays{Instructions(code, spv::Op::OpTypeArray)};
            const auto array =
                std::ranges::find_if(arrays, [&](const auto& a) { return a[0] == (*pointer)[2]; });
            REQUIRE(array != arrays.end());
            const auto constants{Instructions(code, spv::Op::OpConstant)};
            const auto size =
                std::ranges::find_if(constants, [&](const auto& c) { return c[1] == (*array)[2]; });
            REQUIRE(size != constants.end());
            REQUIRE((*size)[2] == expected_words);
        }
    }
    REQUIRE(payload_ids.size() == (expected_words == 0 ? 0 : 1));
    if (expected_words == 0) {
        // No payload pointer type means neither a payload access chain nor load can survive.
        for (const auto& pointer : pointers) {
            REQUIRE(pointer[1] != payload_storage);
        }
    } else {
        bool loads_payload = false;
        for (const auto& access : Instructions(code, spv::Op::OpAccessChain)) {
            if (access[2] == payload_ids.front()) {
                for (const auto& load : Instructions(code, spv::Op::OpLoad)) {
                    loads_payload |= load[2] == access[1];
                }
            }
        }
        REQUIRE(loads_payload);
    }
}

void CheckGroupIndex(const std::vector<u32>& code) {
    u32 workgroup_id{};
    for (const auto& decoration : Instructions(code, spv::Op::OpDecorate)) {
        if (decoration[1] == static_cast<u32>(spv::Decoration::BuiltIn) &&
            decoration[2] == static_cast<u32>(spv::BuiltIn::WorkgroupId)) {
            workgroup_id = decoration[0];
        }
    }
    REQUIRE(workgroup_id != 0);
    bool extracts_group_x = false;
    for (const auto& load : Instructions(code, spv::Op::OpLoad)) {
        if (load[2] == workgroup_id) {
            for (const auto& extract : Instructions(code, spv::Op::OpCompositeExtract)) {
                extracts_group_x |= extract[2] == load[1] && extract[3] == 0;
            }
        }
    }
    REQUIRE(extracts_group_x);
}
} // Anonymous namespace

TEST_CASE("Mesh group index and dead reads need no task payload",
          "[video_core][spirv][mesh_payload]") {
    for (const Read read : {Read::None, Read::Zero, Read::FoldedZero, Read::Dead}) {
        CAPTURE(static_cast<int>(read));
        for (const u32 words : {64U, 800U}) {
            const auto code = Emit(Stage::Mesh, read, words);
            CheckPayload(code, 0);
            if (read == Read::Zero || read == Read::FoldedZero) {
                CheckGroupIndex(code);
            }
        }
    }
}

TEST_CASE("Mesh payload reads retain the captured size", "[video_core][spirv][mesh_payload]") {
    for (const Read read : {Read::Nonzero, Read::Dynamic}) {
        CAPTURE(static_cast<int>(read));
        for (const u32 words : {64U, 800U}) {
            CheckPayload(Emit(Stage::Mesh, read, words), words);
        }
    }
}

TEST_CASE("Task epilogue always retains its payload", "[video_core][spirv][mesh_payload]") {
    for (const u32 words : {64U, 800U}) {
        CheckPayload(Emit(Stage::Task, Read::None, words), words);
        CheckPayload(Emit(Stage::Task, Read::Zero, words), words);
    }
}
