#include "Runtime/SceneRuntimeContext.hpp"
#include "WPSoundParser.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace wallpaper
{
namespace
{

class FakeSoundStream final : public audio::SoundStream {
public:
    uint64_t NextPcmData(void*, uint32_t) override { return 0; }
    void     PassDesc(const Desc&) override {}
};

TEST(SceneScriptSoundLayerSmoke, ScriptControlsNativeSoundLayer) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .canvas_width  = 3840,
        .canvas_height = 2160,
    });
    ASSERT_NE(runtime, nullptr);

    WPSoundStream::StreamFactory factory = [](const audio::SoundStream::Desc&) {
        return std::make_shared<FakeSoundStream>();
    };
    auto sound_layer = std::make_shared<WPSoundStream>(
        std::vector<WPSoundStream::StreamFactory> { std::move(factory) },
        WPSoundStream::Config { .startsilent = true });

    runtime->RegisterSoundLayer("morningAudio", sound_layer);
    ASSERT_TRUE(runtime->HasSoundLayer("morningAudio"));
    ASSERT_FALSE(runtime->SoundLayerPlaying("morningAudio"));

    runtime->RegisterSceneScript(
        R"JS(
function update() {
  var morningAudio = scene.getObject('morningAudio');
  if (!morningAudio.isPlaying()) {
    morningAudio.play();
  }
}
)JS",
        "");
    ASSERT_EQ(runtime->sceneScriptCount(), 1u);

    runtime->Tick(1.0 / 60.0);

    EXPECT_TRUE(runtime->SoundLayerPlaying("morningAudio"));
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

TEST(SceneScriptSoundLayerSmoke, CursorClickSoundLayerDoesNotPlayOnHoverOnly) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .canvas_width  = 3840,
        .canvas_height = 2160,
    });
    ASSERT_NE(runtime, nullptr);

    WPSoundStream::StreamFactory factory = [](const audio::SoundStream::Desc&) {
        return std::make_shared<FakeSoundStream>();
    };
    auto sound_layer = std::make_shared<WPSoundStream>(
        std::vector<WPSoundStream::StreamFactory> { std::move(factory) },
        WPSoundStream::Config { .startsilent = true });

    runtime->RegisterSoundLayer("tapAudio", sound_layer);
    ASSERT_TRUE(runtime->HasSoundLayer("tapAudio"));
    ASSERT_FALSE(runtime->SoundLayerPlaying("tapAudio"));

    runtime->RegisterSceneScript(
        R"JS(
function cursorClick() {
  scene.getObject('tapAudio').play();
}
)JS",
        "");
    ASSERT_EQ(runtime->sceneScriptCount(), 1u);

    runtime->SetCursorInput(0.25f, 0.75f);
    runtime->SetCursorEnter(true);
    bool cursor_was_in_window = runtime->DispatchCursorFrameEvents(false);
    cursor_was_in_window      = runtime->DispatchCursorFrameEvents(cursor_was_in_window);
    runtime->Tick(1.0 / 60.0);

    EXPECT_TRUE(cursor_was_in_window);
    EXPECT_FALSE(runtime->SoundLayerPlaying("tapAudio"));
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

TEST(SceneScriptSoundLayerSmoke, CursorClickSoundLayerPlaysOnceOnTapEdge) {
    auto runtime = CreateSceneRuntimeContext(SceneRuntimeBootstrap {
        .canvas_width  = 3840,
        .canvas_height = 2160,
    });
    ASSERT_NE(runtime, nullptr);

    WPSoundStream::StreamFactory factory = [](const audio::SoundStream::Desc&) {
        return std::make_shared<FakeSoundStream>();
    };
    auto sound_layer = std::make_shared<WPSoundStream>(
        std::vector<WPSoundStream::StreamFactory> { std::move(factory) },
        WPSoundStream::Config { .startsilent = true });

    runtime->RegisterSoundLayer("tapAudio", sound_layer);
    ASSERT_TRUE(runtime->HasSoundLayer("tapAudio"));
    ASSERT_FALSE(runtime->SoundLayerPlaying("tapAudio"));

    runtime->RegisterSceneScript(
        R"JS(
var clickCount = 0;
function cursorClick() {
  clickCount++;
  scene.getObject('tapAudio').play();
}
function update() {
  if (clickCount > 1) {
    scene.getObject('tapAudio').stop();
  }
}
)JS",
        "");
    ASSERT_EQ(runtime->sceneScriptCount(), 1u);

    runtime->SetCursorInput(0.25f, 0.75f);
    runtime->SetCursorEnter(true);
    runtime->SetCursorButtons(0u, 1u, 1u);
    bool cursor_was_in_window = runtime->DispatchCursorFrameEvents(false);
    runtime->SetCursorButtons(0u, 0u, 0u);
    cursor_was_in_window = runtime->DispatchCursorFrameEvents(cursor_was_in_window);
    runtime->Tick(1.0 / 60.0);

    EXPECT_TRUE(cursor_was_in_window);
    EXPECT_TRUE(runtime->SoundLayerPlaying("tapAudio"));
    EXPECT_EQ(runtime->scriptErrorCount(), 0u);
}

} // namespace
} // namespace wallpaper
