#pragma once

#ifndef VK_NO_PROTOTYPES
#    define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <algorithm>
#include <span>
#include <vector>

namespace wallpaper
{
namespace vulkan
{

struct CustomPassRenderInfo {
    VkImage            image {};
    VkImageView        view {};
    VkExtent3D         extent {};
    VkImageLayout      final_layout { VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkAttachmentLoadOp load_op { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
    VkRenderPass       render_pass {};
    VkFramebuffer      framebuffer {};
    VkClearValue       clear_value {};
};

struct CustomPassBatchCandidate {
    bool                 batchable { false };
    bool                 visible { false };
    bool                 clear_only { false };
    CustomPassRenderInfo render {};
};

enum class CustomPassBatchKind
{
    RenderPass,
    ClearImage,
};

struct CustomPassBatchEntry {
    CustomPassBatchKind  kind { CustomPassBatchKind::RenderPass };
    size_t               first { 0 };
    size_t               last { 0 };
    uint32_t             visible_draws { 0 };
    bool                 clear_on_begin { false };
    CustomPassRenderInfo render {};
};

struct CustomPassBatchPlan {
    std::vector<CustomPassBatchEntry> entries;
};

inline bool CompatibleCustomPassAttachments(const CustomPassRenderInfo& a,
                                            const CustomPassRenderInfo& b) {
    return a.image == b.image && a.view == b.view && a.extent.width == b.extent.width &&
           a.extent.height == b.extent.height && a.extent.depth == b.extent.depth &&
           a.final_layout == b.final_layout;
}

inline CustomPassBatchPlan
PlanCustomPassBatches(std::span<const CustomPassBatchCandidate> candidates) {
    CustomPassBatchPlan plan;

    for (size_t i = 0; i < candidates.size();) {
        const auto& first = candidates[i];
        if (! first.batchable) {
            ++i;
            continue;
        }

        if (! first.visible && ! first.clear_only) {
            ++i;
            continue;
        }

        CustomPassBatchEntry entry {};
        entry.first  = i;
        entry.last   = i + 1;
        entry.render = first.render;
        entry.clear_on_begin =
            first.clear_only || first.render.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR;
        entry.visible_draws = first.visible ? 1u : 0u;

        if (! first.visible && first.clear_only) {
            entry.kind = CustomPassBatchKind::ClearImage;
        }

        for (size_t j = i + 1; j < candidates.size(); ++j) {
            const auto& candidate = candidates[j];
            if (! candidate.batchable ||
                ! CompatibleCustomPassAttachments(entry.render, candidate.render)) {
                break;
            }

            if (entry.visible_draws > 0 && candidate.clear_only && ! candidate.visible) {
                break;
            }

            if (entry.visible_draws > 0 && candidate.visible &&
                candidate.render.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                break;
            }

            entry.last = j + 1;
            if (candidate.visible) {
                entry.kind = CustomPassBatchKind::RenderPass;
                ++entry.visible_draws;
            }
            if (candidate.clear_only || candidate.render.load_op == VK_ATTACHMENT_LOAD_OP_CLEAR) {
                entry.clear_on_begin = true;
                if (entry.visible_draws == 0) {
                    entry.render = candidate.render;
                }
            }
        }

        if (entry.visible_draws > 0) {
            entry.kind = CustomPassBatchKind::RenderPass;
        } else if (! first.clear_only) {
            ++i;
            continue;
        }

        plan.entries.push_back(entry);
        i = std::max(entry.last, i + 1);
    }

    return plan;
}

} // namespace vulkan
} // namespace wallpaper
