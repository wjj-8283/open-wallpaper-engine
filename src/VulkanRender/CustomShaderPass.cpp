#include "CustomShaderPass.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneShader.h"
#include "Runtime/SceneRuntimeContext.hpp"

#include "SpecTexs.hpp"
#include "Vulkan/Shader.hpp"
#include "Utils/Logging.h"
#include "Utils/AutoDeletor.hpp"
#include "Resource.hpp"
#include "PassCommon.hpp"
#include "Interface/IImageParser.h"

#include "Core/ArrayHelper.hpp"

#include <cassert>
#include <algorithm>
#include <array>
#include <limits>

using namespace wallpaper::vulkan;

namespace
{
using wallpaper::usize;

} // namespace

CustomShaderPass::CustomShaderPass(const Desc& desc) {
    m_desc.node            = desc.node;
    m_desc.visibility_node = desc.visibility_node;
    m_desc.textures        = desc.textures;
    m_desc.output          = desc.output;
    m_desc.camera_override = desc.camera_override;
    m_desc.sample_count    = desc.sample_count;
    m_desc.sprites_map     = desc.sprites_map;
    m_desc.video_textures  = desc.video_textures;
};
CustomShaderPass::~CustomShaderPass() {}

std::optional<vvk::RenderPass> CreateRenderPass(const vvk::Device& device, VkFormat format,
                                                VkAttachmentLoadOp loadOp,
                                                VkImageLayout      finalLayout,
                                                VkSampleCountFlagBits sample_count) {
    const auto plan = PlanCustomPassMsaaAttachments(sample_count);

    VkAttachmentDescription color {
        .format         = format,
        .samples        = plan.color_samples,
        .loadOp         = loadOp, // VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout =
            plan.needs_resolve_attachment ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : finalLayout, // ShaderReadOnlyOptimal
    };

    if (loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) {
        color.initialLayout = plan.needs_resolve_attachment
                                  ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkAttachmentDescription resolve {
        .format         = format,
        .samples        = plan.resolve_samples,
        .loadOp         = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp        = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout    = finalLayout,
    };

    std::array<VkAttachmentDescription, 2> attachments { color, resolve };

    VkAttachmentReference attachment_ref {
        .attachment = 0,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference resolve_ref {
        .attachment = 1,
        .layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };

    VkSubpassDescription subpass {
        .pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments    = &attachment_ref,
        .pResolveAttachments  = plan.needs_resolve_attachment ? &resolve_ref : nullptr,
    };

    VkSubpassDependency dependency {
        .srcSubpass    = VK_SUBPASS_EXTERNAL,
        .dstSubpass    = 0,
        .srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };

    VkRenderPassCreateInfo creatinfo {
        .sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = plan.attachment_count,
        .pAttachments    = attachments.data(),
        .subpassCount    = 1,
        .pSubpasses      = &subpass,
        .dependencyCount = 1,
        .pDependencies   = &dependency,
    };
    vvk::RenderPass pass;
    if (auto res = device.CreateRenderPass(creatinfo, pass); res == VK_SUCCESS) {
        return pass;
    } else {
        VVK_CHECK(res);
        return std::nullopt;
    }
}

static void UpdateUniform(StagingBuffer* buf, const StagingBufferRef& bufref,
                          const ShaderReflected::Block& block, std::string_view name,
                          const wallpaper::ShaderValue& value) {
    using namespace wallpaper;
    std::span<uint8_t> value_u8 { (uint8_t*)value.data(),
                                  value.size() * sizeof(ShaderValue::value_type) };
    auto               uni = block.member_map.find(name);
    if (uni == block.member_map.end()) {
        // log
        return;
    }

    size_t offset    = uni->second.offset;
    size_t type_size = sizeof(float) * uni->second.num;
    if (uni->second.array_count > 0 && uni->second.array_stride > 0 &&
        value_u8.size() % uni->second.array_count == 0) {
        const size_t element_size = value_u8.size() / uni->second.array_count;
        if (element_size > 0 && element_size <= uni->second.array_stride) {
            for (size_t index = 0; index < uni->second.array_count; ++index) {
                buf->writeToBuf(bufref,
                                value_u8.subspan(index * element_size, element_size),
                                offset + index * uni->second.array_stride);
            }
            return;
        }
    }
    if (type_size != value_u8.size()) {
        // assert(type_size == value_u8.size());
        ; // to do
    }
    buf->writeToBuf(bufref, value_u8, offset);
}

void CustomShaderPass::prepare(Scene& scene, const Device& device, RenderingResources& rr) {
    m_desc.vk_textures.resize(m_desc.textures.size());
    m_desc.video_textures.resize(m_desc.textures.size(), false);
    for (usize i = 0; i < m_desc.textures.size(); i++) {
        auto& tex_name = m_desc.textures[i];
        if (tex_name.empty()) continue;

        ImageSlotsRef img_slots;
        if (IsSpecTex(tex_name)) {
            tex_name = scene.ResolveRenderTargetName(tex_name);
            if (! scene.HasRenderTarget(tex_name)) continue;
            auto& rt  = *scene.FindRenderTarget(tex_name);
            auto  opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            if (! opt.has_value()) continue;
            img_slots.slots = { opt.value() };
        } else {
            auto image = scene.imageParser->Parse(tex_name);
            if (image) {
                m_desc.video_textures[i] = image->header.isVideo;
                img_slots                = device.tex_cache().CreateTex(*image);
            } else {
                LOG_ERROR("parse tex \"%s\" failed", tex_name.c_str());
            }
        }
        m_desc.vk_textures[i] = img_slots;
    }
    {
        auto& tex_name = m_desc.output;
        if (! IsSpecTex(tex_name)) {
            LOG_ERROR("custom shader output is not a spec texture: %s", tex_name.c_str());
            return;
        }
        tex_name = scene.ResolveRenderTargetName(tex_name);
        if (! scene.HasRenderTarget(tex_name)) {
            LOG_ERROR("custom shader output render target is not registered: %s", tex_name.c_str());
            return;
        }
        auto& rt = *scene.FindRenderTarget(tex_name);
        m_desc.sample_count = ResolveCustomPassRenderTargetSampleCount(
            SampleCountValue(m_desc.sample_count), device.limits().framebufferColorSampleCounts);
        if (auto opt = device.tex_cache().Query(tex_name, ToTexKey(rt), ! rt.allowReuse);
            opt.has_value()) {
            m_desc.vk_output = opt.value();
        } else
            return;

        if (m_desc.sample_count != VK_SAMPLE_COUNT_1_BIT) {
            const auto  msaa_key  = ToTexKeyMsaa(rt, m_desc.sample_count);
            const auto  sample_id = SampleCountValue(m_desc.sample_count);
            std::string twin_name =
                tex_name + "::msaa" + std::to_string(static_cast<unsigned>(sample_id));
            if (auto opt = device.tex_cache().Query(twin_name, msaa_key, true);
                opt.has_value()) {
                m_desc.vk_output_msaa = opt.value();
            } else {
                LOG_ERROR("failed to allocate MSAA attachment for render target: %s",
                          tex_name.c_str());
                return;
            }
        }
    }

    SceneMesh& mesh = *(m_desc.node->Mesh());

    std::vector<Uni_ShaderSpv> spvs;
    DescriptorSetInfo          descriptor_info;
    ShaderReflected            ref;
    {
        SceneShader& shader = *(mesh.Material()->customShader.shader);

        if (! GenReflect(shader.codes, spvs, ref)) {
            LOG_ERROR("gen spv reflect failed, %s", shader.name.c_str());
            return;
        }

        auto& bindings = descriptor_info.bindings;
        bindings.resize(ref.binding_map.size());

        /*
        LOG_INFO("----shader------");
        LOG_INFO("%s", shader.name.c_str());
        LOG_INFO("--inputs:");
        for (auto& i : ref.input_location_map) {
            LOG_INFO("%d %s", i.second, i.first.c_str());
        }
        LOG_INFO("--bindings:");
        */

        std::transform(
            ref.binding_map.begin(), ref.binding_map.end(), bindings.begin(), [](auto& item) {
                // LOG_INFO("%d %s", item.second.binding, item.first.c_str());
                return item.second;
            });

        for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
            i32 binding { -1 };
            if (exists(ref.binding_map, WE_GLTEX_NAMES[i]))
                binding = (i32)ref.binding_map.at(WE_GLTEX_NAMES[i]).binding;
            m_desc.vk_tex_binding.push_back(binding);
        }
    }

    m_desc.draw_count = 0;
    std::vector<VkVertexInputBindingDescription>   bind_descriptions;
    std::vector<VkVertexInputAttributeDescription> attr_descriptions;
    {
        m_desc.dyn_vertex = mesh.Dynamic();
        m_desc.vertex_bufs.resize(mesh.VertexCount());

        for (uint i = 0; i < mesh.VertexCount(); i++) {
            const auto& vertex    = mesh.GetVertexArray(i);
            auto        attrs_map = vertex.GetAttrOffsetMap();

            VkVertexInputBindingDescription bind_desc {
                .binding   = i,
                .stride    = (uint32_t)vertex.OneSizeOf(),
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            };
            bind_descriptions.push_back(bind_desc);

            for (auto& item : ref.input_location_map) {
                auto& name   = item.first;
                auto& input  = item.second;
                usize offset = exists(attrs_map, name) ? attrs_map[name].offset : 0;

                VkVertexInputAttributeDescription attr_desc {
                    .location = input.location,
                    .binding  = i,
                    .format   = input.format,
                    .offset   = (u32)offset,
                };
                attr_descriptions.push_back(attr_desc);
            }
            {
                auto& buf = m_desc.vertex_bufs[i];
                if (! m_desc.dyn_vertex) {
                    if (! rr.vertex_buf->allocateSubRef(vertex.CapacitySizeOf(), buf)) return;
                    if (! rr.vertex_buf->writeToBuf(buf, { (uint8_t*)vertex.Data(), buf.size }))
                        return;
                } else {
                    if (! rr.dyn_buf->allocateSubRef(vertex.CapacitySizeOf(), buf)) return;
                }
            }
            m_desc.draw_count += (u32)(vertex.DataSize() / vertex.OneSize());
        }

        if (mesh.IndexCount() > 0) {
            auto&  indice     = mesh.GetIndexArray(0);
            size_t count      = (indice.DataCount() * 2) / 3;
            m_desc.draw_count = (u32)count * 3;
            auto& buf         = m_desc.index_buf;
            if (! m_desc.dyn_vertex) {
                if (! rr.vertex_buf->allocateSubRef(indice.CapacitySizeof(), buf)) return;
                if (! rr.vertex_buf->writeToBuf(buf, { (uint8_t*)indice.Data(), buf.size })) return;
            } else {
                if (! rr.dyn_buf->allocateSubRef(indice.CapacitySizeof(), buf)) return;
            }
        }
    }
    {
        VkPipelineColorBlendAttachmentState color_blend;
        VkAttachmentLoadOp                  loadOp { VK_ATTACHMENT_LOAD_OP_DONT_CARE };
        {
            VkColorComponentFlags colorMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
            if (m_desc.write_alpha) colorMask |= VK_COLOR_COMPONENT_A_BIT;
            color_blend.colorWriteMask = colorMask;

            auto blendmode = mesh.Material()->blenmode;
            SetBlend(blendmode, color_blend);
            m_desc.blending          = color_blend.blendEnable;
            m_desc.alpha_to_coverage = blendmode == BlendMode::AlphaToCoverage;

            loadOp =
                ResolveAttachmentLoadOp(m_desc.preserve_target_contents, m_desc.clear_on_first_use);
        }
        auto opt = CreateRenderPass(device.handle(),
                                    VK_FORMAT_R8G8B8A8_UNORM,
                                    loadOp,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    m_desc.sample_count);
        if (! opt.has_value()) return;
        auto& pass = opt.value();

        descriptor_info.push_descriptor = true;
        GraphicsPipeline pipeline;
        pipeline.toDefault();
        if (m_desc.alpha_to_coverage) {
            pipeline.multisample.alphaToCoverageEnable = VK_TRUE;
        }
        pipeline.addDescriptorSetInfo(spanone { descriptor_info })
            .setColorBlendStates(spanone { color_blend })
            .setTopology(m_desc.index_buf ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
                                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
            .addInputBindingDescription(bind_descriptions)
            .addInputAttributeDescription(attr_descriptions)
            .setSampleCount(m_desc.sample_count);
        for (auto& spv : spvs) pipeline.addStage(std::move(spv));

        if (! pipeline.create(device, pass, m_desc.pipeline)) return;
    }

    {
        auto render_info = renderInfo();
        auto views       = CustomPassFramebufferAttachmentViews(render_info);
        VkFramebufferCreateInfo info {
            .sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext           = nullptr,
            .renderPass      = *m_desc.pipeline.pass,
            .attachmentCount = views.size(),
            .pAttachments    = views.data(),
            .width           = m_desc.vk_output.extent.width,
            .height          = m_desc.vk_output.extent.height,
            .layers          = 1,
        };
        VVK_CHECK_VOID_RE(device.handle().CreateFramebuffer(info, m_desc.fb));
    }

    m_desc.uniform_block.reset();
    const ShaderReflected::Block* uniform_block = nullptr;
    if (! ref.blocks.empty()) {
        m_desc.uniform_block = ref.blocks.front();
        uniform_block        = &m_desc.uniform_block.value();
        rr.dyn_buf->allocateSubRef(
            uniform_block->size, m_desc.ubo_buf, device.limits().minUniformBufferOffsetAlignment);
    }

    std::function<void()> update_dyn_buf_op;
    if (m_desc.dyn_vertex) {
        auto& mesh        = *m_desc.node->Mesh();
        auto* dyn_buf     = rr.dyn_buf;
        auto& vertex_bufs = m_desc.vertex_bufs;
        auto& draw_count  = m_desc.draw_count;
        auto& index_buf   = m_desc.index_buf;
        update_dyn_buf_op = [&mesh, &vertex_bufs, &draw_count, &index_buf, dyn_buf]() {
            if (mesh.Dirty().exchange(false)) {
                for (usize i = 0; i < mesh.VertexCount(); i++) {
                    const auto& vertex = mesh.GetVertexArray(i);
                    auto&       buf    = vertex_bufs[i];
                    if (! dyn_buf->writeToBuf(buf,
                                              { (uint8_t*)vertex.Data(), vertex.DataSizeOf() }))
                        return;
                }
                if (mesh.IndexCount() > 0) {
                    auto& indice = mesh.GetIndexArray(0);
                    u32   count  = (u32)((indice.RenderDataCount() * 2) / 3);
                    draw_count   = count * 3;
                    auto& buf    = index_buf;
                    if (! dyn_buf->writeToBuf(buf,
                                              { (uint8_t*)indice.Data(), indice.DataSizeOf() }))
                        return;
                }
            }
        };
    }

    auto* buf    = rr.dyn_buf;
    auto* bufref = &m_desc.ubo_buf;

    auto* node            = m_desc.node;
    auto* scene_ptr       = &scene;
    auto* device_ptr      = &device;
    auto* shader_updater  = scene.shaderValueUpdater.get();
    auto& sprites         = m_desc.sprites_map;
    auto& textures        = m_desc.textures;
    auto& video_textures  = m_desc.video_textures;
    auto& vk_textures     = m_desc.vk_textures;
    auto  camera_override = m_desc.camera_override;

    m_desc.update_op = [shader_updater,
                        uniform_block,
                        buf,
                        bufref,
                        node,
                        scene_ptr,
                        device_ptr,
                        &textures,
                        &video_textures,
                        &sprites,
                        &vk_textures,
                        camera_override,
                        update_dyn_buf_op]() {
        auto update_unf_op = [uniform_block, buf, bufref](std::string_view       name,
                                                          wallpaper::ShaderValue value) {
            if (uniform_block == nullptr || buf == nullptr || bufref == nullptr || ! (*bufref))
                return;
            UpdateUniform(buf, *bufref, *uniform_block, name, value);
        };
        std::string original_camera;
        bool        restore_camera = false;
        if (! camera_override.empty() && node != nullptr && node->Camera() != camera_override) {
            original_camera = node->Camera();
            node->SetCamera(camera_override);
            restore_camera = true;
        }
        shader_updater->UpdateUniforms(node, sprites, update_unf_op);
        if (restore_camera) {
            node->SetCamera(original_camera);
        }
        if (uniform_block != nullptr && node != nullptr && node->Mesh() != nullptr &&
            node->Mesh()->Material() != nullptr) {
            const auto& const_values = node->Mesh()->Material()->customShader.constValues;
            for (const auto& [name, value] : const_values) {
                UpdateUniform(buf, *bufref, *uniform_block, name, value);
            }
        }
        {
            for (auto& [i, sp] : sprites) {
                if (i >= vk_textures.size()) continue;
                vk_textures.at(i).active = sp.GetCurFrame().imageId;
            }
        }
        for (usize i = 0; i < video_textures.size(); ++i) {
            if (! video_textures[i]) continue;
            if (i >= textures.size() || i >= vk_textures.size()) continue;

            std::string                          error;
            wallpaper::video::VideoPlaybackState playback_state =
                scene_ptr->runtime != nullptr ? scene_ptr->runtime->ResolveVideoPlaybackState(
                                                    textures[i], scene_ptr->elapsingTime)
                                              : wallpaper::video::VideoPlaybackState {};
            if (scene_ptr->runtime == nullptr) {
                playback_state.scene_elapsed_seconds = scene_ptr->elapsingTime;
            }
            if (! device_ptr->tex_cache().UpdateVideoFrame(
                    textures[i], playback_state, &vk_textures[i], &error)) {
                LOG_ERROR("failed to update video texture \"%s\": %s",
                          textures[i].c_str(),
                          error.c_str());
            } else {
                if (scene_ptr->runtime != nullptr) {
                    scene_ptr->runtime->SetVideoTextureDuration(
                        textures[i], device_ptr->tex_cache().GetVideoDuration(textures[i]));
                }
            }
        }
        if (update_dyn_buf_op) update_dyn_buf_op();
    };

    auto exists_unf_op = [uniform_block](std::string_view name) {
        return uniform_block != nullptr && exists(uniform_block->member_map, name);
    };
    shader_updater->InitUniforms(node, exists_unf_op);

    if (uniform_block != nullptr) {
        buf->fillBuf(*bufref, 0, bufref->size, 0);
        {
            auto&      default_values = mesh.Material()->customShader.shader->default_uniforms;
            auto&      const_values   = mesh.Material()->customShader.constValues;
            std::array values_array   = { &default_values, &const_values };
            for (auto& values : values_array) {
                for (auto& v : *values) {
                    if (exists(uniform_block->member_map, v.first)) {
                        UpdateUniform(buf, *bufref, *uniform_block, v.first, v.second);
                    }
                }
            }
        }
    }
    m_desc.update_op();

    {
        m_desc.clear_value =
            ResolveAttachmentClearValue(m_desc.output == SpecTex_Default, scene.clearColor);
    }
    for (auto& tex : releaseTexs()) {
        device.tex_cache().MarkShareReady(tex);
    }
    setPrepared();
}

CustomPassRenderInfo CustomShaderPass::renderInfo() const {
    return CustomPassRenderInfo {
        .image        = m_desc.vk_output.handle,
        .view         = m_desc.vk_output.view,
        .msaa_image   = m_desc.vk_output_msaa.handle,
        .msaa_view    = m_desc.vk_output_msaa.view,
        .extent       = m_desc.vk_output.extent,
        .final_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        .load_op =
            ResolveAttachmentLoadOp(m_desc.preserve_target_contents, m_desc.clear_on_first_use),
        .sample_count = m_desc.sample_count,
        .render_pass = *m_desc.pipeline.pass,
        .framebuffer = *m_desc.fb,
        .clear_value = m_desc.clear_value,
    };
}

CustomPassBatchCandidate CustomShaderPass::preRecord(const Device&, RenderingResources& rr) {
    const bool visible =
        m_desc.visibility_node == nullptr || m_desc.visibility_node->EffectiveVisible();
    CustomPassBatchCandidate candidate {
        .batchable  = true,
        .visible    = visible,
        .clear_only = ! visible && m_desc.clear_on_first_use,
        .render     = renderInfo(),
    };

    if (! visible) {
        return candidate;
    }

    if (m_desc.update_op) m_desc.update_op();
    recordTextureBarriers(rr);
    return candidate;
}

void CustomShaderPass::recordTextureBarriers(RenderingResources& rr) const {
    auto&                   cmd = rr.command;
    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_ARRAY_LAYERS,
    };
    for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
        auto& slot    = m_desc.vk_textures[i];
        int   binding = m_desc.vk_tex_binding[i];
        if (binding < 0) continue;
        if (slot.slots.empty()) continue;
        auto& img = slot.getActive();

        VkImageMemoryBarrier imb {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask    = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout        = img.layout,
            .newLayout        = img.layout,
            .image            = img.handle,
            .subresourceRange = base_srang,
        };

        cmd.PipelineBarrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            imb);
    }
}

void CustomShaderPass::recordDescriptors(RenderingResources& rr) const {
    auto& cmd = rr.command;
    for (usize i = 0; i < m_desc.vk_textures.size(); i++) {
        auto& slot    = m_desc.vk_textures[i];
        int   binding = m_desc.vk_tex_binding[i];
        if (binding < 0) continue;
        if (slot.slots.empty()) continue;
        auto&                 img = slot.getActive();
        VkDescriptorImageInfo desc_img { img.sampler, img.view, img.layout };
        VkWriteDescriptorSet  wset {
             .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .pNext           = nullptr,
             .dstSet          = {},
             .dstBinding      = (uint32_t)binding,
             .descriptorCount = 1,
             .descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo      = &desc_img,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
    }

    if (m_desc.ubo_buf) {
        VkDescriptorBufferInfo desc_buf {
            rr.dyn_buf->gpuBuf(),
            m_desc.ubo_buf.offset,
            m_desc.ubo_buf.size,
        };
        VkWriteDescriptorSet wset {
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext           = nullptr,
            .dstSet          = {},
            .dstBinding      = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo     = &desc_buf,
        };
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.layout, 0, wset);
    }
}

void CustomShaderPass::recordDraw(const Device&, RenderingResources& rr) {
    recordDescriptors(rr);
    auto& cmd    = rr.command;
    auto& outext = m_desc.vk_output.extent;
    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, *m_desc.pipeline.handle);
    VkViewport viewport {
        .x        = 0,
        .y        = (float)outext.height,
        .width    = (float)outext.width,
        .height   = -(float)outext.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor { { 0, 0 }, { outext.width, outext.height } };

    cmd.SetViewport(0, viewport);
    cmd.SetScissor(0, scissor);

    auto gpu_buf = m_desc.dyn_vertex ? rr.dyn_buf->gpuBuf() : rr.vertex_buf->gpuBuf();

    for (usize i = 0; i < m_desc.vertex_bufs.size(); i++) {
        auto& buf = m_desc.vertex_bufs[i];
        cmd.BindVertexBuffers((u32)i, 1, &gpu_buf, &buf.offset);
    }
    if (m_desc.index_buf) {
        cmd.BindIndexBuffer(gpu_buf, m_desc.index_buf.offset, VK_INDEX_TYPE_UINT16);
        cmd.DrawIndexed(m_desc.draw_count, 1, 0, 0, 0);
    } else {
        cmd.Draw(m_desc.draw_count, 1, 0, 0);
    }
}

void CustomShaderPass::recordClear(const Device&, RenderingResources& rr) {
    auto&                   cmd = rr.command;
    VkImageSubresourceRange base_srang {
        .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
        .baseMipLevel   = 0,
        .levelCount     = VK_REMAINING_MIP_LEVELS,
        .baseArrayLayer = 0,
        .layerCount     = VK_REMAINING_ARRAY_LAYERS,
    };
    const auto clear_image = [&](VkImage image, VkImageLayout old_layout,
                                 VkImageLayout final_layout, VkPipelineStageFlags src_stage,
                                 VkAccessFlags src_access, VkPipelineStageFlags dst_stage,
                                 VkAccessFlags dst_access) {
        if (image == VK_NULL_HANDLE) return;
        VkImageMemoryBarrier in_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = src_access,
            .dstAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout        = old_layout,
            .newLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .image            = image,
            .subresourceRange = base_srang,
        };
        cmd.PipelineBarrier(src_stage,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            in_bar);
        cmd.ClearColorImage(image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            &m_desc.clear_value.color,
                            base_srang);
        VkImageMemoryBarrier out_bar {
            .sType            = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext            = nullptr,
            .srcAccessMask    = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask    = dst_access,
            .oldLayout        = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout        = final_layout,
            .image            = image,
            .subresourceRange = base_srang,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                            dst_stage,
                            VK_DEPENDENCY_BY_REGION_BIT,
                            out_bar);
    };
    clear_image(m_desc.vk_output.handle,
                VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                0,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_MEMORY_READ_BIT);
    clear_image(m_desc.vk_output_msaa.handle,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
}

void CustomShaderPass::execute(const Device& device, RenderingResources& rr) {
    const auto candidate = preRecord(device, rr);
    if (! candidate.visible) {
        if (candidate.clear_only) {
            recordClear(device, rr);
        }
        return;
    }

    const auto            info = renderInfo();
    std::array<VkClearValue, 2> clear_values { info.clear_value, VkClearValue {} };
    VkRenderPassBeginInfo pass_begin_info {
        .sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext       = nullptr,
        .renderPass  = info.render_pass,
        .framebuffer = info.framebuffer,
        .renderArea =
            VkRect2D {
                .offset = { 0, 0 },
                .extent = { info.extent.width, info.extent.height },
            },
        .clearValueCount = CustomPassBeginRenderPassClearValueCount(info),
        .pClearValues    = clear_values.data(),
    };
    rr.command.BeginRenderPass(pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
    recordDraw(device, rr);
    rr.command.EndRenderPass();
}

void CustomShaderPass::destory(const Device&, RenderingResources& rr) {
    m_desc.update_op = {};
    {
        auto& buf = m_desc.dyn_vertex ? rr.dyn_buf : rr.vertex_buf;
        for (auto& bufref : m_desc.vertex_bufs) {
            buf->unallocateSubRef(bufref);
        }
    }
    rr.dyn_buf->unallocateSubRef(m_desc.ubo_buf);
}

void CustomShaderPass::setDescTex(u32 index, std::string_view tex_key) {
    assert(index < m_desc.textures.size());
    if (index >= m_desc.textures.size()) return;
    m_desc.textures[index] = tex_key;
}
