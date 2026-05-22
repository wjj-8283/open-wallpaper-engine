#include "WPShaderParser.hpp"

#include "Fs/IBinaryStream.h"
#include "Utils/Logging.h"
#include "Shader/SceneShaderLegalizer.hpp"
#include "Shader/ShaderPreprocessor.hpp"

#include "Fs/VFS.h"
#include "Utils/Sha.hpp"
#include "WPCommon.hpp"

#include "Vulkan/ShaderComp.hpp"

#include <chrono>
#include <regex>
#include <stack>
#include <string>
#include <unordered_set>

static constexpr std::string_view SHADER_PLACEHOLD { "__SHADER_PLACEHOLD__" };

#define SHADER_DIR    "spvs01"
#define SHADER_SUFFIX "spvs"

using namespace wallpaper;

namespace
{
ShaderStartupMetrics g_shader_startup_metrics;

double MeasureElapsedMs(const std::chrono::steady_clock::time_point started)
{
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - started)
        .count();
}

static constexpr const char* pre_shader_code = R"(#version 150
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
#define atan2 atan
#define fmod(x, y) (x-y*trunc(x/y))
#define ddx dFdx
#define ddy(x) dFdy(-(x))
#define saturate(x) (clamp(x, 0.0, 1.0))

#define max(x, y) max(y, x)

#define float1 float
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define lerp mix

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

)";

inline std::string LoadGlslInclude(fs::VFS& vfs, const std::string& input) {
    std::string::size_type pos = 0;
    std::string            output;
    std::string::size_type linePos = std::string::npos;

    while (linePos = input.find("#include", pos), linePos != std::string::npos) {
        auto lineEnd  = input.find_first_of('\n', linePos);
        auto lineSize = lineEnd - linePos;
        auto lineStr  = input.substr(linePos, lineSize);
        output.append(input.substr(pos, linePos - pos));

        auto inP         = lineStr.find_first_of('\"') + 1;
        auto inE         = lineStr.find_last_of('\"');
        auto includeName = lineStr.substr(inP, inE - inP);
        auto includeSrc  = fs::GetFileContent(vfs, "/assets/shaders/" + includeName);
        output.append("\n//-----include " + includeName + "\n");
        output.append(LoadGlslInclude(vfs, includeSrc));
        output.append("\n//-----include end\n");

        pos = lineEnd;
    }
    output.append(input.substr(pos));
    return output;
}

inline usize FindIncludeInsertPos(const std::string& src, usize startPos) {
    /* rule:
    after attribute/varying/uniform/struct
    befor any func
    not in {}
    not in #if #endif
    */
    (void)startPos;

    auto NposToZero = [](usize p) {
        return p == std::string::npos ? 0 : p;
    };
    auto search = [](const std::string& p, usize pos, const auto& re) {
        auto        startpos = p.begin() + (isize)pos;
        std::smatch match;
        if (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            return pos + (usize)match.position();
        }
        return std::string::npos;
    };
    auto searchLast = [](const std::string& p, const auto& re) {
        auto        startpos = p.begin();
        std::smatch match;
        while (startpos < p.end() && std::regex_search(startpos, p.end(), match, re)) {
            startpos++;
            startpos += match.position();
        }
        return startpos >= p.end() ? std::string::npos : usize(startpos - p.begin());
    };
    auto nextLinePos = [](const std::string& p, usize pos) {
        return p.find_first_of('\n', pos) + 1;
    };

    usize mainPos  = src.find("void main(");
    bool  two_main = src.find("void main(", mainPos + 2) != std::string::npos;
    if (two_main) return 0;

    usize pos;
    {
        const std::regex reAfters(R"(\n(attribute|varying|uniform|struct) )");
        usize            afterPos = searchLast(src, reAfters);
        if (afterPos != std::string::npos) {
            afterPos = nextLinePos(src, afterPos + 1);
        }
        pos = std::min({ NposToZero(afterPos), mainPos });
    }
    {
        std::stack<usize> ifStack;
        usize             nowPos { 0 };
        const std::regex  reIfs(R"((#if|#endif))");
        while (true) {
            auto p = search(src, nowPos + 1, reIfs);
            if (p > mainPos || p == std::string::npos) break;
            if (src.substr(p, 3) == "#if") {
                ifStack.push(p);
            } else {
                if (ifStack.empty()) break;
                usize ifp = ifStack.top();
                ifStack.pop();
                usize endp = p;
                if (pos > ifp && pos <= endp) {
                    pos = nextLinePos(src, endp + 1);
                }
            }
            nowPos = p;
        }
        pos = std::min({ pos, mainPos });
    }

    return NposToZero(pos);
}

inline usize FindMatchingParen(std::string_view source, usize open_paren_pos) {
    int depth = 0;
    for (usize pos = open_paren_pos; pos < source.size(); pos++) {
        if (source[pos] == '(') {
            depth++;
        } else if (source[pos] == ')') {
            depth--;
            if (depth == 0) return pos;
        }
    }
    return std::string::npos;
}

inline std::string RewriteTextureInitializerCast(std::string source, std::string_view vector_type) {
    const std::string declaration = std::string(vector_type) + " ";
    const std::string cast_prefix = std::string(vector_type) + "(";
    usize             search_pos  = 0;

    while ((search_pos = source.find(declaration, search_pos)) != std::string::npos) {
        usize line_start = source.rfind('\n', search_pos);
        line_start       = line_start == std::string::npos ? 0 : line_start + 1;

        const usize line_end = source.find(';', search_pos);
        if (line_end == std::string::npos) break;

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

inline std::unordered_set<std::string> CollectVec3Identifiers(const std::string& source) {
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

inline std::unordered_set<std::string> CollectVec2Identifiers(const std::string& source) {
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

inline std::unordered_set<std::string> CollectScalarFunctionIdentifiers(const std::string& source) {
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

inline std::string MaskScalarFunctionCalls(
    std::string source,
    const std::unordered_set<std::string>& scalar_functions) {
    const std::regex function_call_re(R"(\b([A-Za-z_]\w*)\s*\()", std::regex::ECMAScript);
    usize            search_pos = 0;

    while (search_pos < source.size()) {
        std::smatch match;
        auto        begin = source.cbegin() + static_cast<isize>(search_pos);
        if (! std::regex_search(begin, source.cend(), match, function_call_re)) {
            break;
        }

        const usize       match_pos     = search_pos + (usize)match.position(0);
        const usize       name_pos      = search_pos + (usize)match.position(1);
        const std::string function_name = match[1].str();
        const usize       open_paren    = source.find('(', match_pos);
        if (open_paren == std::string::npos) break;

        if (scalar_functions.count(function_name) == 0) {
            search_pos = open_paren + 1;
            continue;
        }

        const usize close_paren = FindMatchingParen(source, open_paren);
        if (close_paren == std::string::npos) break;

        source.replace(name_pos, close_paren - name_pos + 1, "0.0");
        search_pos = name_pos + 3;
    }

    return source;
}

inline std::string RewriteVec3Vec2BinaryOps(std::string source) {
    const auto vec3_identifiers = CollectVec3Identifiers(source);
    if (vec3_identifiers.empty()) return source;

    const std::regex rhs_vec2_re(R"((\b[A-Za-z_]\w*\b)\s*([+\-])\s*\(?vec2\()");
    usize            search_pos = 0;
    while (search_pos < source.size()) {
        const usize line_end  = source.find('\n', search_pos);
        const usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line      = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;
        if (std::regex_search(line, match, rhs_vec2_re)) {
            const std::string identifier = match[1].str();
            if (vec3_identifiers.count(identifier) != 0) {
                const usize vec2_pos    = line.find("vec2(", (usize)match.position());
                const usize open_paren  = line.find('(', vec2_pos);
                const usize close_paren = FindMatchingParen(line, open_paren);
                if (vec2_pos != std::string::npos && close_paren != std::string::npos) {
                    const std::string operand = line.substr(vec2_pos, close_paren - vec2_pos + 1);
                    line.replace(
                        vec2_pos, close_paren - vec2_pos + 1, "vec3(" + operand + ", 0.0)");
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

inline std::string RewriteFloatVectorInitializers(std::string source) {
    const auto vec2_identifiers  = CollectVec2Identifiers(source);
    const auto vec3_identifiers  = CollectVec3Identifiers(source);
    const auto scalar_functions  = CollectScalarFunctionIdentifiers(source);
    if (vec2_identifiers.empty() && vec3_identifiers.empty()) return source;

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
        const usize line_end  = source.find('\n', search_pos);
        const usize slice_end = line_end == std::string::npos ? source.size() : line_end;
        std::string line      = source.substr(search_pos, slice_end - search_pos);
        std::smatch match;
        if (std::regex_match(line, match, float_init_re)) {
            const std::string rhs        = match[1].str();
            const std::string masked_rhs = MaskScalarFunctionCalls(rhs, scalar_functions);
            const bool has_scalar_swizzle =
                std::regex_search(masked_rhs, std::regex(R"(\.\s*[xyzwrgba]\b)", std::regex::ECMAScript));
            if (! has_scalar_swizzle && masked_rhs.find("dot(") == std::string::npos &&
                masked_rhs.find("length(") == std::string::npos) {
                const bool uses_vec3 = contains_unswizzled_identifier(masked_rhs, vec3_identifiers);

                bool uses_vec2 = masked_rhs.find("vec2(") != std::string::npos;
                if (! uses_vec2) {
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

inline std::string RewriteAudioBarsUintModulo(std::string source) {
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

inline std::string RewriteStepToFloatAssignments(std::string source) {
    return std::regex_replace(
        source,
        std::regex(
            R"STEP(\bint(\s+[A-Za-z_][A-Za-z0-9_]*\s*=\s*step\s*\([^;\n]*\);))STEP",
            std::regex::ECMAScript),
        "float$1");
}

inline std::string RewriteMutableVertexInputs(std::string source) {
    struct MutableInput {
        std::string type;
        std::string name;
        std::string renamed;
    };

    std::vector<MutableInput> mutable_inputs;
    const std::regex          input_re(
        R"((^|\n)(\s*)in\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*;)",
        std::regex::ECMAScript);

    for (auto it = std::sregex_iterator(source.begin(), source.end(), input_re);
         it != std::sregex_iterator();
         ++it) {
        const std::string type = (*it)[3].str();
        const std::string name = (*it)[4].str();
        const std::regex  assign_re("\\b" + name + R"(\b\s*(?:[+\-*/]?=))", std::regex::ECMAScript);
        if (std::regex_search(source.begin(), source.end(), assign_re)) {
            mutable_inputs.push_back(MutableInput {
                .type = type,
                .name = name,
                .renamed = name + "_Input",
            });
        }
    }

    if (mutable_inputs.empty()) return source;

    for (const auto& input : mutable_inputs) {
        const std::regex declaration_re(
            R"((^|\n)(\s*)in\s+)" + input.type + R"(\s+)" + input.name + R"(\s*;)",
            std::regex::ECMAScript);
        source = std::regex_replace(
            source, declaration_re, "$1$2in " + input.type + " " + input.renamed + ";");
    }

    const usize main_pos = source.find("void main()");
    if (main_pos == std::string::npos) return source;
    const usize brace_pos = source.find('{', main_pos);
    if (brace_pos == std::string::npos) return source;

    std::string local_copies;
    for (const auto& input : mutable_inputs) {
        local_copies += "\n " + input.type + " " + input.name + " = " + input.renamed + ";\n";
    }
    source.insert(brace_pos + 1, local_copies);
    return source;
}

inline EShLanguage ToGLSL(ShaderType type) {
    switch (type) {
    case ShaderType::VERTEX: return EShLangVertex;
    case ShaderType::FRAGMENT: return EShLangFragment;
    default: return EShLangVertex;
    }
}

inline std::string Preprocessor(const std::string& in_src, ShaderType type, const Combos& combos,
                                WPPreprocessorInfo& process_info) {
    const auto preprocess_started = std::chrono::steady_clock::now();
    std::string preprocessed =
        wallpaper::shader::PreprocessStageSource(in_src, type, combos, process_info);
    g_shader_startup_metrics.preprocess_ms += MeasureElapsedMs(preprocess_started);

    const auto legalize_started = std::chrono::steady_clock::now();
    auto legalized = wallpaper::shader::LegalizeStageSource(preprocessed, type);
    g_shader_startup_metrics.legalize_ms += MeasureElapsedMs(legalize_started);
    process_info = std::move(legalized.preprocess_info);
    return std::move(legalized.source);
}

inline std::string Finalprocessor(const WPShaderUnit& unit, const WPPreprocessorInfo* pre,
                                  const WPPreprocessorInfo* next) {
    const auto finalize_started = std::chrono::steady_clock::now();
    auto finalized = wallpaper::shader::FinalizeStageSource(unit, pre, next);
    g_shader_startup_metrics.final_assembly_ms += MeasureElapsedMs(finalize_started);
    return finalized;
}

inline std::string GenSha1(std::span<const WPShaderUnit> units, const WPShaderInfo* shader_info) {
    std::string shas;
    shas += "rev:";
    shas += std::to_string(WPShaderParser::kShaderPipelineRevision);
    shas.push_back('\n');
    if (shader_info != nullptr) {
        for (const auto& [name, value] : shader_info->combos) {
            shas += "combo:";
            shas += name;
            shas.push_back('=');
            shas += value;
            shas.push_back('\n');
        }
    }
    for (auto& unit : units) {
        shas += "stage:";
        shas += std::to_string(static_cast<int>(unit.stage));
        shas.push_back('\n');
        shas += utils::genSha1(unit.src);
        shas.push_back('\n');
    }
    return utils::genSha1(shas);
}
inline std::string GetCachePath(std::string_view scene_id, std::string_view filename) {
    return std::string("/cache/") + std::string(scene_id) + "/" SHADER_DIR "/" +
           std::string(filename) + "." SHADER_SUFFIX;
}

inline bool LoadShaderFromFile(std::vector<ShaderCode>& codes, fs::IBinaryStream& file) {
    codes.clear();
    i32 ver = ReadSPVVesion(file);

    usize count = file.ReadUint32();
    assert(count <= 16 && count >= 0);
    if (count > 16) return false;

    codes.resize(count);
    for (usize i = 0; i < count; i++) {
        auto& c = codes[i];

        u32 size = file.ReadUint32();
        assert(size % 4 == 0);
        if (size % 4 != 0) return false;

        c.resize(size / 4);
        file.Read((char*)c.data(), size);
    }
    return true;
}

inline void SaveShaderToFile(std::span<const ShaderCode> codes, fs::IBinaryStreamW& file) {
    char nop[256] { '\0' };

    WriteSPVVesion(file, 1);
    file.WriteUint32((u32)codes.size());
    for (const auto& c : codes) {
        u32 size = (u32)c.size() * 4;
        file.WriteUint32(size);
        file.Write((const char*)c.data(), size);
    }
    file.Write(nop, sizeof(nop));
}

} // namespace

std::string WPShaderParser::PreShaderSrc(fs::VFS& vfs, const std::string& src,
                                         WPShaderInfo*                       pWPShaderInfo,
                                         const std::vector<WPShaderTexInfo>& texinfos) {
    const auto include_started = std::chrono::steady_clock::now();
    auto expansion = wallpaper::shader::ExpandIncludes(vfs, src);
    g_shader_startup_metrics.include_expand_ms += MeasureElapsedMs(include_started);

    const auto metadata_started = std::chrono::steady_clock::now();
    wallpaper::shader::ExtractMetadata(expansion.expanded_includes, pWPShaderInfo, texinfos);
    wallpaper::shader::ExtractMetadata(expansion.source_without_includes, pWPShaderInfo, texinfos);
    g_shader_startup_metrics.metadata_extract_ms += MeasureElapsedMs(metadata_started);
    return wallpaper::shader::MergeExpandedIncludes(std::move(expansion));
}

std::string WPShaderParser::PreShaderHeader(const std::string& src, const Combos& combos,
                                            ShaderType type) {
    return wallpaper::shader::BuildShaderHeader(src, combos, type);
}

void WPShaderParser::InitGlslang() { glslang::InitializeProcess(); }
void WPShaderParser::FinalGlslang() { glslang::FinalizeProcess(); }

void WPShaderParser::ResetStartupMetrics() { g_shader_startup_metrics = {}; }
ShaderStartupMetrics WPShaderParser::GetStartupMetrics() { return g_shader_startup_metrics; }

bool WPShaderParser::CompileToSpv(std::string_view scene_id, std::span<WPShaderUnit> units,
                                  std::vector<ShaderCode>& codes, fs::VFS& vfs,
                                  WPShaderInfo* shader_info, std::span<const WPShaderTexInfo> texs) {
    (void)texs;

    auto preprocess_units = [shader_info](std::span<WPShaderUnit> units) {
        const Combos empty_combos {};
        const auto& combos = shader_info != nullptr ? shader_info->combos : empty_combos;
        for (auto& unit : units) {
            unit.src = Preprocessor(unit.src, unit.stage, combos, unit.preprocess_info);
        }
    };

    auto finalize_units = [](std::span<WPShaderUnit> units) {
        std::vector<vulkan::ShaderCompUnit> vunits(units.size());
        for (usize i = 0; i < units.size(); i++) {
            auto&               unit     = units[i];
            auto&               vunit    = vunits[i];
            WPPreprocessorInfo* pre_info = i >= 1 ? &units[i - 1].preprocess_info : nullptr;
            WPPreprocessorInfo* post_info =
                i + 1 < units.size() ? &units[i + 1].preprocess_info : nullptr;

            unit.src = Finalprocessor(unit, pre_info, post_info);

            vunit.src   = unit.src;
            vunit.stage = ToGLSL(unit.stage);
        }
        return vunits;
    };

    auto compile = [](std::vector<vulkan::ShaderCompUnit>& vunits, std::vector<ShaderCode>& codes) {
        vulkan::ShaderCompOpt opt;
        opt.client_ver             = glslang::EShTargetVulkan_1_1;
        opt.auto_map_bindings      = true;
        opt.auto_map_locations     = true;
        opt.relaxed_errors_glsl    = true;
        opt.relaxed_rules_vulkan   = true;
        opt.suppress_warnings_glsl = true;

        std::vector<vulkan::Uni_ShaderSpv> spvs(vunits.size());

        const auto compile_started = std::chrono::steady_clock::now();
        if (! vulkan::CompileAndLinkShaderUnits(vunits, opt, spvs)) {
            g_shader_startup_metrics.compile_ms += MeasureElapsedMs(compile_started);
            return false;
        }
        g_shader_startup_metrics.compile_ms += MeasureElapsedMs(compile_started);

        codes.clear();
        for (auto& spv : spvs) {
            codes.emplace_back(std::move(spv->spirv));
        }
        return true;
    };

    bool has_cache_dir = vfs.IsMounted("cache");

    if (has_cache_dir) {
        std::string sha1            = GenSha1(units, shader_info);
        std::string cache_file_path = GetCachePath(scene_id, sha1);

        if (vfs.Contains(cache_file_path)) {
            g_shader_startup_metrics.cache_hits++;
            const auto cache_read_started = std::chrono::steady_clock::now();
            auto cache_file = vfs.Open(cache_file_path);
            const bool loaded = cache_file && ::LoadShaderFromFile(codes, *cache_file);
            g_shader_startup_metrics.cache_read_ms += MeasureElapsedMs(cache_read_started);
            if (!loaded) {
                LOG_ERROR("load shader from \'%s\' failed", cache_file_path.c_str());
                return false;
            }
            preprocess_units(units);
            (void)finalize_units(units);
        } else {
            g_shader_startup_metrics.cache_misses++;
            preprocess_units(units);
            auto vunits = finalize_units(units);
            if (! compile(vunits, codes)) return false;
            const auto cache_write_started = std::chrono::steady_clock::now();
            if (auto cache_file = vfs.OpenW(cache_file_path); cache_file) {
                ::SaveShaderToFile(codes, *cache_file);
            }
            g_shader_startup_metrics.cache_write_ms += MeasureElapsedMs(cache_write_started);
        }
        return true;

    } else {
        preprocess_units(units);
        auto vunits = finalize_units(units);
        return compile(vunits, codes);
    }
}
