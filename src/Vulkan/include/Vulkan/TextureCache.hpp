#pragma once

#include "Video/VideoTextureSource.hpp"
#include "Parameters.hpp"
#include "Type.hpp"
#include "Core/NoCopyMove.hpp"
#include "Core/MapSet.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace wallpaper
{

struct Image;

namespace vulkan
{

struct RenderFrameStats;

VkFormat             ToVkType(TextureFormat);
VkSamplerAddressMode ToVkType(TextureWrap);
VkFilter             ToVkType(TextureFilter);
VkSamplerCreateInfo  GenRenderTargetSamplerInfo();

enum class TexUsage
{
    COLOR,
    DEPTH
};

using TexHash = std::size_t;

struct TextureKey {
    i32           width;
    i32           height;
    TexUsage      usage;
    TextureFormat format;
    TextureSample sample;
    uint          mipmap_level { 1 };

    static TexHash HashValue(const TextureKey&);
};

class TextureCache : NoCopy, NoMove {
public:
    TextureCache(const Device&);
    ~TextureCache();

    void Clear();
    void SetFrameStats(RenderFrameStats* stats);

    std::optional<ExImageParameters> CreateExTex(uint32_t witdh, uint32_t height, VkFormat,
                                                 VkImageTiling);
    ImageSlotsRef                    CreateTex(Image&);
    void                             SetVideoPlaybackPaused(bool paused);
    void                             SetVideoPlaybackRate(float rate);
    double                           GetVideoDuration(std::string_view key) const;
    bool UpdateVideoFrame(std::string_view key, const video::VideoPlaybackState& playback_state,
                          ImageSlotsRef* out, std::string* error = nullptr);
    bool ReadbackImageSample(const ImageParameters& image, uint32_t x, uint32_t y, uint32_t width,
                             uint32_t height, std::vector<std::uint8_t>* out,
                             std::string* error = nullptr);

    std::optional<ImageParameters> Query(std::string_view key, TextureKey content_hash,
                                         bool persist = false);

    void MarkShareReady(std::string_view key);

    void RecGenerateMipmaps(vvk::CommandBuffer& cmd, const ImageParameters& image) const;

private:
    std::optional<VmaImageParameters> CreateTex(TextureKey);
    VkSampler                         GetOrCreateSampler(TextureKey, std::string* error);
    void*                             GetMetalDeviceHandle(std::string* error);
    void                              allocateCmd();
    void                              allocateVideoImportCmd();
    bool                              waitForPendingVideoImport(std::string* error);
    vvk::CommandBuffers               m_tex_cmds;
    vvk::CommandBuffer                m_tex_cmd;
    vvk::CommandBuffers               m_video_import_cmds;
    vvk::CommandBuffer                m_video_import_cmd;
    vvk::Fence                        m_video_import_fence;
    bool                              m_video_import_pending { false };

    const Device&                m_device;
    Map<std::string, ImageSlots> m_tex_map;
    struct ImportedVideoFrame {
        ExImageParameters     image;
        std::shared_ptr<void> metal_texture;
        uint64_t              generation { 0 };
        void*                 surface_identity { nullptr };
    };
    struct VideoTex {
        TextureSample                              sample;
        std::shared_ptr<video::VideoTextureSource> source;
        std::unique_ptr<ImportedVideoFrame>        current_frame;
    };
    Map<std::string, std::unique_ptr<VideoTex>> m_video_tex_map;
    video::VideoPlaybackState                   m_video_playback_state {};
    RenderFrameStats*                           m_frame_stats { nullptr };
    void*                                       m_metal_device { nullptr };
    bool                                        m_metal_device_queried { false };

    struct CachedSampler {
        TexHash      hash { 0 };
        vvk::Sampler sampler;
    };
    std::vector<CachedSampler> m_sampler_cache;

    struct QueryTex {
        idx                index { 0 };
        bool               share_ready { false };
        bool               persist { false };
        TexHash            content_hash;
        VmaImageParameters image;
        Set<std::string>   query_keys;
    };
    std::vector<std::unique_ptr<QueryTex>> m_query_texs;
    Map<std::string, QueryTex*>            m_query_map;
};

} // namespace vulkan
} // namespace wallpaper
