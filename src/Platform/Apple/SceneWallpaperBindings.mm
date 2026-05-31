#include "Platform/Apple/SceneWallpaperBindings.h"

#include "Audio/AudioResponseService.h"
#include "SceneWallpaper.hpp"
#include "SceneWallpaperSurface.hpp"
#include "Utils/Logging.h"

#import <QuartzCore/CAMetalLayer.h>

#include <vulkan/vulkan.h>
#include <vulkan/vulkan_metal.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace
{
thread_local std::string g_last_error;

void clear_last_error()
{
    g_last_error.clear();
}

int finish_with_error(std::string error)
{
    g_last_error = std::move(error);
    return 1;
}

bool valid_scene(owe_scene_wallpaper* scene)
{
    return scene != nullptr;
}

std::string_view view_or_empty(const char* value)
{
    return value == nullptr ? std::string_view {} : std::string_view { value };
}

bool validate_dimensions(uint32_t width, uint32_t height, std::string* error)
{
    if (width == 0 || height == 0) {
        *error = "render dimensions must be non-zero";
        return false;
    }
    if (width > std::numeric_limits<uint16_t>::max() ||
        height > std::numeric_limits<uint16_t>::max()) {
        *error = "render dimensions exceed SceneWallpaper limits";
        return false;
    }
    return true;
}

bool validate_render_resolution(uint32_t width, uint32_t height, std::string* error)
{
    if (width == 0 && height == 0) return true;
    if (width == 0 || height == 0) {
        *error = "render resolution must provide both width and height";
        return false;
    }
    if (width > std::numeric_limits<uint16_t>::max() ||
        height > std::numeric_limits<uint16_t>::max()) {
        *error = "render resolution exceeds SceneWallpaper limits";
        return false;
    }
    return true;
}

wallpaper::RenderInitInfo make_render_init_info(
    void* metal_layer_handle,
    uint32_t width,
    uint32_t height,
    uint32_t render_width,
    uint32_t render_height,
    double display_scale_factor)
{
    // RenderInitInfo owns C++ callbacks and vectors, which bindgen cannot model
    // safely. Keep that construction here and pass only primitive inputs from
    // wallpaper-core.
    wallpaper::RenderInitInfo info;
    info.offscreen = false;
    info.width = static_cast<uint16_t>(width);
    info.height = static_cast<uint16_t>(height);
    info.render_width = static_cast<uint16_t>(render_width == 0 ? width : render_width);
    info.render_height = static_cast<uint16_t>(render_height == 0 ? height : render_height);
    info.display_scale_factor =
        display_scale_factor > 0.0 && std::isfinite(display_scale_factor)
            ? display_scale_factor
            : 1.0;
    info.surface_info.instanceExts = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_EXT_METAL_SURFACE_EXTENSION_NAME,
    };
    info.surface_info.createSurfaceOp =
        [metal_layer_handle](VkInstance instance, VkSurfaceKHR* surface) {
            auto* create_surface = reinterpret_cast<PFN_vkCreateMetalSurfaceEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateMetalSurfaceEXT"));
            if (create_surface == nullptr) return VK_ERROR_EXTENSION_NOT_PRESENT;

            auto* metal_layer = (__bridge CAMetalLayer*)metal_layer_handle;
            const VkMetalSurfaceCreateInfoEXT create_info {
                .sType = VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT,
                .pNext = nullptr,
                .flags = 0,
                .pLayer = metal_layer,
            };
            return create_surface(instance, &create_info, nullptr, surface);
        };
    return info;
}

const char* property_name(std::string_view value)
{
    // std::string_view constants in SceneWallpaper.hpp need stable C pointers
    // for Rust. The thread-local buffer is valid until the next property-name
    // request on the same thread, which is longer than each setter call needs.
    thread_local std::string name;
    name.assign(value);
    return name.c_str();
}
} // namespace

struct owe_scene_wallpaper {
    wallpaper::SceneWallpaper scene;
};

extern "C" void owe_set_log_callback(owe_log_callback callback)
{
    SetWallpaperLogCallback(callback);
}

extern "C" int owe_scene_wallpaper_new(owe_scene_wallpaper** out_scene)
{
    clear_last_error();
    if (out_scene == nullptr) return finish_with_error("out_scene must not be null");

    *out_scene = new owe_scene_wallpaper();
    return 0;
}

extern "C" int owe_scene_wallpaper_delete(owe_scene_wallpaper* scene)
{
    clear_last_error();
    delete scene;
    return 0;
}

extern "C" int owe_scene_wallpaper_init(owe_scene_wallpaper* scene)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (!scene->scene.init()) return finish_with_error("failed to initialize SceneWallpaper");
    return 0;
}

extern "C" int owe_scene_wallpaper_shutdown(owe_scene_wallpaper* scene)
{
    clear_last_error();
    if (scene == nullptr) return 0;

    scene->scene.shutdown();
    return 0;
}

extern "C" int owe_scene_wallpaper_init_metal_vulkan(
    owe_scene_wallpaper* scene,
    void* metal_layer,
    uint32_t width,
    uint32_t height,
    uint32_t render_width,
    uint32_t render_height,
    double display_scale_factor)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (metal_layer == nullptr) return finish_with_error("metal_layer must not be null");

    std::string error;
    if (!validate_dimensions(width, height, &error)) return finish_with_error(error);
    if (!validate_render_resolution(render_width, render_height, &error)) {
        return finish_with_error(error);
    }

    scene->scene.initVulkan(make_render_init_info(
        metal_layer,
        width,
        height,
        render_width,
        render_height,
        display_scale_factor));
    return 0;
}

extern "C" int owe_scene_wallpaper_begin_surface_reconfigure(owe_scene_wallpaper* scene)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");

    if (!scene->scene.beginSurfaceReconfigure()) {
        return finish_with_error("surface reconfigure begin failed");
    }
    return 0;
}

extern "C" int owe_scene_wallpaper_finish_surface_reconfigure(
    owe_scene_wallpaper* scene,
    void* metal_layer,
    uint32_t width,
    uint32_t height,
    uint32_t render_width,
    uint32_t render_height,
    double display_scale_factor)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (metal_layer == nullptr) return finish_with_error("metal_layer must not be null");

    std::string error;
    if (!validate_dimensions(width, height, &error)) return finish_with_error(error);
    if (!validate_render_resolution(render_width, render_height, &error)) {
        return finish_with_error(error);
    }

    if (!scene->scene.finishSurfaceReconfigure(make_render_init_info(
            metal_layer,
            width,
            height,
            render_width,
            render_height,
            display_scale_factor)))
    {
        return finish_with_error("surface reconfigure finish failed");
    }
    return 0;
}

extern "C" int owe_scene_wallpaper_apply_config(
    owe_scene_wallpaper* scene,
    const char* source,
    const char* assets,
    const char* cache_path,
    uint32_t fps,
    bool paused,
    bool force_shader_refresh,
    const char* project_property_override_json)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (source == nullptr || source[0] == '\0') {
        return finish_with_error("source must not be empty");
    }
    if (fps == 0) return finish_with_error("fps must be greater than zero");

    wallpaper::SceneWallpaperConfig config;
    config.source = std::string(view_or_empty(source));
    config.assets = std::string(view_or_empty(assets));
    config.cache_path = std::string(view_or_empty(cache_path));
    config.fps = fps;
    config.paused = paused;
    config.force_shader_refresh = force_shader_refresh;
    config.has_project_property_override = project_property_override_json != nullptr;
    config.project_property_override_json =
        std::string(view_or_empty(project_property_override_json));
    scene->scene.applyConfig(config);
    return 0;
}

extern "C" int owe_scene_wallpaper_set_target_fps(owe_scene_wallpaper* scene, uint32_t fps)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (fps == 0) return finish_with_error("fps must be greater than zero");

    scene->scene.setTargetFps(fps);
    return 0;
}

extern "C" int owe_scene_wallpaper_set_paused(owe_scene_wallpaper* scene, bool paused)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");

    scene->scene.setPaused(paused);
    return 0;
}

extern "C" int owe_scene_wallpaper_mouse_input(owe_scene_wallpaper* scene, double x, double y)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (!std::isfinite(x) || !std::isfinite(y)) return finish_with_error("mouse coordinates must be finite");

    scene->scene.mouseInput(x, y);
    return 0;
}

extern "C" int owe_scene_wallpaper_mouse_button(owe_scene_wallpaper* scene, int button, bool pressed)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (button < 0 || button > 31) return finish_with_error("mouse button must be in range 0..31");

    scene->scene.mouseButton(button, pressed);
    return 0;
}

extern "C" int owe_scene_wallpaper_mouse_enter(owe_scene_wallpaper* scene, bool entered)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");

    scene->scene.mouseEnter(entered);
    return 0;
}

extern "C" int owe_scene_wallpaper_set_property_bool(
    owe_scene_wallpaper* scene,
    const char* name,
    bool value)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (name == nullptr || name[0] == '\0') return finish_with_error("property name is empty");

    scene->scene.setPropertyBool(name, value);
    return 0;
}

extern "C" int owe_scene_wallpaper_set_property_int32(
    owe_scene_wallpaper* scene,
    const char* name,
    int32_t value)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (name == nullptr || name[0] == '\0') return finish_with_error("property name is empty");

    scene->scene.setPropertyInt32(name, value);
    return 0;
}

extern "C" int owe_scene_wallpaper_set_property_float(
    owe_scene_wallpaper* scene,
    const char* name,
    float value)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (name == nullptr || name[0] == '\0') return finish_with_error("property name is empty");

    scene->scene.setPropertyFloat(name, value);
    return 0;
}

extern "C" int owe_scene_wallpaper_set_property_string(
    owe_scene_wallpaper* scene,
    const char* name,
    const char* value)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (name == nullptr || name[0] == '\0') return finish_with_error("property name is empty");

    scene->scene.setPropertyString(name, std::string(view_or_empty(value)));
    return 0;
}

extern "C" int owe_scene_wallpaper_set_audio_volume(owe_scene_wallpaper* scene, float volume)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (!std::isfinite(volume) || volume < 0.0f || volume > 1.0f) {
        return finish_with_error("audio volume must be finite and between 0.0 and 1.0");
    }

    scene->scene.setPropertyFloat(wallpaper::PROPERTY_VOLUME, volume);
    return 0;
}

extern "C" int owe_scene_wallpaper_set_audio_muted(owe_scene_wallpaper* scene, bool muted)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");

    scene->scene.setPropertyBool(wallpaper::PROPERTY_MUTED, muted);
    return 0;
}

extern "C" int owe_scene_wallpaper_submit_media_event_json(
    owe_scene_wallpaper* scene,
    const char* event_json)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (event_json == nullptr || event_json[0] == '\0') {
        return finish_with_error("media event json must not be empty");
    }

    scene->scene.setPropertyString(
        wallpaper::PROPERTY_MEDIA_EVENT_JSON,
        std::string(event_json));
    return 0;
}

extern "C" int owe_scene_wallpaper_apply_system_media_artwork(
    owe_scene_wallpaper* scene,
    uint32_t width,
    uint32_t height,
    const uint8_t* rgba,
    uintptr_t rgba_len)
{
    clear_last_error();
    if (!valid_scene(scene)) return finish_with_error("scene must not be null");
    if (width == 0 || height == 0) {
        return finish_with_error("media artwork dimensions must be non-zero");
    }
    if (rgba == nullptr) return finish_with_error("media artwork rgba must not be null");
    const std::size_t expected_len = static_cast<std::size_t>(width) * height * 4;
    if (rgba_len != expected_len) {
        return finish_with_error("media artwork rgba length must equal width * height * 4");
    }

    scene->scene.applySystemMediaArtwork(
        width,
        height,
        rgba,
        static_cast<std::size_t>(rgba_len));
    return 0;
}

extern "C" const char* owe_property_scaling_mode(void)
{
    return property_name(wallpaper::PROPERTY_SCALINGMODE);
}

extern "C" const char* owe_property_scaling_factor(void)
{
    return property_name(wallpaper::PROPERTY_SCALINGFACTOR);
}

extern "C" const char* owe_property_audio_response_enabled(void)
{
    return property_name(wallpaper::PROPERTY_AUDIO_RESPONSE_ENABLED);
}

extern "C" const char* owe_property_media_integration_enabled(void)
{
    return property_name(wallpaper::PROPERTY_MEDIA_INTEGRATION_ENABLED);
}

extern "C" const char* owe_property_force_shader_refresh(void)
{
    return property_name(wallpaper::PROPERTY_FORCE_SHADER_REFRESH);
}

extern "C" const char* owe_property_project_property_override_json(void)
{
    return property_name(wallpaper::PROPERTY_PROJECT_PROPERTY_OVERRIDE_JSON);
}

extern "C" const char* owe_property_project_property_reset(void)
{
    return property_name(wallpaper::PROPERTY_PROJECT_PROPERTY_RESET);
}

extern "C" int owe_audio_submit_mono_frames(
    uint32_t sample_rate,
    uint32_t frame_count,
    const float* pcm_frames)
{
    clear_last_error();
    std::string error;
    if (!wallpaper::audio::SubmitMonoAudioFrames(sample_rate, frame_count, pcm_frames, &error)) {
        return finish_with_error(error);
    }
    return 0;
}

extern "C" int owe_audio_submit_frames(
    uint32_t sample_rate,
    uint32_t frame_count,
    const float* pcm_frames)
{
    clear_last_error();
    std::string error;
    if (!wallpaper::audio::SubmitAudioFrames(sample_rate, frame_count, pcm_frames, &error)) {
        return finish_with_error(error);
    }
    return 0;
}

extern "C" int owe_audio_current_spectrum_128(
    float* out_bins,
    uintptr_t out_len,
    uint64_t* out_generation)
{
    clear_last_error();
    if (out_bins == nullptr) {
        return finish_with_error("out_bins must not be null");
    }
    if (out_len < 128u) {
        return finish_with_error("out_len must be at least 128");
    }

    const auto snapshot = wallpaper::audio::CurrentAudioSpectrumSnapshot();
    for (size_t i = 0; i < 64u; ++i) {
        out_bins[i]       = snapshot.left64[i];
        out_bins[i + 64u] = snapshot.right64[i];
    }
    if (out_generation != nullptr) {
        *out_generation = snapshot.generation;
    }
    return 0;
}

extern "C" const char* owe_last_error(void)
{
    return g_last_error.c_str();
}
