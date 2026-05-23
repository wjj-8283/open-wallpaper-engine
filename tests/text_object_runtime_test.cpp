#include "Audio/SoundManager.h"
#include "Fs/Fs.h"
#include "Fs/MemBinaryStream.h"
#include "Fs/PhysicalFs.h"
#include "Fs/VFS.h"
#include "Interface/IImageParser.h"
#include "RenderGraph/RenderGraph.hpp"
#include "Runtime/DynamicValue.hpp"
#include "Runtime/SceneRuntimeContext.hpp"
#include "VulkanRender/CustomShaderPass.hpp"
#include "VulkanRender/SceneToRenderGraph.hpp"
#include "Scene/Scene.h"
#include "Scene/SceneNode.h"
#include "Scripting/ScriptEngine.hpp"
#include "Project/ProjectProperties.hpp"
#include "SpecTexs.hpp"
#include "Text/SystemFontResolver.hpp"
#include "Text/TextLayer.hpp"
#include "Vulkan/Shader.hpp"
#include "WPSceneParser.hpp"
#include "WPPkgFs.hpp"

#include <gtest/gtest.h>
#include <spirv_reflect.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wallpaper
{
namespace
{

class MemoryFs final : public fs::Fs {
public:
    explicit MemoryFs(std::map<std::string, std::string> files): m_files(std::move(files)) {}

    bool Contains(std::string_view path) const override {
        return m_files.contains(std::string(path));
    }

    std::shared_ptr<fs::IBinaryStream> Open(std::string_view path) override {
        const auto it = m_files.find(std::string(path));
        if (it == m_files.end()) return nullptr;
        const auto& s = it->second;
        return std::make_shared<fs::MemBinaryStream>(std::vector<uint8_t>(s.begin(), s.end()));
    }

    std::shared_ptr<fs::IBinaryStreamW> OpenW(std::string_view) override { return nullptr; }

private:
    std::map<std::string, std::string> m_files;
};

void MountAssets(fs::VFS& vfs, std::map<std::string, std::string> files = {}) {
    ASSERT_TRUE(vfs.Mount("/assets", std::make_unique<MemoryFs>(std::move(files))));
}

bool MountPhysicalAssets(fs::VFS& vfs, const std::filesystem::path& assets_path) {
    auto physical_fs = fs::CreatePhysicalFs(assets_path.string());
    if (physical_fs == nullptr) return false;
    return vfs.Mount("/assets", std::move(physical_fs), "assets");
}

std::filesystem::path HomePath() {
    const char* home = std::getenv("HOME");
    return home != nullptr ? std::filesystem::path(home) : std::filesystem::path();
}

std::optional<std::string> ReadTextFile(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (! input.good()) return std::nullopt;
    std::string content((std::istreambuf_iterator<char>(input)),
                        std::istreambuf_iterator<char>());
    return content;
}

std::string MinimalSceneObjects(std::string objects_json) {
    return R"({
      "camera": {"center":[0,0,0], "eye":[0,0,1], "up":[0,1,0]},
      "general": {
        "ambientcolor":[0,0,0], "skylightcolor":[0,0,0],
        "clearcolor":[0,0,0], "cameraparallax":false,
        "cameraparallaxamount":0, "cameraparallaxdelay":0,
        "cameraparallaxmouseinfluence":0,
        "orthogonalprojection":{"width":640,"height":360}
      },
      "objects": )" +
           objects_json + "\n}";
}

SceneNode* FindRootChild(Scene& scene, std::string_view name) {
    if (scene.sceneGraph == nullptr) return nullptr;
    const auto& children = scene.sceneGraph->GetChildren();
    const auto  it       = std::find_if(children.begin(), children.end(), [name](const auto& node) {
        return node != nullptr && node->Name() == name;
    });
    return it == children.end() ? nullptr : it->get();
}

struct MeshBounds {
    float min_x { 0.0f };
    float max_x { 0.0f };
    float min_y { 0.0f };
    float max_y { 0.0f };

    Eigen::Vector2f Size() const { return Eigen::Vector2f(max_x - min_x, max_y - min_y); }
};

MeshBounds MeshLocalBounds(const SceneMesh& mesh) {
    const auto& vertices = mesh.GetVertexArray(0);
    const auto  stride   = vertices.OneSize();
    const auto* data     = vertices.Data();
    if (data == nullptr || vertices.VertexCount() == 0 || stride < 2u) {
        return {};
    }

    MeshBounds bounds {
        .min_x = data[0],
        .max_x = data[0],
        .min_y = data[1],
        .max_y = data[1],
    };
    for (std::size_t index = 1; index < vertices.VertexCount(); ++index) {
        const float x = data[index * stride + 0u];
        const float y = data[index * stride + 1u];
        bounds.min_x  = std::min(bounds.min_x, x);
        bounds.max_x  = std::max(bounds.max_x, x);
        bounds.min_y  = std::min(bounds.min_y, y);
        bounds.max_y  = std::max(bounds.max_y, y);
    }
    return bounds;
}

Eigen::Vector2f MeshSize(const SceneMesh& mesh) {
    return MeshLocalBounds(mesh).Size();
}

bool ShaderUsesUniformMember(const ShaderCode& spirv, std::string_view member_name) {
    spv_reflect::ShaderModule module(spirv, SPV_REFLECT_MODULE_FLAG_NO_COPY);

    uint32_t binding_count = 0;
    if (module.EnumerateDescriptorBindings(&binding_count, nullptr) != SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    std::vector<SpvReflectDescriptorBinding*> bindings(binding_count);
    if (module.EnumerateDescriptorBindings(&binding_count, bindings.data()) !=
        SPV_REFLECT_RESULT_SUCCESS) {
        return false;
    }

    for (const auto* binding : bindings) {
        if (binding == nullptr || ! binding->accessed ||
            binding->descriptor_type != SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            continue;
        }
        const auto& block = binding->block;
        for (uint32_t index = 0; index < block.member_count; ++index) {
            const auto* name = block.members[index].name;
            if (name != nullptr && member_name == name) return true;
        }
    }
    return false;
}

int VisibleAlphaPixels(const Image& image) {
    int visible_pixels = 0;
    if (image.slots.empty() || image.slots[0].mipmaps.empty()) return visible_pixels;

    const auto& mip = image.slots[0].mipmaps[0];
    if (mip.data == nullptr) return visible_pixels;
    for (int y = 0; y < mip.height; ++y) {
        for (int x = 0; x < mip.width; ++x) {
            const auto alpha =
                mip.data.get()[(static_cast<std::size_t>(y) * mip.width + x) * 4u + 3u];
            if (alpha > 0u) ++visible_pixels;
        }
    }
    return visible_pixels;
}

const vulkan::CustomShaderPass* FindCustomPassForNode(const rg::RenderGraph& graph,
                                                      const SceneNode*       node) {
    for (const auto id : graph.topologicalOrder()) {
        auto* pass = dynamic_cast<const vulkan::CustomShaderPass*>(graph.getPass(id));
        if (pass != nullptr && pass->desc().node == node) return pass;
    }
    return nullptr;
}

std::optional<VkDescriptorSetLayoutBinding>
ShaderDescriptorBinding(const std::vector<ShaderCode>& spirv, std::string_view name) {
    std::vector<vulkan::Uni_ShaderSpv> spvs;
    vulkan::ShaderReflected            reflected;
    if (! vulkan::GenReflect(spirv, spvs, reflected)) return std::nullopt;

    const auto iterator = reflected.binding_map.find(std::string(name));
    if (iterator == reflected.binding_map.end()) return std::nullopt;

    return iterator->second;
}

std::vector<VkDescriptorSetLayoutBinding>
ShaderDescriptorBindings(const std::vector<ShaderCode>& spirv) {
    std::vector<vulkan::Uni_ShaderSpv> spvs;
    vulkan::ShaderReflected            reflected;
    if (! vulkan::GenReflect(spirv, spvs, reflected)) return {};

    std::vector<VkDescriptorSetLayoutBinding> bindings;
    bindings.reserve(reflected.binding_map.size());
    for (const auto& [name, binding] : reflected.binding_map) {
        (void)name;
        bindings.push_back(binding);
    }
    return bindings;
}

} // namespace

TEST(TextObjectRuntime, ParserCreatesRuntimeTextNodeAndState) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;

    ProjectProperties properties;
    SceneParseRequest request {
        .scene_id           = "text-object",
        .project_properties = &properties,
    };
    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": {"value": "hello"},
            "font": {"value": "Arial"},
            "pointsize": 20,
            "padding": 4,
            "origin": [100, 50, 0],
            "horizontalalign": "right",
            "verticalalign": "bottom",
            "alignment": "left",
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_TRUE(scene->runtime->HasNodeNamed("caption"));
    EXPECT_EQ(scene->runtime->NodeText("caption"), "hello");

    const auto size = scene->runtime->NodeSize("caption");
    EXPECT_GT(size.x(), 8.0f);
    EXPECT_GT(size.y(), 8.0f);

    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    EXPECT_NE(node->Mesh(), nullptr);
    EXPECT_LT(node->Translate().x(), 100.0f);
    EXPECT_GT(node->Translate().y(), 50.0f);
}

TEST(TextObjectRuntime, ParserCreatesRenderableTextMaterialAndTexture) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;

    ProjectProperties properties;
    SceneParseRequest request {
        .scene_id           = "text-renderable-material",
        .project_properties = &properties,
    };
    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": "I I",
            "font": "Arial",
            "pointsize": 20,
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);

    auto* material = node->Mesh()->MaterialForSlot(0);
    ASSERT_NE(material, nullptr);
    EXPECT_EQ(material->name, "text");
    ASSERT_EQ(material->textures.size(), 1u);
    EXPECT_FALSE(material->textures.front().empty());
    EXPECT_FALSE(IsSpecTex(material->textures.front()));
    EXPECT_TRUE(scene->textures.contains(material->textures.front()));
    auto image = scene->imageParser->Parse(material->textures.front());
    ASSERT_NE(image, nullptr);
    ASSERT_EQ(image->slots.size(), 1u);
    ASSERT_EQ(image->slots[0].mipmaps.size(), 1u);
    const auto& mip = image->slots[0].mipmaps[0];
    ASSERT_NE(mip.data, nullptr);
    ASSERT_GT(mip.width, 1);
    ASSERT_GT(mip.height, 1);
    bool has_visible_alpha            = false;
    bool has_transparent_space_column = false;
    for (int y = 1; y < mip.height - 1; ++y) {
        for (int x = 1; x < mip.width - 1; ++x) {
            const auto alpha =
                mip.data.get()[(static_cast<std::size_t>(y) * mip.width + x) * 4u + 3u];
            has_visible_alpha = has_visible_alpha || alpha > 0u;
        }
    }
    const int min_space_x = mip.width / 4;
    const int max_space_x = (mip.width * 3) / 4;
    for (int x = min_space_x; x < max_space_x && ! has_transparent_space_column; ++x) {
        bool column_clear = true;
        for (int y = 1; y < mip.height - 1; ++y) {
            const auto alpha =
                mip.data.get()[(static_cast<std::size_t>(y) * mip.width + x) * 4u + 3u];
            if (alpha != 0u) {
                column_clear = false;
                break;
            }
        }
        has_transparent_space_column = column_clear;
    }
    EXPECT_TRUE(has_visible_alpha);
    EXPECT_TRUE(has_transparent_space_column);
    ASSERT_NE(material->customShader.shader, nullptr);
    ASSERT_FALSE(material->customShader.shader->codes.empty());
    EXPECT_TRUE(ShaderUsesUniformMember(material->customShader.shader->codes.front(),
                                        "g_ModelViewProjectionMatrix"));
    const auto texture_binding =
        ShaderDescriptorBinding(material->customShader.shader->codes, "g_Texture0");
    ASSERT_TRUE(texture_binding.has_value());
    EXPECT_EQ(texture_binding->descriptorType, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
    EXPECT_NE(texture_binding->binding, 0u);
    const auto descriptor_bindings = ShaderDescriptorBindings(material->customShader.shader->codes);
    ASSERT_FALSE(descriptor_bindings.empty());
    for (const auto& binding : descriptor_bindings) {
        if (binding.descriptorType == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            EXPECT_NE(binding.binding, texture_binding->binding);
        }
    }
    EXPECT_EQ(material->blenmode, BlendMode::Translucent);
}

TEST(TextObjectRuntime, ExplicitTextObjectSizeIsMinimumRuntimeTextureDimensions) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;

    ProjectProperties properties;
    SceneParseRequest request {
        .scene_id           = "text-explicit-size-texture",
        .project_properties = &properties,
    };
    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "clock",
            "text": "12:34",
            "font": "Arial",
            "pointsize": 20,
            "size": [320, 90],
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "clock");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    auto* material = node->Mesh()->MaterialForSlot(0);
    ASSERT_NE(material, nullptr);
    ASSERT_EQ(material->textures.size(), 1u);

    auto image = scene->imageParser->Parse(material->textures.front());
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->header.width, 320);
    EXPECT_EQ(image->header.height, 90);

    ASSERT_TRUE(scene->runtime->SetNodeText("clock", "12:34:56 PM UTC"));
    scene->runtime->PumpTextLayerCache();

    auto updated = scene->imageParser->Parse(material->textures.front());
    ASSERT_NE(updated, nullptr);
    EXPECT_GT(updated->header.width, 320);
    EXPECT_EQ(updated->header.height, 90);
    const auto runtime_size = scene->runtime->NodeSize("clock");
    EXPECT_FLOAT_EQ(runtime_size.x(), 320.0f);
    EXPECT_FLOAT_EQ(runtime_size.y(), 90.0f);
    const auto mesh_size = MeshSize(*node->Mesh());
    EXPECT_GT(mesh_size.x(), runtime_size.x());
    EXPECT_NEAR(mesh_size.y(), runtime_size.y(), 1.0f);
}

TEST(TextObjectRuntime, ExplicitTextObjectSizeRemainsLogicalFrameWhenRasterExpands) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;

    ProjectProperties properties;
    SceneParseRequest request {
        .scene_id           = "text-explicit-layout-frame",
        .project_properties = &properties,
    };
    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": "Saturday",
            "font": "Arial",
            "pointsize": 20,
            "size": [180, 60],
            "origin": [300, 100, 0],
            "scale": [2, 3, 1],
            "horizontalalign": "right",
            "verticalalign": "bottom",
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);

    const auto logical_size = scene->runtime->NodeSize("caption");
    EXPECT_FLOAT_EQ(logical_size.x(), 180.0f);
    EXPECT_FLOAT_EQ(logical_size.y(), 60.0f);
    EXPECT_NEAR(node->Translate().x(), 300.0f - 180.0f, 1.0e-4f);
    EXPECT_NEAR(node->Translate().y(), 100.0f + 90.0f, 1.0e-4f);

    const auto initial_mesh_size = MeshSize(*node->Mesh());
    EXPECT_GT(initial_mesh_size.x(), logical_size.x());
    EXPECT_GT(initial_mesh_size.y(), logical_size.y());
    const auto initial_mesh_bounds = MeshLocalBounds(*node->Mesh());
    EXPECT_NEAR(initial_mesh_bounds.max_x, logical_size.x() * 0.5f, 1.0e-4f);
    EXPECT_NEAR(initial_mesh_bounds.min_y, -logical_size.y() * 0.5f, 1.0e-4f);

    ASSERT_TRUE(scene->runtime->SetNodeText("caption", "Saturday Saturday Saturday"));
    const auto resized_logical_size = scene->runtime->NodeSize("caption");
    EXPECT_FLOAT_EQ(resized_logical_size.x(), 180.0f);
    EXPECT_FLOAT_EQ(resized_logical_size.y(), 60.0f);
    EXPECT_NEAR(node->Translate().x(), 300.0f - 180.0f, 1.0e-4f);
    EXPECT_NEAR(node->Translate().y(), 100.0f + 90.0f, 1.0e-4f);

    const auto resized_mesh_size = MeshSize(*node->Mesh());
    EXPECT_GT(resized_mesh_size.x(), initial_mesh_size.x());
    EXPECT_NEAR(resized_mesh_size.y(), initial_mesh_size.y(), 1.0f);
    const auto resized_mesh_bounds = MeshLocalBounds(*node->Mesh());
    EXPECT_NEAR(resized_mesh_bounds.max_x, logical_size.x() * 0.5f, 1.0e-4f);
    EXPECT_NEAR(resized_mesh_bounds.min_y, -logical_size.y() * 0.5f, 1.0e-4f);
    EXPECT_TRUE(scene->runtime->ConsumeSceneGraphMutationFlag());
}

TEST(TextObjectRuntime, TextStyleColorAlphaAndBrightnessTintRuntimeTexture) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;

    ProjectProperties properties;
    SceneParseRequest request {
        .scene_id           = "text-style-tint",
        .project_properties = &properties,
    };
    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": "IIII",
            "font": "Arial",
            "pointsize": 20,
            "color": [0.25, 0.5, 0.75],
            "brightness": 0.5,
            "alpha": 0.25,
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    auto* material = node->Mesh()->MaterialForSlot(0);
    ASSERT_NE(material, nullptr);
    ASSERT_EQ(material->textures.size(), 1u);

    auto image = scene->imageParser->Parse(material->textures.front());
    ASSERT_NE(image, nullptr);
    ASSERT_EQ(image->slots.size(), 1u);
    ASSERT_EQ(image->slots[0].mipmaps.size(), 1u);
    const auto& mip = image->slots[0].mipmaps[0];
    ASSERT_NE(mip.data, nullptr);

    bool saw_tinted_pixel         = false;
    bool saw_straight_alpha_pixel = false;
    int  max_alpha                = 0;
    for (int y = 0; y < mip.height; ++y) {
        for (int x = 0; x < mip.width; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * mip.width + static_cast<std::size_t>(x)) * 4u;
            const auto alpha = mip.data.get()[offset + 3u];
            max_alpha        = std::max(max_alpha, static_cast<int>(alpha));
            if (alpha == 0u || saw_tinted_pixel) continue;

            EXPECT_LT(mip.data.get()[offset + 0u], mip.data.get()[offset + 1u]);
            EXPECT_LT(mip.data.get()[offset + 1u], mip.data.get()[offset + 2u]);
            saw_tinted_pixel = true;
        }
    }
    for (int y = 0; y < mip.height && ! saw_straight_alpha_pixel; ++y) {
        for (int x = 0; x < mip.width; ++x) {
            const auto offset =
                (static_cast<std::size_t>(y) * mip.width + static_cast<std::size_t>(x)) * 4u;
            const auto alpha = mip.data.get()[offset + 3u];
            if (alpha < 48u) continue;

            EXPECT_GE(mip.data.get()[offset + 0u], 24u);
            EXPECT_GE(mip.data.get()[offset + 1u], 48u);
            EXPECT_GE(mip.data.get()[offset + 2u], 72u);
            saw_straight_alpha_pixel = true;
            break;
        }
    }
    EXPECT_TRUE(saw_tinted_pixel);
    EXPECT_TRUE(saw_straight_alpha_pixel);
    EXPECT_LE(max_alpha, 64);
}

TEST(TextObjectRuntime, RasterizedTextUsesFontPixelSizeAndAntialiasesEdges) {
    TextLayerState state {
        .text                   = "Hg",
        .font_key               = "systemfont_Helvetica",
        .resolved_font_kind     = "system",
        .resolved_font_identity = "Helvetica",
        .resolved_font_path     = ResolveSystemFontPath("systemfont_Helvetica"),
        .point_size             = 40.0f,
        .padding                = 4.0f,
    };
#ifdef __APPLE__
    ASSERT_FALSE(state.resolved_font_path.empty());
#endif

    constexpr uint32_t   width  = 420;
    constexpr uint32_t   height = 220;
    std::vector<uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u, 0u);
    RasterizeTextLayer(state, width, height, rgba);

    int  min_y                  = static_cast<int>(height);
    int  max_y                  = -1;
    bool has_partial_alpha_edge = false;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto alpha = rgba[(static_cast<std::size_t>(y) * width + x) * 4u + 3u];
            if (alpha == 0u) continue;
            min_y                  = std::min(min_y, static_cast<int>(y));
            max_y                  = std::max(max_y, static_cast<int>(y));
            has_partial_alpha_edge = has_partial_alpha_edge || (alpha > 0u && alpha < 255u);
        }
    }

    ASSERT_GE(max_y, min_y);
    EXPECT_GT(max_y - min_y + 1, 110);
    EXPECT_TRUE(has_partial_alpha_edge);
}

TEST(TextObjectRuntime, SansSerifAliasRasterizesClockDigits) {
    TextLayerState state {
        .text                   = "12:34",
        .font_key               = "systemfont_sansserif",
        .resolved_font_kind     = "system",
        .resolved_font_identity = "sansserif",
        .resolved_font_path     = ResolveSystemFontPath("systemfont_sansserif"),
        .point_size             = 33.0f,
        .padding                = 32.0f,
        .explicit_size          = Eigen::Vector2f(342.0f, 156.0f),
        .color                  = Eigen::Vector3f(1.0f, 0.81176f, 0.87059f),
        .horizontal_align       = "center",
        .vertical_align         = "center",
    };
#ifdef __APPLE__
    ASSERT_FALSE(state.resolved_font_path.empty());
#endif

    const auto     size   = TextLayerRasterSize(state);
    const uint32_t width  = static_cast<uint32_t>(std::ceil(size.x()));
    const uint32_t height = static_cast<uint32_t>(std::ceil(size.y()));
    ASSERT_GT(width, 0u);
    ASSERT_GT(height, 0u);

    std::vector<uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u, 0u);
    RasterizeTextLayer(state, width, height, rgba);

    int visible_pixels = 0;
    int max_alpha      = 0;
    for (std::size_t offset = 3; offset < rgba.size(); offset += 4u) {
        const auto alpha = rgba[offset];
        if (alpha == 0u) continue;
        ++visible_pixels;
        max_alpha = std::max(max_alpha, static_cast<int>(alpha));
    }

    EXPECT_GT(visible_pixels, 200);
    EXPECT_GT(max_alpha, 128);
}

TEST(TextObjectRuntime, ParserResolvesFamilyFontsForFreeTypeRasterization) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;

    ProjectProperties properties;
    SceneParseRequest request {
        .scene_id           = "text-family-freetype",
        .project_properties = &properties,
    };
    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": "Hg",
            "font": "Helvetica",
            "pointsize": 40,
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    const auto state = scene->runtime->NodeTextState("caption");
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->resolved_font_kind, "family");
#ifdef __APPLE__
    EXPECT_FALSE(state->resolved_font_path.empty());
#endif
    const auto runtime_size = scene->runtime->NodeSize("caption");
    EXPECT_GT(runtime_size.x(), 130.0f);
    EXPECT_GT(runtime_size.y(), 120.0f);
    EXPECT_LT(runtime_size.x(), 280.0f);
    EXPECT_LT(runtime_size.y(), 220.0f);

    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    const auto& vertex_array = node->Mesh()->GetVertexArray(0);
    ASSERT_EQ(vertex_array.VertexCount(), 4u);
    const float* vertices = vertex_array.Data();
    ASSERT_NE(vertices, nullptr);
    const auto stride = vertex_array.OneSize();
    float      min_x  = vertices[0];
    float      max_x  = vertices[0];
    float      min_y  = vertices[1];
    float      max_y  = vertices[1];
    for (std::size_t index = 1; index < vertex_array.VertexCount(); ++index) {
        const float x = vertices[index * stride + 0u];
        const float y = vertices[index * stride + 1u];
        min_x         = std::min(min_x, x);
        max_x         = std::max(max_x, x);
        min_y         = std::min(min_y, y);
        max_y         = std::max(max_y, y);
    }
    EXPECT_NEAR(max_x - min_x, runtime_size.x(), 1.0f);
    EXPECT_NEAR(max_y - min_y, runtime_size.y(), 1.0f);

    auto* material = node->Mesh()->MaterialForSlot(0);
    ASSERT_NE(material, nullptr);
    ASSERT_EQ(material->textures.size(), 1u);
    auto image = scene->imageParser->Parse(material->textures.front());
    ASSERT_NE(image, nullptr);
    ASSERT_EQ(image->slots.size(), 1u);
    ASSERT_EQ(image->slots[0].mipmaps.size(), 1u);
    const auto& mip = image->slots[0].mipmaps[0];
    ASSERT_NE(mip.data, nullptr);

    bool has_partial_alpha_edge = false;
    for (int y = 0; y < mip.height && ! has_partial_alpha_edge; ++y) {
        for (int x = 0; x < mip.width; ++x) {
            const auto alpha =
                mip.data.get()[(static_cast<std::size_t>(y) * mip.width + x) * 4u + 3u];
            if (alpha > 0u && alpha < 255u) {
                has_partial_alpha_edge = true;
                break;
            }
        }
    }
    EXPECT_TRUE(has_partial_alpha_edge);
}

TEST(TextObjectRuntime, FreeTypeEstimatedSizeContainsTallGlyphs) {
    TextLayerState state {
        .text                   = "Hg",
        .font_key               = "systemfont_Helvetica",
        .resolved_font_kind     = "system",
        .resolved_font_identity = "Helvetica",
        .resolved_font_path     = ResolveSystemFontPath("systemfont_Helvetica"),
        .point_size             = 40.0f,
        .padding                = 4.0f,
    };
#ifdef __APPLE__
    ASSERT_FALSE(state.resolved_font_path.empty());
#endif

    const auto size = TextLayerRasterSize(state);
    EXPECT_GT(size.x(), 130.0f);
    EXPECT_GT(size.y(), 120.0f);
    EXPECT_LT(size.x(), 280.0f);
    EXPECT_LT(size.y(), 220.0f);

    std::vector<uint8_t> rgba(static_cast<std::size_t>(std::ceil(size.x())) *
                                  static_cast<std::size_t>(std::ceil(size.y())) * 4u,
                              0u);
    RasterizeTextLayer(state,
                       static_cast<uint32_t>(std::ceil(size.x())),
                       static_cast<uint32_t>(std::ceil(size.y())),
                       rgba);

    bool has_visible_alpha = false;
    for (std::size_t offset = 3; offset < rgba.size(); offset += 4u) {
        if (rgba[offset] > 0u) {
            has_visible_alpha = true;
            break;
        }
    }
    EXPECT_TRUE(has_visible_alpha);
}

TEST(TextObjectRuntime, TextRasterExpandsBeyondExplicitBoxWhenNeeded) {
    TextLayerState state {
        .text                   = "Saturday",
        .font_key               = "systemfont_Helvetica",
        .resolved_font_kind     = "system",
        .resolved_font_identity = "Helvetica",
        .resolved_font_path     = ResolveSystemFontPath("systemfont_Helvetica"),
        .point_size             = 20.0f,
        .padding                = 0.0f,
        .explicit_size          = Eigen::Vector2f(180.0f, 60.0f),
        .horizontal_align       = "left",
        .vertical_align         = "top",
    };
#ifdef __APPLE__
    ASSERT_FALSE(state.resolved_font_path.empty());
#endif

    const auto size = TextLayerRasterSize(state);
    EXPECT_GT(size.x(), 220.0f);
    EXPECT_GT(size.y(), 70.0f);

    const uint32_t       width  = static_cast<uint32_t>(std::ceil(size.x()));
    const uint32_t       height = static_cast<uint32_t>(std::ceil(size.y()));
    std::vector<uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u, 0u);
    RasterizeTextLayer(state, width, height, rgba);

    int min_x = static_cast<int>(width);
    int max_x = -1;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto alpha = rgba[(static_cast<std::size_t>(y) * width + x) * 4u + 3u];
            if (alpha == 0u) continue;
            min_x = std::min(min_x, static_cast<int>(x));
            max_x = std::max(max_x, static_cast<int>(x));
        }
    }

    ASSERT_GE(max_x, min_x);
    EXPECT_LT(min_x, 8);
    EXPECT_GT(max_x, 220);
    EXPECT_LT(max_x, static_cast<int>(width));
}

TEST(TextObjectRuntime, DynamicTextTransformSettingsUpdateRuntimeNode) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties {
          { "text_origin", RuntimeScalarValue::String("30 40 0") },
          { "text_scale", RuntimeScalarValue::Float(2.5f) },
    };
    SceneParseRequest request {
        .scene_id           = "text-dynamic-transform",
        .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": "clock",
            "font": "Arial",
            "pointsize": 20,
            "origin": {"value": "10 20 0", "user": "text_origin"},
            "scale": {"value": "1 1 1", "user": "text_scale"},
            "horizontalalign": "center",
            "verticalalign": "center",
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);

    EXPECT_FLOAT_EQ(node->Translate().x(), 30.0f);
    EXPECT_FLOAT_EQ(node->Translate().y(), 40.0f);
    EXPECT_FLOAT_EQ(node->Scale().x(), 2.5f);
    EXPECT_FLOAT_EQ(node->Scale().y(), 2.5f);

    scene->runtime->ApplyProjectPropertyOverride({
        { "text_origin", RuntimeScalarValue::String("50 60 0") },
        { "text_scale", RuntimeScalarValue::Float(3.0f) },
    });
    scene->runtime->Tick(1.0 / 60.0);

    EXPECT_FLOAT_EQ(node->Translate().x(), 50.0f);
    EXPECT_FLOAT_EQ(node->Translate().y(), 60.0f);
    EXPECT_FLOAT_EQ(node->Scale().x(), 3.0f);
    EXPECT_FLOAT_EQ(node->Scale().y(), 3.0f);
}

TEST(TextObjectRuntime, DynamicTextTransformScriptsUseCanvasSize) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties {
          { "x", RuntimeScalarValue::Float(0.8f) },
          { "y", RuntimeScalarValue::Float(0.25f) },
    };
    SceneParseRequest request {
        .scene_id           = "text-dynamic-transform-script",
        .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              R"JSON({
          "camera": {"center":[0,0,0], "eye":[0,0,1], "up":[0,1,0]},
          "general": {
            "ambientcolor":[0,0,0], "skylightcolor":[0,0,0],
            "clearcolor":[0,0,0], "cameraparallax":false,
            "cameraparallaxamount":0, "cameraparallaxdelay":0,
            "cameraparallaxmouseinfluence":0,
            "orthogonalprojection":{"width":1000,"height":500}
          },
          "objects": [
            {
              "id": 1,
              "name": "caption",
              "text": "clock",
              "font": "Arial",
              "pointsize": 20,
              "origin": {
                "value": "0 0 0",
                "scriptproperties": {
                  "x": {"user": "x", "value": 0.5},
                  "y": {"user": "y", "value": 0.5}
                },
                "script": "export var scriptProperties = createScriptProperties().addSlider({name:'x',value:0.5}).addSlider({name:'y',value:0.5}).finish(); export function update(value) { value.x = scriptProperties.x * engine.canvasSize.x; value.y = scriptProperties.y * engine.canvasSize.y; return value; }"
              },
              "horizontalalign": "center",
              "verticalalign": "center",
              "visible": true
            }
          ]
        })JSON",
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);

    scene->runtime->Tick(1.0 / 60.0);

    EXPECT_FLOAT_EQ(node->Translate().x(), 800.0f);
    EXPECT_FLOAT_EQ(node->Translate().y(), 125.0f);
    EXPECT_EQ(scene->runtime->scriptErrorCount(), 0u);
}

TEST(TextObjectRuntime, ParserUsesUniqueTextureKeysForDuplicateTextLayerNames) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;

    ProjectProperties properties;
    SceneParseRequest request {
        .scene_id           = "duplicate-text-names",
        .project_properties = &properties,
    };
    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "duplicate",
            "text": "first",
            "font": "Arial",
            "pointsize": 20,
            "visible": true
          },
          {
            "id": 2,
            "name": "duplicate",
            "text": "second much wider",
            "font": "Arial",
            "pointsize": 20,
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->sceneGraph, nullptr);
    auto* first  = FindRootChild(*scene, "__we_text_1");
    auto* second = FindRootChild(*scene, "__we_text_2");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(first->Mesh(), nullptr);
    ASSERT_NE(second->Mesh(), nullptr);
    auto* first_material  = first->Mesh()->MaterialForSlot(0);
    auto* second_material = second->Mesh()->MaterialForSlot(0);
    ASSERT_NE(first_material, nullptr);
    ASSERT_NE(second_material, nullptr);
    ASSERT_EQ(first_material->textures.size(), 1u);
    ASSERT_EQ(second_material->textures.size(), 1u);

    EXPECT_NE(first_material->textures.front(), second_material->textures.front());
    EXPECT_TRUE(scene->runtime->HasNodeNamed("__we_text_1"));
    EXPECT_TRUE(scene->runtime->HasNodeNamed("__we_text_2"));
    EXPECT_EQ(scene->runtime->NodeText("__we_text_1"), "first");
    EXPECT_EQ(scene->runtime->NodeText("__we_text_2"), "second much wider");
}

TEST(TextObjectRuntime, ParserReadsSupportedTextFormsIntoRuntimeState) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-forms",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "plain", "text": "plain text", "font": "Arial", "visible": true},
          {"id": 2, "name": "nested", "text": {"text": "nested text"}, "font": {"value": "Arial"}, "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_EQ(scene->runtime->NodeText("plain"), "plain text");
    EXPECT_EQ(scene->runtime->NodeText("nested"), "nested text");
}

TEST(TextObjectRuntime, ParserResolvesFontFamiliesAssetsAndSystemFontAliases) {
    fs::VFS vfs;
    MountAssets(vfs,
                {
                    { "/fonts/asset.ttf", "fake-font-bytes" },
                    { "/fonts/prefixed.ttf", "fake-font-bytes" },
                });
    ASSERT_TRUE(vfs.Mount("/provided",
                          std::make_unique<MemoryFs>(std::map<std::string, std::string> {
                              { "/absolute.otf", "fake-font-bytes" },
                          })));
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-fonts",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "family", "text": "a", "font": "Arial", "visible": true},
          {"id": 2, "name": "asset", "text": "a", "font": "fonts/asset.ttf", "visible": true},
          {"id": 3, "name": "system", "text": "a", "font": "systemfont_Helvetica", "visible": true},
          {"id": 4, "name": "provided", "text": "a", "font": "/provided/absolute.otf", "visible": true},
          {"id": 5, "name": "prefixed", "text": "a", "font": "assets/fonts/prefixed.ttf", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);

    auto family = scene->runtime->NodeTextState("family");
    ASSERT_TRUE(family.has_value());
    EXPECT_EQ(family->resolved_font_kind, "family");
    EXPECT_EQ(family->resolved_font_identity, "Arial");

    auto asset = scene->runtime->NodeTextState("asset");
    ASSERT_TRUE(asset.has_value());
    EXPECT_EQ(asset->resolved_font_kind, "asset");
    EXPECT_EQ(asset->resolved_font_path, "/assets/fonts/asset.ttf");
    EXPECT_EQ(std::string(asset->resolved_font_data.begin(), asset->resolved_font_data.end()),
              "fake-font-bytes");

    auto system = scene->runtime->NodeTextState("system");
    ASSERT_TRUE(system.has_value());
    EXPECT_EQ(system->resolved_font_kind, "system");
    EXPECT_EQ(system->resolved_font_identity, "Helvetica");

    auto provided = scene->runtime->NodeTextState("provided");
    ASSERT_TRUE(provided.has_value());
    EXPECT_EQ(provided->resolved_font_kind, "asset");
    EXPECT_EQ(provided->resolved_font_path, "/provided/absolute.otf");
    EXPECT_EQ(std::string(provided->resolved_font_data.begin(), provided->resolved_font_data.end()),
              "fake-font-bytes");

    auto prefixed = scene->runtime->NodeTextState("prefixed");
    ASSERT_TRUE(prefixed.has_value());
    EXPECT_EQ(prefixed->resolved_font_kind, "asset");
    EXPECT_EQ(prefixed->resolved_font_path, "/assets/fonts/prefixed.ttf");
}

TEST(TextObjectRuntime, ParserMapsSystemFontAliasesToPlatformFontPath) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-system-font-path",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "system", "text": "a", "font": "systemfont_Helvetica", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);

    auto system = scene->runtime->NodeTextState("system");
    ASSERT_TRUE(system.has_value());
    EXPECT_EQ(system->resolved_font_kind, "system");
    EXPECT_EQ(system->resolved_font_identity, "Helvetica");
#ifdef __APPLE__
    EXPECT_FALSE(system->resolved_font_path.empty());
#else
    EXPECT_TRUE(system->resolved_font_path.empty());
#endif
}

TEST(TextObjectRuntime, HiddenTextStillCreatesNodeAndRuntimeState) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "hidden-text",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "hiddenCaption", "text": "secret", "font": "Arial", "visible": false}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_TRUE(scene->runtime->HasNodeNamed("hiddenCaption"));
    EXPECT_EQ(scene->runtime->NodeText("hiddenCaption"), "secret");
    auto* node = FindRootChild(*scene, "hiddenCaption");
    ASSERT_NE(node, nullptr);
    EXPECT_FALSE(node->Visible());
}

TEST(TextObjectRuntime, TextVisibleScriptUpdatesVisibilityOnTick) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-visible-script",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": "scripted",
            "font": "Arial",
            "visible": {
              "value": true,
              "script": "export function update(value) { return false; }"
            }
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);

    scene->runtime->Tick(1.0 / 60.0);
    EXPECT_FALSE(node->Visible());
    EXPECT_EQ(scene->runtime->scriptErrorCount(), 0u);
}

TEST(TextObjectRuntime, TextVisibleUserBindingFollowsProjectOverride) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties {
          { "show_text", RuntimeScalarValue::Bool(true) },
    };
    SceneParseRequest request {
        .scene_id           = "text-visible-user",
        .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": "user",
            "font": "Arial",
            "visible": {"value": true, "user": "show_text"}
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    EXPECT_TRUE(node->Visible());

    scene->runtime->ApplyProjectPropertyOverride({
        { "show_text", RuntimeScalarValue::Bool(false) },
    });
    scene->runtime->Tick(1.0 / 60.0);
    EXPECT_FALSE(node->Visible());
}

TEST(TextObjectRuntime, TextFieldScriptUpdatesRuntimeTextOnTick) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-field-script",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": {
              "value": "before",
              "script": "export function update(value) { return value + ' after'; }"
            },
            "font": "Arial",
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_EQ(scene->runtime->NodeText("caption"), "before");

    scene->runtime->Tick(1.0 / 60.0);

    EXPECT_EQ(scene->runtime->NodeText("caption"), "before after");
    EXPECT_EQ(scene->runtime->scriptErrorCount(), 0u);
}

TEST(TextObjectRuntime, StaticObjectTextDoesNotOverwriteRuntimeMutationOnTick) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-static-object-field",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "caption",
            "text": {"text": "before"},
            "font": "Arial",
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_EQ(scene->runtime->NodeText("caption"), "before");

    scene->runtime->SetNodeText("caption", "after");
    scene->runtime->Tick(1.0 / 60.0);

    EXPECT_EQ(scene->runtime->NodeText("caption"), "after");
    EXPECT_EQ(scene->runtime->scriptErrorCount(), 0u);
}

TEST(TextObjectRuntime, EventOnlyTextScriptDoesNotOverwriteRuntimeMutationOnTick) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-event-script-object-field",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"JSON([
          {
            "id": 1,
            "name": "caption",
            "text": {
              "value": "before",
              "script": "engine.on('custom', function() {})"
            },
            "font": "Arial",
            "visible": true
          }
        ])JSON"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_EQ(scene->runtime->NodeText("caption"), "before");

    scene->runtime->SetNodeText("caption", "after");
    scene->runtime->Tick(1.0 / 60.0);

    EXPECT_EQ(scene->runtime->NodeText("caption"), "after");
    EXPECT_EQ(scene->runtime->scriptErrorCount(), 0u);
}

TEST(TextObjectRuntime, EventOnlyTextScriptRegistersAsSceneScript) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-event-script-scene-script",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"JSON([
          {
            "id": 1,
            "name": "caption",
            "text": {
              "value": "before",
              "script": "engine.on('cursorDown', function() { thisLayer.text = 'clicked'; })"
            },
            "font": "Arial",
            "visible": true
          }
        ])JSON"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_EQ(scene->runtime->NodeText("caption"), "before");
    EXPECT_EQ(scene->runtime->sceneScriptCount(), 1u);

    scene->runtime->DispatchCursorDown(0);

    EXPECT_EQ(scene->runtime->NodeText("caption"), "clicked");
    EXPECT_EQ(scene->runtime->scriptErrorCount(), 0u);
}

TEST(TextObjectRuntime, TextParentReusesPlaceholderWhenChildAppearsFirst) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-placeholder",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 2, "name": "childText", "parent": 1, "text": "child", "font": "Arial", "visible": true},
          {"id": 1, "name": "parentText", "text": "parent", "font": "Arial", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    auto* parent = FindRootChild(*scene, "parentText");
    ASSERT_NE(parent, nullptr);
    EXPECT_NE(parent->Mesh(), nullptr);
    ASSERT_EQ(parent->GetChildren().size(), 1u);
    EXPECT_EQ(parent->GetChildren().front()->Name(), "childText");
    EXPECT_EQ(parent->GetChildren().front()->Parent(), parent);
}

TEST(TextObjectRuntime, UnnamedTextPlaceholderDoesNotLeaveStaleRuntimeLayerKey) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "unnamed-text-placeholder",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 2, "name": "childText", "parent": 1, "text": "child", "font": "Arial", "visible": true},
          {"id": 1, "text": "parent", "font": "Arial", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    EXPECT_FALSE(scene->runtime->HasNodeNamed("__we_layer_1"));
    EXPECT_TRUE(scene->runtime->HasNodeNamed("__we_text_1"));
    EXPECT_EQ(scene->runtime->NodeText("__we_text_1"), "parent");
}

TEST(TextObjectRuntime, TextStateUsesRuntimeRgbaTextureBackend) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {});
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNode("caption", node.get());
    runtime->RegisterTextLayer("caption",
                               TextLayerState {
                                   .text       = "layout only",
                                   .font_key   = "Arial",
                                   .point_size = 12.0f,
                               });

    const auto state = runtime->NodeTextState("caption");
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->render_backend, "runtime-rgba-texture");
}

TEST(TextObjectRuntime, RuntimeTextMutationUpdatesPersistentTextAndSize) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {});
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNode("caption", node.get());
    runtime->RegisterTextLayer("caption",
                               TextLayerState {
                                   .text             = "a",
                                   .font_key         = "Arial",
                                   .point_size       = 10.0f,
                                   .padding          = 2.0f,
                                   .horizontal_align = "left",
                                   .vertical_align   = "top",
                                   .anchor           = "left top",
                               });

    const auto before       = runtime->NodeSize("caption");
    const auto before_state = runtime->NodeTextState("caption");
    ASSERT_TRUE(before_state.has_value());
    EXPECT_FALSE(before_state->texture_cache_key.empty());
    EXPECT_EQ(before_state->cache_revision, 1u);
    ASSERT_TRUE(runtime->SetNodeText("caption", "longer text"));

    EXPECT_EQ(runtime->NodeText("caption"), "longer text");
    const auto after = runtime->NodeSize("caption");
    EXPECT_GT(after.x(), before.x());
    EXPECT_FLOAT_EQ(after.y(), before.y());
    EXPECT_TRUE(runtime->NodeTextDirty("caption"));
    const auto dirty_state = runtime->NodeTextState("caption");
    ASSERT_TRUE(dirty_state.has_value());
    EXPECT_EQ(dirty_state->cache_revision, before_state->cache_revision + 1u);
    EXPECT_TRUE(dirty_state->cache_dirty);
    EXPECT_TRUE(dirty_state->full_dirty);

    runtime->ClearNodeTextDirty("caption");
    const auto clean_state = runtime->NodeTextState("caption");
    ASSERT_TRUE(clean_state.has_value());
    EXPECT_EQ(clean_state->text, "longer text");
    EXPECT_EQ(clean_state->cache_revision, dirty_state->cache_revision);
    EXPECT_FALSE(clean_state->cache_dirty);
    EXPECT_FALSE(clean_state->full_dirty);
    EXPECT_FALSE(runtime->NodeTextDirty("caption"));
}

TEST(TextObjectRuntime, UnchangedRuntimeTextMutationSkipsDirtyAndCacheRevision) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {});
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNode("caption", node.get());
    runtime->RegisterTextLayer("caption",
                               TextLayerState {
                                   .text             = "12:34",
                                   .font_key         = "systemfont_sansserif",
                                   .point_size       = 33.0f,
                                   .padding          = 32.0f,
                                   .explicit_size    = Eigen::Vector2f(342.0f, 156.0f),
                                   .horizontal_align = "center",
                                   .vertical_align   = "center",
                               });
    runtime->RegisterTextValue("caption", std::make_unique<DynamicValue>("12:34"));
    runtime->ClearNodeTextDirty("caption");
    ASSERT_FALSE(runtime->ConsumeSceneGraphMutationFlag());

    const auto before_state = runtime->NodeTextState("caption");
    ASSERT_TRUE(before_state.has_value());
    ASSERT_FALSE(before_state->cache_dirty);
    ASSERT_FALSE(before_state->full_dirty);

    ResetTextLayerMeasurementCountForTests();
    runtime->Tick(1.0 / 60.0);

    const auto after_state = runtime->NodeTextState("caption");
    ASSERT_TRUE(after_state.has_value());
    EXPECT_EQ(TextLayerMeasurementCountForTests(), 0u);
    EXPECT_EQ(after_state->cache_revision, before_state->cache_revision);
    EXPECT_FALSE(after_state->cache_dirty);
    EXPECT_FALSE(after_state->full_dirty);
    EXPECT_FALSE(runtime->NodeTextDirty("caption"));
    EXPECT_FALSE(runtime->ConsumeSceneGraphMutationFlag());
}

TEST(TextObjectRuntime, ClockScriptUpdatesSameSizeTextOnceThenSkipsMeasurements) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-clock-script-same-size",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {
            "id": 1,
            "name": "Clock",
            "text": {
              "value": "06:15",
              "script": "export function update(value) { return '12:34'; }"
            },
            "font": "systemfont_sansserif",
            "pointsize": 33,
            "padding": 32,
            "size": "342 156",
            "horizontalalign": "center",
            "verticalalign": "center",
            "visible": true
          }
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* runtime = scene->runtime.get();

    EXPECT_EQ(runtime->NodeText("Clock"), "06:15");
    runtime->ClearNodeTextDirty("Clock");
    ASSERT_FALSE(runtime->NodeTextDirty("Clock"));

    runtime->Tick(1.0 / 60.0);
    EXPECT_EQ(runtime->NodeText("Clock"), "12:34");
    EXPECT_TRUE(runtime->NodeTextDirty("Clock"));
    const auto changed_state = runtime->NodeTextState("Clock");
    ASSERT_TRUE(changed_state.has_value());
    EXPECT_TRUE(changed_state->cache_dirty);

    runtime->ClearNodeTextDirty("Clock");
    ASSERT_FALSE(runtime->NodeTextDirty("Clock"));
    ResetTextLayerMeasurementCountForTests();
    runtime->Tick(1.0 / 60.0);

    EXPECT_EQ(runtime->NodeText("Clock"), "12:34");
    EXPECT_EQ(TextLayerMeasurementCountForTests(), 0u);
    EXPECT_FALSE(runtime->NodeTextDirty("Clock"));
}

TEST(TextObjectRuntime, Workshop3409533530ClockParentKeepsVisibleRuntimeTexture) {
    fs::VFS vfs;
    MountAssets(vfs,
                {
                    {
                        "/fonts/workshop/3261114750/SourceHanSansCN-Normal.otf",
                        "not-used-by-clock",
                    },
                });
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties {
          { "clock", RuntimeScalarValue::Bool(true) },
          { "time", RuntimeScalarValue::Bool(true) },
          { "x", RuntimeScalarValue::Float(0.8f) },
          { "y", RuntimeScalarValue::Float(0.25f) },
          { "newproperty1", RuntimeScalarValue::String("1 0.8117647 0.8705882") },
          { "newproperty2", RuntimeScalarValue::Float(2.5f) },
    };
    SceneParseRequest request {
          .scene_id           = "workshop-3409533530-clock-repro",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              R"JSON({
          "camera": {"center":"0.00000 0.00000 -1.00000", "eye":"0.00000 0.00000 0.00000", "up":"0.00000 1.00000 0.00000"},
          "general": {
            "ambientcolor":"0.30000 0.30000 0.30000",
            "skylightcolor":"0.30000 0.30000 0.30000",
            "clearcolor":"0.70000 0.70000 0.70000",
            "clearenabled":true,
            "cameraparallax":false,
            "cameraparallaxamount":0,
            "cameraparallaxdelay":0,
            "cameraparallaxmouseinfluence":0,
            "orthogonalprojection":{"width":3840,"height":2160}
          },
          "objects": [
            {
              "id": 31,
              "name": "Clock",
              "text": {
                "value": "06:15",
                "scriptproperties": {
                  "delimiter": ":",
                  "showCD": false,
                  "showTime": false,
                  "use12hFormat": {"user": "show12h", "value": false}
                },
                "script": "export var scriptProperties = createScriptProperties().addCheckbox({name:'use12hFormat',value:false}).addText({name:'delimiter',value:':'}).addCheckbox({name:'showTime',value:false}).addCheckbox({name:'showCD',value:false}).finish(); export function update(value) { let time = new Date(); var hours = time.getHours(); if (scriptProperties.use12hFormat) { hours %= 12; if (hours == 0) hours = 12; } hours = ('00' + hours).slice(-2); let minutes = ('00' + time.getMinutes()).slice(-2); value = hours + scriptProperties.delimiter + minutes; if (scriptProperties.showTime) value = '\\0'; if (scriptProperties.showCD) value = '\\0'; return value; }"
              },
              "font": "systemfont_sansserif",
              "pointsize": 33.0,
              "padding": 32,
              "size": "342.00000 156.00000",
              "origin": {
                "value": "0.00000 0.00000 0.00000",
                "scriptproperties": {
                  "x": {"user": "x", "value": 1},
                  "y": {"user": "y", "value": 1}
                },
                "script": "export var scriptProperties = createScriptProperties().addSlider({name:'x',value:0.5}).addSlider({name:'y',value:0.5}).finish(); export function update(value) { value.x = scriptProperties.x * engine.canvasSize.x; value.y = scriptProperties.y * engine.canvasSize.y; return value; }"
              },
              "scale": {"user": "newproperty2", "value": "2.50000 2.50000 2.50000"},
              "visible": {"user": "clock", "value": true},
              "color": {"user": "newproperty1", "value": "1.00000 0.81176 0.87059"},
              "horizontalalign": "center",
              "verticalalign": "center",
              "anchor": "none"
            },
            {
              "id": 32,
              "name": "Date",
              "parent": 31,
              "text": "DATE",
              "font": "systemfont_sansserif",
              "pointsize": 20.0,
              "padding": 36,
              "size": "987.00000 125.00000",
              "origin": "283.14212 -113.36209 0.00000",
              "scale": "0.28000 0.28000 0.00000",
              "visible": {"user": "time", "value": true},
              "horizontalalign": "right",
              "verticalalign": "center",
              "anchor": "none"
            },
            {
              "id": 33,
              "name": "Dy",
              "parent": 31,
              "text": "PM",
              "font": "systemfont_sansserif",
              "pointsize": 32.0,
              "padding": 32,
              "size": "190.00000 200.00000",
              "origin": "230.29727 -31.03779 0.00000",
              "scale": "0.61854 0.56577 0.43846",
              "horizontalalign": "center",
              "verticalalign": "center",
              "anchor": "none"
            }
          ]
        })JSON",
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* runtime = scene->runtime.get();

    auto* clock = FindRootChild(*scene, "Clock");
    ASSERT_NE(clock, nullptr);
    ASSERT_NE(clock->Mesh(), nullptr);
    ASSERT_EQ(clock->GetChildren().size(), 2u);
    EXPECT_EQ(clock->GetChildren().front()->Name(), "Date");
    EXPECT_EQ(clock->GetChildren().back()->Name(), "Dy");

    runtime->Tick(1.0 / 60.0);
    runtime->PumpTextLayerCache();

    EXPECT_TRUE(runtime->NodeVisible("Clock"));
    EXPECT_TRUE(clock->EffectiveVisible());
    EXPECT_NE(runtime->NodeText("Clock"), std::string("\0", 1));
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);

    const auto translate = runtime->NodeTranslate("Clock");
    const auto scale     = runtime->NodeScale("Clock");
    EXPECT_NEAR(translate.x(), 3072.0f, 1.0e-3f);
    EXPECT_NEAR(translate.y(), 540.0f, 1.0e-3f);
    EXPECT_NEAR(scale.x(), 2.5f, 1.0e-3f);
    EXPECT_NEAR(scale.y(), 2.5f, 1.0e-3f);

    auto state = runtime->NodeTextState("Clock");
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->resolved_font_kind, "system");
#ifdef __APPLE__
    EXPECT_FALSE(state->resolved_font_path.empty());
#endif

    auto* material = clock->Mesh()->MaterialForSlot(0);
    ASSERT_NE(material, nullptr);
    ASSERT_EQ(material->textures.size(), 1u);
    auto image = scene->imageParser->Parse(material->textures.front());
    ASSERT_NE(image, nullptr);
    EXPECT_GT(image->header.width, 300);
    EXPECT_GT(image->header.height, 140);
    EXPECT_GT(VisibleAlphaPixels(*image), 200);

    runtime->ClearNodeTextDirty("Clock");
    ResetTextLayerMeasurementCountForTests();
    runtime->Tick(1.0 / 60.0);
    EXPECT_EQ(TextLayerMeasurementCountForTests(), 0u);
}

TEST(TextObjectRuntime, Workshop3409533530FullSceneKeepsClockRenderPassAndTexture) {
    const auto home = HomePath();
    if (home.empty()) GTEST_SKIP() << "HOME is not set";

    const auto workshop_root =
        home / "Library/Application Support/Steam/steamapps/workshop/content/431960/3409533530";
    const auto project_json = ReadTextFile(workshop_root / "project.json");
    if (! project_json.has_value() || ! std::filesystem::exists(workshop_root / "scene.pkg")) {
        GTEST_SKIP() << "local workshop 3409533530 package is not available";
    }

    fs::VFS vfs;
    const auto common_assets =
        home / "Library/Application Support/Steam/steamapps/common/wallpaper_engine/assets";
    if (std::filesystem::exists(common_assets)) {
        ASSERT_TRUE(MountPhysicalAssets(vfs, common_assets));
    }
    ASSERT_TRUE(vfs.Mount("/assets", fs::WPPkgFs::CreatePkgFs((workshop_root / "scene.pkg").string())));
    auto scene_stream = vfs.Open("/assets/scene.json");
    ASSERT_NE(scene_stream, nullptr);
    const auto scene_json = scene_stream->ReadAllStr();

    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties {
          { "clock", RuntimeScalarValue::Bool(true) },
          { "time", RuntimeScalarValue::Bool(true) },
          { "x", RuntimeScalarValue::Float(0.8f) },
          { "y", RuntimeScalarValue::Float(0.25f) },
          { "newproperty1", RuntimeScalarValue::String("1 0.8117647 0.8705882") },
          { "newproperty2", RuntimeScalarValue::Float(2.5f) },
    };
    SceneParseRequest request {
          .scene_id           = "workshop-3409533530-full-scene-clock-repro",
          .project_path       = (workshop_root / "project.json").string(),
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request, scene_json, vfs, sound_manager);
    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);

    auto* clock = FindRootChild(*scene, "Clock");
    ASSERT_NE(clock, nullptr);
    ASSERT_NE(clock->Mesh(), nullptr);
    EXPECT_GE(clock->GetChildren().size(), 4u);

    auto* runtime = scene->runtime.get();
    runtime->Tick(1.0 / 60.0);
    runtime->PumpTextLayerCache();

    EXPECT_TRUE(runtime->NodeVisible("Clock"));
    EXPECT_TRUE(clock->EffectiveVisible());
    EXPECT_NE(runtime->NodeText("Clock"), std::string("\0", 1));
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);

    auto* material = clock->Mesh()->MaterialForSlot(0);
    ASSERT_NE(material, nullptr);
    ASSERT_EQ(material->textures.size(), 1u);
    EXPECT_EQ(material->textures.front(), TextTextureName("Clock"));
    auto image = scene->imageParser->Parse(material->textures.front());
    ASSERT_NE(image, nullptr);
    EXPECT_GT(VisibleAlphaPixels(*image), 200);

    const auto graph = sceneToRenderGraph(*scene);
    ASSERT_NE(graph, nullptr);
    const auto* clock_pass = FindCustomPassForNode(*graph, clock);
    ASSERT_NE(clock_pass, nullptr);
    ASSERT_EQ(clock_pass->desc().textures.size(), 1u);
    EXPECT_EQ(clock_pass->desc().textures.front(), TextTextureName("Clock"));
    EXPECT_EQ(clock_pass->desc().visibility_node, clock);
    EXPECT_EQ(clock_pass->desc().output, SpecTex_Default);
    EXPECT_NE(clock_pass->desc().uploaded_mesh_dirty_generation, clock->Mesh()->DirtyGeneration());

    runtime->ClearNodeTextDirty("Clock");
    ResetTextLayerMeasurementCountForTests();
    runtime->Tick(1.0 / 60.0);
    EXPECT_EQ(TextLayerMeasurementCountForTests(), 0u);
}

TEST(TextObjectRuntime, DynamicTextPassStartsWithPendingInitialZeroGenerationUpload) {
    SceneMesh mesh(true);
    ASSERT_TRUE(mesh.Dynamic());
    ASSERT_EQ(mesh.DirtyGeneration(), 0u);

    vulkan::CustomShaderPass::Desc desc;
    vulkan::CustomShaderPass       pass(desc);

    EXPECT_NE(pass.desc().uploaded_mesh_dirty_generation, mesh.DirtyGeneration());
}

TEST(TextObjectRuntime, RuntimeTextMutationResizesRenderableMesh) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-runtime-mesh-resize",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "caption", "text": "a", "font": "Arial", "pointsize": 10, "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    EXPECT_TRUE(node->Mesh()->Dynamic());

    auto mesh_width = [](const SceneMesh& mesh) {
        const auto& vertices = mesh.GetVertexArray(0);
        const auto  stride   = vertices.OneSize();
        const auto* data     = vertices.Data();
        float       min_x    = data[0];
        float       max_x    = data[0];
        for (std::size_t index = 1; index < vertices.VertexCount(); ++index) {
            const float x = data[index * stride];
            min_x         = std::min(min_x, x);
            max_x         = std::max(max_x, x);
        }
        return max_x - min_x;
    };

    const auto before_size  = scene->runtime->NodeSize("caption");
    const auto before_width = mesh_width(*node->Mesh());

    ASSERT_TRUE(scene->runtime->SetNodeText("caption", "aaaaaaaaaa"));

    const auto after_size  = scene->runtime->NodeSize("caption");
    const auto after_width = mesh_width(*node->Mesh());
    EXPECT_GT(after_size.x(), before_size.x());
    EXPECT_GT(after_width, before_width);
    EXPECT_NEAR(after_width, after_size.x(), 1.0f);
    EXPECT_TRUE(scene->runtime->ConsumeSceneGraphMutationFlag());
}

TEST(TextObjectRuntime, AlignedTextReanchorsWhenTextSizeChanges) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-reanchor",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "caption", "text": "a", "font": "Arial", "pointsize": 10, "origin": [100, 50, 0], "horizontalalign": "right", "verticalalign": "bottom", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    const auto before_translate = node->Translate();
    const auto before_size      = scene->runtime->NodeSize("caption");

    ASSERT_TRUE(scene->runtime->SetNodeText("caption", "aaaaaaaaaa"));

    const auto after_translate = node->Translate();
    const auto after_size      = scene->runtime->NodeSize("caption");
    EXPECT_GT(after_size.x(), before_size.x());
    EXPECT_LT(after_translate.x(), before_translate.x());
    EXPECT_FLOAT_EQ(after_translate.y(), before_translate.y());
    EXPECT_NEAR(after_translate.x(), 100.0f - after_size.x() * 0.5f, 1.0e-4f);
    EXPECT_NEAR(after_translate.y(), 50.0f + after_size.y() * 0.5f, 1.0e-4f);
}

TEST(TextObjectRuntime, ScaledTextAnchoringUsesRenderedSizeFromOriginalOrigin) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-scaled-reanchor",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "caption", "text": "aa", "font": "Arial", "pointsize": 10, "origin": [100, 50, 0], "scale": [2, 3, 1], "horizontalalign": "right", "verticalalign": "bottom", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);

    const auto size = scene->runtime->NodeSize("caption");
    EXPECT_NEAR(node->Translate().x(), 100.0f - size.x(), 1.0e-4f);
    EXPECT_NEAR(node->Translate().y(), 50.0f + size.y() * 1.5f, 1.0e-4f);

    ASSERT_TRUE(scene->runtime->SetNodeText("caption", "aaaaaa"));
    const auto resized = scene->runtime->NodeSize("caption");
    EXPECT_NEAR(node->Translate().x(), 100.0f - resized.x(), 1.0e-4f);
    EXPECT_NEAR(node->Translate().y(), 50.0f + resized.y() * 1.5f, 1.0e-4f);
}

TEST(TextObjectRuntime, PumpTextLayerCacheClearsDirtyStateWithoutLosingText) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {});
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNode("caption", node.get());
    runtime->RegisterTextLayer("caption",
                               TextLayerState {
                                   .text       = "before",
                                   .font_key   = "Arial",
                                   .point_size = 12.0f,
                               });

    ASSERT_TRUE(runtime->SetNodeText("caption", "after"));
    ASSERT_TRUE(runtime->NodeTextDirty("caption"));
    runtime->PumpTextLayerCache();

    const auto state = runtime->NodeTextState("caption");
    ASSERT_TRUE(state.has_value());
    EXPECT_EQ(state->text, "after");
    EXPECT_FALSE(state->cache_dirty);
    EXPECT_FALSE(state->full_dirty);
    EXPECT_FALSE(runtime->NodeTextDirty("caption"));
}

TEST(TextObjectRuntime, PumpTextLayerCacheDoesNotKeepDirtyWhenSceneCannotAcceptRuntimeTexture) {
    Scene scene;
    scene.runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {});
    ASSERT_NE(scene.runtime, nullptr);
    scene.runtime->AttachScene(&scene);

    auto node = std::make_shared<SceneNode>();
    scene.runtime->RegisterNode("caption", node.get());
    scene.runtime->RegisterTextLayer("caption",
                                     TextLayerState {
                                         .text       = "before",
                                         .font_key   = "Arial",
                                         .point_size = 12.0f,
                                     });

    ASSERT_TRUE(scene.runtime->SetNodeText("caption", "after"));
    ASSERT_TRUE(scene.runtime->NodeTextDirty("caption"));

    scene.runtime->PumpTextLayerCache();

    const auto state = scene.runtime->NodeTextState("caption");
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->cache_dirty);
    EXPECT_FALSE(state->full_dirty);
    EXPECT_FALSE(scene.runtime->NodeTextDirty("caption"));
    EXPECT_FALSE(scene.runtime->ConsumeSceneGraphMutationFlag());
}

TEST(TextObjectRuntime, PumpTextLayerCacheRerasterizesAttachedSceneTexture) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-rerasterize",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "caption", "text": "a", "font": "Arial", "pointsize": 20, "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    ASSERT_NE(scene->runtime, nullptr);
    auto* node = FindRootChild(*scene, "caption");
    ASSERT_NE(node, nullptr);
    ASSERT_NE(node->Mesh(), nullptr);
    auto* material = node->Mesh()->MaterialForSlot(0);
    ASSERT_NE(material, nullptr);
    ASSERT_EQ(material->textures.size(), 1u);
    const auto texture_name = material->textures.front();

    auto before = scene->imageParser->Parse(texture_name);
    ASSERT_NE(before, nullptr);
    ASSERT_TRUE(scene->runtime->SetNodeText("caption", "aaaaaa"));
    ASSERT_TRUE(scene->runtime->NodeTextDirty("caption"));

    scene->runtime->PumpTextLayerCache();

    auto after = scene->imageParser->Parse(texture_name);
    ASSERT_NE(after, nullptr);
    EXPECT_NE(before->key, after->key);
    EXPECT_GT(after->header.width, before->header.width);
    EXPECT_FALSE(scene->runtime->NodeTextDirty("caption"));
    EXPECT_TRUE(scene->runtime->ConsumeSceneGraphMutationFlag());
}

TEST(TextObjectRuntime, TextCacheKeysAreUniquePerLayerEvenWithSameFontAndSize) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {});
    ASSERT_NE(runtime, nullptr);

    auto first  = std::make_shared<SceneNode>();
    auto second = std::make_shared<SceneNode>();
    runtime->RegisterNode("first", first.get());
    runtime->RegisterNode("second", second.get());
    runtime->RegisterTextLayer("first",
                               TextLayerState {
                                   .text       = "one",
                                   .font_key   = "Arial",
                                   .point_size = 12.0f,
                               });
    runtime->RegisterTextLayer("second",
                               TextLayerState {
                                   .text       = "two",
                                   .font_key   = "Arial",
                                   .point_size = 12.0f,
                               });

    const auto first_state  = runtime->NodeTextState("first");
    const auto second_state = runtime->NodeTextState("second");
    ASSERT_TRUE(first_state.has_value());
    ASSERT_TRUE(second_state.has_value());
    EXPECT_NE(first_state->texture_cache_key, second_state->texture_cache_key);
}

TEST(TextObjectRuntime, HorizontalAlignOverridesLegacyAlignment) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-align-horizontal",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "fallback", "text": "aaaa", "font": "Arial", "pointsize": 10, "origin": [100, 50, 0], "alignment": "left", "visible": true},
          {"id": 2, "name": "override", "text": "aaaa", "font": "Arial", "pointsize": 10, "origin": [100, 50, 0], "alignment": "left", "horizontalalign": "right", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    auto* fallback = FindRootChild(*scene, "fallback");
    auto* override = FindRootChild(*scene, "override");
    ASSERT_NE(fallback, nullptr);
    ASSERT_NE(override, nullptr);
    EXPECT_GT(fallback->Translate().x(), 100.0f);
    EXPECT_LT(override->Translate().x(), 100.0f);
    EXPECT_FLOAT_EQ(fallback->Translate().y(), 50.0f);
    EXPECT_FLOAT_EQ(override->Translate().y(), 50.0f);
}

TEST(TextObjectRuntime, VerticalAlignOverridesLegacyAlignment) {
    fs::VFS vfs;
    MountAssets(vfs);
    audio::SoundManager sound_manager;
    WPSceneParser       parser;
    ProjectProperties   properties;
    SceneParseRequest   request {
          .scene_id           = "text-align-vertical",
          .project_properties = &properties,
    };

    auto scene = parser.Parse(request,
                              MinimalSceneObjects(R"([
          {"id": 1, "name": "fallback", "text": "aaaa", "font": "Arial", "pointsize": 10, "origin": [100, 50, 0], "alignment": "top", "visible": true},
          {"id": 2, "name": "override", "text": "aaaa", "font": "Arial", "pointsize": 10, "origin": [100, 50, 0], "alignment": "top", "verticalalign": "bottom", "visible": true}
        ])"),
                              vfs,
                              sound_manager);

    ASSERT_NE(scene, nullptr);
    auto* fallback = FindRootChild(*scene, "fallback");
    auto* override = FindRootChild(*scene, "override");
    ASSERT_NE(fallback, nullptr);
    ASSERT_NE(override, nullptr);
    EXPECT_LT(fallback->Translate().y(), 50.0f);
    EXPECT_GT(override->Translate().y(), 50.0f);
    EXPECT_FLOAT_EQ(fallback->Translate().x(), 100.0f);
    EXPECT_FLOAT_EQ(override->Translate().x(), 100.0f);
}

TEST(TextObjectRuntime, ScriptThisLayerTextSetterMutatesRuntimeText) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {});
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNode("caption", node.get());
    runtime->RegisterTextLayer("caption",
                               TextLayerState {
                                   .text       = "before",
                                   .font_key   = "Arial",
                                   .point_size = 12.0f,
                                   .padding    = 0.0f,
                               });

    auto program = runtime->scriptEngine().CreatePropertyScriptProgram(runtime.get(),
                                                                       R"JS(
export function update(value) {
  thisLayer.text = "after";
  var indirect = thisScene.getLayer('caption');
  indirect.text = indirect.text + " indirect";
  return thisLayer.text === "after indirect" ? 1 : -1;
}
)JS",
                                                                       "caption",
                                                                       {},
                                                                       DynamicValue(0.0f),
                                                                       runtime->hostContext());
    ASSERT_NE(program, nullptr);

    const auto result = program->Evaluate(runtime->hostContext(), DynamicValue(0.0f));
    ASSERT_NE(result, nullptr);
    EXPECT_FLOAT_EQ(result->getFloat(), 1.0f);
    EXPECT_EQ(runtime->NodeText("caption"), "after indirect");
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

TEST(TextObjectRuntime, ResolvesWallpaperEngineSansSerifSystemFontAlias) {
#ifdef __APPLE__
    EXPECT_FALSE(ResolveSystemFontPath("systemfont_sansserif").empty());
#endif
}

TEST(TextObjectRuntime, FreeTypeRasterizationFallsBackForMissingPrimaryGlyph) {
#ifdef __APPLE__
    TextLayerState state {
        .text                   = "\xE4\xB8\xAD",
        .font_key               = "systemfont_Helvetica",
        .resolved_font_kind     = "system",
        .resolved_font_identity = "Helvetica",
        .resolved_font_path     = ResolveSystemFontPath("systemfont_Helvetica"),
        .point_size             = 40.0f,
        .padding                = 4.0f,
    };
    ASSERT_FALSE(state.resolved_font_path.empty());

    const auto size = TextLayerRasterSize(state);
    EXPECT_GT(size.x(), 40.0f);
    EXPECT_GT(size.y(), 40.0f);

    const auto           width  = static_cast<uint32_t>(std::ceil(size.x()));
    const auto           height = static_cast<uint32_t>(std::ceil(size.y()));
    std::vector<uint8_t> rgba(static_cast<std::size_t>(width) * height * 4u, 0u);
    RasterizeTextLayer(state, width, height, rgba);

    TextLayerState unsupported_state = state;
    unsupported_state.text           = "\xF4\x8F\xBF\xBF";
    const auto unsupported_size      = TextLayerRasterSize(unsupported_state);
    const auto unsupported_width     = static_cast<uint32_t>(std::ceil(unsupported_size.x()));
    const auto unsupported_height    = static_cast<uint32_t>(std::ceil(unsupported_size.y()));
    std::vector<uint8_t> unsupported_rgba(
        static_cast<std::size_t>(unsupported_width) * unsupported_height * 4u, 0u);
    RasterizeTextLayer(unsupported_state, unsupported_width, unsupported_height, unsupported_rgba);

    int min_x = static_cast<int>(width);
    int max_x = -1;
    int min_y = static_cast<int>(height);
    int max_y = -1;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const auto alpha = rgba[(static_cast<std::size_t>(y) * width + x) * 4u + 3u];
            if (alpha == 0u) continue;
            min_x = std::min(min_x, static_cast<int>(x));
            max_x = std::max(max_x, static_cast<int>(x));
            min_y = std::min(min_y, static_cast<int>(y));
            max_y = std::max(max_y, static_cast<int>(y));
        }
    }

    ASSERT_GE(max_x, min_x);
    ASSERT_GE(max_y, min_y);
    EXPECT_GT(max_x - min_x + 1, 20);
    EXPECT_GT(max_y - min_y + 1, 20);
    EXPECT_NE(rgba, unsupported_rgba);
#endif
}

} // namespace wallpaper
