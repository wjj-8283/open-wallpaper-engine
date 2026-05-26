#include "WPShaderParser.hpp"

#include "Fs/IBinaryStream.h"
#include "Fs/VFS.h"
#include "Shader/RustShaderBridge.hpp"
#include "Utils/Logging.h"

#include "Vulkan/ShaderComp.hpp"

#include <chrono>
#include <optional>
#include <string>

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

} // namespace

void WPShaderParser::InitGlslang() { glslang::InitializeProcess(); }
void WPShaderParser::FinalGlslang() { glslang::FinalizeProcess(); }

void WPShaderParser::ResetStartupMetrics() { g_shader_startup_metrics = {}; }
ShaderStartupMetrics WPShaderParser::GetStartupMetrics() { return g_shader_startup_metrics; }

bool WPShaderParser::CompileToSpvRust(std::string_view scene_id, std::string_view shader_name,
                                      std::span<WPShaderUnit> units,
                                      std::vector<ShaderCode>& codes, fs::VFS& vfs,
                                      WPShaderInfo* shader_info,
                                      std::span<const WPShaderTexInfo> texs,
                                      std::string* reflection_json) {
    if (shader_info == nullptr) return false;

    wallpaper::shader::RustShaderRequest request {
        .shader_name   = std::string(shader_name),
        .scene_id      = std::string(scene_id),
        .cache_enabled = vfs.IsMounted("cache"),
    };
    request.combos = shader_info->combos;
    request.stages.reserve(units.size());
    for (const auto& unit : units) {
        request.stages.push_back(wallpaper::shader::RustShaderStageSource {
            .kind   = unit.stage,
            .source = unit.src,
        });
    }
    request.textures.reserve(texs.size());
    for (usize slot = 0; slot < texs.size(); ++slot) {
        request.textures.push_back(wallpaper::shader::RustShaderTextureInfo {
            .slot       = static_cast<uint32_t>(slot),
            .enabled    = texs[slot].enabled,
            .format     = "unknown",
            .components = texs[slot].composEnabled,
        });
    }

    const auto compile_started = std::chrono::steady_clock::now();
    wallpaper::shader::RustShaderOutput output;
    const auto include_reader = [&vfs](std::string_view path) -> std::optional<std::string> {
        std::string asset_path = "/assets/shaders/" + std::string(path);
        if (auto stream = vfs.Open(asset_path); stream != nullptr) return stream->ReadAllStr();

        std::string direct_path(path);
        if (auto stream = vfs.Open(direct_path); stream != nullptr) return stream->ReadAllStr();

        return std::nullopt;
    };
    if (! wallpaper::shader::CompileRustShaderProgram(request, output, include_reader)) {
        g_shader_startup_metrics.compile_ms += MeasureElapsedMs(compile_started);
        const auto error = wallpaper::shader::LastRustShaderError();
        LOG_ERROR("Rust shader compile failed for '%s': %s",
                  std::string(shader_name).c_str(),
                  error.c_str());
        return false;
    }
    g_shader_startup_metrics.compile_ms += MeasureElapsedMs(compile_started);

    codes = std::move(output.codes);
    shader_info->combos.insert(output.shader_info.combos.begin(), output.shader_info.combos.end());
    shader_info->svs.insert(output.shader_info.svs.begin(), output.shader_info.svs.end());
    shader_info->alias.insert(output.shader_info.alias.begin(), output.shader_info.alias.end());
    shader_info->defTexs.insert(
        shader_info->defTexs.end(), output.shader_info.defTexs.begin(), output.shader_info.defTexs.end());
    if (reflection_json != nullptr) {
        *reflection_json = std::move(output.reflection_json);
    }

    for (auto& unit : units) {
        if (unit.stage == ShaderType::VERTEX) {
            unit.preprocess_info = output.vertex_preprocessor_info;
        } else if (unit.stage == ShaderType::FRAGMENT) {
            unit.preprocess_info = output.fragment_preprocessor_info;
        }
    }

    return true;
}
