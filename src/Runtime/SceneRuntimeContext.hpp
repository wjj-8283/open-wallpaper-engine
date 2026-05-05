#pragma once

#include "Project/ProjectProperties.hpp"
#include "Audio/include/Audio/AudioResponseService.h"
#include "Runtime/ScalarAnimation.hpp"
#include "Video/VideoTextureSource.hpp"

#include <Eigen/Dense>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace wallpaper
{

class DynamicValue;
class ScriptEngine;
class SceneScriptProgram;
class SceneMaterial;
class ScriptedDynamicValue;
class SceneNode;
class SceneImageEffectLayer;
class Scene;
struct ScriptHostContext;

struct SceneRuntimeBootstrap
{
    int               canvas_width { 0 };
    int               canvas_height { 0 };
    ProjectProperties project_properties {};
};

class SceneRuntimeContext
{
public:
    explicit SceneRuntimeContext(SceneRuntimeBootstrap bootstrap);
    ~SceneRuntimeContext();

    void Tick(double frame_time);

    ScriptEngine&             scriptEngine();
    const ScriptHostContext&  hostContext() const;
    const ProjectProperties&  defaultProjectProperties() const;
    const ProjectProperties&  projectProperties() const;
    void                      ApplyProjectPropertyOverride(const ProjectProperties& override_properties);
    void                      ResetProjectPropertyOverride();
    void                      AttachScene(Scene* scene);
    void                      SetCursorWorldPosition(const Eigen::Vector3f& value);
    DynamicValue*             FindPropertyValue(std::string_view name) const;
    void                      RegisterScriptedValue(ScriptedDynamicValue* value);
    void                      RegisterNode(std::string name, SceneNode* node);
    void                      RegisterNodeSize(std::string name, Eigen::Vector2f value);
    void RegisterLayerTemplate(
        std::string template_path,
        std::shared_ptr<SceneNode> node,
        Eigen::Vector2f size);
    void RegisterNodeVisibility(
        std::string name,
        SceneNode* node,
        std::unique_ptr<DynamicValue> value);
    void RegisterNodeTranslate(
        std::string name,
        SceneNode* node,
        std::unique_ptr<DynamicValue> value);
    void RegisterNodeScale(
        std::string name,
        SceneNode* node,
        std::unique_ptr<DynamicValue> value);
    void RegisterNodeRotation(
        std::string name,
        SceneNode* node,
        std::unique_ptr<DynamicValue> value);
    void RegisterNodeEffectFinal(std::string name, SceneNode* node, SceneImageEffectLayer* layer);
    void RegisterMaterialAlphaAnimation(SceneMaterial* material, ScalarAnimation animation);
    void        RegisterSceneScript(std::string script_source, std::string layer_name);
    void        RegisterNodeVideoTexture(std::string name, std::string texture_key);
    std::size_t sceneScriptCount() const;
    bool        HasNodeNamed(std::string_view name) const;
    int         NodeSiblingIndex(std::string_view name) const;
    std::string CreateLayerFromTemplate(
        std::string_view requested_template_path,
        std::string_view current_layer_name);
    bool        SortNode(std::string_view name, int index);
    bool        NodeVisible(std::string_view name) const;
    bool        SetNodeVisible(std::string_view name, bool visible);
    bool        SetNodeTranslate(std::string_view name, const Eigen::Vector3f& value);
    bool        SetNodeScale(std::string_view name, const Eigen::Vector3f& value);
    bool        SetNodeRotation(std::string_view name, const Eigen::Vector3f& value);
    Eigen::Vector3f NodeTranslate(std::string_view name) const;
    Eigen::Vector3f NodeScale(std::string_view name) const;
    Eigen::Vector3f NodeRotation(std::string_view name) const;
    Eigen::Vector2f NodeSize(std::string_view name) const;
    bool            NodeHasVideoTexture(std::string_view name) const;
    bool            PlayNodeVideoTexture(std::string_view name);
    bool            PauseNodeVideoTexture(std::string_view name);
    bool            SetNodeVideoTextureCurrentTime(std::string_view name, double seconds);
    double          NodeVideoTextureCurrentTime(std::string_view name) const;
    bool            SetNodeVideoTextureRate(std::string_view name, float rate);
    float           NodeVideoTextureRate(std::string_view name) const;
    double          NodeVideoTextureDuration(std::string_view name) const;
    void            SetVideoTextureDuration(std::string_view texture_key, double seconds);
    video::VideoPlaybackState ResolveVideoPlaybackState(
        std::string_view texture_key,
        double           fallback_scene_elapsed_seconds) const;
    void DispatchCursorClick();
    void DispatchCursorDown();
    void DispatchCursorEnter();
    void DispatchCursorLeave();
    void DispatchCursorMove();
    void DispatchCursorUp();
    void DispatchMediaThumbnailChanged(
        const Eigen::Vector3f& primary_color,
        const Eigen::Vector3f& text_color);
    void MarkSceneRequiresAudioResponse();
    bool SceneRequiresAudioResponse() const;
    void SetAudioResponseEnabled(bool enabled);
    bool AudioResponseEnabled() const;
    bool AudioResponseActive() const;
    bool ConsumeSceneGraphMutationFlag();
    audio::AudioSpectrumSnapshot CurrentAudioSpectrumSnapshot() const;
    void RecordScriptError(std::string message);
    std::size_t scriptErrorCount() const;
    const std::vector<std::string>& scriptErrors() const;
    void ClearScriptErrors();

private:
    struct NodeVisibilityBinding
    {
        SceneNode*    node { nullptr };
        DynamicValue* value { nullptr };
    };
    struct NodeVec3Binding
    {
        SceneNode*    node { nullptr };
        DynamicValue* value { nullptr };
    };
    struct NodeEffectFinalBinding
    {
        SceneNode*              node { nullptr };
        SceneImageEffectLayer*  layer { nullptr };
    };
    struct MaterialAlphaBinding
    {
        SceneMaterial* material { nullptr };
        ScalarAnimation animation {};
    };
    struct VideoTexturePlaybackBinding
    {
        double duration_seconds { 0.0 };
        double absolute_seconds { 0.0 };
        float  rate { 1.0f };
        bool   paused { false };
    };
    struct LayerTemplateBinding
    {
        std::string                canonical_path;
        std::shared_ptr<SceneNode> node;
        Eigen::Vector2f            size { Eigen::Vector2f::Zero() };
    };

    std::unique_ptr<ScriptEngine>                                  m_script_engine;
    std::unique_ptr<ScriptHostContext>                             m_host_context;
    Scene*                                                         m_scene { nullptr };
    ProjectProperties                                              m_default_project_properties;
    ProjectProperties                                              m_project_property_overrides;
    ProjectProperties                                              m_project_properties;
    std::unordered_map<std::string, std::unique_ptr<DynamicValue>> m_property_values;
    std::vector<std::unique_ptr<DynamicValue>>                     m_owned_values;
    std::unordered_map<std::string, SceneNode*>                    m_nodes;
    std::unordered_map<std::string, NodeVisibilityBinding>         m_node_visibility;
    std::unordered_map<std::string, NodeVec3Binding>               m_node_translate;
    std::unordered_map<std::string, NodeVec3Binding>               m_node_scale;
    std::unordered_map<std::string, NodeVec3Binding>               m_node_rotation;
    std::unordered_map<std::string, NodeEffectFinalBinding>        m_node_effect_final;
    std::vector<MaterialAlphaBinding>                              m_material_alpha;
    std::unordered_map<std::string, Eigen::Vector2f>               m_node_size;
    std::unordered_map<std::string, std::string>                   m_node_template_paths;
    std::unordered_map<std::string, LayerTemplateBinding>          m_layer_templates;
    std::unordered_map<std::string, std::string>                   m_node_video_textures;
    std::unordered_map<std::string, VideoTexturePlaybackBinding>   m_video_texture_playback;
    std::vector<ScriptedDynamicValue*>                             m_scripted_values;
    std::vector<std::unique_ptr<SceneScriptProgram>>               m_scene_scripts;
    std::vector<std::string>                                       m_script_errors;
    uint64_t                                                       m_next_generated_layer_id { 1 };
    bool                                                           m_scene_requires_audio_response { false };
    bool                                                           m_audio_response_enabled { false };
    bool                                                           m_scene_graph_mutated { false };
};

std::unique_ptr<SceneRuntimeContext> CreateSceneRuntimeContext(SceneRuntimeBootstrap bootstrap);

} // namespace wallpaper
