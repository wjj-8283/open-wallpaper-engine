#include "Fs/PhysicalFs.h"
#include "Fs/VFS.h"
#include "Shader/SceneShaderLegalizer.hpp"
#include "Shader/ShaderPreprocessor.hpp"
#include "WPShaderParser.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <cctype>
#include <string>
#include <vector>

#include <unistd.h>

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
    const auto* test_info = ::testing::UnitTest::GetInstance()->current_test_info();
    std::string test_name = "unknown";
    if (test_info != nullptr) {
        test_name = std::string(test_info->test_suite_name()) + "-" + test_info->name();
    }
    for (char& ch : test_name) {
        if (!std::isalnum(static_cast<unsigned char>(ch))) {
            ch = '-';
        }
    }

    auto root = std::filesystem::temp_directory_path() /
                ("wpe-shader-preprocessor-compat-test-" + std::to_string(getpid()) + "-" + test_name);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "assets" / "shaders", ec);
    return root;
}

void MountAssets(fs::VFS& vfs, const std::filesystem::path& root) {
    EXPECT_TRUE(vfs.Mount("/assets", fs::CreatePhysicalFs((root / "assets").string(), true), "assets"));
}

void WriteText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream out(path);
    out << text;
}

bool CompileFragmentShader(std::string fragment_source, Combos combos = {}) {
    GlslangEnvironment glslang;
    const auto         temp = MakeTempDir();
    fs::VFS            vfs;
    MountAssets(vfs, temp);

    WPShaderInfo info;
    info.combos = std::move(combos);
    std::vector<WPShaderTexInfo> textures;
    std::vector<WPShaderUnit> units {
        WPShaderUnit {
            .stage = ShaderType::VERTEX,
            .src = R"(
in vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)",
            .preprocess_info = {},
        },
        WPShaderUnit {
            .stage = ShaderType::FRAGMENT,
            .src = std::move(fragment_source),
            .preprocess_info = {},
        },
    };

    for (auto& unit : units) {
        unit.src = WPShaderParser::PreShaderSrc(vfs, unit.src, &info, textures);
    }

    std::vector<ShaderCode> codes;
    return WPShaderParser::CompileToSpv("compat-fragment", units, codes, vfs, &info, textures);
}

TEST(ShaderPreprocessorCompatTest, IdentifierNamedSampleCompiles) {
    EXPECT_TRUE(CompileFragmentShader(R"(
void main() {
    float sample = 0.25;
    gl_FragColor = vec4(sample, sample, sample, 1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, ScalarFileScopeConstCompiles) {
    EXPECT_TRUE(CompileFragmentShader(R"(
const float kAlpha = 0.75;
void main() {
    gl_FragColor = vec4(kAlpha, 0.0, 0.0, 1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, UserDefinedModDoesNotCreateDuplicateDefinition) {
    EXPECT_TRUE(CompileFragmentShader(R"(
float mod(float a, float b) {
    return a - b * floor(a / b);
}
void main() {
    gl_FragColor = vec4(mod(5.0, 2.0), 0.0, 0.0, 1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, UserDefinedScalarModPreservesBuiltinVectorMod) {
    const std::string source = R"(
float mod(float a, float b) {
    return a - b * floor(a / b);
}
void main() {
    float scalar = mod(5.0, 2.0);
    vec3 vector = mod(vec3(5.0, 6.0, 7.0), vec3(2.0));
    gl_FragColor = vec4(vector.x + scalar, vector.yz, 1.0);
}
)";

    WPPreprocessorInfo info;
    const Combos       combos;
    const auto preprocessed = shader::PreprocessStageSource(source, ShaderType::FRAGMENT, combos, info);
    auto       legalized = shader::LegalizeStageSource(preprocessed, ShaderType::FRAGMENT);
    EXPECT_NE(legalized.source.find("_ww_user_mod(float a, float b)"), std::string::npos);
    EXPECT_NE(legalized.source.find("_ww_user_mod(5.0, 2.0)"), std::string::npos);
    EXPECT_NE(legalized.source.find("mod(vec3(5.0, 6.0, 7.0), vec3(2.0))"), std::string::npos);

    EXPECT_TRUE(CompileFragmentShader(source));
}

TEST(ShaderPreprocessorCompatTest, UserDefinedScalarModPreservesVectorVariableBuiltinMod) {
    const std::string source = R"(
float mod(float a, float b) {
    return a - b * floor(a / b);
}
void main() {
    float scalar = mod(5.0, 2.0);
    vec3 a = vec3(5.0, 6.0, 7.0);
    vec3 b = vec3(2.0);
    vec3 vector = mod(a, b);
    gl_FragColor = vec4(vector.x + scalar, vector.yz, 1.0);
}
)";

    WPPreprocessorInfo info;
    const Combos       combos;
    const auto preprocessed = shader::PreprocessStageSource(source, ShaderType::FRAGMENT, combos, info);
    auto       legalized = shader::LegalizeStageSource(preprocessed, ShaderType::FRAGMENT);
    EXPECT_NE(legalized.source.find("_ww_user_mod(5.0, 2.0)"), std::string::npos);
    EXPECT_NE(legalized.source.find("mod(a, b)"), std::string::npos);

    EXPECT_TRUE(CompileFragmentShader(source));
}

TEST(ShaderPreprocessorCompatTest, UserDefinedScalarModPreservesMultilineMacroBody) {
    const std::string source = R"(
float mod(float a, float b) {
    return a - b * floor(a / b);
}
#define MOD_THROUGH_MACRO(a, b) \
    mod(a, b)
void main() {
    float scalar = mod(5.0, 2.0);
    gl_FragColor = vec4(scalar, 0.0, 0.0, 1.0);
}
)";

    WPPreprocessorInfo info;
    const Combos       combos;
    const auto preprocessed = shader::PreprocessStageSource(source, ShaderType::FRAGMENT, combos, info);
    auto       legalized = shader::LegalizeStageSource(preprocessed, ShaderType::FRAGMENT);
    EXPECT_NE(legalized.source.find("#define MOD_THROUGH_MACRO(a, b) \\\n    mod(a, b)"), std::string::npos);
    EXPECT_NE(legalized.source.find("_ww_user_mod(5.0, 2.0)"), std::string::npos);

    EXPECT_TRUE(CompileFragmentShader(source));
}

TEST(ShaderPreprocessorCompatTest, ExtraStrayEndifIsTolerated) {
    EXPECT_TRUE(CompileFragmentShader(R"(
#endif
void main() {
    gl_FragColor = vec4(1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, InPlaceIncludeOrderingPreservesMacroVisibility) {
    GlslangEnvironment glslang;
    const auto         temp = MakeTempDir();
    WriteText(temp / "assets" / "shaders" / "compat_defs.h", "#define INCLUDED_TINT vec4(0.2, 0.4, 0.6, 1.0)\n");
    fs::VFS vfs;
    MountAssets(vfs, temp);

    WPShaderInfo info;
    std::vector<WPShaderTexInfo> textures;
    std::vector<WPShaderUnit> units {
        WPShaderUnit {
            .stage = ShaderType::VERTEX,
            .src = R"(
in vec3 a_Position;
void main() {
    gl_Position = vec4(a_Position, 1.0);
}
)",
            .preprocess_info = {},
        },
        WPShaderUnit {
            .stage = ShaderType::FRAGMENT,
            .src = R"(
#include "compat_defs.h"
void main() {
    gl_FragColor = INCLUDED_TINT;
}
)",
            .preprocess_info = {},
        },
    };

    for (auto& unit : units) {
        unit.src = WPShaderParser::PreShaderSrc(vfs, unit.src, &info, textures);
    }

    std::vector<ShaderCode> codes;
    EXPECT_TRUE(WPShaderParser::CompileToSpv("compat-include", units, codes, vfs, &info, textures));
}

TEST(ShaderPreprocessorCompatTest, PerformLightingV1StubCompiles) {
    EXPECT_TRUE(CompileFragmentShader(R"(
void main() {
    vec3 lit = PerformLighting_V1(vec3(0.0), vec3(0.8), vec3(0.0, 0.0, 1.0),
                                  vec3(0.0, 0.0, 1.0), vec3(1.0), vec3(0.04), 0.5, 0.0);
    gl_FragColor = vec4(lit, 1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, ClipIntrinsicCompiles) {
    EXPECT_TRUE(CompileFragmentShader(R"(
void main() {
    clip(1.0);
    gl_FragColor = vec4(1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, Log10IntrinsicCompiles) {
    EXPECT_TRUE(CompileFragmentShader(R"(
void main() {
    float v = log10(100.0);
    gl_FragColor = vec4(v, 0.0, 0.0, 1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, MixedIntVectorPowAndClampCompile) {
    EXPECT_TRUE(CompileFragmentShader(R"(
void main() {
    vec3 base = vec3(2.0, 3.0, 4.0);
    vec3 powered = pow(base, 2);
    vec3 clamped = clamp(powered, 0, 8);
    gl_FragColor = vec4(clamped / 8.0, 1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, MixedScalarVectorPowAndClampCompile) {
    EXPECT_TRUE(CompileFragmentShader(R"(
void main() {
    vec3 exponent = vec3(1.0, 2.0, 3.0);
    vec3 powered = pow(2.0, exponent);
    vec3 clamped = clamp(0.5, vec3(0.0), vec3(1.0));
    gl_FragColor = vec4((powered + clamped) / 9.0, 1.0);
}
)"));
}

TEST(ShaderPreprocessorCompatTest, ScalarTextureSampleMaskCompiles) {
    EXPECT_TRUE(CompileFragmentShader(R"(
uniform sampler2D g_Texture0; // {"hidden":true}
uniform sampler2D g_Texture1; // {"combo":"OPACITY","default":"util/white","material":"mask","mode":"opacitymask"}
uniform float u_strength;
uniform float u_radius;
uniform vec2 g_TexelSize;

varying vec4 v_TexCoord;

#define Src(a,b) texSample2D(g_Texture0, fragCoord + vec2(a,b) * g_TexelSize)

vec4 sharpen(vec2 fragCoord, float mask) {
    vec4 orig = Src(0, 0);
    vec4 c1 = Src(-u_radius, -u_radius);
    vec4 c2 = Src(0, -u_radius);
    vec4 c3 = Src(u_radius,-u_radius);
    vec4 c4 = Src(-u_radius, 0);
    vec4 c5 = Src(u_radius, 0);
    vec4 c6 = Src(-u_radius, u_radius);
    vec4 c7 = Src(0, u_radius);
    vec4 c8 = Src(u_radius, u_radius);
    vec4 blur = (c1 + c3 + c6 + c8 + 2 * (c2 + c4 + c5 + c7) + 4 * orig)/16;
    vec4 corr = (1 + u_strength * mask) * orig - u_strength * mask * blur;
    return corr;
}

void main() {
    vec4 albedo = texSample2D(g_Texture0, v_TexCoord.xy);
    float mask = texSample2D(g_Texture1, v_TexCoord.xy);
    if (mask > 0.1) albedo = sharpen(v_TexCoord.xy, mask);
    gl_FragColor = albedo;
}
)"));
}

TEST(ShaderPreprocessorCompatTest, NumericComboMacroTernaryConditionCompiles) {
    EXPECT_TRUE(CompileFragmentShader(R"(
void main() {
    float mask = 0.5;
    mask = INVERT ? 1.0 - mask : mask;
    gl_FragColor = vec4(mask, 0.0, 0.0, 1.0);
}
)", Combos { { "INVERT", "0" } }));
}

TEST(ShaderPreprocessorCompatTest, MacroAliasedSvPositionIsNotCrossStageIo) {
    WPPreprocessorInfo info;
    const Combos       combos;
    const auto preprocessed = shader::PreprocessStageSource(R"(
#define gl_Position _ww_sv_position
out vec2 v_TexCoord;
out vec4 _ww_sv_position;
void main() {
    v_TexCoord = vec2(0.0);
    _ww_sv_position = vec4(0.0);
}
)", ShaderType::VERTEX, combos, info);
    auto legalized = shader::LegalizeStageSource(preprocessed, ShaderType::VERTEX);
    EXPECT_FALSE(legalized.preprocess_info.output.contains("_ww_sv_position"));
    EXPECT_TRUE(legalized.preprocess_info.output.contains("v_TexCoord"));
}

TEST(ShaderPreprocessorCompatTest, GenericRopeParticleGeometrySourceDiscoveryRemainsDisabled) {
    const auto temp = MakeTempDir();
    WriteText(temp / "assets" / "shaders" / "genericropeparticle.vert", "void main() {}\n");
    WriteText(temp / "assets" / "shaders" / "genericropeparticle.frag", "void main() {}\n");
    WriteText(temp / "assets" / "shaders" / "genericropeparticle.geom", "#error geometry must remain undiscovered\n");

    fs::VFS vfs;
    MountAssets(vfs, temp);
    EXPECT_FALSE(fs::GetFileContent(vfs, "/assets/shaders/genericropeparticle.geom").empty());

    WPShaderInfo info;
    std::vector<WPShaderTexInfo> textures;
    const auto vertex = WPShaderParser::PreShaderSrc(
        vfs,
        fs::GetFileContent(vfs, "/assets/shaders/genericropeparticle.vert"),
        &info,
        textures);
    const auto fragment = WPShaderParser::PreShaderSrc(
        vfs,
        fs::GetFileContent(vfs, "/assets/shaders/genericropeparticle.frag"),
        &info,
        textures);

    EXPECT_TRUE(vertex.find("geometry must remain undiscovered") == std::string::npos);
    EXPECT_TRUE(fragment.find("geometry must remain undiscovered") == std::string::npos);
}

} // namespace
} // namespace wallpaper
