#include "Scene/Parse/WPShaderParser.hpp"

#include "Scene/Parse/ShaderLex.hpp"
#include "WPJson.hpp"
#include "wpscene/WPUniform.h"

#include <algorithm>
#include <charconv>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace wallpaper
{
namespace
{
using shader_lex::Cursor;
using shader_lex::LineWalker;

bool ParseInlineJson(std::string_view source, nlohmann::json* result)
{
    if (source.empty()) {
        return false;
    }
    auto parsed = nlohmann::json::parse(source, nullptr, false);
    if (parsed.is_discarded()) {
        return false;
    }
    *result = std::move(parsed);
    return true;
}

bool SkipLayoutQualifier(Cursor* cursor)
{
    const auto save = cursor->Save();
    if (!cursor->MatchKeyword("layout")) {
        return false;
    }
    cursor->SkipHSpace();
    if (!cursor->MatchChar('(')) {
        cursor->Restore(save);
        return false;
    }

    int depth = 1;
    while (!cursor->Eof() && depth > 0) {
        const char ch = cursor->Peek();
        cursor->Advance();
        if (ch == '(') {
            depth++;
        } else if (ch == ')') {
            depth--;
        }
    }
    if (depth != 0) {
        cursor->Restore(save);
        return false;
    }
    cursor->SkipHSpace();
    return true;
}

bool SkipUniformQualifiers(Cursor* cursor)
{
    bool saw_qualifier = false;
    for (;;) {
        const auto save = cursor->Save();
        if (cursor->MatchKeyword("highp") ||
            cursor->MatchKeyword("mediump") ||
            cursor->MatchKeyword("lowp")) {
            cursor->SkipHSpace();
            saw_qualifier = true;
            continue;
        }
        cursor->Restore(save);
        return saw_qualifier;
    }
}

bool StartsUniformDeclaration(std::string_view line)
{
    Cursor cursor(line);
    cursor.SkipHSpace();
    (void)SkipLayoutQualifier(&cursor);
    return cursor.MatchKeyword("uniform");
}

void HandleComboLine(WPShaderInfo* shader_info, std::string_view line)
{
    const auto brace = line.find('{');
    if (brace == std::string_view::npos) {
        return;
    }

    nlohmann::json combo_json;
    if (!ParseInlineJson(line.substr(brace), &combo_json) || !combo_json.contains("combo")) {
        return;
    }

    std::string name;
    int32_t value = 0;
    GET_JSON_NAME_VALUE(combo_json, "combo", name);
    GET_JSON_NAME_VALUE(combo_json, "default", value);
    if (!name.empty()) {
        shader_info->combos[name] = std::to_string(value);
    }
}

void HandleTextureUniform(
    WPShaderInfo* shader_info,
    std::span<const WPShaderTexInfo> tex_infos,
    std::string_view uniform_name,
    const nlohmann::json& shader_value_json)
{
    wpscene::WPUniformTex uniform_texture;
    uniform_texture.FromJson(shader_value_json);

    i32 index { 0 };
    const auto suffix = uniform_name.substr(9);
    const auto [_, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), index);
    (void)ec;

    if (!uniform_texture.default_.empty()) {
        shader_info->defTexs.push_back({ index, uniform_texture.default_ });
    }

    const idx texture_count = static_cast<idx>(tex_infos.size());
    if (!uniform_texture.combo.empty()) {
        shader_info->combos[uniform_texture.combo] = index >= texture_count ? "0" : "1";
    }

    if (index < texture_count && tex_infos[static_cast<usize>(index)].enabled) {
        const auto& components = tex_infos[static_cast<usize>(index)].composEnabled;
        const usize count = std::min(std::size(components), std::size(uniform_texture.components));
        for (usize i = 0; i < count; i++) {
            if (components[i] && !uniform_texture.components[i].combo.empty()) {
                shader_info->combos[uniform_texture.components[i].combo] = "1";
            }
        }
    }
}

void HandleScalarUniform(
    WPShaderInfo* shader_info,
    std::string_view uniform_name,
    const nlohmann::json& shader_value_json)
{
    const std::string name(uniform_name);
    if (shader_value_json.contains("default")) {
        const auto value = shader_value_json.at("default");
        ShaderValue shader_value;
        if (value.is_string()) {
            std::vector<float> values;
            GET_JSON_VALUE(value, values);
            shader_value = std::span<const float>(values);
        } else if (value.is_number()) {
            shader_value.setSize(1);
            GET_JSON_VALUE(value, shader_value[0]);
        }
        shader_info->svs[name] = shader_value;
    }
    if (shader_value_json.contains("combo")) {
        std::string combo_name;
        GET_JSON_NAME_VALUE(shader_value_json, "combo", combo_name);
        if (!combo_name.empty()) {
            shader_info->combos[combo_name] = "1";
        }
    }
}

void HandleUniformLine(
    WPShaderInfo* shader_info,
    std::span<const WPShaderTexInfo> tex_infos,
    std::string_view line)
{
    Cursor cursor(line);
    cursor.SkipHSpace();
    (void)SkipLayoutQualifier(&cursor);
    if (!cursor.MatchKeyword("uniform")) return;
    cursor.SkipHSpace();
    (void)SkipUniformQualifiers(&cursor);
    auto type = cursor.ReadIdent();
    if (!type) return;
    cursor.SkipHSpace();
    auto name = cursor.ReadIdent();
    if (!name) return;
    cursor.SkipHSpace();
    (void)cursor.ReadArraySuffix();
    cursor.SkipHSpace();
    if (!cursor.MatchChar(';')) return;

    while (!cursor.Eof() && cursor.Peek() != '/') {
        cursor.Advance();
    }
    if (!cursor.MatchPunct("//")) return;
    while (!cursor.Eof() && cursor.Peek() != '{') {
        cursor.Advance();
    }
    if (cursor.Eof()) return;

    nlohmann::json shader_value_json;
    if (!ParseInlineJson(line.substr(cursor.Pos()), &shader_value_json)) {
        return;
    }

    std::string material;
    GET_JSON_NAME_VALUE_NOWARN(shader_value_json, "material", material);
    if (!material.empty()) {
        shader_info->alias[material] = std::string(*name);
    }

    if (name->starts_with("g_Texture")) {
        HandleTextureUniform(shader_info, tex_infos, *name, shader_value_json);
    } else {
        HandleScalarUniform(shader_info, *name, shader_value_json);
    }
}
} // namespace

void ParseWPShaderAnnotations(
    std::string_view source,
    WPShaderInfo* shader_info,
    std::span<const WPShaderTexInfo> tex_infos)
{
    if (shader_info == nullptr) {
        return;
    }

    LineWalker walker(source);
    for (; !walker.Done(); walker.Step()) {
        const auto line = walker.Line();
        if (line.empty()) {
            continue;
        }
        if (line.find("void main(") != std::string_view::npos ||
            line.find("void main()") != std::string_view::npos) {
            break;
        }
        if (line.find("// [COMBO]") != std::string_view::npos) {
            HandleComboLine(shader_info, line);
            continue;
        }

        if (StartsUniformDeclaration(line)) {
            HandleUniformLine(shader_info, tex_infos, line);
        }
    }
}
} // namespace wallpaper
