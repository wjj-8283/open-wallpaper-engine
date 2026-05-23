#pragma once
#include "VulkanPass.hpp"
#include <string>
#include <vector>

#include "Vulkan/Device.hpp"
#include "Scene/Scene.h"
#include "Vulkan/StagingBuffer.hpp"
#include "Vulkan/GraphicsPipeline.hpp"
#include "Vulkan/Shader.hpp"
#include "VulkanRender/PassBatch.hpp"
#include "SpriteAnimation.hpp"
#include "Interface/IShaderValueUpdater.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace wallpaper
{

namespace vulkan
{

class CustomShaderPass : public VulkanPass {
public:
    struct Desc {
        // in
        SceneNode*               node { nullptr };
        SceneNode*               visibility_node { nullptr };
        std::vector<std::string> textures;
        std::string              output;
        std::string              camera_override;
        std::size_t              submesh_index { 0 };
        uint32_t                 material_slot { 0 };
        bool                     clear_on_first_use { false };
        bool                     preserve_target_contents { false };
        bool                     write_alpha { true };
        bool                     alpha_to_coverage { false };
        VkSampleCountFlagBits    sample_count { VK_SAMPLE_COUNT_1_BIT };
        sprite_map_t             sprites_map;

        // -----prepared
        // vulkan texs
        std::vector<ImageSlotsRef> vk_textures;
        std::vector<std::string>   vk_texture_image_keys;
        std::vector<i32>           vk_tex_binding;
        std::vector<bool>          video_textures;
        ImageParameters            vk_output;
        ImageParameters            vk_output_msaa;

        // bufs
        bool                          dyn_vertex { false };
        std::vector<StagingBufferRef> vertex_bufs;
        StagingBufferRef              index_buf;
        StagingBufferRef              ubo_buf;

        // pipeline
        VkClearValue       clear_value;
        bool               blending { false };
        vvk::Framebuffer   fb;
        PipelineParameters pipeline;
        u32                draw_count { 0 };
        std::vector<SceneMesh::DrawRange> draw_ranges;

        // uniforms
        std::optional<ShaderReflected::Block> uniform_block;
        std::function<void()>                 update_op;
        uint64_t uploaded_mesh_dirty_generation { std::numeric_limits<uint64_t>::max() };
    };

    CustomShaderPass(const Desc&);
    virtual ~CustomShaderPass();

    void        setDescTex(u32 index, std::string_view tex_key);
    Desc&       desc() { return m_desc; }
    const Desc& desc() const { return m_desc; }

    void prepare(Scene&, const Device&, RenderingResources&) override;
    void execute(const Device&, RenderingResources&) override;
    void destory(const Device&, RenderingResources&) override;

    CustomPassBatchCandidate preRecord(const Device&, RenderingResources&);
    CustomPassRenderInfo     renderInfo() const;
    void                     recordDraw(const Device&, RenderingResources&);
    void                     recordClear(const Device&, RenderingResources&);

private:
    void recordTextureBarriers(RenderingResources&) const;
    void recordDescriptors(RenderingResources&) const;

    Desc m_desc;
};

} // namespace vulkan
} // namespace wallpaper
