/* Host-only translator for Unity 2018's SMOL-V-compressed Vulkan programs.
 * It restores Unity's serialized resource names, lowers the exact SPIR-V to
 * ESSL100 with SPIRV-Cross, and never becomes part of the runtime package. */
#include "smolv.h"
#include "spirv_glsl.hpp"
#include "GLSL.std.450.h"
#include <spirv-tools/optimizer.hpp>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Binding = std::pair<std::uint32_t, std::uint32_t>;

struct ConstantBufferMeta {
    std::string name;
    std::map<std::uint32_t, std::string> members;
};

struct Metadata {
    std::map<std::uint32_t, std::string> inputs;
    std::map<Binding, ConstantBufferMeta> constant_buffers;
    std::map<Binding, std::string> textures;
    std::map<std::uint32_t, std::string> direct_members;
};

std::vector<std::string> split_tabs(const std::string &line) {
    std::vector<std::string> fields;
    std::size_t begin = 0;
    while (true) {
        const std::size_t end = line.find('\t', begin);
        fields.push_back(line.substr(begin, end - begin));
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }
    return fields;
}

std::uint32_t parse_u32(const std::string &value, const char *field) {
    std::size_t used = 0;
    const unsigned long parsed = std::stoul(value, &used, 0);
    if (used != value.size() || parsed > UINT32_MAX) {
        throw std::runtime_error(std::string("invalid ") + field + ": " + value);
    }
    return static_cast<std::uint32_t>(parsed);
}

Metadata read_metadata(const std::string &path) {
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("cannot open metadata: " + path);
    }
    Metadata meta;
    std::string line;
    std::uint32_t line_number = 0;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty() || line[0] == '#') {
            continue;
        }
        const auto fields = split_tabs(line);
        if (fields[0] == "input" && fields.size() == 3) {
            meta.inputs[parse_u32(fields[1], "input location")] = fields[2];
        } else if (fields[0] == "cb" && fields.size() == 4) {
            const Binding binding(parse_u32(fields[1], "descriptor set"),
                                  parse_u32(fields[2], "binding"));
            meta.constant_buffers[binding].name = fields[3];
        } else if (fields[0] == "member" && fields.size() == 6) {
            const Binding binding(parse_u32(fields[1], "descriptor set"),
                                  parse_u32(fields[2], "binding"));
            const std::uint32_t offset = parse_u32(fields[3], "member offset");
            // The fifth field is the member kind.  It is retained in the TSV
            // for auditing; the Python side already applies Unity's hlslcc
            // matrix prefix to fields[5] when necessary.
            meta.constant_buffers[binding].members[offset] = fields[5];
        } else if (fields[0] == "texture" && fields.size() == 4) {
            const Binding binding(parse_u32(fields[1], "descriptor set"),
                                  parse_u32(fields[2], "binding"));
            meta.textures[binding] = fields[3];
        } else if (fields[0] == "direct" && fields.size() == 4) {
            const std::uint32_t order = parse_u32(fields[1], "direct order");
            // fields[2] is the auditable vector/matrix kind.
            meta.direct_members[order] = fields[3];
        } else {
            throw std::runtime_error("invalid metadata line " +
                                     std::to_string(line_number));
        }
    }
    return meta;
}

std::vector<std::uint8_t> read_binary(const std::string &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("cannot open input: " + path);
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0) {
        throw std::runtime_error("cannot determine input size");
    }
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
    if (!data.empty() &&
        !in.read(reinterpret_cast<char *>(data.data()), size)) {
        throw std::runtime_error("cannot read input");
    }
    return data;
}

void write_text(const std::string &path, const std::string &text) {
    std::ofstream out(path, std::ios::trunc);
    if (!out || !(out << text)) {
        throw std::runtime_error("cannot write output: " + path);
    }
}

std::uint32_t read_u32(const std::vector<std::uint8_t> &data,
                       std::size_t offset) {
    if (offset + 4 > data.size()) {
        throw std::runtime_error("truncated source container header");
    }
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::vector<std::uint32_t> decode_smolv(
        const std::vector<std::uint8_t> &container,
        std::uint32_t offset,
        std::uint32_t size) {
    if (size == 0 || static_cast<std::uint64_t>(offset) + size > container.size()) {
        throw std::runtime_error("invalid SMOL-V range");
    }
    const void *encoded = container.data() + offset;
    const std::size_t decoded_size = smolv::GetDecodedBufferSize(encoded, size);
    if (decoded_size == 0 || decoded_size % 4 != 0) {
        throw std::runtime_error("invalid SMOL-V header");
    }
    std::vector<std::uint32_t> decoded(decoded_size / 4);
    if (!smolv::Decode(encoded, size, decoded.data(), decoded_size,
                       smolv::kDecodeFlagUse20160831AsZeroVersion)) {
        throw std::runtime_error("SMOL-V decode failed");
    }
    return decoded;
}

struct SpirvInstruction {
    spv::Op opcode;
    std::vector<std::uint32_t> words;
};

std::string spirv_string(const std::vector<std::uint32_t> &words,
                         std::size_t first_word) {
    if (first_word >= words.size()) {
        return std::string();
    }
    const char *begin = reinterpret_cast<const char *>(words.data() + first_word);
    const std::size_t bytes = (words.size() - first_word) * sizeof(std::uint32_t);
    return std::string(begin, strnlen(begin, bytes));
}

std::vector<std::uint32_t> lower_legacy_essl(
        const std::vector<std::uint32_t> &module) {
    if (module.size() < 5) {
        throw std::runtime_error("truncated SPIR-V module");
    }
    std::vector<SpirvInstruction> instructions;
    for (std::size_t cursor = 5; cursor < module.size();) {
        const std::uint32_t word_count = module[cursor] >> 16;
        const auto opcode = static_cast<spv::Op>(module[cursor] & 0xffff);
        if (word_count == 0 || cursor + word_count > module.size()) {
            throw std::runtime_error("malformed SPIR-V instruction stream");
        }
        instructions.push_back({opcode,
            std::vector<std::uint32_t>(module.begin() + cursor,
                                       module.begin() + cursor + word_count)});
        cursor += word_count;
    }

    std::set<std::uint32_t> float32_types;
    std::set<std::uint32_t> int32_types;
    std::unordered_map<std::uint32_t, std::size_t> definitions;
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> constants;
    std::uint32_t glsl450_import = 0;
    for (std::size_t index = 0; index < instructions.size(); ++index) {
        const auto &inst = instructions[index];
        if (inst.opcode == spv::OpTypeFloat && inst.words.size() >= 3 &&
            inst.words[2] == 32) {
            float32_types.insert(inst.words[1]);
            definitions[inst.words[1]] = index;
        } else if (inst.opcode == spv::OpTypeInt && inst.words.size() >= 4 &&
                   inst.words[2] == 32) {
            int32_types.insert(inst.words[1]);
            definitions[inst.words[1]] = index;
        } else if (inst.opcode == spv::OpExtInstImport && inst.words.size() >= 3) {
            definitions[inst.words[1]] = index;
            if (spirv_string(inst.words, 2) == "GLSL.std.450") {
                glsl450_import = inst.words[1];
            }
        } else if (inst.opcode == spv::OpConstant && inst.words.size() == 4) {
            definitions[inst.words[2]] = index;
            constants[{inst.words[1], inst.words[3]}] = inst.words[2];
        } else if ((inst.opcode == spv::OpSelect ||
                    inst.opcode == spv::OpBitcast ||
                    inst.opcode == spv::OpLoad) && inst.words.size() >= 4) {
            definitions[inst.words[2]] = index;
        }
    }

    // ESSL100 has no roundEven(), but SPIRV-Cross already carries a stable
    // legacy lowering for Round: floor(x + 0.5).  Pixel-snap coordinates are
    // non-negative at this point, so replacing only this extended opcode is
    // the exact practical ES2 fallback used by Unity-era mobile shaders.
    for (auto &inst : instructions) {
        if (inst.opcode == spv::OpExtInst && inst.words.size() >= 6 &&
            inst.words[3] == glsl450_import &&
            inst.words[4] == GLSLstd450RoundEven) {
            inst.words[4] = GLSLstd450Round;
        }
    }

    // A tiny set of authored Unity shaders uses asint/asfloat on material
    // floats after the lossless constant/select cases above.  ESSL100 has no
    // bit reinterpretation primitive.  The mobile fallback is the numeric
    // conversion, which preserves the user's numeric property and avoids a
    // missing shader; this path is intentionally reached only for residual
    // scalar load bitcasts.
    for (auto &inst : instructions) {
        if (inst.opcode != spv::OpBitcast || inst.words.size() != 4) {
            continue;
        }
        const auto operand_def = definitions.find(inst.words[3]);
        if (operand_def == definitions.end()) {
            continue;
        }
        const auto &operand = instructions[operand_def->second];
        if (operand.words.size() < 3) {
            continue;
        }
        const std::uint32_t output_type = inst.words[1];
        const std::uint32_t input_type = operand.words[1];
        if (int32_types.count(output_type) && float32_types.count(input_type)) {
            inst.opcode = spv::OpConvertFToS;
            inst.words[0] = static_cast<std::uint32_t>(
                (4u << 16) | spv::OpConvertFToS);
        } else if (float32_types.count(output_type) && int32_types.count(input_type)) {
            inst.opcode = spv::OpConvertSToF;
            inst.words[0] = static_cast<std::uint32_t>(
                (4u << 16) | spv::OpConvertSToF);
        }
    }

    std::uint32_t next_id = module[3];
    std::vector<SpirvInstruction> added_constants;
    auto constant_for_bits = [&](std::uint32_t type,
                                 std::uint32_t bits) -> std::uint32_t {
        const auto key = std::make_pair(type, bits);
        const auto found = constants.find(key);
        if (found != constants.end()) {
            return found->second;
        }
        const std::uint32_t id = next_id++;
        added_constants.push_back({spv::OpConstant,
            {static_cast<std::uint32_t>((4u << 16) | spv::OpConstant),
             type, id, bits}});
        constants[key] = id;
        return id;
    };

    // HLSL compilers commonly materialize bool -> 0.0/1.0 as
    // select(int bit-patterns) followed by OpBitcast.  Legacy ESSL cannot
    // express the generic bitcast, but this specific graph can be rewritten
    // losslessly as a float/int select with constants carrying the same bits.
    for (auto &inst : instructions) {
        if (inst.opcode != spv::OpBitcast || inst.words.size() != 4) {
            continue;
        }
        const std::uint32_t output_type = inst.words[1];
        if (!float32_types.count(output_type) && !int32_types.count(output_type)) {
            continue;
        }
        const auto operand_def = definitions.find(inst.words[3]);
        if (operand_def == definitions.end()) {
            continue;
        }
        const auto &operand = instructions[operand_def->second];
        if (operand.opcode == spv::OpConstant && operand.words.size() == 4) {
            const std::uint32_t replacement =
                constant_for_bits(output_type, operand.words[3]);
            inst.opcode = spv::OpCopyObject;
            inst.words = {
                static_cast<std::uint32_t>((4u << 16) | spv::OpCopyObject),
                output_type, inst.words[2], replacement};
        } else if (operand.opcode == spv::OpSelect && operand.words.size() == 6) {
            const auto true_def = definitions.find(operand.words[4]);
            const auto false_def = definitions.find(operand.words[5]);
            if (true_def == definitions.end() || false_def == definitions.end()) {
                continue;
            }
            const auto &true_value = instructions[true_def->second];
            const auto &false_value = instructions[false_def->second];
            if (true_value.opcode != spv::OpConstant || true_value.words.size() != 4 ||
                false_value.opcode != spv::OpConstant || false_value.words.size() != 4) {
                continue;
            }
            const std::uint32_t true_id =
                constant_for_bits(output_type, true_value.words[3]);
            const std::uint32_t false_id =
                constant_for_bits(output_type, false_value.words[3]);
            inst.opcode = spv::OpSelect;
            inst.words = {
                static_cast<std::uint32_t>((6u << 16) | spv::OpSelect),
                output_type, inst.words[2], operand.words[3], true_id, false_id};
        }
    }

    std::vector<std::uint32_t> lowered(module.begin(), module.begin() + 5);
    lowered[3] = next_id;
    bool inserted_constants = false;
    for (const auto &inst : instructions) {
        if (!inserted_constants && inst.opcode == spv::OpFunction) {
            for (const auto &constant : added_constants) {
                lowered.insert(lowered.end(), constant.words.begin(),
                               constant.words.end());
            }
            inserted_constants = true;
        }
        lowered.insert(lowered.end(), inst.words.begin(), inst.words.end());
    }
    if (!inserted_constants) {
        for (const auto &constant : added_constants) {
            lowered.insert(lowered.end(), constant.words.begin(),
                           constant.words.end());
        }
    }
    return lowered;
}

std::vector<std::uint32_t> optimize_spirv(
        const std::vector<std::uint32_t> &module) {
    spvtools::Optimizer optimizer(SPV_ENV_VULKAN_1_0);
    std::string diagnostic;
    optimizer.SetMessageConsumer(
        [&diagnostic](spv_message_level_t, const char *,
                      const spv_position_t &position, const char *message) {
            diagnostic = "SPIR-V optimizer at " +
                         std::to_string(position.index) + ": " + message;
        });
    optimizer.RegisterPerformancePasses();
    std::vector<std::uint32_t> optimized;
    if (!optimizer.Run(module.data(), module.size(), &optimized)) {
        throw std::runtime_error(diagnostic.empty() ?
                                 "SPIR-V optimizer failed" : diagnostic);
    }
    return optimized;
}

Binding resource_binding(spirv_cross::CompilerGLSL &compiler,
                         const spirv_cross::Resource &resource) {
    return Binding(
        compiler.get_decoration(resource.id, spv::DecorationDescriptorSet),
        compiler.get_decoration(resource.id, spv::DecorationBinding));
}

std::string safe_identifier(std::string name) {
    for (char &ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') ||
                        (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '_';
        if (!ok) {
            ch = '_';
        }
    }
    if (name.empty() || (name[0] >= '0' && name[0] <= '9')) {
        name.insert(name.begin(), '_');
    }
    return name;
}

std::string compile_stage(std::vector<std::uint32_t> spirv,
                          const Metadata &meta,
                          bool vertex) {
    spirv_cross::CompilerGLSL compiler(std::move(spirv));
    auto options = compiler.get_common_options();
    options.version = 100;
    options.es = true;
    options.enable_420pack_extension = false;
    options.emit_uniform_buffer_as_plain_uniforms = true;
    /* Unity's Vulkan SPIR-V carries the backend Y inversion in the shader.
     * GLES receives an OpenGL projection matrix and must not retain that
     * Vulkan-only flip.  SPIRV-Cross' vertex option applies the inverse flip,
     * cancelling Unity's epilogue and matching Unity's native GLES2 blobs. */
    options.vertex.flip_vert_y = true;
    compiler.set_common_options(options);

    const std::uint32_t dummy_sampler =
        compiler.build_dummy_sampler_for_combined_images();
    if (dummy_sampler != 0) {
        compiler.set_decoration(dummy_sampler, spv::DecorationDescriptorSet, 0);
        compiler.set_decoration(dummy_sampler, spv::DecorationBinding, 0xffff);
        compiler.set_name(dummy_sampler, "UnityDummySampler");
    }

    auto resources = compiler.get_shader_resources();

    if (vertex) {
        for (const auto &input : resources.stage_inputs) {
            const std::uint32_t location =
                compiler.get_decoration(input.id, spv::DecorationLocation);
            const auto found = meta.inputs.find(location);
            if (found == meta.inputs.end()) {
                throw std::runtime_error("unmapped vertex input location " +
                                         std::to_string(location));
            }
            compiler.set_name(input.id, found->second);
        }
        for (const auto &output : resources.stage_outputs) {
            const std::uint32_t location =
                compiler.get_decoration(output.id, spv::DecorationLocation);
            compiler.set_name(output.id, "vs_TEXCOORD" +
                                         std::to_string(location));
        }
    } else {
        for (const auto &input : resources.stage_inputs) {
            const std::uint32_t location =
                compiler.get_decoration(input.id, spv::DecorationLocation);
            compiler.set_name(input.id, "vs_TEXCOORD" +
                                        std::to_string(location));
        }
    }

    bool used_direct_members = false;
    for (const auto &ubo : resources.uniform_buffers) {
        const Binding binding = resource_binding(compiler, ubo);
        auto found = meta.constant_buffers.find(binding);
        ConstantBufferMeta direct_buffer;
        if (found == meta.constant_buffers.end() || found->second.name.empty()) {
            if (used_direct_members || meta.direct_members.empty()) {
                throw std::runtime_error("unmapped uniform buffer set=" +
                                         std::to_string(binding.first) + " binding=" +
                                         std::to_string(binding.second));
            }
            direct_buffer.name = "DirectGlobals";
            std::uint32_t member_index = 0;
            for (const auto &direct : meta.direct_members) {
                direct_buffer.members[member_index++] = direct.second;
            }
            used_direct_members = true;
        }
        const ConstantBufferMeta &buffer =
            found == meta.constant_buffers.end() ? direct_buffer : found->second;
        const std::string suffix = safe_identifier(buffer.name) + "_" +
                                   std::to_string(binding.first) + "_" +
                                   std::to_string(binding.second);
        compiler.set_name(ubo.id, "UnityCB_" + suffix);
        compiler.set_name(ubo.base_type_id, "UnityType_" + suffix);

        const auto &type = compiler.get_type(ubo.base_type_id);
        for (std::uint32_t member = 0; member < type.member_types.size(); ++member) {
            const std::uint32_t offset =
                compiler.type_struct_member_offset(type, member);
            auto member_found = buffer.members.find(offset);
            if (used_direct_members && &buffer == &direct_buffer) {
                member_found = buffer.members.find(member);
            }
            if (member_found == buffer.members.end()) {
                throw std::runtime_error("unmapped uniform member offset=" +
                                         std::to_string(offset) + " in " +
                                         buffer.name);
            }
            compiler.set_member_name(ubo.base_type_id, member,
                                     member_found->second);
        }
    }

    if (!resources.storage_buffers.empty() || !resources.storage_images.empty() ||
        !resources.subpass_inputs.empty() || !resources.push_constant_buffers.empty()) {
        throw std::runtime_error("shader uses a resource class unavailable in GLES2");
    }

    std::unordered_map<std::uint32_t, std::string> image_names;
    for (const auto &image : resources.separate_images) {
        const Binding binding = resource_binding(compiler, image);
        const auto found = meta.textures.find(binding);
        if (found == meta.textures.end()) {
            throw std::runtime_error("unmapped texture set=" +
                                     std::to_string(binding.first) + " binding=" +
                                     std::to_string(binding.second));
        }
        image_names[image.id] = found->second;
        compiler.set_name(image.id, "UnityImage_" +
                                    safe_identifier(found->second) + "_" +
                                    std::to_string(image.id));
    }
    for (const auto &sampler : resources.separate_samplers) {
        compiler.set_name(sampler.id, "UnitySampler_" +
                                      std::to_string(sampler.id));
    }

    compiler.build_combined_image_samplers();
    for (const auto &combined : compiler.get_combined_image_samplers()) {
        const auto found = image_names.find(combined.image_id);
        compiler.set_name(combined.combined_id,
                          found == image_names.end() ? "UnityDummyTexture" :
                                                      found->second);
    }

    return compiler.compile();
}

}  // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "usage: unity-smolv-cross SOURCE META VERTEX_OUT FRAGMENT_OUT\n";
        return 2;
    }
    try {
        const auto source = read_binary(argv[1]);
        if (source.size() < 20) {
            throw std::runtime_error("source container is too short");
        }
        const auto meta = read_metadata(argv[2]);
        const auto vertex = decode_smolv(source, read_u32(source, 4),
                                         read_u32(source, 8));
        const auto fragment = decode_smolv(source, read_u32(source, 12),
                                           read_u32(source, 16));
        write_text(argv[3], compile_stage(
            lower_legacy_essl(optimize_spirv(vertex)), meta, true));
        write_text(argv[4], compile_stage(
            lower_legacy_essl(optimize_spirv(fragment)), meta, false));
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << "\n";
        return 1;
    }
}
