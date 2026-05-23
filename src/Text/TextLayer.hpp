#pragma once

#include <Eigen/Dense>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace wallpaper
{

struct TextLayerState {
    std::string          text;
    std::string          layer_key;
    std::string          font_key;
    std::string          resolved_font_kind { "family" };
    std::string          resolved_font_identity;
    std::string          resolved_font_path;
    std::vector<uint8_t> resolved_font_data;
    float                point_size { 12.0f };
    float                padding { 0.0f };
    Eigen::Vector2f      explicit_size { Eigen::Vector2f::Zero() };
    Eigen::Vector3f      color { Eigen::Vector3f::Ones() };
    float                alpha { 1.0f };
    float                brightness { 1.0f };
    std::string          horizontal_align;
    std::string          vertical_align;
    std::string          anchor;
    bool                 dirty { false };
    bool                 cache_dirty { false };
    bool                 full_dirty { false };
    uint64_t             cache_revision { 0 };
    std::string          texture_cache_key;
    std::string          render_backend { "runtime-rgba-texture" };
    Eigen::Vector2f      layout_size { Eigen::Vector2f::Zero() };
};

struct TextLayerRenderBounds {
    float left { 0.0f };
    float right { 0.0f };
    float bottom { 0.0f };
    float top { 0.0f };
};

class TextLayer {
public:
    explicit TextLayer(TextLayerState state);

    const TextLayerState& state() const { return m_state; }
    const std::string&    text() const { return m_state.text; }
    Eigen::Vector2f       size() const { return m_state.layout_size; }
    Eigen::Vector2f       rasterSize() const;
    TextLayerRenderBounds renderBounds() const;
    bool                  dirty() const { return m_state.dirty; }

    void SetText(std::string text);
    void ClearDirty();

private:
    void Relayout();
    void EnsureCacheIdentity();
    void MarkCacheDirty();

    TextLayerState m_state;
};

Eigen::Vector2f EstimateTextLayerSize(std::string_view text, float point_size, float padding);
Eigen::Vector2f MeasureTextLayerSize(const TextLayerState& state);
Eigen::Vector2f TextLayerLayoutSize(const TextLayerState& state);
Eigen::Vector2f TextLayerRasterSize(const TextLayerState& state);
TextLayerRenderBounds TextLayerRenderBoundsForRasterSize(const TextLayerState& state,
                                                         Eigen::Vector2f raster_size);
std::string     TextTextureName(std::string_view layer_key);
void            RasterizeTextLayer(const TextLayerState& state, uint32_t width, uint32_t height,
                                   std::vector<uint8_t>& rgba);
#ifdef WESCENE_BUILD_TESTS
uint64_t        TextLayerMeasurementCountForTests();
void            ResetTextLayerMeasurementCountForTests();
#endif

} // namespace wallpaper
