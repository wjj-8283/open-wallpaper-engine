#include "Runtime/SceneRuntimeContext.hpp"

#include "Runtime/DynamicValue.hpp"
#include "Runtime/ScriptedDynamicValue.hpp"
#include "Scene/include/Scene/Scene.h"
#include "Scene/include/Scene/SceneImageEffectLayer.h"
#include "Scene/include/Scene/SceneMaterial.h"
#include "Scene/include/Scene/SceneMesh.h"
#include "Scene/SceneNode.h"
#include "Scripting/ScriptEngine.hpp"
#include "Utils/Logging.h"
#include "WPSoundParser.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace wallpaper
{

namespace
{
std::string NormalizeTemplatePath(std::string_view value) {
    if (value.empty()) return {};
    return std::filesystem::path(std::string(value)).generic_string();
}

std::string MakeWorkshopLocalAlias(std::string_view canonical_path) {
    const std::string          normalized = NormalizeTemplatePath(canonical_path);
    constexpr std::string_view prefix     = "models/workshop/";
    if (! normalized.starts_with(prefix)) return {};

    const std::size_t workshop_begin = prefix.size();
    const std::size_t slash          = normalized.find('/', workshop_begin);
    if (slash == std::string::npos || slash + 1 >= normalized.size()) return {};

    return std::string("models/") + normalized.substr(slash + 1);
}

struct ClonedMaterialBinding {
    SceneMaterial* source_material { nullptr };
    SceneMaterial* cloned_material { nullptr };
};

std::shared_ptr<SceneMesh> CloneMesh(SceneMesh&                          mesh,
                                     std::vector<ClonedMaterialBinding>* material_bindings) {
    auto clone = std::make_shared<SceneMesh>(mesh.Dynamic());
    clone->SetPrimitive(mesh.Primitive());
    clone->SetPointSize(mesh.PointSize());
    clone->SetID(mesh.ID());
    clone->ChangeMeshDataFrom(mesh);
    if (mesh.Material() != nullptr) {
        clone->AddMaterial(SceneMaterial(*mesh.Material()));
        if (material_bindings != nullptr) {
            material_bindings->push_back(ClonedMaterialBinding {
                .source_material = mesh.Material(),
                .cloned_material = clone->Material(),
            });
        }
    }
    return clone;
}

std::shared_ptr<SceneNode> CloneNodeShallow(SceneNode& node, std::string name,
                                            std::vector<ClonedMaterialBinding>* material_bindings) {
    auto clone = std::make_shared<SceneNode>(
        node.Translate(), node.Scale(), node.Rotation(), std::move(name));
    clone->SetVisible(node.Visible());
    clone->SetSkipRenderPass(node.SkipRenderPass());
    clone->SetCamera(node.Camera());
    clone->ID() = node.ID();
    if (node.HasRenderTransformOverride()) {
        clone->SetRenderTransformOverride(node.RenderTrans());
    }
    if (node.Mesh() != nullptr) {
        clone->AddMesh(CloneMesh(*node.Mesh(), material_bindings));
    }
    return clone;
}

template<typename ListT>
bool SortNodeInList(ListT& nodes, SceneNode* target, int index) {
    if (target == nullptr) return false;

    auto current = std::find_if(nodes.begin(), nodes.end(), [target](const auto& entry) {
        return entry.get() == target;
    });
    if (current == nodes.end()) return false;

    const int size          = static_cast<int>(nodes.size());
    int       clamped_index = std::clamp(index, 0, std::max(0, size - 1));
    auto      destination   = nodes.begin();
    std::advance(destination, clamped_index);
    if (destination == current) return false;

    nodes.splice(destination, nodes, current);
    return true;
}

template<typename ListT>
int NodeIndexInList(const ListT& nodes, const SceneNode* target) {
    if (target == nullptr) return -1;

    int index = 0;
    for (const auto& entry : nodes) {
        if (entry.get() == target) return index;
        ++index;
    }
    return -1;
}

double WrapSeconds(double seconds, double duration_seconds) {
    if (! (duration_seconds > 0.0) || ! std::isfinite(seconds)) return std::max(0.0, seconds);
    double wrapped = std::fmod(seconds, duration_seconds);
    if (wrapped < 0.0) wrapped += duration_seconds;
    if (wrapped >= duration_seconds) wrapped = 0.0;
    return wrapped;
}

std::unique_ptr<DynamicValue> MakePropertyValue(const RuntimeScalarValue& value) {
    switch (value.kind) {
    case RuntimeScalarValue::Kind::Bool: return std::make_unique<DynamicValue>(value.asBool());
    case RuntimeScalarValue::Kind::Float: return std::make_unique<DynamicValue>(value.asFloat());
    case RuntimeScalarValue::Kind::String: return std::make_unique<DynamicValue>(value.asString());
    }

    return std::make_unique<DynamicValue>();
}

void ApplyMaterialAlpha(SceneMaterial& material, float alpha) {
    auto& values = material.customShader.constValues;

    values["g_UserAlpha"] = ShaderValue(alpha);

    if (auto alpha_value = values.find("g_Alpha"); alpha_value != values.end()) {
        alpha_value->second = ShaderValue(alpha);
    }

    std::array<float, 4> color { 1.0f, 1.0f, 1.0f, alpha };
    if (const auto color_value = values.find("g_Color4"); color_value != values.end()) {
        const size_t component_count = std::min<size_t>(3, color_value->second.size());
        for (size_t index = 0; index < component_count; ++index) {
            color[index] = color_value->second[index];
        }
    }
    values["g_Color4"] = color;
}

ShaderValue ShaderValueFromDynamicValue(const DynamicValue& value) {
    switch (value.getType()) {
    case DynamicValue::Vec4: return ShaderValue(value.getVec4().data(), 4);
    case DynamicValue::Vec3: return ShaderValue(value.getVec3().data(), 3);
    case DynamicValue::Vec2: return ShaderValue(value.getVec2().data(), 2);
    case DynamicValue::IVec4: {
        const auto           source = value.getIVec4();
        std::array<float, 4> converted {
            static_cast<float>(source.x()),
            static_cast<float>(source.y()),
            static_cast<float>(source.z()),
            static_cast<float>(source.w()),
        };
        return ShaderValue(converted);
    }
    case DynamicValue::IVec3: {
        const auto           source = value.getIVec3();
        std::array<float, 3> converted {
            static_cast<float>(source.x()),
            static_cast<float>(source.y()),
            static_cast<float>(source.z()),
        };
        return ShaderValue(converted);
    }
    case DynamicValue::IVec2: {
        const auto           source = value.getIVec2();
        std::array<float, 2> converted {
            static_cast<float>(source.x()),
            static_cast<float>(source.y()),
        };
        return ShaderValue(converted);
    }
    case DynamicValue::Boolean:
    case DynamicValue::Float:
    case DynamicValue::Int: return ShaderValue(value.getFloat());
    case DynamicValue::String: {
        std::istringstream stream(value.getString());
        std::vector<float> parsed;
        float              component = 0.0f;
        while (stream >> component) {
            parsed.push_back(component);
        }
        return parsed.empty() ? ShaderValue(0.0f) : ShaderValue(parsed);
    }
    case DynamicValue::Null: return ShaderValue(0.0f);
    }

    return ShaderValue(0.0f);
}

void ApplyMaterialConstant(SceneMaterial& material, const std::string& name,
                           const DynamicValue& value) {
    if (name.empty()) return;
    material.customShader.constValues[name] = ShaderValueFromDynamicValue(value);
}

bool AlignmentContains(std::string_view alignment, std::string_view part) {
    return alignment.find(part) != std::string_view::npos;
}

void SyncEffectFinalNode(SceneNode& source, SceneImageEffectLayer& layer) {
    source.UpdateTrans();
    const auto sync_node = [&source](SceneNode& target) {
        target.SetRenderTransformOverride(source.ModelTrans());
        target.SetTranslate(source.Translate());
        target.SetScale(source.Scale());
        target.SetRotation(source.Rotation());
    };
    sync_node(layer.FinalNode());
    if (auto* resolved = layer.ResolvedFinalRenderNode(); resolved != nullptr) {
        sync_node(*resolved);
    }
}

} // namespace

SceneRuntimeContext::SceneRuntimeContext(SceneRuntimeBootstrap bootstrap)
    : m_script_engine(std::make_unique<ScriptEngine>()),
      m_host_context(std::make_unique<ScriptHostContext>()),
      m_default_project_properties(bootstrap.project_properties),
      m_project_properties(bootstrap.project_properties) {
    m_host_context->canvas_size = Eigen::Vector2f(static_cast<float>(bootstrap.canvas_width),
                                                  static_cast<float>(bootstrap.canvas_height));
    m_host_context->cursor_normalized_position = Eigen::Vector2f(0.5f, 0.5f);
    m_host_context->cursor_world_position      = Eigen::Vector3f::Zero();
    m_host_context->frame_time                 = 0.0;
    m_host_context->runtime_seconds            = 0.0;

    for (const auto& [name, value] : m_project_properties) {
        m_property_values.emplace(name, MakePropertyValue(value));
    }
}

SceneRuntimeContext::~SceneRuntimeContext() = default;

void SceneRuntimeContext::Tick(double frame_time) {
    m_host_context->frame_time = frame_time;
    m_host_context->runtime_seconds += frame_time;
    for (auto& [texture_key, playback] : m_video_texture_playback) {
        (void)texture_key;
        if (playback.paused) continue;
        const double scaled_delta = static_cast<double>(playback.rate) * frame_time;
        if (scaled_delta <= 0.0) continue;
        playback.absolute_seconds += scaled_delta;
    }
    for (auto* value : m_scripted_values) {
        if (value != nullptr) value->reevaluate();
    }
    for (auto& [name, binding] : m_node_visibility) {
        (void)name;
        if (binding.node != nullptr && binding.value != nullptr) {
            binding.node->SetVisible(binding.value->getBool());
        }
    }
    for (auto& [name, binding] : m_node_translate) {
        (void)name;
        if (binding.node != nullptr && binding.value != nullptr) {
            m_node_alignment[name].origin = binding.value->getVec3();
            ApplyNodeTransform(name);
        }
    }
    for (auto& [name, binding] : m_node_scale) {
        (void)name;
        if (binding.node != nullptr && binding.value != nullptr) {
            m_node_alignment[name].scale = binding.value->getVec3();
            if (m_node_alignment[name].alignment.empty()) {
                binding.node->SetScale(binding.value->getVec3());
            } else {
                ApplyNodeTransform(name);
            }
        }
    }
    for (auto& [name, binding] : m_node_rotation) {
        (void)name;
        if (binding.node != nullptr && binding.value != nullptr) {
            binding.node->SetRotation(binding.value->getVec3());
        }
    }
    for (auto& binding : m_text_values) {
        if (binding.value != nullptr) SetNodeText(binding.name, binding.value->toString());
    }
    for (auto& [name, binding] : m_node_effect_final) {
        (void)name;
        if (binding.node == nullptr || binding.layer == nullptr) continue;
        SyncEffectFinalNode(*binding.node, *binding.layer);
    }
    for (auto& binding : m_material_alpha) {
        if (binding.material == nullptr) continue;
        ApplyMaterialAlpha(*binding.material,
                           binding.animation.Evaluate(m_host_context->runtime_seconds));
    }
    for (auto& binding : m_material_constants) {
        if (binding.material == nullptr || binding.value == nullptr) continue;
        ApplyMaterialConstant(*binding.material, binding.name, *binding.value);
    }
    for (auto& script : m_scene_scripts) {
        if (script != nullptr) script->Tick(*m_host_context);
    }
    for (auto& binding : m_material_constants) {
        if (binding.material == nullptr || binding.value == nullptr) continue;
        ApplyMaterialConstant(*binding.material, binding.name, *binding.value);
    }
}

ScriptEngine& SceneRuntimeContext::scriptEngine() { return *m_script_engine; }

const ScriptHostContext& SceneRuntimeContext::hostContext() const { return *m_host_context; }

const ProjectProperties& SceneRuntimeContext::defaultProjectProperties() const {
    return m_default_project_properties;
}

const ProjectProperties& SceneRuntimeContext::projectProperties() const {
    return m_project_properties;
}

void SceneRuntimeContext::ApplyProjectPropertyOverride(
    const ProjectProperties& override_properties) {
    m_project_property_overrides = override_properties;
    m_project_properties =
        MergeProjectProperties(m_default_project_properties, m_project_property_overrides);

    for (const auto& [name, value] : m_project_properties) {
        const auto it = m_property_values.find(name);
        if (it == m_property_values.end()) {
            m_property_values.emplace(name, MakePropertyValue(value));
            continue;
        }

        auto next_value = MakePropertyValue(value);
        it->second->update(*next_value);
    }

    for (auto& script : m_scene_scripts) {
        if (script != nullptr) script->ApplyProjectProperties(m_project_properties);
    }
}

void SceneRuntimeContext::ResetProjectPropertyOverride() { ApplyProjectPropertyOverride({}); }

void SceneRuntimeContext::AttachScene(Scene* scene) { m_scene = scene; }

void SceneRuntimeContext::SetCursorWorldPosition(const Eigen::Vector3f& value) {
    m_host_context->cursor_world_position = value;
}

void SceneRuntimeContext::SetCursorInput(float x, float y) {
    x                                          = std::clamp(x, 0.0f, 1.0f);
    y                                          = std::clamp(y, 0.0f, 1.0f);
    m_host_context->cursor_normalized_position = Eigen::Vector2f(x, y);
    m_host_context->cursor_world_position      = Eigen::Vector3f(
        x * m_host_context->canvas_size.x(), y * m_host_context->canvas_size.y(), 0.0f);
}

void SceneRuntimeContext::SetCursorEnter(bool entered) {
    m_host_context->cursor_in_window = entered;
}

void SceneRuntimeContext::SetCursorButton(int button, bool pressed) {
    if (button < 0 || button > 31) return;
    const uint32_t mask = 1u << static_cast<uint32_t>(button);
    if (pressed) {
        if ((m_host_context->mouse_buttons_down & mask) == 0) {
            m_host_context->mouse_buttons_down |= mask;
            m_host_context->mouse_buttons_pressed |= mask;
        }
        return;
    }
    if ((m_host_context->mouse_buttons_down & mask) != 0) {
        m_host_context->mouse_buttons_down &= ~mask;
        m_host_context->mouse_buttons_released |= mask;
    }
}

void SceneRuntimeContext::SetCursorButtons(uint32_t down, uint32_t pressed, uint32_t released) {
    m_host_context->mouse_buttons_down     = down;
    m_host_context->mouse_buttons_pressed  = pressed;
    m_host_context->mouse_buttons_released = released;
}

void SceneRuntimeContext::BeginFrame() {
    m_host_context->mouse_buttons_pressed  = 0;
    m_host_context->mouse_buttons_released = 0;
}

DynamicValue* SceneRuntimeContext::FindPropertyValue(std::string_view name) const {
    const auto iterator = m_property_values.find(std::string(name));
    if (iterator == m_property_values.end()) return nullptr;
    return iterator->second.get();
}

void SceneRuntimeContext::RegisterScriptedValue(ScriptedDynamicValue* value) {
    m_scripted_values.push_back(value);
}

void SceneRuntimeContext::RegisterNode(std::string name, SceneNode* node) {
    if (node == nullptr || name.empty()) return;
    std::string key  = std::move(name);
    m_nodes[key]     = node;
    auto& alignment  = m_node_alignment[key];
    alignment.origin = node->Translate();
    alignment.scale  = node->Scale();
}

void SceneRuntimeContext::UnregisterNode(std::string_view name) {
    const std::string key(name);
    m_nodes.erase(key);
    m_node_visibility.erase(key);
    m_node_translate.erase(key);
    m_node_scale.erase(key);
    m_node_rotation.erase(key);
    m_node_effect_final.erase(key);
    m_node_size.erase(key);
    m_text_layers.erase(key);
    m_text_values.erase(std::remove_if(m_text_values.begin(),
                                       m_text_values.end(),
                                       [&key](const auto& binding) {
                                           return binding.name == key;
                                       }),
                        m_text_values.end());
    m_node_alignment.erase(key);
    m_node_template_paths.erase(key);
    m_node_video_textures.erase(key);
    m_sound_layers.erase(key);
}

void SceneRuntimeContext::RegisterNodeSize(std::string name, Eigen::Vector2f value) {
    if (name.empty()) return;
    m_node_size[std::move(name)] = value;
}

void SceneRuntimeContext::RegisterLayerTemplate(std::string                template_path,
                                                std::shared_ptr<SceneNode> node,
                                                Eigen::Vector2f            size) {
    if (node == nullptr) return;

    const std::string canonical_path = NormalizeTemplatePath(template_path);
    if (canonical_path.empty()) return;

    LayerTemplateBinding binding {
        .canonical_path = canonical_path,
        .node           = std::move(node),
        .size           = size,
    };
    m_layer_templates[canonical_path] = binding;

    if (const auto alias = MakeWorkshopLocalAlias(canonical_path); ! alias.empty()) {
        m_layer_templates[alias] = binding;
    }

    if (! binding.node->Name().empty()) {
        m_node_template_paths[binding.node->Name()] = canonical_path;
    }
}

void SceneRuntimeContext::RegisterNodeVisibility(std::string name, SceneNode* node,
                                                 std::unique_ptr<DynamicValue> value) {
    if (node == nullptr || value == nullptr) return;

    RegisterNode(name, node);
    node->SetVisible(value->getBool());
    auto* raw = value.get();
    m_owned_values.push_back(std::move(value));
    m_node_visibility[std::move(name)] = NodeVisibilityBinding {
        .node  = node,
        .value = raw,
    };
}

void SceneRuntimeContext::RegisterNodeTranslate(std::string name, SceneNode* node,
                                                std::unique_ptr<DynamicValue> value) {
    if (node == nullptr || value == nullptr) return;

    RegisterNode(name, node);
    SetNodeTranslate(name, value->getVec3());
    auto* raw = value.get();
    m_owned_values.push_back(std::move(value));
    m_node_translate[std::move(name)] = NodeVec3Binding {
        .node  = node,
        .value = raw,
    };
}

void SceneRuntimeContext::RegisterNodeScale(std::string name, SceneNode* node,
                                            std::unique_ptr<DynamicValue> value) {
    if (node == nullptr || value == nullptr) return;

    RegisterNode(name, node);
    SetNodeScale(name, value->getVec3());
    auto* raw = value.get();
    m_owned_values.push_back(std::move(value));
    m_node_scale[std::move(name)] = NodeVec3Binding {
        .node  = node,
        .value = raw,
    };
}

void SceneRuntimeContext::RegisterNodeRotation(std::string name, SceneNode* node,
                                               std::unique_ptr<DynamicValue> value) {
    if (node == nullptr || value == nullptr) return;

    RegisterNode(name, node);
    node->SetRotation(value->getVec3());
    auto* raw = value.get();
    m_owned_values.push_back(std::move(value));
    m_node_rotation[std::move(name)] = NodeVec3Binding {
        .node  = node,
        .value = raw,
    };
}

void SceneRuntimeContext::RegisterTextLayer(std::string name, TextLayerState state) {
    if (name.empty()) return;
    if (state.layer_key.empty()) state.layer_key = name;
    auto layer = TextLayer(std::move(state));
    m_node_size[name] = layer.size();
    m_text_layers.insert_or_assign(std::move(name), std::move(layer));
}

void SceneRuntimeContext::RegisterTextValue(std::string name, std::unique_ptr<DynamicValue> value) {
    if (name.empty() || value == nullptr) return;
    SetNodeText(name, value->toString());
    auto* raw = value.get();
    m_owned_values.push_back(std::move(value));
    m_text_values.push_back(TextValueBinding {
        .name  = std::move(name),
        .value = raw,
    });
}

void SceneRuntimeContext::RegisterMaterialConstant(SceneMaterial* material, std::string name,
                                                   std::unique_ptr<DynamicValue> value) {
    if (material == nullptr || name.empty() || value == nullptr) return;

    ApplyMaterialConstant(*material, name, *value);
    auto* raw = value.get();
    m_owned_values.push_back(std::move(value));
    m_material_constants.push_back(MaterialConstantBinding {
        .material = material,
        .name     = std::move(name),
        .value    = raw,
    });
}

void SceneRuntimeContext::RegisterNodeEffectFinal(std::string name, SceneNode* node,
                                                  SceneImageEffectLayer* layer) {
    if (node == nullptr || layer == nullptr) return;
    RegisterNode(name, node);
    m_node_effect_final[std::move(name)] = NodeEffectFinalBinding {
        .node  = node,
        .layer = layer,
    };
}

void SceneRuntimeContext::RegisterMaterialAlphaAnimation(SceneMaterial*  material,
                                                         ScalarAnimation animation) {
    if (material == nullptr) return;
    ApplyMaterialAlpha(*material, animation.Evaluate(m_host_context->runtime_seconds));
    m_material_alpha.push_back(MaterialAlphaBinding {
        .material  = material,
        .animation = std::move(animation),
    });
}

void SceneRuntimeContext::RegisterSceneScript(std::string script_source, std::string layer_name) {
    auto script = m_script_engine->CreateSceneScriptProgram(*this,
                                                            std::move(script_source),
                                                            std::move(layer_name),
                                                            m_project_properties,
                                                            *m_host_context);
    if (script != nullptr && script->Valid()) {
        m_scene_scripts.push_back(std::move(script));
    }
}

void SceneRuntimeContext::RegisterNodeVideoTexture(std::string name, std::string texture_key) {
    if (name.empty() || texture_key.empty()) return;
    auto& texture_keys = m_node_video_textures[name];
    if (std::find(texture_keys.begin(), texture_keys.end(), texture_key) == texture_keys.end()) {
        texture_keys.push_back(texture_key);
    }
    if (! m_video_texture_playback.contains(texture_key)) {
        m_video_texture_playback.emplace(std::move(texture_key), VideoTexturePlaybackBinding {});
    }
}

void SceneRuntimeContext::RegisterSoundLayer(std::string                    name,
                                             std::shared_ptr<WPSoundStream> stream) {
    if (name.empty() || stream == nullptr) return;
    m_sound_layers[std::move(name)] = std::move(stream);
}

std::size_t SceneRuntimeContext::sceneScriptCount() const {
    return m_scripted_values.size() + m_scene_scripts.size();
}

bool SceneRuntimeContext::HasNodeNamed(std::string_view name) const {
    return m_nodes.count(std::string(name)) != 0;
}

bool SceneRuntimeContext::HasSoundLayer(std::string_view name) const {
    return LockSoundLayer(name) != nullptr;
}

bool SceneRuntimeContext::PlaySoundLayer(std::string_view name) {
    auto stream = LockSoundLayer(name);
    if (stream == nullptr) return false;
    stream->Play();
    DispatchMediaPlaybackChanged(name, true);
    return true;
}

bool SceneRuntimeContext::PauseSoundLayer(std::string_view name) {
    auto stream = LockSoundLayer(name);
    if (stream == nullptr) return false;
    stream->Pause();
    DispatchMediaPlaybackChanged(name, false);
    return true;
}

bool SceneRuntimeContext::StopSoundLayer(std::string_view name) {
    auto stream = LockSoundLayer(name);
    if (stream == nullptr) return false;
    stream->Stop();
    DispatchMediaPlaybackChanged(name, false);
    return true;
}

bool SceneRuntimeContext::SoundLayerPlaying(std::string_view name) const {
    auto stream = LockSoundLayer(name);
    return stream != nullptr && stream->IsPlaying();
}

bool SceneRuntimeContext::SetSoundLayerVolume(std::string_view name, float volume) {
    auto stream = LockSoundLayer(name);
    if (stream == nullptr) return false;
    stream->SetVolume(volume);
    return true;
}

float SceneRuntimeContext::SoundLayerVolume(std::string_view name) const {
    auto stream = LockSoundLayer(name);
    return stream != nullptr ? stream->Volume() : 0.0f;
}

bool SceneRuntimeContext::SetSoundLayerMuted(std::string_view name, bool muted) {
    auto stream = LockSoundLayer(name);
    if (stream == nullptr) return false;
    stream->SetMuted(muted);
    return true;
}

bool SceneRuntimeContext::SoundLayerMuted(std::string_view name) const {
    auto stream = LockSoundLayer(name);
    return stream != nullptr && stream->Muted();
}

int SceneRuntimeContext::NodeSiblingIndex(std::string_view name) const {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return -1;

    const SceneNode* node = iterator->second;
    if (node->Parent() != nullptr) {
        return NodeIndexInList(node->Parent()->GetChildren(), node);
    }

    if (m_scene == nullptr || m_scene->sceneGraph == nullptr) return -1;
    return NodeIndexInList(m_scene->sceneGraph->GetChildren(), node);
}

std::string SceneRuntimeContext::CreateLayerFromTemplate(std::string_view requested_template_path,
                                                         std::string_view current_layer_name) {
    if (m_scene == nullptr || m_scene->sceneGraph == nullptr) return {};

    const std::string requested_path = NormalizeTemplatePath(requested_template_path);
    if (requested_path.empty()) return {};

    auto template_iterator = m_layer_templates.find(requested_path);
    if (template_iterator == m_layer_templates.end()) {
        if (const auto owner = m_node_template_paths.find(std::string(current_layer_name));
            owner != m_node_template_paths.end()) {
            if (const auto alias = MakeWorkshopLocalAlias(owner->second);
                ! alias.empty() && alias == requested_path) {
                template_iterator = m_layer_templates.find(owner->second);
            } else if (owner->second.starts_with("models/workshop/") &&
                       requested_path.starts_with("models/")) {
                constexpr std::string_view prefix         = "models/workshop/";
                const std::size_t          workshop_begin = prefix.size();
                const std::size_t          slash          = owner->second.find('/', workshop_begin);
                if (slash != std::string::npos) {
                    const std::string candidate =
                        owner->second.substr(0, slash + 1) +
                        requested_path.substr(std::string("models/").size());
                    template_iterator = m_layer_templates.find(candidate);
                }
            }
        }
    }

    if (template_iterator == m_layer_templates.end()) {
        LOG_INFO("scene runtime createLayer template not found: current=\"%s\" template=\"%s\"",
                 std::string(current_layer_name).c_str(),
                 requested_path.c_str());
        return {};
    }

    const auto& binding = template_iterator->second;
    if (binding.node == nullptr) return {};
    if (! binding.node->Camera().empty() || ! binding.node->GetChildren().empty()) {
        LOG_INFO(
            "scene runtime createLayer unsupported template shape: current=\"%s\" template=\"%s\"",
            std::string(current_layer_name).c_str(),
            binding.canonical_path.c_str());
        return {};
    }

    const std::string generated_name =
        "__generated_layer_" + std::to_string(m_next_generated_layer_id++) + ":" + requested_path;
    std::vector<ClonedMaterialBinding> material_bindings;
    auto generated = CloneNodeShallow(*binding.node, generated_name, &material_bindings);
    if (generated == nullptr) return {};

    SceneNode* parent = nullptr;
    if (const auto current = m_nodes.find(std::string(current_layer_name));
        current != m_nodes.end() && current->second != nullptr) {
        parent = current->second->Parent();
    }

    if (parent != nullptr) {
        parent->AppendChild(generated);
    } else {
        m_scene->sceneGraph->AppendChild(generated);
    }

    RegisterNode(generated_name, generated.get());
    RegisterNodeSize(generated_name, binding.size);
    m_node_template_paths[generated_name] = binding.canonical_path;
    const auto source_constant_count      = m_material_constants.size();
    for (const auto& material_binding : material_bindings) {
        if (material_binding.source_material == nullptr ||
            material_binding.cloned_material == nullptr) {
            continue;
        }
        for (std::size_t index = 0; index < source_constant_count; ++index) {
            const auto& constant_binding = m_material_constants[index];
            if (constant_binding.material != material_binding.source_material ||
                constant_binding.value == nullptr) {
                continue;
            }
            ApplyMaterialConstant(
                *material_binding.cloned_material, constant_binding.name, *constant_binding.value);
            m_material_constants.push_back(MaterialConstantBinding {
                .material = material_binding.cloned_material,
                .name     = constant_binding.name,
                .value    = constant_binding.value,
            });
        }
    }
    m_scene_graph_mutated = true;
    return generated_name;
}

bool SceneRuntimeContext::SortNode(std::string_view name, int index) {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;

    SceneNode* node   = iterator->second;
    bool       sorted = false;
    if (node->Parent() != nullptr) {
        sorted = SortNodeInList(node->Parent()->GetChildren(), node, index);
    } else {
        if (m_scene == nullptr || m_scene->sceneGraph == nullptr) return false;
        sorted = SortNodeInList(m_scene->sceneGraph->GetChildren(), node, index);
    }
    if (sorted) m_scene_graph_mutated = true;
    return sorted;
}

bool SceneRuntimeContext::NodeVisible(std::string_view name) const {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;
    return iterator->second->Visible();
}

bool SceneRuntimeContext::SetNodeVisible(std::string_view name, bool visible) {
    if (const auto binding = m_node_visibility.find(std::string(name));
        binding != m_node_visibility.end() && binding->second.value != nullptr) {
        binding->second.value->update(visible);
    }
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;
    iterator->second->SetVisible(visible);
    return true;
}

bool SceneRuntimeContext::SetNodeTranslate(std::string_view name, const Eigen::Vector3f& value) {
    if (const auto binding = m_node_translate.find(std::string(name));
        binding != m_node_translate.end() && binding->second.value != nullptr) {
        binding->second.value->update(value);
    }
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;
    m_node_alignment[std::string(name)].origin = value;
    ApplyNodeTransform(name);
    return true;
}

bool SceneRuntimeContext::SetNodeScale(std::string_view name, const Eigen::Vector3f& value) {
    if (const auto binding = m_node_scale.find(std::string(name));
        binding != m_node_scale.end() && binding->second.value != nullptr) {
        binding->second.value->update(value);
    }
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;
    auto& binding = m_node_alignment[std::string(name)];
    binding.scale = value;
    if (binding.alignment.empty()) {
        iterator->second->SetScale(value);
    } else {
        ApplyNodeTransform(name);
    }
    return true;
}

bool SceneRuntimeContext::SetNodeAlignment(std::string_view name, std::string alignment) {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;

    auto& binding = m_node_alignment[std::string(name)];
    if (binding.alignment.empty()) {
        binding.origin = iterator->second->Translate();
        binding.scale  = iterator->second->Scale();
    }
    binding.alignment = std::move(alignment);
    binding.size_anchor = false;
    ApplyNodeTransform(name);
    return true;
}

bool SceneRuntimeContext::SetNodeTextAlignment(std::string_view name,
                                               std::string      alignment,
                                               const Eigen::Vector3f& origin) {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;

    auto& binding = m_node_alignment[std::string(name)];
    binding.alignment   = std::move(alignment);
    binding.origin      = origin;
    binding.scale       = iterator->second->Scale();
    binding.size_anchor = true;
    ApplyNodeTransform(name);
    return true;
}

bool SceneRuntimeContext::SetNodeRotation(std::string_view name, const Eigen::Vector3f& value) {
    if (const auto binding = m_node_rotation.find(std::string(name));
        binding != m_node_rotation.end() && binding->second.value != nullptr) {
        binding->second.value->update(value);
    }
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return false;
    iterator->second->SetRotation(value);
    return true;
}

Eigen::Vector3f SceneRuntimeContext::NodeTranslate(std::string_view name) const {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return Eigen::Vector3f::Zero();
    const auto alignment = m_node_alignment.find(std::string(name));
    if (alignment != m_node_alignment.end() && ! alignment->second.alignment.empty())
        return alignment->second.origin;
    return iterator->second->Translate();
}

Eigen::Vector3f SceneRuntimeContext::NodeScale(std::string_view name) const {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return Eigen::Vector3f::Zero();
    return iterator->second->Scale();
}

Eigen::Vector3f SceneRuntimeContext::NodeRotation(std::string_view name) const {
    const auto iterator = m_nodes.find(std::string(name));
    if (iterator == m_nodes.end() || iterator->second == nullptr) return Eigen::Vector3f::Zero();
    return iterator->second->Rotation();
}

Eigen::Vector2f SceneRuntimeContext::NodeSize(std::string_view name) const {
    const auto iterator = m_node_size.find(std::string(name));
    if (iterator == m_node_size.end()) return Eigen::Vector2f::Zero();
    return iterator->second;
}

std::string SceneRuntimeContext::NodeText(std::string_view name) const {
    const auto iterator = m_text_layers.find(std::string(name));
    if (iterator == m_text_layers.end()) return {};
    return iterator->second.text();
}

bool SceneRuntimeContext::NodeTextDirty(std::string_view name) const {
    const auto iterator = m_text_layers.find(std::string(name));
    return iterator != m_text_layers.end() && iterator->second.dirty();
}

std::optional<TextLayerState> SceneRuntimeContext::NodeTextState(std::string_view name) const {
    const auto iterator = m_text_layers.find(std::string(name));
    if (iterator == m_text_layers.end()) return std::nullopt;
    return iterator->second.state();
}

bool SceneRuntimeContext::SetNodeText(std::string_view name, std::string text) {
    const auto iterator = m_text_layers.find(std::string(name));
    if (iterator == m_text_layers.end()) return false;
    iterator->second.SetText(std::move(text));
    m_node_size[std::string(name)] = iterator->second.size();
    ApplyNodeTransform(name);
    return true;
}

bool SceneRuntimeContext::ClearNodeTextDirty(std::string_view name) {
    const auto iterator = m_text_layers.find(std::string(name));
    if (iterator == m_text_layers.end()) return false;
    iterator->second.ClearDirty();
    return true;
}

void SceneRuntimeContext::PumpTextLayerCache() {
    for (auto& [name, layer] : m_text_layers) {
        (void)name;
        layer.ClearDirty();
    }
}

bool SceneRuntimeContext::NodeHasVideoTexture(std::string_view name) const {
    const auto iterator = m_node_video_textures.find(std::string(name));
    return iterator != m_node_video_textures.end() && ! iterator->second.empty();
}

bool SceneRuntimeContext::PlayNodeVideoTexture(std::string_view name) {
    const auto iterator = m_node_video_textures.find(std::string(name));
    if (iterator == m_node_video_textures.end()) return false;
    bool controlled = false;
    for (const auto& texture_key : iterator->second) {
        auto& playback  = m_video_texture_playback[texture_key];
        playback.paused = false;
        controlled      = true;
    }
    return controlled;
}

bool SceneRuntimeContext::PauseNodeVideoTexture(std::string_view name) {
    const auto iterator = m_node_video_textures.find(std::string(name));
    if (iterator == m_node_video_textures.end()) return false;
    bool controlled = false;
    for (const auto& texture_key : iterator->second) {
        auto& playback  = m_video_texture_playback[texture_key];
        playback.paused = true;
        controlled      = true;
    }
    return controlled;
}

bool SceneRuntimeContext::SetNodeVideoTextureCurrentTime(std::string_view name, double seconds) {
    const auto iterator = m_node_video_textures.find(std::string(name));
    if (iterator == m_node_video_textures.end()) return false;
    bool controlled = false;
    for (const auto& texture_key : iterator->second) {
        auto& playback            = m_video_texture_playback[texture_key];
        playback.absolute_seconds = playback.duration_seconds > 0.0
                                        ? WrapSeconds(std::max(0.0, seconds),
                                                      playback.duration_seconds)
                                        : std::max(0.0, seconds);
        controlled                = true;
    }
    return controlled;
}

double SceneRuntimeContext::NodeVideoTextureCurrentTime(std::string_view name) const {
    const auto node_iterator = m_node_video_textures.find(std::string(name));
    if (node_iterator == m_node_video_textures.end() || node_iterator->second.empty()) return 0.0;
    const auto playback_iterator = m_video_texture_playback.find(node_iterator->second.front());
    if (playback_iterator == m_video_texture_playback.end()) return 0.0;
    const auto& playback = playback_iterator->second;
    return playback.duration_seconds > 0.0
               ? WrapSeconds(playback.absolute_seconds, playback.duration_seconds)
               : std::max(0.0, playback.absolute_seconds);
}

bool SceneRuntimeContext::SetNodeVideoTextureRate(std::string_view name, float rate) {
    const auto iterator = m_node_video_textures.find(std::string(name));
    if (iterator == m_node_video_textures.end()) return false;
    bool controlled = false;
    for (const auto& texture_key : iterator->second) {
        auto& playback = m_video_texture_playback[texture_key];
        playback.rate  = std::max(0.0f, rate);
        controlled     = true;
    }
    return controlled;
}

float SceneRuntimeContext::NodeVideoTextureRate(std::string_view name) const {
    const auto node_iterator = m_node_video_textures.find(std::string(name));
    if (node_iterator == m_node_video_textures.end() || node_iterator->second.empty()) return 1.0f;
    const auto playback_iterator = m_video_texture_playback.find(node_iterator->second.front());
    if (playback_iterator == m_video_texture_playback.end()) return 1.0f;
    return playback_iterator->second.rate;
}

double SceneRuntimeContext::NodeVideoTextureDuration(std::string_view name) const {
    const auto node_iterator = m_node_video_textures.find(std::string(name));
    if (node_iterator == m_node_video_textures.end() || node_iterator->second.empty()) return 0.0;
    const auto playback_iterator = m_video_texture_playback.find(node_iterator->second.front());
    if (playback_iterator == m_video_texture_playback.end()) return 0.0;
    return playback_iterator->second.duration_seconds;
}

void SceneRuntimeContext::SetVideoTextureDuration(std::string_view texture_key, double seconds) {
    if (texture_key.empty() || ! std::isfinite(seconds) || seconds <= 0.0) return;
    auto& playback            = m_video_texture_playback[std::string(texture_key)];
    playback.duration_seconds = seconds;
    playback.absolute_seconds = WrapSeconds(playback.absolute_seconds, seconds);
}

video::VideoPlaybackState
SceneRuntimeContext::ResolveVideoPlaybackState(std::string_view texture_key,
                                               double fallback_scene_elapsed_seconds) const {
    video::VideoPlaybackState state {};
    const auto                iterator = m_video_texture_playback.find(std::string(texture_key));
    if (iterator == m_video_texture_playback.end()) {
        state.scene_elapsed_seconds = fallback_scene_elapsed_seconds;
        return state;
    }

    const auto& playback        = iterator->second;
    state.paused                = playback.paused;
    state.rate                  = playback.rate;
    state.scene_elapsed_seconds = std::max(0.0, playback.absolute_seconds);
    return state;
}

std::shared_ptr<WPSoundStream> SceneRuntimeContext::LockSoundLayer(std::string_view name) const {
    const auto iterator = m_sound_layers.find(std::string(name));
    if (iterator == m_sound_layers.end()) return nullptr;
    return iterator->second.lock();
}

void SceneRuntimeContext::ApplyNodeTransform(std::string_view name) {
    const auto node_iterator = m_nodes.find(std::string(name));
    if (node_iterator == m_nodes.end() || node_iterator->second == nullptr) return;

    auto&           binding   = m_node_alignment[std::string(name)];
    Eigen::Vector3f translate = binding.origin;
    const auto      size      = NodeSize(name);

    if (binding.size_anchor) {
        if (AlignmentContains(binding.alignment, "bottom")) {
            translate.y() += size.y() * binding.scale.y() * 0.5f;
        } else if (AlignmentContains(binding.alignment, "top")) {
            translate.y() -= size.y() * binding.scale.y() * 0.5f;
        }

        if (AlignmentContains(binding.alignment, "left")) {
            translate.x() += size.x() * binding.scale.x() * 0.5f;
        } else if (AlignmentContains(binding.alignment, "right")) {
            translate.x() -= size.x() * binding.scale.x() * 0.5f;
        }
    } else {
        if (AlignmentContains(binding.alignment, "bottom")) {
            translate.y() += size.y() * (binding.scale.y() - 1.0f) * 0.5f;
        } else if (AlignmentContains(binding.alignment, "top")) {
            translate.y() -= size.y() * (binding.scale.y() - 1.0f) * 0.5f;
        }

        if (AlignmentContains(binding.alignment, "left")) {
            translate.x() += size.x() * (binding.scale.x() - 1.0f) * 0.5f;
        } else if (AlignmentContains(binding.alignment, "right")) {
            translate.x() -= size.x() * (binding.scale.x() - 1.0f) * 0.5f;
        }
    }

    node_iterator->second->SetScale(binding.scale);
    node_iterator->second->SetTranslate(translate);
}

void SceneRuntimeContext::DispatchMediaPlaybackChanged(std::string_view name, bool playing) {
    (void)name;
    (void)playing;
}

namespace
{
template<typename ScriptDispatch>
void DispatchToScripts(std::vector<wallpaper::ScriptedDynamicValue*>&               scripted_values,
                       std::vector<std::unique_ptr<wallpaper::SceneScriptProgram>>& scene_scripts,
                       const wallpaper::ScriptHostContext&                          host_context,
                       ScriptDispatch&&                                             dispatch) {
    for (auto* value : scripted_values) {
        if (value != nullptr) dispatch(*value, host_context);
    }
    for (auto& script : scene_scripts) {
        if (script != nullptr) dispatch(*script, host_context);
    }
}
} // namespace

void SceneRuntimeContext::DispatchCursorClick(int button) {
    m_host_context->cursor_button = button;
    DispatchToScripts(
        m_scripted_values, m_scene_scripts, *m_host_context, [](auto& target, const auto& host) {
            target.DispatchCursorClick(host);
        });
}

void SceneRuntimeContext::DispatchCursorDown(int button) {
    m_host_context->cursor_button = button;
    DispatchToScripts(
        m_scripted_values, m_scene_scripts, *m_host_context, [](auto& target, const auto& host) {
            target.DispatchCursorDown(host);
        });
}

void SceneRuntimeContext::DispatchCursorEnter() {
    DispatchToScripts(
        m_scripted_values, m_scene_scripts, *m_host_context, [](auto& target, const auto& host) {
            target.DispatchCursorEnter(host);
        });
}

void SceneRuntimeContext::DispatchCursorLeave() {
    DispatchToScripts(
        m_scripted_values, m_scene_scripts, *m_host_context, [](auto& target, const auto& host) {
            target.DispatchCursorLeave(host);
        });
}

void SceneRuntimeContext::DispatchCursorMove() {
    DispatchToScripts(
        m_scripted_values, m_scene_scripts, *m_host_context, [](auto& target, const auto& host) {
            target.DispatchCursorMove(host);
        });
}

void SceneRuntimeContext::DispatchCursorUp(int button) {
    m_host_context->cursor_button = button;
    DispatchToScripts(
        m_scripted_values, m_scene_scripts, *m_host_context, [](auto& target, const auto& host) {
            target.DispatchCursorUp(host);
        });
}

void SceneRuntimeContext::DispatchMediaThumbnailChanged(const Eigen::Vector3f& primary_color,
                                                        const Eigen::Vector3f& text_color) {
    for (auto* value : m_scripted_values) {
        if (value != nullptr) value->DispatchMediaThumbnailChanged(primary_color, text_color);
    }
    for (auto& script : m_scene_scripts) {
        if (script != nullptr) script->DispatchMediaThumbnailChanged(primary_color, text_color);
    }
}

void SceneRuntimeContext::SetMediaIntegrationEnabled(bool enabled) {
    m_media_integration_enabled = enabled;
}

bool SceneRuntimeContext::MediaIntegrationEnabled() const { return m_media_integration_enabled; }

void SceneRuntimeContext::DispatchMediaEventJson(std::string_view event_json) {
    if (! m_media_integration_enabled || event_json.empty()) return;

    for (auto* value : m_scripted_values) {
        if (value != nullptr) value->DispatchMediaEventJson(event_json);
    }
    for (auto& script : m_scene_scripts) {
        if (script != nullptr) script->DispatchMediaEventJson(event_json);
    }
}

void SceneRuntimeContext::MarkSceneRequiresAudioResponse() {
    m_scene_requires_audio_response = true;
}

bool SceneRuntimeContext::SceneRequiresAudioResponse() const {
    return m_scene_requires_audio_response;
}

void SceneRuntimeContext::SetAudioResponseEnabled(bool enabled) {
    m_audio_response_enabled = enabled;
}

bool SceneRuntimeContext::AudioResponseEnabled() const { return m_audio_response_enabled; }

bool SceneRuntimeContext::AudioResponseActive() const {
    return m_audio_response_enabled && m_scene_requires_audio_response;
}

bool SceneRuntimeContext::ConsumeSceneGraphMutationFlag() {
    const bool mutated    = m_scene_graph_mutated;
    m_scene_graph_mutated = false;
    return mutated;
}

audio::AudioSpectrumSnapshot SceneRuntimeContext::CurrentAudioSpectrumSnapshot() const {
    if (! AudioResponseActive()) {
        return {};
    }
    return audio::CurrentAudioSpectrumSnapshot();
}

void SceneRuntimeContext::RecordScriptError(std::string message) {
    if (message.empty()) return;
    m_script_errors.push_back(std::move(message));
}

std::size_t SceneRuntimeContext::scriptErrorCount() const { return m_script_errors.size(); }

const std::vector<std::string>& SceneRuntimeContext::scriptErrors() const {
    return m_script_errors;
}

void SceneRuntimeContext::ClearScriptErrors() { m_script_errors.clear(); }

std::unique_ptr<SceneRuntimeContext> CreateSceneRuntimeContext(SceneRuntimeBootstrap bootstrap) {
    return std::make_unique<SceneRuntimeContext>(std::move(bootstrap));
}

} // namespace wallpaper
