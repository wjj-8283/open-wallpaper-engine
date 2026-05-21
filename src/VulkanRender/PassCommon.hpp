#pragma once
#include "Vulkan/Instance.hpp"
#include "Vulkan/SampleCount.hpp"
#include "Type.hpp"
#include "Vulkan/TextureCache.hpp"
#include "Scene/SceneRenderTarget.h"

namespace wallpaper
{
namespace vulkan
{
inline void SetBlend(BlendMode bm, VkPipelineColorBlendAttachmentState& state) {
    state.blendEnable  = true;
    state.colorBlendOp = VK_BLEND_OP_ADD;
    state.alphaBlendOp = VK_BLEND_OP_ADD;
    switch (bm) {
    case BlendMode::Disable: state.blendEnable = false; break;
    case BlendMode::Normal:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case BlendMode::AlphaToCoverage:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Translucent:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    case BlendMode::Additive:
        state.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        state.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        state.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    }
}
inline VkAttachmentLoadOp ResolveAttachmentLoadOp(bool preserve_target_contents,
                                                  bool clear_on_first_use) {
    if (clear_on_first_use) return VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (preserve_target_contents) return VK_ATTACHMENT_LOAD_OP_LOAD;
    return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
}

inline VkClearValue ResolveAttachmentClearValue(bool scene_output,
                                                const std::array<float, 3>& clear_color) {
    if (scene_output) {
        return VkClearValue { .color = { clear_color[0], clear_color[1], clear_color[2], 1.0f } };
    }
    return VkClearValue { .color = { 0.0f, 0.0f, 0.0f, 0.0f } };
}

inline VkSampleCountFlagBits
ResolveCustomPassRenderTargetSampleCount(std::uint32_t requested_sample_count,
                                         VkSampleCountFlags supported_color_samples) {
    return ResolveSampleCount(requested_sample_count, supported_color_samples);
}

inline void SetAttachmentLoadOp(BlendMode bm, VkAttachmentLoadOp& load_op) {
    switch (bm) {
    case BlendMode::Disable:
    case BlendMode::Normal:
    case BlendMode::AlphaToCoverage: load_op = VK_ATTACHMENT_LOAD_OP_DONT_CARE; break;
    case BlendMode::Additive:
    case BlendMode::Translucent: load_op = VK_ATTACHMENT_LOAD_OP_LOAD; break;
    }
}

inline TextureKey ToTexKey(wallpaper::SceneRenderTarget rt) {
    return TextureKey {
        .width        = rt.width,
        .height       = rt.height,
        .usage        = {},
        .format       = wallpaper::TextureFormat::RGBA8,
        .sample       = rt.sample,
        .mipmap_level = rt.mipmap_level,
        .sample_count = SampleCountFromValue(rt.sample_count),
    };
}

inline TextureKey ToTexKeyMsaa(wallpaper::SceneRenderTarget rt,
                               VkSampleCountFlagBits        sample_count) {
    auto key          = ToTexKey(rt);
    key.usage         = TexUsage::MSAA_COLOR;
    key.mipmap_level  = 1;
    key.sample_count  = sample_count;
    return key;
}
} // namespace vulkan
} // namespace wallpaper
