#include "Runtime/SceneRuntimeContext.hpp"
#include "Runtime/SceneSettingResolver.hpp"
#include "Scene/SceneNode.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <memory>

namespace wallpaper
{
namespace
{

struct RuntimeWithProbeNodes {
    std::shared_ptr<SceneNode>           exported_probe;
    std::shared_ptr<SceneNode>           callback_probe;
    std::unique_ptr<SceneRuntimeContext> runtime;
};

RuntimeWithProbeNodes CreateRuntimeWithProbeNodes() {
    RuntimeWithProbeNodes fixture;
    fixture.runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .canvas_width  = 3840,
        .canvas_height = 2160,
    });
    if (fixture.runtime == nullptr) return fixture;

    fixture.exported_probe = std::make_shared<SceneNode>();
    fixture.callback_probe = std::make_shared<SceneNode>();
    fixture.exported_probe->SetVisible(false);
    fixture.callback_probe->SetVisible(false);

    fixture.runtime->RegisterNode("exportedProbe", fixture.exported_probe.get());
    fixture.runtime->RegisterNode("callbackProbe", fixture.callback_probe.get());
    return fixture;
}

TEST(SceneScriptMediaEventSmoke, ExportedHandlerAndSceneCallbackReceivePlaybackEvent) {
    auto fixture = CreateRuntimeWithProbeNodes();
    ASSERT_NE(fixture.runtime, nullptr);
    ASSERT_FALSE(fixture.runtime->NodeVisible("exportedProbe"));
    ASSERT_FALSE(fixture.runtime->NodeVisible("callbackProbe"));

    fixture.runtime->RegisterSceneScript(
        R"JS(
function mediaPlaybackChanged(event) {
  if (event.state === 0) {
    scene.getObject('exportedProbe').visible = true;
  }
}

scene.on('mediaPlaybackChanged', function(event) {
  if (event.state === 0) {
    scene.getObject('callbackProbe').visible = true;
  }
});
)JS",
        "");
    ASSERT_EQ(fixture.runtime->sceneScriptCount(), 1u);

    fixture.runtime->SetMediaIntegrationEnabled(true);
    fixture.runtime->DispatchMediaEventJson(R"({"type":"mediaPlaybackChanged","state":0})");

    EXPECT_TRUE(fixture.runtime->NodeVisible("exportedProbe"));
    EXPECT_TRUE(fixture.runtime->NodeVisible("callbackProbe"));
    EXPECT_EQ(fixture.runtime->scriptErrorCount(), 0u);
}

TEST(SceneScriptMediaEventSmoke, DisabledMediaIntegrationSuppressesPlaybackEvents) {
    auto fixture = CreateRuntimeWithProbeNodes();
    ASSERT_NE(fixture.runtime, nullptr);

    fixture.runtime->RegisterSceneScript(
        R"JS(
function mediaPlaybackChanged(event) {
  scene.getObject('exportedProbe').visible = true;
}

scene.on('mediaPlaybackChanged', function(event) {
  scene.getObject('callbackProbe').visible = true;
});
)JS",
        "");
    ASSERT_EQ(fixture.runtime->sceneScriptCount(), 1u);

    fixture.runtime->SetMediaIntegrationEnabled(false);
    fixture.runtime->DispatchMediaEventJson(R"({"type":"mediaPlaybackChanged","state":0})");

    EXPECT_FALSE(fixture.runtime->NodeVisible("exportedProbe"));
    EXPECT_FALSE(fixture.runtime->NodeVisible("callbackProbe"));
    EXPECT_EQ(fixture.runtime->scriptErrorCount(), 0u);
}

TEST(SceneScriptMediaEventSmoke, StringBackedVec3ProjectPropertyKeepsVectorValue) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .project_properties = {
            { "scale", RuntimeScalarValue::String("2 3 4") },
        },
    });
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNode("probe", node.get());
    runtime->RegisterNodeScale(
        "probe",
        node.get(),
        ResolveVec3Setting(
            *runtime,
            nlohmann::json {
                { "user", "scale" },
                { "value", "1 1 1" },
            },
            "probe"));

    EXPECT_FLOAT_EQ(runtime->NodeScale("probe").x(), 2.0f);
    EXPECT_FLOAT_EQ(runtime->NodeScale("probe").y(), 3.0f);
    EXPECT_FLOAT_EQ(runtime->NodeScale("probe").z(), 4.0f);

    runtime->ApplyProjectPropertyOverride({
        { "scale", RuntimeScalarValue::String("5 6 7") },
    });
    runtime->Tick(1.0 / 60.0);

    EXPECT_FLOAT_EQ(runtime->NodeScale("probe").x(), 5.0f);
    EXPECT_FLOAT_EQ(runtime->NodeScale("probe").y(), 6.0f);
    EXPECT_FLOAT_EQ(runtime->NodeScale("probe").z(), 7.0f);
}

TEST(SceneScriptMediaEventSmoke, UserBoundScriptedVisibleCanHideLayer) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .project_properties = {
            { "newproperty24", RuntimeScalarValue::Bool(false) },
        },
    });
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNodeVisibility(
        "part",
        node.get(),
        ResolveBoolSetting(
            *runtime,
            nlohmann::json {
                { "script", "export function update(value) { return value; }" },
                { "user", "newproperty24" },
                { "value", true },
            },
            "part"));

    runtime->Tick(1.0 / 60.0);

    EXPECT_FALSE(runtime->NodeVisible("part"));
    EXPECT_FALSE(node->Visible());
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

TEST(SceneScriptMediaEventSmoke, UserBoundScriptedVisibleIgnoresObjectReturn) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .project_properties = {
            { "newproperty24", RuntimeScalarValue::Bool(false) },
        },
    });
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNodeVisibility(
        "part",
        node.get(),
        ResolveBoolSetting(
            *runtime,
            nlohmann::json {
                {
                    "script",
                    R"JS(
var weizhi = { x: 1, y: 2, z: 3 };

export function init(value) {
  return value;
}

export function update(value) {
  return weizhi;
}
)JS",
                },
                { "user", "newproperty24" },
                { "value", true },
            },
            "part"));

    EXPECT_FALSE(runtime->NodeVisible("part"));
    EXPECT_FALSE(node->Visible());

    runtime->Tick(1.0 / 60.0);
    runtime->Tick(3.0);

    EXPECT_FALSE(runtime->NodeVisible("part"));
    EXPECT_FALSE(node->Visible());
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

TEST(SceneScriptMediaEventSmoke, UserBoundScriptedVisibleKeepsLaterOverride) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .project_properties = {
            { "newproperty24", RuntimeScalarValue::Bool(true) },
        },
    });
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNodeVisibility(
        "part",
        node.get(),
        ResolveBoolSetting(
            *runtime,
            nlohmann::json {
                {
                    "script",
                    R"JS(
var weizhi = { x: 1, y: 2, z: 3 };

export function init(value) {
  weizhi = value;
  return value;
}

export function update(value) {
  return weizhi;
}
)JS",
                },
                { "user", "newproperty24" },
                { "value", true },
            },
            "part"));

    runtime->ApplyProjectPropertyOverride({
        { "newproperty24", RuntimeScalarValue::Bool(false) },
    });
    runtime->Tick(1.0 / 60.0);
    runtime->Tick(3.0);

    EXPECT_FALSE(runtime->NodeVisible("part"));
    EXPECT_FALSE(node->Visible());
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

TEST(SceneScriptMediaEventSmoke, ScriptPropertyOverrideDrivesLayerOrigin) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .canvas_width  = 3840,
        .canvas_height = 2160,
        .project_properties = {
            { "x", RuntimeScalarValue::Float(0.8f) },
            { "y", RuntimeScalarValue::Float(0.25f) },
        },
    });
    ASSERT_NE(runtime, nullptr);

    auto node = std::make_shared<SceneNode>();
    runtime->RegisterNode("Clock", node.get());
    runtime->RegisterNodeTranslate(
        "Clock",
        node.get(),
        ResolveVec3Setting(
            *runtime,
            nlohmann::json {
                {
                    "script",
                    R"JS(
export var scriptProperties = createScriptProperties()
  .addSlider({ name: 'x', value: 0.5 })
  .addSlider({ name: 'y', value: 0.5 })
  .finish();

export function update(value) {
  value.x = scriptProperties.x * engine.canvasSize.x;
  value.y = scriptProperties.y * engine.canvasSize.y;
  return value;
}
)JS",
                },
                {
                    "scriptproperties",
                    {
                        { "x", { { "user", "x" }, { "value", 1.0f } } },
                        { "y", { { "user", "y" }, { "value", 1.0f } } },
                    },
                },
                { "value", "0.00000 0.00000 0.00000" },
            },
            "Clock"));

    runtime->Tick(1.0 / 60.0);

    EXPECT_FLOAT_EQ(runtime->NodeTranslate("Clock").x(), 3072.0f);
    EXPECT_FLOAT_EQ(runtime->NodeTranslate("Clock").y(), 540.0f);
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

} // namespace
} // namespace wallpaper
