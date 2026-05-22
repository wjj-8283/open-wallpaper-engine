#include "Shader/ShaderPreprocessor.hpp"

#include <gtest/gtest.h>

using wallpaper::WPShaderInfo;
using wallpaper::WPShaderTexInfo;

namespace
{
WPShaderInfo Parse(std::string_view src, std::size_t texture_slot_count = 8)
{
    WPShaderInfo info {};
    std::vector<WPShaderTexInfo> textures(texture_slot_count);
    for (auto& texture : textures) {
        texture.enabled = true;
    }
    wallpaper::shader::ExtractMetadata(src, &info, textures);
    return info;
}
} // namespace

TEST(WPShaderParser, TextureDefaultCollectedRegardlessOfIfdef) {
    const std::string src = R"(
#if LIGHTS_SHADOW_MAPPING
uniform sampler2D g_Texture6; // {"hidden":true,"default":"_rt_shadowAtlas"}
#endif
void main(){}
)";
    auto info = Parse(src);
    ASSERT_EQ(info.defTexs.size(), 1u);
    EXPECT_EQ(info.defTexs[0].first, 6);
    EXPECT_EQ(info.defTexs[0].second, "_rt_shadowAtlas");
}

TEST(WPShaderParser, TextureComboFlagSetRegardlessOfIfdef) {
    const std::string src = R"(
#if MASK == 1
uniform sampler2D g_Texture2; // {"material":"mask","combo":"MASK","default":"util/white"}
#endif
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.combos.at("MASK"), "1");
}

TEST(WPShaderParser, ComboLineCollectedRegardlessOfIfdef) {
    const std::string src = R"(
#if 0
// [COMBO] {"combo":"NEVER","default":1}
#endif
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.combos.at("NEVER"), "1");
}

TEST(WPShaderParser, LineCommentUniformNotCollected) {
    const std::string src = R"(
// uniform vec4 g_Foo; // {"default":42}
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Foo"), 0u);
}

TEST(WPShaderParser, BlockCommentUniformNotCollected) {
    const std::string src = R"(
/*
uniform vec4 g_Y; // {"default":1}
*/
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Y"), 0u);
}

TEST(WPShaderParser, BlockCommentDoesNotEatLaterUniforms) {
    const std::string src = R"(
/* unused */
uniform float g_Real; // {"default":2.5}
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.svs.count("g_Real"), 1u);
}

TEST(WPShaderParser, ComboDefaultIsRecorded) {
    const std::string src = R"(
// [COMBO] {"combo":"BLENDMODE","default":9}
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.combos.at("BLENDMODE"), "9");
}

TEST(WPShaderParser, ScalarDefaultAndAliasRecorded) {
    const std::string src = R"(
uniform float g_Brightness; // {"material":"brightness","default":1.5,"range":[0,10]}
void main(){}
)";
    auto info = Parse(src);
    ASSERT_EQ(info.svs.count("g_Brightness"), 1u);
    ASSERT_EQ(info.svs.at("g_Brightness").size(), 1u);
    EXPECT_FLOAT_EQ(info.svs.at("g_Brightness")[0], 1.5f);
    EXPECT_EQ(info.alias.at("brightness"), "g_Brightness");
}

TEST(WPShaderParser, TextureAliasAndDefaultRecorded) {
    const std::string src = R"(
uniform sampler2D g_Texture0; // {"material":"albedo","label":"Albedo","default":"util/white"}
void main(){}
)";
    auto info = Parse(src);
    ASSERT_EQ(info.defTexs.size(), 1u);
    EXPECT_EQ(info.defTexs[0].first, 0);
    EXPECT_EQ(info.defTexs[0].second, "util/white");
    EXPECT_EQ(info.alias.at("albedo"), "g_Texture0");
}

TEST(WPShaderParser, TextureBoundComboSetForEnabledTextureSlot) {
    const std::string src = R"(
uniform sampler2D g_Texture0; // {"combo":"HASTEX","default":"util/white"}
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.combos.at("HASTEX"), "1");
}

TEST(WPShaderParser, QualifiedScalarDefaultAndAliasRecorded) {
    const std::string src = R"(
uniform highp float g_Value; // {"material":"value","default":2.0}
void main(){}
)";
    auto info = Parse(src);
    ASSERT_EQ(info.svs.count("g_Value"), 1u);
    ASSERT_EQ(info.svs.at("g_Value").size(), 1u);
    EXPECT_FLOAT_EQ(info.svs.at("g_Value")[0], 2.0f);
    EXPECT_EQ(info.alias.at("value"), "g_Value");
}

TEST(WPShaderParser, LayoutTextureAliasDefaultAndComboRecorded) {
    const std::string src = R"(
layout(binding = 0) uniform sampler2D g_Texture0; // {"material":"albedo","combo":"HASTEX","default":"util/white"}
void main(){}
)";
    auto info = Parse(src);
    ASSERT_EQ(info.defTexs.size(), 1u);
    EXPECT_EQ(info.defTexs[0].first, 0);
    EXPECT_EQ(info.defTexs[0].second, "util/white");
    EXPECT_EQ(info.alias.at("albedo"), "g_Texture0");
    EXPECT_EQ(info.combos.at("HASTEX"), "1");
}

TEST(WPShaderParser, ComboInsideMidLineBlockCommentNotCollected) {
    const std::string src = R"(
float ignored; /* // [COMBO] {"combo":"HIDDEN","default":1}
*/
void main(){}
)";
    auto info = Parse(src);
    EXPECT_EQ(info.combos.count("HIDDEN"), 0u);
}

TEST(WPShaderParser, UniformAfterSameLineBlockCommentIsCollected) {
    const std::string src = R"(
/* note */ uniform float g_Value; // {"material":"value","default":1.0}
void main(){}
)";
    auto info = Parse(src);
    ASSERT_EQ(info.svs.count("g_Value"), 1u);
    ASSERT_EQ(info.svs.at("g_Value").size(), 1u);
    EXPECT_FLOAT_EQ(info.svs.at("g_Value")[0], 1.0f);
    EXPECT_EQ(info.alias.at("value"), "g_Value");
}
