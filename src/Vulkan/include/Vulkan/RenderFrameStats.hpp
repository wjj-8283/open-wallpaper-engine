#pragma once

#include <cstdint>

namespace wallpaper
{
namespace vulkan
{

struct RenderFrameStats {
    uint64_t frames { 0 };
    uint64_t pass_count { 0 };
    uint64_t custom_draw_count { 0 };
    uint64_t render_pass_begin_count { 0 };
    uint64_t render_pass_end_count { 0 };
    uint64_t clear_only_pass_count { 0 };
    uint64_t skipped_noop_pass_count { 0 };
    uint64_t dynamic_upload_bytes { 0 };
    uint64_t framebuffer_creations { 0 };
    uint64_t texture_creations { 0 };
    uint64_t video_imports { 0 };

    void reset() { *this = {}; }

    void accumulate(const RenderFrameStats& other) {
        frames += other.frames;
        pass_count += other.pass_count;
        custom_draw_count += other.custom_draw_count;
        render_pass_begin_count += other.render_pass_begin_count;
        render_pass_end_count += other.render_pass_end_count;
        clear_only_pass_count += other.clear_only_pass_count;
        skipped_noop_pass_count += other.skipped_noop_pass_count;
        dynamic_upload_bytes += other.dynamic_upload_bytes;
        framebuffer_creations += other.framebuffer_creations;
        texture_creations += other.texture_creations;
        video_imports += other.video_imports;
    }
};

} // namespace vulkan
} // namespace wallpaper
