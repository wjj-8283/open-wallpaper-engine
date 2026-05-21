#include "Vulkan/TextureCache.hpp"

#include <gtest/gtest.h>

namespace wallpaper
{
namespace
{

TEST(VideoTextureSubmissionSmoke, MergesGlobalAndLayerVideoPlaybackState) {
    video::VideoPlaybackState global_state {
        .paused                = true,
        .rate                  = 2.0f,
        .scene_elapsed_seconds = 100.0,
    };
    video::VideoPlaybackState layer_state {
        .paused                = false,
        .rate                  = 0.25f,
        .scene_elapsed_seconds = 4.5,
    };

    const auto merged =
        vulkan::ResolveEffectiveVideoPlaybackState(global_state, layer_state);

    EXPECT_TRUE(merged.paused);
    EXPECT_FLOAT_EQ(merged.rate, 0.5f);
    EXPECT_DOUBLE_EQ(merged.scene_elapsed_seconds, 4.5);
}

TEST(VideoTextureSubmissionSmoke, ClampsNegativeVideoPlaybackRates) {
    video::VideoPlaybackState global_state {
        .paused = false,
        .rate   = -2.0f,
    };
    video::VideoPlaybackState layer_state {
        .paused = true,
        .rate   = 3.0f,
    };

    const auto merged =
        vulkan::ResolveEffectiveVideoPlaybackState(global_state, layer_state);

    EXPECT_TRUE(merged.paused);
    EXPECT_FLOAT_EQ(merged.rate, 0.0f);
}

TEST(VideoTextureSubmissionSmoke, ImportStatsDefaultToZero) {
    vulkan::VideoTextureSubmissionStats stats;

    EXPECT_EQ(stats.update_calls, 0u);
    EXPECT_EQ(stats.cache_hits, 0u);
    EXPECT_EQ(stats.new_imports, 0u);
    EXPECT_EQ(stats.fence_waits, 0u);
    EXPECT_EQ(stats.evictions, 0u);
    EXPECT_EQ(stats.command_buffer_allocations, 0u);
    EXPECT_EQ(stats.fence_allocations, 0u);
}

} // namespace
} // namespace wallpaper
