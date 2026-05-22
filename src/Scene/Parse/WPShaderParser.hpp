#pragma once

#include <span>
#include "Scene/Scene.h"
#include "Scene/SceneShader.h"
#include "Type.hpp"

namespace wallpaper
{
namespace fs
{
class VFS;
}
using Combos = Map<std::string, std::string>;

// ui material name to gl uniform name
using WPAliasValueDict = Map<std::string, std::string>;

using WPDefaultTexs = std::vector<std::pair<i32, std::string>>;

struct WPShaderInfo {
    Combos           combos;
    ShaderValueMap   svs;
    ShaderValueMap   baseConstSvs;
    WPAliasValueDict alias;
    WPDefaultTexs    defTexs;
};

struct WPPreprocessorInfo {
    Map<std::string, std::string> input; // name to line
    Map<std::string, std::string> output;

    Set<uint> active_tex_slots;
};

struct WPShaderTexInfo {
    bool                enabled { false };
    std::array<bool, 3> composEnabled { false, false, false };
};

struct WPShaderUnit {
    ShaderType         stage;
    std::string        src;
    WPPreprocessorInfo preprocess_info;
};

void ParseWPShaderAnnotations(
    std::string_view source,
    WPShaderInfo* shader_info,
    std::span<const WPShaderTexInfo> tex_infos);

struct ShaderStartupMetrics {
    uint64_t cache_hits { 0 };
    uint64_t cache_misses { 0 };
    double   include_expand_ms { 0.0 };
    double   metadata_extract_ms { 0.0 };
    double   preprocess_ms { 0.0 };
    double   legalize_ms { 0.0 };
    double   final_assembly_ms { 0.0 };
    double   compile_ms { 0.0 };
    double   cache_read_ms { 0.0 };
    double   cache_write_ms { 0.0 };
};

class WPShaderParser {
public:
    static constexpr uint32_t kShaderPipelineRevision = 2;

    static std::string PreShaderSrc(fs::VFS&, const std::string& src, WPShaderInfo* pWPShaderInfo,
                                    const std::vector<WPShaderTexInfo>& texs);

    static std::string PreShaderHeader(const std::string& src, const Combos& combos, ShaderType);

    static void InitGlslang();
    static void FinalGlslang();

    static void                 ResetStartupMetrics();
    static ShaderStartupMetrics GetStartupMetrics();

    static bool CompileToSpv(std::string_view         scene_id, std::span<WPShaderUnit>,
                             std::vector<ShaderCode>& spvs, fs::VFS&, WPShaderInfo*,
                             std::span<const WPShaderTexInfo>);
};
} // namespace wallpaper
