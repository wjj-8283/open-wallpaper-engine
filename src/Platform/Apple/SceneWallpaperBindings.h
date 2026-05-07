#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bindgen-safe renderer surface for wallpaper-core.
 *
 * wallpaper-core owns scene handles, reconciliation, windows, and mutable
 * runtime state. These functions only expose transparent operations on
 * wallpaper::SceneWallpaper and audio response helpers. Do not add a registry,
 * display map, runtime manager, or descriptor mirror here.
 */

/* Opaque wallpaper::SceneWallpaper owner. */
typedef struct owe_scene_wallpaper owe_scene_wallpaper;

/* Renderer lifetime. */
int owe_scene_wallpaper_new(owe_scene_wallpaper** out_scene);
int owe_scene_wallpaper_delete(owe_scene_wallpaper* scene);
int owe_scene_wallpaper_init(owe_scene_wallpaper* scene);
int owe_scene_wallpaper_shutdown(owe_scene_wallpaper* scene);

/*
 * Initializes the Vulkan renderer with a CAMetalLayer-backed surface.
 *
 * Render width/height may be 0/0 to use the output dimensions. A single zero is
 * invalid and rejected by the implementation.
 */
int owe_scene_wallpaper_init_metal_vulkan(
    owe_scene_wallpaper* scene,
    void* metal_layer,
    uint32_t width,
    uint32_t height,
    uint32_t render_width,
    uint32_t render_height,
    double display_scale_factor);

/* Applies SceneWallpaperConfig fields without persisting a duplicate config. */
int owe_scene_wallpaper_apply_config(
    owe_scene_wallpaper* scene,
    const char* source,
    const char* assets,
    const char* cache_path,
    uint32_t fps,
    bool paused);

/* Direct property forwarding to SceneWallpaper::setProperty*. */
int owe_scene_wallpaper_set_property_bool(
    owe_scene_wallpaper* scene,
    const char* name,
    bool value);
int owe_scene_wallpaper_set_property_int32(
    owe_scene_wallpaper* scene,
    const char* name,
    int32_t value);
int owe_scene_wallpaper_set_property_float(
    owe_scene_wallpaper* scene,
    const char* name,
    float value);
int owe_scene_wallpaper_set_property_string(
    owe_scene_wallpaper* scene,
    const char* name,
    const char* value);

/*
 * Upstream property-name accessors. Rust uses these instead of duplicating
 * string literals that must match SceneWallpaper.hpp.
 */
const char* owe_property_scaling_mode(void);
const char* owe_property_scaling_factor(void);
const char* owe_property_audio_response_enabled(void);
const char* owe_property_force_shader_refresh(void);
const char* owe_property_project_property_override_json(void);
const char* owe_property_project_property_reset(void);

/* Audio-response sample submission shared by all renderer scenes. */
int owe_audio_submit_frames(
    uint32_t sample_rate,
    uint32_t frame_count,
    const float* pcm_frames);

/* Thread-local error text for the last non-zero-returning call on this thread. */
const char* owe_last_error(void);

#ifdef __cplusplus
}
#endif
