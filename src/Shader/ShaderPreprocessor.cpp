#include "Shader/ShaderPreprocessor.hpp"

#include "Fs/VFS.h"
#include "Utils/Logging.h"
#include "Vulkan/ShaderComp.hpp"

#include <algorithm>
#include <regex>
#include <stack>
#include <string>
#include <unordered_set>

using namespace wallpaper;

namespace
{
static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

static constexpr const char* pre_shader_code = R"(#version 330
#extension GL_EXT_spec_constant_composites : enable
#define GLSL 1
#define HLSL 0
#define highp

#define CAST2(x) (vec2(x))
#define CAST3(x) (vec3(x))
#define CAST4(x) (vec4(x))
#define CAST3X3(x) (mat3(x))

#define texSample2D texture
#define texSample2DLod textureLod
#define mul(x, y) ((y) * (x))
#define frac fract
#define log10(x) log2(x) * 0.301029995663981
#define atan2 atan
#define fmod(x, y) ((x)-(y)*trunc((x)/(y)))
#define ddx dFdx
#define ddy(x) dFdy(-(x))
#define saturate(x) (clamp(x, 0.0, 1.0))

#define float1 float
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix

vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal, vec3 viewVector,
                        vec3 specularTint, vec3 f0, float roughness, float metallic) {
    return albedo * max(dot(normalize(normal), normalize(viewVector)), 0.0);
}
vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal, vec3 viewVector,
                        vec3 specularTint, vec3 f0, float roughness, float metallic,
                        float ao) {
    return albedo * ao * max(dot(normalize(normal), normalize(viewVector)), 0.0);
}

__SHADER_PLACEHOLD__

)";

static constexpr const char* pre_shader_code_vert = R"(
#define attribute in
#define varying out

)";

static constexpr const char* pre_shader_code_frag = R"(
#define varying in
#define gl_FragColor glOutColor
out vec4 glOutColor;
void clip(float value) { if (value < 0.0) discard; }
void clip(vec2 value) { if (any(lessThan(value, vec2(0.0)))) discard; }
void clip(vec3 value) { if (any(lessThan(value, vec3(0.0)))) discard; }
void clip(vec4 value) { if (any(lessThan(value, vec4(0.0)))) discard; }

)";

EShLanguage ToGLSL(wallpaper::ShaderType type)
{
    switch (type) {
    case wallpaper::ShaderType::VERTEX: return EShLangVertex;
    case wallpaper::ShaderType::FRAGMENT: return EShLangFragment;
    default: return EShLangVertex;
    }
}

std::string LoadGlslInclude(wallpaper::fs::VFS& vfs, const std::string& input)
{
    std::string::size_type pos = 0;
    std::string output;
    std::string::size_type line_pos = std::string::npos;

    while ((line_pos = input.find("#include", pos)) != std::string::npos) {
        const auto line_end = input.find_first_of('\n', line_pos);
        const auto line_size = line_end - line_pos;
        const auto line = input.substr(line_pos, line_size);
        output.append(input.substr(pos, line_pos - pos));

        const auto include_begin = line.find_first_of('"') + 1;
        const auto include_end = line.find_last_of('"');
        const auto include_name = line.substr(include_begin, include_end - include_begin);
        const auto include_source =
            wallpaper::fs::GetFileContent(vfs, "/assets/shaders/" + include_name);

        output.append("\n//-----include " + include_name + "\n");
        output.append(LoadGlslInclude(vfs, include_source));
        output.append("\n//-----include end\n");

        pos = line_end;
    }

    output.append(input.substr(pos));
    return output;
}

usize FindIncludeInsertPos(const std::string& source)
{
    auto is_identifier_char = [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
    };
    auto skip_line_comment = [](const std::string& input, usize pos) {
        const usize end = input.find('\n', pos);
        return end == std::string::npos ? input.size() : end;
    };
    auto skip_block_comment = [](const std::string& input, usize pos) {
        const usize end = input.find("*/", pos);
        return end == std::string::npos ? input.size() : end + 2;
    };
    auto skip_whitespace_and_comments = [&](usize pos) {
        while (pos < source.size()) {
            if (std::isspace(static_cast<unsigned char>(source[pos]))) {
                pos++;
                continue;
            }
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '/') {
                pos = skip_line_comment(source, pos + 2);
                continue;
            }
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '*') {
                pos = skip_block_comment(source, pos + 2);
                continue;
            }
            break;
        }
        return pos;
    };
    auto next_line_pos = [&](usize pos) {
        const usize end = source.find('\n', pos);
        return end == std::string::npos ? source.size() : end + 1;
    };
    auto starts_with_directive = [&](usize pos, std::string_view directive) {
        if (pos >= source.size() || source[pos] != '#') {
            return false;
        }
        return source.compare(pos, directive.size(), directive) == 0;
    };
    auto starts_with_keyword = [&](usize pos, std::string_view keyword) {
        if (pos + keyword.size() > source.size()) {
            return false;
        }
        if (source.compare(pos, keyword.size(), keyword) != 0) {
            return false;
        }
        if (pos > 0 && is_identifier_char(source[pos - 1])) {
            return false;
        }
        const usize end = pos + keyword.size();
        return end >= source.size() || !is_identifier_char(source[end]);
    };
    auto find_statement_end = [&](usize pos) {
        int paren_depth = 0;
        for (; pos < source.size(); ++pos) {
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '/') {
                pos = skip_line_comment(source, pos + 2);
                if (pos >= source.size()) break;
                continue;
            }
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '*') {
                pos = skip_block_comment(source, pos + 2);
                if (pos >= source.size()) break;
                continue;
            }
            if (source[pos] == '(') {
                paren_depth++;
            } else if (source[pos] == ')') {
                paren_depth = std::max(0, paren_depth - 1);
            } else if (source[pos] == ';' && paren_depth == 0) {
                return pos;
            }
        }
        return std::string::npos;
    };
    auto find_function_signature_end = [&](usize pos) {
        int paren_depth = 0;
        bool saw_open_paren = false;
        for (; pos < source.size(); ++pos) {
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '/') {
                pos = skip_line_comment(source, pos + 2);
                if (pos >= source.size()) break;
                continue;
            }
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '*') {
                pos = skip_block_comment(source, pos + 2);
                if (pos >= source.size()) break;
                continue;
            }
            if (source[pos] == '(') {
                saw_open_paren = true;
                paren_depth++;
            } else if (source[pos] == ')') {
                paren_depth = std::max(0, paren_depth - 1);
            } else if (saw_open_paren && paren_depth == 0) {
                if (source[pos] == '{') {
                    return pos;
                }
                if (source[pos] == ';') {
                    return pos;
                }
            }
        }
        return std::string::npos;
    };
    auto find_struct_end = [&](usize pos) {
        pos = source.find('{', pos);
        if (pos == std::string::npos) {
            return pos;
        }
        int brace_depth = 0;
        for (; pos < source.size(); ++pos) {
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '/') {
                pos = skip_line_comment(source, pos + 2);
                if (pos >= source.size()) break;
                continue;
            }
            if (pos + 1 < source.size() && source[pos] == '/' && source[pos + 1] == '*') {
                pos = skip_block_comment(source, pos + 2);
                if (pos >= source.size()) break;
                continue;
            }
            if (source[pos] == '{') {
                brace_depth++;
            } else if (source[pos] == '}') {
                brace_depth--;
                if (brace_depth == 0) {
                    return find_statement_end(pos);
                }
            }
        }
        return std::string::npos;
    };
    auto previous_significant_char = [&](usize pos) {
        while (pos > 0) {
            pos--;
            if (std::isspace(static_cast<unsigned char>(source[pos]))) {
                continue;
            }
            return source[pos];
        }
        return '\0';
    };

    const usize main_pos = source.find("void main(");
    if (main_pos != std::string::npos &&
        source.find("void main(", main_pos + 2) != std::string::npos) {
        return 0;
    }

    const usize limit = main_pos == std::string::npos ? source.size() : main_pos;
    usize insert_pos = 0;
    usize pos = 0;
    while (pos < limit) {
        pos = skip_whitespace_and_comments(pos);
        if (pos >= limit) {
            break;
        }
        if (source[pos] == '#') {
            if (starts_with_directive(pos, "#if") ||
                starts_with_directive(pos, "#ifdef") ||
                starts_with_directive(pos, "#ifndef") ||
                starts_with_directive(pos, "#elif") ||
                starts_with_directive(pos, "#else") ||
                starts_with_directive(pos, "#endif")) {
                return insert_pos == 0 ? pos : insert_pos;
            }
            pos = next_line_pos(pos);
            continue;
        }
        if (starts_with_keyword(pos, "uniform") || starts_with_keyword(pos, "attribute") ||
            starts_with_keyword(pos, "varying")) {
            const usize end = find_statement_end(pos);
            if (end == std::string::npos || end >= limit) {
                break;
            }
            insert_pos = next_line_pos(end + 1);
            pos = insert_pos;
            continue;
        }
        if (starts_with_keyword(pos, "struct")) {
            const usize end = find_struct_end(pos);
            if (end == std::string::npos || end >= limit) {
                break;
            }
            insert_pos = next_line_pos(end + 1);
            pos = insert_pos;
            continue;
        }
        const usize function_signature_end = find_function_signature_end(pos);
        if (function_signature_end != std::string::npos && function_signature_end < limit &&
            source[function_signature_end] == '{') {
            return insert_pos == 0 ? pos : insert_pos;
        }
        if (source[pos] == '{' && previous_significant_char(pos) == ')') {
            return insert_pos == 0 ? pos : insert_pos;
        }
        pos++;
    }

    return insert_pos;
}

usize FindMatchingParen(std::string_view source, usize open_paren_pos)
{
    int depth = 0;
    for (usize pos = open_paren_pos; pos < source.size(); pos++) {
        if (source[pos] == '(') {
            depth++;
        } else if (source[pos] == ')') {
            depth--;
            if (depth == 0) {
                return pos;
            }
        }
    }
    return std::string::npos;
}

std::string RewriteTextureInitializerCast(std::string source, std::string_view vector_type)
{
    const std::string declaration = std::string(vector_type) + " ";
    const std::string cast_prefix = std::string(vector_type) + "(";
    usize search_pos = 0;

    while ((search_pos = source.find(declaration, search_pos)) != std::string::npos) {
        usize line_start = source.rfind('\n', search_pos);
        line_start = line_start == std::string::npos ? 0 : line_start + 1;

        const usize line_end = source.find(';', search_pos);
        if (line_end == std::string::npos) {
            break;
        }

        std::string line = source.substr(line_start, line_end - line_start + 1);
        const usize equal_pos = line.find('=');
        if (equal_pos == std::string::npos) {
            search_pos = line_end + 1;
            continue;
        }

        usize call_pos = line.find("texture(", equal_pos);
        if (call_pos == std::string::npos) {
            call_pos = line.find("texSample2D(", equal_pos);
        }
        if (call_pos == std::string::npos) {
            search_pos = line_end + 1;
            continue;
        }

        if (call_pos >= cast_prefix.size() &&
            line.substr(call_pos - cast_prefix.size(), cast_prefix.size()) == cast_prefix) {
            search_pos = line_end + 1;
            continue;
        }

        const usize open_paren = line.find('(', call_pos);
        const usize close_paren = FindMatchingParen(line, open_paren);
        if (close_paren == std::string::npos) {
            search_pos = line_end + 1;
            continue;
        }
        if (close_paren + 1 < line.size() && line[close_paren + 1] == '.') {
            search_pos = line_end + 1;
            continue;
        }

        line.insert(close_paren + 1, ")");
        line.insert(call_pos, cast_prefix);

        source.replace(line_start, line_end - line_start + 1, line);
        search_pos = line_start + line.size();
    }

    return source;
}

std::unordered_set<std::string> CollectVec3Identifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    const std::regex vec3_re(
        R"((?:^|\n)\s*(?:in|out|uniform)?\s*vec3\s+([A-Za-z_]\w*)\s*(?:[;=]))",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), vec3_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

std::unordered_set<std::string> CollectVec2Identifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    const std::regex vec2_re(
        R"((?:^|\n)\s*(?:in|out|uniform)?\s*vec2\s+([A-Za-z_]\w*)\s*(?:[;=]))",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), vec2_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

std::unordered_set<std::string> CollectScalarFunctionIdentifiers(const std::string& source)
{
    std::unordered_set<std::string> identifiers;
    const std::regex scalar_function_re(
        R"((?:^|\n)\s*(?:float|int|bool)\s+([A-Za-z_]\w*)\s*\()",
        std::regex::ECMAScript);
    for (auto it = std::sregex_iterator(source.begin(), source.end(), scalar_function_re);
         it != std::sregex_iterator();
         ++it) {
        identifiers.insert((*it)[1].str());
    }
    return identifiers;
}

std::string MaskScalarFunctionCalls(
    std::string source,
    const std::unordered_set<std::string>& scalar_functions)
{
    const std::regex function_call_re(R"(\b([A-Za-z_]\w*)\s*\()", std::regex::ECMAScript);
    usize search_pos = 0;

    while (search_pos < source.size()) {
        std::smatch match;
        auto begin = source.cbegin() + static_cast<isize>(search_pos);
        if (!std::regex_search(begin, source.cend(), match, function_call_re)) {
            break;
        }

        const usize match_pos = search_pos + static_cast<usize>(match.position(0));
        const usize name_pos = search_pos + static_cast<usize>(match.position(1));
        const std::string function_name = match[1].str();
        const usize open_paren = source.find('(', match_pos);
        if (open_paren == std::string::npos) {
            break;
        }

        if (scalar_functions.count(function_name) == 0) {
            search_pos = open_paren + 1;
            continue;
        }

        const usize close_paren = FindMatchingParen(source, open_paren);
        if (close_paren == std::string::npos) {
            break;
        }

        source.replace(name_pos, close_paren - name_pos + 1, "0.0");
        search_pos = name_pos + 3;
    }

    return source;
}

std::string RewriteVec3Vec2BinaryOps(std::string source)
{
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    if (vec3_identifiers.empty()) {
        return source;
    }

    const std::regex rhs_vec2_re(R"((\b[A-Za-z_]\w*\b)\s*([+\-])\s*\(?vec2\()");
    usize search_pos = 0;
    while (search_pos < source.size()) {
        const usize line_end = source.find('\n', search_pos);
        const usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;
        if (std::regex_search(line, match, rhs_vec2_re)) {
            const std::string identifier = match[1].str();
            if (vec3_identifiers.count(identifier) != 0) {
                const usize vec2_pos = line.find("vec2(", match.position());
                const usize open_paren = line.find('(', vec2_pos);
                const usize close_paren = FindMatchingParen(line, open_paren);
                if (vec2_pos != std::string::npos && close_paren != std::string::npos) {
                    const std::string operand = line.substr(vec2_pos, close_paren - vec2_pos + 1);
                    line.replace(
                        vec2_pos,
                        close_paren - vec2_pos + 1,
                        "vec3(" + operand + ", 0.0)");
                    source.replace(search_pos, slice_end - search_pos, line);
                    search_pos += line.size();
                    continue;
                }
            }
        }
        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

std::string RewriteFloatVectorInitializers(std::string source)
{
    const auto vec2_identifiers = CollectVec2Identifiers(source);
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    const auto scalar_functions = CollectScalarFunctionIdentifiers(source);
    if (vec2_identifiers.empty() && vec3_identifiers.empty()) {
        return source;
    }

    const std::regex float_init_re(R"(^\s*float\s+[A-Za-z_]\w*\s*=\s*(.+);$)");
    auto contains_unswizzled_identifier = [](std::string_view expression,
                                             const std::unordered_set<std::string>& identifiers) {
        for (const auto& identifier : identifiers) {
            const std::regex identifier_re(
                "\\b" + identifier + R"(\b(?!\s*\.))", std::regex::ECMAScript);
            if (std::regex_search(expression.begin(), expression.end(), identifier_re)) {
                return true;
            }
        }
        return false;
    };
    usize search_pos = 0;
    while (search_pos < source.size()) {
        const usize line_end = source.find('\n', search_pos);
        const usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;
        if (std::regex_match(line, match, float_init_re)) {
            const std::string rhs = match[1].str();
            const std::string masked_rhs = MaskScalarFunctionCalls(rhs, scalar_functions);
            const bool has_scalar_swizzle =
                std::regex_search(masked_rhs, std::regex(R"(\.\s*[xyzwrgba]\b)", std::regex::ECMAScript));
            if (!has_scalar_swizzle && masked_rhs.find("dot(") == std::string::npos &&
                masked_rhs.find("length(") == std::string::npos) {
                const bool uses_vec3 = contains_unswizzled_identifier(masked_rhs, vec3_identifiers);

                bool uses_vec2 = masked_rhs.find("vec2(") != std::string::npos;
                if (!uses_vec2) {
                    uses_vec2 = contains_unswizzled_identifier(masked_rhs, vec2_identifiers);
                }

                if (uses_vec3 || uses_vec2) {
                    std::string scalarized_rhs;
                    if (uses_vec3) {
                        scalarized_rhs =
                            "max(max((" + rhs + ").x, (" + rhs + ").y), (" + rhs + ").z)";
                    } else {
                        scalarized_rhs = "max((" + rhs + ").x, (" + rhs + ").y)";
                    }

                    const usize equal_pos = line.find('=');
                    line.replace(equal_pos + 1, std::string::npos, " " + scalarized_rhs + ";");
                    source.replace(search_pos, slice_end - search_pos, line);
                    search_pos += line.size();
                    continue;
                }
            }
        }

        search_pos = line_end == std::string::npos ? source.size() : line_end + 1;
    }

    return source;
}

std::string RewriteAudioBarsUintModulo(std::string source)
{
    source = std::regex_replace(
        source,
        std::regex(
            R"(\buint\s+barFreq1\s*=\s*frequency\s*%\s*([0-9]+)\s*;)",
            std::regex::ECMAScript),
        "uint barFreq1 = uint(frequency) % $1u;");
    source = std::regex_replace(
        source,
        std::regex(
            R"(\buint\s+barFreq2\s*=\s*\(barFreq1\s*\+\s*1\s*\)\s*%\s*([0-9]+)\s*;)",
            std::regex::ECMAScript),
        "uint barFreq2 = (barFreq1 + 1u) % $1u;");
    source = std::regex_replace(
        source,
        std::regex(
            R"(\buint\s+barFreq1\s*=\s*frequency\s*%\s*RESOLUTION\s*;)",
            std::regex::ECMAScript),
        "uint barFreq1 = uint(frequency) % uint(RESOLUTION);");
    source = std::regex_replace(
        source,
        std::regex(
            R"(\(barFreq1\s*\+\s*1\s*\)\s*%\s*RESOLUTION)",
            std::regex::ECMAScript),
        "(barFreq1 + 1u) % uint(RESOLUTION)");
    return source;
}

std::string RewriteStepToFloatAssignments(std::string source)
{
    return std::regex_replace(
        source,
        std::regex(
            R"STEP(\bint(\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*step\s*\([^;\n]*\);))STEP",
            std::regex::ECMAScript),
        "float$1");
}

std::string CleanupScalarSwizzleArtifacts(std::string source)
{
    const std::regex scalarized_vec2_re(
        R"(max\(\((.+?\.[xyzwrgba])\)\.x,\s*\(\1\)\.y\))",
        std::regex::ECMAScript);
    source = std::regex_replace(source, scalarized_vec2_re, "$1");

    const std::regex scalarized_vec3_re(
        R"(max\(max\(\((.+?\.[xyzwrgba])\)\.x,\s*\(\1\)\.y\),\s*\(\1\)\.z\))",
        std::regex::ECMAScript);
    source = std::regex_replace(source, scalarized_vec3_re, "$1");

    return source;
}

std::string RewriteMutableVertexInputs(std::string source)
{
    struct MutableInput
    {
        std::string type;
        std::string name;
        std::string renamed;
    };

    std::vector<MutableInput> mutable_inputs;
    const std::regex input_re(
        R"((^|\n)(\s*)in\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;)",
        std::regex::ECMAScript);

    for (auto it = std::sregex_iterator(source.begin(), source.end(), input_re);
         it != std::sregex_iterator();
         ++it) {
        const std::string type = (*it)[3].str();
        const std::string name = (*it)[4].str();
        const std::regex assign_re("\\b" + name + R"(\b\s*(?:[+\-*/]?=))", std::regex::ECMAScript);
        if (std::regex_search(source.begin(), source.end(), assign_re)) {
            mutable_inputs.push_back(MutableInput {
                .type = type,
                .name = name,
                .renamed = name + "_Input",
            });
        }
    }

    if (mutable_inputs.empty()) {
        return source;
    }

    for (const auto& input : mutable_inputs) {
        const std::regex declaration_re(
            R"((^|\n)(\s*)in\s+)" + input.type + R"(\s+)" + input.name + R"(\s*;)",
            std::regex::ECMAScript);
        source = std::regex_replace(
            source,
            declaration_re,
            "$1$2in " + input.type + " " + input.renamed + ";");
    }

    const usize main_pos = source.find("void main()");
    if (main_pos == std::string::npos) {
        return source;
    }
    const usize brace_pos = source.find('{', main_pos);
    if (brace_pos == std::string::npos) {
        return source;
    }

    std::string local_copies;
    for (const auto& input : mutable_inputs) {
        local_copies += "\n " + input.type + " " + input.name + " = " + input.renamed + ";\n";
    }
    source.insert(brace_pos + 1, local_copies);
    return source;
}
} // namespace

namespace wallpaper::shader
{

IncludeExpansionResult ExpandIncludes(fs::VFS& vfs, const std::string& source)
{
    IncludeExpansionResult expansion { .source_without_includes = source };

    std::string::size_type pos = 0;
    while ((pos = source.find("#include", pos)) != std::string::npos) {
        const auto begin = pos;
        pos = source.find_first_of('\n', pos);
        expansion.source_without_includes.replace(begin, pos - begin, pos - begin, ' ');
        expansion.expanded_includes.append(source.substr(begin, pos - begin) + "\n");
    }

    expansion.expanded_includes = LoadGlslInclude(vfs, expansion.expanded_includes);
    return expansion;
}

void ExtractMetadata(
    std::string_view source,
    WPShaderInfo* shader_info,
    std::span<const WPShaderTexInfo> tex_infos)
{
    ParseWPShaderAnnotations(source, shader_info, tex_infos);
}

std::string MergeExpandedIncludes(IncludeExpansionResult expansion)
{
    expansion.source_without_includes.insert(
        FindIncludeInsertPos(expansion.source_without_includes), expansion.expanded_includes);
    return expansion.source_without_includes;
}

std::string BuildShaderHeader(std::string_view source, const Combos& combos, ShaderType type)
{
    std::string pre(pre_shader_code);
    if (type == ShaderType::VERTEX) {
        pre += pre_shader_code_vert;
    } else if (type == ShaderType::FRAGMENT) {
        pre += pre_shader_code_frag;
    }

    std::string header(pre);
    for (const auto& combo : combos) {
        std::string upper(combo.first);
        std::transform(combo.first.begin(), combo.first.end(), upper.begin(), ::toupper);
        if (combo.second.empty()) {
            LOG_ERROR("combo '%s' can't be empty", upper.c_str());
            continue;
        }
        header.append("#define " + upper + " " + combo.second + "\n");
    }

    return header + std::string(source);
}

std::string PreprocessStageSource(
    std::string_view source,
    ShaderType type,
    const Combos& combos,
    WPPreprocessorInfo& process_info)
{
    (void)process_info;
    std::string stage_source = BuildShaderHeader(source, combos, type);

    {
        const std::regex require_re("(^|\r?\n)#require (.+)(\r?\n)");
        stage_source = std::regex_replace(stage_source, require_re, "$1//#require $2$3");
    }

    return stage_source;
}

std::string FinalizeStageSource(
    const WPShaderUnit& unit,
    const WPPreprocessorInfo* previous,
    const WPPreprocessorInfo* next)
{
    std::string insert_source;
    std::string source = unit.src;
    const auto& current = unit.preprocess_info;

    auto replace_declaration = [](std::string& target, const std::string& from, const std::string& to) {
        const usize pos = target.find(from);
        if (pos != std::string::npos) {
            target.replace(pos, from.size(), to);
        }
    };

    if (previous != nullptr) {
        for (const auto& [name, line] : previous->output) {
            if (!exists(current.input, name)) {
                insert_source += std::regex_replace(line, std::regex(R"(\s*out\s)"), " in ");
                insert_source.push_back('\n');
            }
        }
    }
    if (next != nullptr) {
        for (const auto& [name, line] : next->input) {
            if (exists(current.output, name)) {
                const auto& current_line = current.output.at(name);
                if (current_line != line) {
                    replace_declaration(
                        source,
                        current_line,
                        std::regex_replace(line, std::regex(R"(\bin\b)"), "out"));
                }
            } else {
                insert_source += std::regex_replace(line, std::regex(R"(\s*in\s)"), " out ");
                insert_source.push_back('\n');
            }
        }
    }

    return std::regex_replace(source, std::regex(SHADER_PLACEHOLD.data()), insert_source);
}

std::string ApplyCompatibilityRewrites(std::string source, ShaderType type)
{
    if (type == ShaderType::VERTEX) {
        source = RewriteMutableVertexInputs(std::move(source));
    }
    source = RewriteTextureInitializerCast(std::move(source), "vec2");
    source = RewriteTextureInitializerCast(std::move(source), "vec3");
    source = RewriteVec3Vec2BinaryOps(std::move(source));
    source = RewriteFloatVectorInitializers(std::move(source));
    source = RewriteAudioBarsUintModulo(std::move(source));
    source = RewriteStepToFloatAssignments(std::move(source));
    return source;
}

} // namespace wallpaper::shader
