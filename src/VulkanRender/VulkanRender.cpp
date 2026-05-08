#include "VulkanRender.hpp"

#include "Utils/Logging.h"
#include "RenderGraph/RenderGraph.hpp"
#include "Scene/Scene.h"
#include "Interface/IShaderValueUpdater.h"

#include "Utils/Algorism.h"

#include <glslang/Public/ShaderLang.h>

#include "Vulkan/Device.hpp"
#include "Vulkan/TextureCache.hpp"
#include "Vulkan/Swapchain.hpp"
#include "Vulkan/VulkanExSwapchain.hpp"

#include "VulkanPass.hpp"
#include "CustomShaderPass.hpp"
#include "PrePass.hpp"
#include "FinPass.hpp"
#include "Resource.hpp"
#include "SpecTexs.hpp"

#include "Core/ArrayHelper.hpp"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#include <unistd.h>
#include <vector>

#if ENABLE_RENDERDOC_API
#    include "RenderDoc.h"
#endif

using namespace wallpaper::vulkan;

constexpr uint64_t vk_wait_time { 10u * 1000u * 1000000u };
constexpr uint32_t vk_command_num { 2 };

constexpr std::array base_inst_exts {
    Extension { false, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME },
};
std::vector<Extension> BaseDeviceExtensions() {
    std::vector<Extension> extensions {
        Extension { false, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME },
        Extension { true, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME },
    };
#if defined(__APPLE__)
    extensions.push_back({ true, "VK_EXT_metal_objects" });
#endif
#if defined(__linux__)
    extensions.push_back({ true, VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME });
    extensions.push_back({ true, VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME });
    extensions.push_back({ true, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME });
    extensions.push_back({ true, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME });
#endif
    return extensions;
}

namespace
{
const char* WallpaperScalingModeName(wallpaper::WallpaperScalingMode mode) {
    switch (mode) {
    case wallpaper::WallpaperScalingMode::NONE: return "none";
    case wallpaper::WallpaperScalingMode::STRETCH: return "stretch";
    case wallpaper::WallpaperScalingMode::FIT: return "fit";
    case wallpaper::WallpaperScalingMode::FILL: return "fill";
    }

    return "unknown";
}

double NormalizeScaleFactor(double scale_factor) {
    if (! std::isfinite(scale_factor) || scale_factor <= 0.0) return 1.0;
    return scale_factor;
}

VkExtent2D ResolveSceneSourceExtent(const wallpaper::Scene& scene,
                                    const VkExtent2D&       fallback_extent) {
    auto spec_rt = scene.renderTargets.find(std::string(wallpaper::SpecTex_Default));
    if (spec_rt != scene.renderTargets.end()) {
        const auto width  = static_cast<uint32_t>(std::max(1, spec_rt->second.width));
        const auto height = static_cast<uint32_t>(std::max(1, spec_rt->second.height));
        if (width > 1 && height > 1) return { width, height };
    }

    const auto ortho_width  = static_cast<uint32_t>(std::max(0, scene.ortho[0]));
    const auto ortho_height = static_cast<uint32_t>(std::max(0, scene.ortho[1]));
    if (ortho_width > 2 && ortho_height > 2) return { ortho_width, ortho_height };

    return {
        std::max(1u, fallback_extent.width),
        std::max(1u, fallback_extent.height),
    };
}

bool RenderPerfLogEnabled() {
    const char* value = std::getenv("WE_RENDER_PERF_LOG");
    if (value == nullptr || value[0] == '\0') return false;
    const std::string_view view(value);
    return view != "0" && view != "false" && view != "FALSE";
}

} // namespace

struct VulkanRender::Impl {
    Impl()  = default;
    ~Impl() = default;

    bool init(RenderInitInfo);
    void destroy();

    void drawFrame(Scene&);
    void beginFrameStats();
    void finishFrameStats();

    bool CreateRenderingResource(RenderingResources&);
    void DestroyRenderingResource(RenderingResources&);

    void clearLastRenderGraph();
    void compileRenderGraph(Scene&, rg::RenderGraph&);
    void UpdateCameraFillMode(Scene&, wallpaper::FillMode);
    void SetWallpaperScalingMode(wallpaper::WallpaperScalingMode);
    void SetWallpaperScalingFactor(double);

    bool initRes();
    void executePreparedPasses(RenderingResources&);
    void drawFrameSwapchain();
    void drawFrameOffscreen();
    void setRenderTargetSize(Scene&, rg::RenderGraph&);
    void updateScalingLayout(const Scene&, uint32_t output_width, uint32_t output_height);

    Instance                m_instance;
    std::unique_ptr<Device> m_device;

    std::unique_ptr<PrePass> m_prepass { nullptr };
    std::unique_ptr<FinPass> m_finpass { nullptr };

    std::unique_ptr<FinPass> m_testpass { nullptr };
    ReDrawCB                 m_redraw_cb;

    std::unique_ptr<StagingBuffer> m_vertex_buf { nullptr };
    std::unique_ptr<StagingBuffer> m_dyn_buf { nullptr };

    vvk::CommandBuffers m_cmds;
    vvk::CommandBuffer  m_upload_cmd;
    vvk::CommandBuffer  m_render_cmd;

    bool m_with_surface { false };
    bool m_inited { false };
    bool m_pass_loaded { false };

    std::unique_ptr<VulkanExSwapchain>    m_ex_swapchain;
    RenderingResources                    m_rendering_resources;
    WallpaperScalingLayout                m_scaling_layout {};
    WallpaperScalingMode                  m_scaling_mode { WallpaperScalingMode::FIT };
    double                                m_scaling_factor { 1.0 };
    double                                m_display_scale_factor { 1.0 };
    VkExtent2D                            m_requested_render_extent {};
    bool                                  m_perf_log_enabled { false };
    RenderFrameStats                      m_frame_stats {};
    RenderFrameStats                      m_pending_frame_stats {};
    RenderFrameStats                      m_perf_stats_accum {};
    std::chrono::steady_clock::time_point m_last_perf_log {};

    // Exported dma_fence sync_file fd for the most recently completed
    // offscreen frame. Written by drawFrameOffscreen(), consumed by
    // takeLastFrameSyncFd() (called from the host's redraw callback).
    // -1 means no frame has been exported yet. Ownership: the taker.
    std::atomic<int> m_last_sync_fd { -1 };

    std::vector<VulkanPass*> m_passes;
};

VulkanRender::VulkanRender(): pImpl(std::make_unique<Impl>()) {}
VulkanRender::~VulkanRender() {};

bool VulkanRender::inited() const { return pImpl->m_inited; }

int VulkanRender::takeLastFrameSyncFd() {
    return pImpl->m_last_sync_fd.exchange(-1, std::memory_order_acq_rel);
}

bool VulkanRender::init(RenderInitInfo info) { return pImpl->init(info); }
void VulkanRender::destroy() { pImpl->destroy(); }
void VulkanRender::drawFrame(Scene& scene) { pImpl->drawFrame(scene); };
void VulkanRender::clearLastRenderGraph() { pImpl->clearLastRenderGraph(); };
void VulkanRender::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    pImpl->compileRenderGraph(scene, rg);
};
void VulkanRender::UpdateCameraFillMode(Scene& scene, wallpaper::FillMode fill) {
    pImpl->UpdateCameraFillMode(scene, fill);
};
void VulkanRender::SetWallpaperScalingMode(wallpaper::WallpaperScalingMode mode) {
    pImpl->SetWallpaperScalingMode(mode);
}
void VulkanRender::SetWallpaperScalingFactor(double factor) {
    pImpl->SetWallpaperScalingFactor(factor);
}
void VulkanRender::SetVideoPlaybackPaused(bool paused) {
    if (pImpl->m_device != nullptr) {
        pImpl->m_device->tex_cache().SetVideoPlaybackPaused(paused);
    }
}
void VulkanRender::SetVideoPlaybackRate(float rate) {
    if (pImpl->m_device != nullptr) {
        pImpl->m_device->tex_cache().SetVideoPlaybackRate(rate);
    }
}

wallpaper::ExSwapchain* VulkanRender::exSwapchain() const { return pImpl->m_ex_swapchain.get(); };

bool VulkanRender::Impl::init(RenderInitInfo info) {
    if (m_inited) return true;

    m_perf_log_enabled        = RenderPerfLogEnabled();
    m_redraw_cb               = info.redraw_callback;
    m_display_scale_factor    = NormalizeScaleFactor(info.display_scale_factor);
    m_requested_render_extent = { info.render_width, info.render_height };
    VkExtent2D extent { info.width, info.height };
    if (extent.width * extent.height < 500 * 500) {
        LOG_ERROR("too small swapchain image size: %dx%d", extent.width, extent.height);
    } else {
        LOG_INFO("set swapchain image size: %dx%d", extent.width, extent.height);
    }
    LOG_INFO("wallpaper render init: output_px=%ux%u requested_render_px=%ux%u display_scale=%.3f",
             extent.width,
             extent.height,
             m_requested_render_extent.width,
             m_requested_render_extent.height,
             m_display_scale_factor);

    std::vector<Extension> inst_exts { base_inst_exts.begin(), base_inst_exts.end() };
    std::vector<Extension> device_exts = BaseDeviceExtensions();

    if (! info.offscreen) {
        std::transform(info.surface_info.instanceExts.begin(),
                       info.surface_info.instanceExts.end(),
                       std::back_inserter(inst_exts),
                       [](const auto& s) {
                           return Extension { true, s.c_str() };
                       });
        device_exts.push_back({ true, VK_KHR_SWAPCHAIN_EXTENSION_NAME });
    } else {
#if defined(__linux__)
        // Iteration 1a: offscreen FDs are real Linux DMA-BUFs so they can be
        // imported by arbitrary external consumers. These extensions are
        // strictly required on the offscreen path; if a driver lacks them
        // we fail fast in Device::CheckGPU.
        device_exts.push_back({ true, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME });
        device_exts.push_back({ true, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME });
        device_exts.push_back({ true, VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME });
#else
        LOG_ERROR("offscreen external-memory rendering is only supported on Linux");
        return false;
#endif
    }

    std::vector<InstanceLayer> inst_layers;
    // valid layer
    if (info.enable_valid_layer) {
        inst_layers.push_back({ true, VALIDATION_LAYER_NAME });
        LOG_INFO("vulkan valid layer \"%s\" enabled", VALIDATION_LAYER_NAME.data());
    }

    if (! Instance::Create(m_instance, inst_exts, inst_layers)) {
        LOG_ERROR("init vulkan failed");
        return false;
    }
    if (! info.offscreen) {
        VkSurfaceKHR surface;
        VVK_CHECK_ACT(
            {
                LOG_ERROR("create vulkan surface failed");
                return false;
            },
            info.surface_info.createSurfaceOp(*m_instance.inst(), &surface));
        m_instance.setSurface(VkSurfaceKHR(surface));
        m_with_surface = true;
    }
    {
        auto surface   = *m_instance.surface();
        auto check_gpu = [&device_exts, surface](const vvk::PhysicalDevice& gpu) {
            return Device::CheckGPU(gpu, device_exts, surface);
        };
        if (! m_instance.ChoosePhysicalDevice(check_gpu, info.uuid)) return false;
    }

    {
        m_device = std::make_unique<Device>();
        if (! Device::Create(m_instance, device_exts, extent, *m_device)) {
            LOG_ERROR("init vulkan device failed");
            return false;
        }
    }

    if (info.offscreen) {
        m_ex_swapchain = CreateExSwapchain(*m_device,
                                           extent.width,
                                           extent.height,
                                           (info.offscreen_tiling == TexTiling::OPTIMAL
                                                ? VK_IMAGE_TILING_OPTIMAL
                                                : VK_IMAGE_TILING_LINEAR));
        m_with_surface = false;
    }

    if (! initRes()) return false;
    ;

    m_inited = true;
    return m_inited;
}

bool VulkanRender::Impl::initRes() {
    m_prepass = std::make_unique<PrePass>(PrePass::Desc {});
    m_finpass = std::make_unique<FinPass>(FinPass::Desc {});
    if (m_with_surface) {
        m_finpass->setPresentFormat(m_device->swapchain().format());
        m_finpass->setPresentQueueIndex(m_device->present_queue().family_index);
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else {
        m_finpass->setPresentFormat(m_ex_swapchain->format());
        m_finpass->setPresentLayout(VK_IMAGE_LAYOUT_GENERAL);
        m_finpass->setPresentQueueIndex(VK_QUEUE_FAMILY_EXTERNAL);
    }
    /*
    m_testpass = std::make_unique<FinPass>(FinPass::Desc{});
    m_testpass->setPresentFormat(m_ex_swapchain->format());
    m_testpass->setPresentQueueIndex(m_device->graphics_queue().family_index);
    m_testpass->setPresentLayout(vk::ImageLayout::ePresentSrcKHR);
    */

    m_vertex_buf = std::make_unique<StagingBuffer>(*m_device,
                                                   2 * 1024 * 1024,
                                                   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    m_dyn_buf    = std::make_unique<StagingBuffer>(*m_device,
                                                2 * 1024 * 1024,
                                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    if (! m_vertex_buf->allocate()) return false;
    if (! m_dyn_buf->allocate()) return false;
    {
        auto& pool = m_device->cmd_pool();
        VVK_CHECK_BOOL_RE(pool.Allocate(vk_command_num, VK_COMMAND_BUFFER_LEVEL_PRIMARY, m_cmds));
        m_upload_cmd = vvk::CommandBuffer(m_cmds[0], m_device->handle().Dispatch());
        m_render_cmd = vvk::CommandBuffer(m_cmds[1], m_device->handle().Dispatch());
    }
    if (! CreateRenderingResource(m_rendering_resources)) return false;

#if ENABLE_RENDERDOC_API
    load_renderdoc_api();
#endif
    return true;
}

void VulkanRender::Impl::destroy() {
    if (! m_inited) return;
    if (m_device && m_device->handle()) {
        VVK_CHECK(m_device->handle().WaitIdle());

        // res
        for (auto& p : m_passes) {
            p->destory(*m_device, m_rendering_resources);
        }
        m_vertex_buf->destroy();
        m_dyn_buf->destroy();

        m_device->Destroy();
    }
    m_instance.Destroy();
}

bool VulkanRender::Impl::CreateRenderingResource(RenderingResources& rr) {
    rr.command     = m_render_cmd;
    rr.frame_stats = &m_pending_frame_stats;
    VVK_CHECK_BOOL_RE(m_device->handle().CreateFence(
        VkFenceCreateInfo {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        },
        rr.fence_frame));

    rr.fence_frame.Reset();

    if (m_with_surface) {
        VkSemaphoreCreateInfo ci { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                                   .pNext = nullptr };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_finish));
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_swap_wait_image));
    }

    // Exportable SYNC_FD semaphore used by the waywallen-renderer host
    // to ship a dma_fence sync_file to display clients on each
    // FrameReady event. Created in both offscreen and surface modes —
    // only the offscreen drawFrame path currently signals it, but
    // having it always present keeps the lifetime simple.
    {
        VkExportSemaphoreCreateInfo export_info {
            .sType       = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
            .pNext       = nullptr,
            .handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        VkSemaphoreCreateInfo ci {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &export_info,
            .flags = 0,
        };
        VVK_CHECK_BOOL_RE(m_device->handle().CreateSemaphore(ci, rr.sem_export));
    }

    rr.vertex_buf = m_vertex_buf.get();
    rr.dyn_buf    = m_dyn_buf.get();
    return true;
}

void VulkanRender::Impl::DestroyRenderingResource(RenderingResources& rr) {}

// VulkanExSwapchain* VulkanRender::exSwapchain() const { return m_ex_swapchain.get(); }

void VulkanRender::Impl::beginFrameStats() {
    m_frame_stats.reset();
    m_frame_stats.accumulate(m_pending_frame_stats);
    m_pending_frame_stats.reset();
    m_frame_stats.frames              = 1;
    m_rendering_resources.frame_stats = &m_frame_stats;
    if (m_device != nullptr) {
        m_device->tex_cache().SetFrameStats(&m_frame_stats);
    }
}

void VulkanRender::Impl::finishFrameStats() {
    if (m_device != nullptr) {
        m_device->tex_cache().SetFrameStats(nullptr);
    }
    m_rendering_resources.frame_stats = &m_pending_frame_stats;
    if (! m_perf_log_enabled) return;

    const auto now = std::chrono::steady_clock::now();
    m_perf_stats_accum.accumulate(m_frame_stats);
    if (m_last_perf_log.time_since_epoch().count() == 0) {
        m_last_perf_log = now;
        return;
    }

    const auto elapsed = std::chrono::duration<double>(now - m_last_perf_log).count();
    if (elapsed < 1.0) return;

    LOG_INFO("[render-perf] frames=%llu passes=%llu custom_draws=%llu render_passes=%llu/%llu "
             "clear_only=%llu skipped_noop=%llu dyn_upload=%lluB fb_create=%llu tex_create=%llu "
             "video_imports=%llu",
             static_cast<unsigned long long>(m_perf_stats_accum.frames),
             static_cast<unsigned long long>(m_perf_stats_accum.pass_count),
             static_cast<unsigned long long>(m_perf_stats_accum.custom_draw_count),
             static_cast<unsigned long long>(m_perf_stats_accum.render_pass_begin_count),
             static_cast<unsigned long long>(m_perf_stats_accum.render_pass_end_count),
             static_cast<unsigned long long>(m_perf_stats_accum.clear_only_pass_count),
             static_cast<unsigned long long>(m_perf_stats_accum.skipped_noop_pass_count),
             static_cast<unsigned long long>(m_perf_stats_accum.dynamic_upload_bytes),
             static_cast<unsigned long long>(m_perf_stats_accum.framebuffer_creations),
             static_cast<unsigned long long>(m_perf_stats_accum.texture_creations),
             static_cast<unsigned long long>(m_perf_stats_accum.video_imports));
    m_perf_stats_accum.reset();
    m_last_perf_log = now;
}

void VulkanRender::Impl::drawFrame(Scene& scene) {
    if (! (m_inited && m_pass_loaded)) return;
    beginFrameStats();

    const auto output_extent = m_device->out_extent();
    updateScalingLayout(
        scene, std::max(1u, output_extent.width), std::max(1u, output_extent.height));
    m_rendering_resources.wallpaper_viewport = MakeWallpaperViewport(m_scaling_layout);
    m_rendering_resources.wallpaper_scissor  = MakeWallpaperScissor(m_scaling_layout);

    // LOG_INFO("used ram: %fm", (m_device->GetUsage()/1024.0f)/1024.0f);

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->StartFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif

    if (m_instance.offscreen()) {
        drawFrameOffscreen();
    } else {
        drawFrameSwapchain();
    }

    if (m_redraw_cb) m_redraw_cb();

    finishFrameStats();

#if ENABLE_RENDERDOC_API
    if (rdoc_api)
        rdoc_api->EndFrameCapture(
            RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE((VkInstance)m_instance.inst()), NULL);
#endif
}

void VulkanRender::Impl::executePreparedPasses(RenderingResources& rr) {
    size_t i = 0;
    while (i < m_passes.size()) {
        auto* pass = m_passes[i];
        if (pass == nullptr || ! pass->prepared()) {
            ++i;
            continue;
        }

        auto* custom = dynamic_cast<CustomShaderPass*>(pass);
        if (custom == nullptr) {
            if (rr.frame_stats != nullptr) ++rr.frame_stats->pass_count;
            pass->execute(*m_device, rr);
            ++i;
            continue;
        }

        std::vector<CustomShaderPass*>        custom_passes;
        std::vector<CustomPassBatchCandidate> candidates;
        for (; i < m_passes.size(); ++i) {
            auto* run_pass = m_passes[i];
            if (run_pass == nullptr || ! run_pass->prepared()) break;
            auto* run_custom = dynamic_cast<CustomShaderPass*>(run_pass);
            if (run_custom == nullptr) break;

            if (rr.frame_stats != nullptr) ++rr.frame_stats->pass_count;
            custom_passes.push_back(run_custom);
            candidates.push_back(run_custom->preRecord(*m_device, rr));
        }

        const auto plan = PlanCustomPassBatches(candidates);
        for (const auto& entry : plan.entries) {
            const size_t first = entry.first;
            const size_t last  = entry.last;
            if (entry.kind == CustomPassBatchKind::ClearImage) {
                custom_passes[first]->recordClear(*m_device, rr);
                continue;
            }

            VkRenderPassBeginInfo pass_begin_info {
                .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                .pNext       = nullptr,
                .renderPass  = entry.render.render_pass,
                .framebuffer = entry.render.framebuffer,
                .renderArea =
                    VkRect2D {
                        .offset = { 0, 0 },
                        .extent = { entry.render.extent.width, entry.render.extent.height },
                    },
                .clearValueCount = 1,
                .pClearValues    = &entry.render.clear_value,
            };
            rr.command.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
            if (rr.frame_stats != nullptr) ++rr.frame_stats->render_pass_begin_count;

            for (size_t local = first; local < last; ++local) {
                if (! candidates[local].visible) continue;
                custom_passes[local]->recordDraw(*m_device, rr);
            }

            rr.command.EndRenderPass();
            if (rr.frame_stats != nullptr) ++rr.frame_stats->render_pass_end_count;
        }
    }
}

void VulkanRender::Impl::drawFrameSwapchain() {
    static size_t resource_index = 0;

    RenderingResources& rr = m_rendering_resources;
    resource_index         = (resource_index + 1) % 3;
    uint32_t image_index   = 0;
    {
        VVK_CHECK_VOID_RE(m_device->handle().AcquireNextImageKHR(*m_device->swapchain().handle(),
                                                                 vk_wait_time,
                                                                 *rr.sem_swap_wait_image,
                                                                 {},
                                                                 &image_index));
    }
    const auto& image = m_device->swapchain().images()[image_index];

    m_finpass->setPresent(image);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_dyn_buf->recordUpload(rr.command, rr.frame_stats);
    executePreparedPasses(rr);
    (void)rr.command.End();

    VkPipelineStageFlags wait_dst_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo         sub_info {
                .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext                = nullptr,
                .waitSemaphoreCount   = 1,
                .pWaitSemaphores      = rr.sem_swap_wait_image.address(),
                .pWaitDstStageMask    = &wait_dst_stage,
                .commandBufferCount   = 1,
                .pCommandBuffers      = rr.command.address(),
                .signalSemaphoreCount = 1,
                .pSignalSemaphores    = rr.sem_swap_finish.address(),
    };

    VVK_CHECK_VOID_RE(m_device->present_queue().handle.Submit(sub_info, *rr.fence_frame));
    VkPresentInfoKHR present_info {
        .sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext              = nullptr,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores    = rr.sem_swap_finish.address(),
        .swapchainCount     = 1,
        .pSwapchains        = m_device->swapchain().handle().address(),
        .pImageIndices      = &image_index,
    };
    VVK_CHECK_VOID_RE(m_device->present_queue().handle.Present(present_info));

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());
}
void VulkanRender::Impl::drawFrameOffscreen() {
    RenderingResources& rr    = m_rendering_resources;
    ImageParameters     image = m_ex_swapchain->GetInprogressImage();

    m_finpass->setPresent(image);

    (void)rr.command.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    });
    m_dyn_buf->recordUpload(rr.command, rr.frame_stats);
    executePreparedPasses(rr);

    (void)rr.command.End();

    VkSubmitInfo sub_info {
        .sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext                = nullptr,
        .commandBufferCount   = 1,
        .pCommandBuffers      = rr.command.address(),
        .signalSemaphoreCount = 1,
        .pSignalSemaphores    = rr.sem_export.address(),
    };
    VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, *rr.fence_frame));

    VVK_CHECK_VOID_RE(rr.fence_frame.Wait(vk_wait_time));
    VVK_CHECK_VOID_RE(rr.fence_frame.Reset());

    // Export the signaled semaphore as a dma_fence sync_file fd. The
    // export resets the semaphore's payload, so the next submit can
    // signal it again. Stored in an atomic slot that the host reads in
    // send_frame_ready_locked via takeLastFrameSyncFd().
    {
        int                     fd = -1;
        VkSemaphoreGetFdInfoKHR gi {
            .sType      = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
            .pNext      = nullptr,
            .semaphore  = *rr.sem_export,
            .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT_KHR,
        };
        if (m_device->handle().GetSemaphoreFdKHR(gi, &fd) == VK_SUCCESS && fd >= 0) {
            int old = m_last_sync_fd.exchange(fd, std::memory_order_acq_rel);
            if (old >= 0) ::close(old);
        }
    }

    m_ex_swapchain->renderFrame();
}

void VulkanRender::Impl::setRenderTargetSize(Scene& scene, rg::RenderGraph& rg) {
    auto&      ext           = m_device->out_extent();
    const auto source_extent = ResolveSceneSourceExtent(scene, ext);
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.enable && rt.bind.screen) {
            rt.width  = std::max(1,
                                static_cast<i32>(std::lround(
                                    rt.bind.scale * static_cast<double>(source_extent.width))));
            rt.height = std::max(1,
                                 static_cast<i32>(std::lround(
                                     rt.bind.scale * static_cast<double>(source_extent.height))));
        }
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (rt.bind.screen || ! rt.bind.enable) continue;
        auto bind_rt = scene.renderTargets.find(rt.bind.name);
        if (rt.bind.name.empty() || bind_rt == scene.renderTargets.end()) {
            LOG_ERROR("unknonw render target bind: %s", rt.bind.name.c_str());
            continue;
        }
        rt.width  = (i32)(rt.bind.scale * bind_rt->second.width);
        rt.height = (i32)(rt.bind.scale * bind_rt->second.height);
    }
    for (auto& item : scene.renderTargets) {
        auto& rt = item.second;
        if (! item.first.empty() && (rt.width * rt.height <= 4)) {
            LOG_ERROR("wrong size for render target: %s", item.first.c_str());
        } else if (rt.has_mipmap) {
            rt.mipmap_level =
                std::max(3u,
                         static_cast<uint>(std::floor(std::log2(std::min(rt.width, rt.height))))) -
                2u;
        }
    }
    scene.shaderValueUpdater->SetScreenSize(static_cast<i32>(source_extent.width),
                                            static_cast<i32>(source_extent.height));
}

void VulkanRender::Impl::updateScalingLayout(const Scene& scene, uint32_t output_width,
                                             uint32_t output_height) {
    const double scale_factor  = NormalizeScaleFactor(m_display_scale_factor);
    const auto   source_extent = ResolveSceneSourceExtent(
        scene, { std::max(1u, output_width), std::max(1u, output_height) });
    const uint32_t logical_width = std::max(
        1u, static_cast<uint32_t>(std::lround(static_cast<double>(output_width) / scale_factor)));
    const uint32_t logical_height = std::max(
        1u, static_cast<uint32_t>(std::lround(static_cast<double>(output_height) / scale_factor)));

    m_scaling_layout = ComputeWallpaperScalingLayout(m_scaling_mode,
                                                     source_extent.width,
                                                     source_extent.height,
                                                     logical_width,
                                                     logical_height,
                                                     scale_factor,
                                                     m_scaling_factor);
}

void VulkanRender::Impl::UpdateCameraFillMode(wallpaper::Scene&   scene,
                                              wallpaper::FillMode fillmode) {
    using namespace wallpaper;
    auto width  = m_device->out_extent().width;
    auto height = m_device->out_extent().height;

    if (width == 0) return;
    double sw = scene.ortho[0], sh = scene.ortho[1];
    double fboAspect = width / (double)height, sAspect = sw / sh;
    auto&  gCam    = *scene.cameras.at("global");
    auto&  gPerCam = *scene.cameras.at("global_perspective");
    // assum cam
    switch (fillmode) {
    case FillMode::STRETCH:
        gCam.SetWidth(sw);
        gCam.SetHeight(sh);
        gPerCam.SetAspect(sAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTFIT:
        if (fboAspect < sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    case FillMode::ASPECTCROP:
    default:
        if (fboAspect > sAspect) {
            // scale height
            gCam.SetWidth(sw);
            gCam.SetHeight(sw / fboAspect);
        } else {
            gCam.SetWidth(sh * fboAspect);
            gCam.SetHeight(sh);
        }
        gPerCam.SetAspect(fboAspect);
        gPerCam.SetFov(algorism::CalculatePersperctiveFov(1000.0f, gCam.Height()));
        break;
    }
    gCam.Update();
    gPerCam.Update();
    scene.UpdateLinkedCamera("global");
}

void VulkanRender::Impl::SetWallpaperScalingMode(wallpaper::WallpaperScalingMode mode) {
    m_scaling_mode = mode;
    LOG_INFO("wallpaper scaling mode: %s", WallpaperScalingModeName(m_scaling_mode));
}

void VulkanRender::Impl::SetWallpaperScalingFactor(double factor) {
    m_scaling_factor = NormalizeScaleFactor(factor);
    LOG_INFO("wallpaper scaling factor: %.3f", m_scaling_factor);
}

void VulkanRender::Impl::clearLastRenderGraph() {
    for (auto& p : m_passes) {
        p->destory(*m_device, m_rendering_resources);
    }
    m_passes.clear();
    m_device->tex_cache().Clear();

    m_vertex_buf->destroy();
    m_dyn_buf->destroy();

    m_vertex_buf->allocate();
    m_dyn_buf->allocate();
}

void VulkanRender::Impl::compileRenderGraph(Scene& scene, rg::RenderGraph& rg) {
    if (! m_inited) return;
    m_pass_loaded                     = false;
    m_rendering_resources.frame_stats = &m_pending_frame_stats;
    m_device->tex_cache().SetFrameStats(&m_pending_frame_stats);

    auto nodes             = rg.topologicalOrder();
    auto node_release_texs = rg.getLastReadTexs(nodes);

    m_passes.clear();
    m_passes.resize(nodes.size());

    std::transform(nodes.begin(),
                   nodes.end(),
                   node_release_texs.begin(),
                   m_passes.begin(),
                   [&rg](auto& id, auto& texs) {
                       auto* pass = rg.getPass(id);
                       assert(pass != nullptr);
                       VulkanPass* vpass = static_cast<VulkanPass*>(pass);
                       // LOG_INFO("----release tex");
                       for (auto& tex : texs) {
                           vpass->addReleaseTexs(spanone<const std::string_view> { tex->key() });
                           //    LOG_INFO("%s", tex->key().data());
                       }
                       return vpass;
                   });

    m_passes.insert(m_passes.begin(), m_prepass.get());
    m_passes.push_back(m_finpass.get());

    setRenderTargetSize(scene, rg);

    glslang::InitializeProcess();
    for (auto* p : m_passes) {
        if (! p->prepared()) {
            p->prepare(scene, *m_device, m_rendering_resources);
        }
    }
    glslang::FinalizeProcess();

    VVK_CHECK_VOID_RE(m_upload_cmd.Begin(VkCommandBufferBeginInfo {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    }));
    m_vertex_buf->recordUpload(m_upload_cmd);
    VVK_CHECK_VOID_RE(m_upload_cmd.End());
    {
        VkSubmitInfo sub_info {
            .sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext              = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers    = m_upload_cmd.address(),
        };
        VVK_CHECK_VOID_RE(m_device->graphics_queue().handle.Submit(sub_info, {}));
        VVK_CHECK_VOID_RE(m_device->handle().WaitIdle());
    }
    m_pass_loaded = true;
    m_device->tex_cache().SetFrameStats(nullptr);
};
