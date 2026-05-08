#include "VulkanRender/PassBatch.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace
{
using wallpaper::vulkan::CustomPassBatchCandidate;
using wallpaper::vulkan::CustomPassBatchKind;
using wallpaper::vulkan::CustomPassBatchPlan;
using wallpaper::vulkan::PlanCustomPassBatches;

CustomPassBatchCandidate makeCandidate(uintptr_t image, uintptr_t view, uint32_t width,
                                       bool visible = true) {
    CustomPassBatchCandidate candidate {};
    candidate.batchable           = true;
    candidate.visible             = visible;
    candidate.render.image        = reinterpret_cast<VkImage>(image);
    candidate.render.view         = reinterpret_cast<VkImageView>(view);
    candidate.render.extent       = { width, 64, 1 };
    candidate.render.final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    candidate.render.load_op      = VK_ATTACHMENT_LOAD_OP_LOAD;
    candidate.render.render_pass  = reinterpret_cast<VkRenderPass>(0x11);
    candidate.render.framebuffer  = reinterpret_cast<VkFramebuffer>(0x21);
    return candidate;
}

void adjacentVisibleCustomPassesShareOneBatch() {
    std::vector<CustomPassBatchCandidate> candidates {
        makeCandidate(0x100, 0x200, 64),
        makeCandidate(0x100, 0x200, 64),
        makeCandidate(0x100, 0x200, 64),
    };

    const CustomPassBatchPlan plan = PlanCustomPassBatches(candidates);

    assert(plan.entries.size() == 1);
    assert(plan.entries[0].kind == CustomPassBatchKind::RenderPass);
    assert(plan.entries[0].first == 0);
    assert(plan.entries[0].last == 3);
    assert(plan.entries[0].visible_draws == 3);
}

void nonCustomBoundarySplitsBatches() {
    std::vector<CustomPassBatchCandidate> candidates {
        makeCandidate(0x100, 0x200, 64),
        CustomPassBatchCandidate { .batchable = false },
        makeCandidate(0x100, 0x200, 64),
    };

    const CustomPassBatchPlan plan = PlanCustomPassBatches(candidates);

    assert(plan.entries.size() == 2);
    assert(plan.entries[0].first == 0);
    assert(plan.entries[0].last == 1);
    assert(plan.entries[1].first == 2);
    assert(plan.entries[1].last == 3);
}

void differentOutputsDoNotBatch() {
    std::vector<CustomPassBatchCandidate> candidates {
        makeCandidate(0x100, 0x200, 64),
        makeCandidate(0x101, 0x201, 64),
    };

    const CustomPassBatchPlan plan = PlanCustomPassBatches(candidates);

    assert(plan.entries.size() == 2);
    assert(plan.entries[0].visible_draws == 1);
    assert(plan.entries[1].visible_draws == 1);
}

void invisibleClearOnlyPassFoldsIntoNextVisiblePass() {
    auto clear           = makeCandidate(0x100, 0x200, 64, false);
    clear.clear_only     = true;
    clear.render.load_op = VK_ATTACHMENT_LOAD_OP_CLEAR;

    std::vector<CustomPassBatchCandidate> candidates {
        clear,
        makeCandidate(0x100, 0x200, 64),
    };

    const CustomPassBatchPlan plan = PlanCustomPassBatches(candidates);

    assert(plan.entries.size() == 1);
    assert(plan.entries[0].kind == CustomPassBatchKind::RenderPass);
    assert(plan.entries[0].first == 0);
    assert(plan.entries[0].last == 2);
    assert(plan.entries[0].visible_draws == 1);
    assert(plan.entries[0].clear_on_begin);
}
} // namespace

int main() {
    adjacentVisibleCustomPassesShareOneBatch();
    nonCustomBoundarySplitsBatches();
    differentOutputsDoNotBatch();
    invisibleClearOnlyPassFoldsIntoNextVisiblePass();
    return 0;
}
