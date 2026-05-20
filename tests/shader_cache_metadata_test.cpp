#include "Fs/PhysicalFs.h"
#include "Fs/VFS.h"
#include "WPShaderParser.hpp"
#include "Vulkan/ShaderComp.hpp"

#include <spirv_reflect.h>

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace wallpaper
{
namespace
{

class GlslangEnvironment final {
public:
    GlslangEnvironment() { WPShaderParser::InitGlslang(); }
    ~GlslangEnvironment() { WPShaderParser::FinalGlslang(); }
};

std::filesystem::path MakeTempDir() {
    auto root = std::filesystem::temp_directory_path() / "wpe-shader-cache-metadata-test";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "cache", ec);
    std::filesystem::create_directories(root / "assets", ec);
    return root;
}

std::array<WPShaderUnit, 2> MakeShaderUnits(fs::VFS& vfs, WPShaderInfo& info) {
    std::array<WPShaderUnit, 2> units {
        WPShaderUnit {
            .stage           = ShaderType::VERTEX,
            .src             = R"(
in vec3 a_Position;
out vec2 v_TexCoord;
void main() {
    v_TexCoord = vec2(0.5, 0.5);
    gl_Position = vec4(a_Position, 1.0);
}
)",
            .preprocess_info = {},
        },
        WPShaderUnit {
            .stage           = ShaderType::FRAGMENT,
            .src             = R"(
in vec2 v_TexCoord;
uniform sampler2D g_Texture0;
void main() {
    gl_FragColor = texture(g_Texture0, v_TexCoord);
}
)",
            .preprocess_info = {},
        },
    };

    std::vector<WPShaderTexInfo> textures { WPShaderTexInfo { .enabled = true } };
    for (auto& unit : units) {
        unit.src = WPShaderParser::PreShaderSrc(vfs, unit.src, &info, textures);
    }
    return units;
}

std::uint32_t ReflectedOutputLocation(const std::vector<unsigned int>& spirv,
                                      const char*                      name) {
    spv_reflect::ShaderModule module(spirv, SPV_REFLECT_MODULE_FLAG_NO_COPY);

    std::uint32_t output_count = 0;
    EXPECT_EQ(module.EnumerateOutputVariables(&output_count, nullptr), SPV_REFLECT_RESULT_SUCCESS);

    std::vector<SpvReflectInterfaceVariable*> outputs(output_count);
    EXPECT_EQ(module.EnumerateOutputVariables(&output_count, outputs.data()),
              SPV_REFLECT_RESULT_SUCCESS);

    for (const auto* output : outputs) {
        if (output == nullptr || output->name == nullptr) continue;
        if (std::string(output->name) == name) return output->location;
    }

    return std::numeric_limits<std::uint32_t>::max();
}

std::uint32_t ReflectedInputLocation(const std::vector<unsigned int>& spirv,
                                     const char*                      name) {
    spv_reflect::ShaderModule module(spirv, SPV_REFLECT_MODULE_FLAG_NO_COPY);

    std::uint32_t input_count = 0;
    EXPECT_EQ(module.EnumerateInputVariables(&input_count, nullptr), SPV_REFLECT_RESULT_SUCCESS);

    std::vector<SpvReflectInterfaceVariable*> inputs(input_count);
    EXPECT_EQ(module.EnumerateInputVariables(&input_count, inputs.data()),
              SPV_REFLECT_RESULT_SUCCESS);

    for (const auto* input : inputs) {
        if (input == nullptr || input->name == nullptr) continue;
        if (std::string(input->name) == name) return input->location;
    }

    return std::numeric_limits<std::uint32_t>::max();
}

TEST(ShaderCacheMetadataTest, CacheHitStillPopulatesActiveTextureSlots) {
    GlslangEnvironment glslang;
    const auto         temp = MakeTempDir();

    fs::VFS vfs;
    ASSERT_TRUE(
        vfs.Mount("/cache", fs::CreatePhysicalFs((temp / "cache").string(), true), "cache"));
    ASSERT_TRUE(
        vfs.Mount("/assets", fs::CreatePhysicalFs((temp / "assets").string(), true), "assets"));

    std::vector<WPShaderTexInfo> textures { WPShaderTexInfo { .enabled = true } };

    {
        WPShaderInfo            info;
        auto                    units = MakeShaderUnits(vfs, info);
        std::vector<ShaderCode> codes;
        ASSERT_TRUE(WPShaderParser::CompileToSpv("demo-scene", units, codes, vfs, &info, textures));
        ASSERT_TRUE(units[1].preprocess_info.active_tex_slots.contains(0));
    }

    {
        WPShaderInfo            info;
        auto                    units = MakeShaderUnits(vfs, info);
        std::vector<ShaderCode> codes;
        ASSERT_TRUE(WPShaderParser::CompileToSpv("demo-scene", units, codes, vfs, &info, textures));
        ASSERT_TRUE(units[1].preprocess_info.active_tex_slots.contains(0));
    }
}

TEST(ShaderCacheMetadataTest, AutoMappedVaryingLocationsMatchAcrossStages) {
    GlslangEnvironment glslang;

    std::vector<vulkan::ShaderCompUnit> units {
        vulkan::ShaderCompUnit {
            .stage = EShLangVertex,
            .src   = R"(
#version 330
layout(location = 0) in vec3 a_Position;
out vec4 v_TexCoord;
out vec2 v_MaskCoord;
void main() {
    v_TexCoord = vec4(0.0, 1.0, 0.0, 1.0);
    v_MaskCoord = vec2(0.5, 0.25);
    gl_Position = vec4(a_Position, 1.0);
}
)",
        },
        vulkan::ShaderCompUnit {
            .stage = EShLangFragment,
            .src   = R"(
#version 330
in vec2 v_MaskCoord;
in vec4 v_TexCoord;
out vec4 glOutColor;
void main() {
    glOutColor = vec4(v_TexCoord.xy, v_MaskCoord.xy);
}
)",
        },
    };

    vulkan::ShaderCompOpt opt;
    opt.client_ver             = glslang::EShTargetVulkan_1_1;
    opt.auto_map_bindings      = true;
    opt.auto_map_locations     = true;
    opt.relaxed_errors_glsl    = true;
    opt.relaxed_rules_vulkan   = true;
    opt.suppress_warnings_glsl = true;

    std::vector<vulkan::Uni_ShaderSpv> spvs;
    ASSERT_TRUE(vulkan::CompileAndLinkShaderUnits(units, opt, spvs));
    ASSERT_EQ(spvs.size(), 2u);

    EXPECT_EQ(ReflectedOutputLocation(spvs[0]->spirv, "v_TexCoord"),
              ReflectedInputLocation(spvs[1]->spirv, "v_TexCoord"));
    EXPECT_EQ(ReflectedOutputLocation(spvs[0]->spirv, "v_MaskCoord"),
              ReflectedInputLocation(spvs[1]->spirv, "v_MaskCoord"));
}

} // namespace
} // namespace wallpaper
