#include "Audio/AudioResponseService.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <string>
#include <thread>

namespace wallpaper::audio
{
namespace
{

constexpr uint32_t kSubmitSampleRate = 12'000u;
constexpr uint32_t kChunkFrameCount = 200u;
constexpr uint32_t kChunkSubmitCount = 6u;

AudioSpectrumSnapshot WaitForGeneration() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);

    while (std::chrono::steady_clock::now() < deadline) {
        auto snapshot = CurrentAudioSpectrumSnapshot();
        if (snapshot.generation > 0u) {
            return snapshot;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    return CurrentAudioSpectrumSnapshot();
}

bool HasNonZeroAverage64Bin(const AudioSpectrumSnapshot& snapshot) {
    for (const float bin : snapshot.average64) {
        if (bin != 0.0f) {
            return true;
        }
    }
    return false;
}

TEST(AudioResponseMonoTest, MonoSubmitUpdatesSnapshotAt12Khz) {
    ResetAudioResponseServiceForTesting();

    std::array<float, 200> samples {};
    for (std::size_t index = 0; index < samples.size(); ++index) {
        samples[index] = std::sin(static_cast<float>(index) * 0.05f);
    }

    std::string error;
    for (uint32_t submit = 0; submit < kChunkSubmitCount; ++submit) {
        ASSERT_TRUE(SubmitMonoAudioFrames(kSubmitSampleRate, static_cast<uint32_t>(samples.size()), samples.data(), &error))
            << error;
    }

    auto snapshot = WaitForGeneration();
    EXPECT_EQ(snapshot.sample_rate, 12'000u);
    EXPECT_EQ(snapshot.last_submit_sample_rate, kSubmitSampleRate);
    EXPECT_EQ(snapshot.accepted_frame_count, samples.size() * kChunkSubmitCount);
    EXPECT_GT(snapshot.generation, 0u);
    EXPECT_TRUE(HasNonZeroAverage64Bin(snapshot));
    EXPECT_EQ(snapshot.left64, snapshot.average64);
    EXPECT_EQ(snapshot.right64, snapshot.average64);
    EXPECT_EQ(snapshot.left32, snapshot.average32);
    EXPECT_EQ(snapshot.right32, snapshot.average32);
    EXPECT_EQ(snapshot.left16, snapshot.average16);
    EXPECT_EQ(snapshot.right16, snapshot.average16);
}

TEST(AudioResponseMonoTest, MonoSubmitRejectsInvalidInput) {
    ResetAudioResponseServiceForTesting();

    std::array<float, 200> samples {};
    std::string error;

    EXPECT_FALSE(SubmitMonoAudioFrames(0, kChunkFrameCount, samples.data(), &error));
    EXPECT_NE(error.find("sample_rate"), std::string::npos);

    error.clear();
    EXPECT_FALSE(SubmitMonoAudioFrames(kSubmitSampleRate, 0, samples.data(), &error));
    EXPECT_NE(error.find("frame_count"), std::string::npos);

    error.clear();
    EXPECT_FALSE(SubmitMonoAudioFrames(kSubmitSampleRate, kChunkFrameCount, nullptr, &error));
    EXPECT_NE(error.find("pcm_frames"), std::string::npos);
}

TEST(AudioResponseMonoTest, StereoCompatibilityWrapperDownmixesToMono) {
    ResetAudioResponseServiceForTesting();

    std::array<float, 400> stereo {};
    for (std::size_t frame = 0; frame < 200; ++frame) {
        stereo[frame * 2u] = 0.25f;
        stereo[(frame * 2u) + 1u] = 0.75f;
    }

    std::string error;
    for (uint32_t submit = 0; submit < kChunkSubmitCount; ++submit) {
        ASSERT_TRUE(SubmitAudioFrames(kSubmitSampleRate, kChunkFrameCount, stereo.data(), &error))
            << error;
    }

    auto stereo_snapshot = WaitForGeneration();

    ResetAudioResponseServiceForTesting();

    std::array<float, 200> mono {};
    mono.fill(0.5f);

    error.clear();
    for (uint32_t submit = 0; submit < kChunkSubmitCount; ++submit) {
        ASSERT_TRUE(SubmitMonoAudioFrames(kSubmitSampleRate, kChunkFrameCount, mono.data(), &error))
            << error;
    }

    auto mono_snapshot = WaitForGeneration();
    EXPECT_EQ(stereo_snapshot.sample_rate, 12'000u);
    EXPECT_EQ(stereo_snapshot.last_submit_sample_rate, kSubmitSampleRate);
    EXPECT_EQ(stereo_snapshot.accepted_frame_count, kChunkFrameCount * kChunkSubmitCount);
    EXPECT_GT(stereo_snapshot.generation, 0u);
    EXPECT_EQ(mono_snapshot.sample_rate, 12'000u);
    EXPECT_EQ(mono_snapshot.last_submit_sample_rate, kSubmitSampleRate);
    EXPECT_EQ(mono_snapshot.accepted_frame_count, kChunkFrameCount * kChunkSubmitCount);
    EXPECT_GT(mono_snapshot.generation, 0u);
    EXPECT_EQ(stereo_snapshot.average64, mono_snapshot.average64);
    EXPECT_EQ(stereo_snapshot.left64, stereo_snapshot.average64);
    EXPECT_EQ(stereo_snapshot.right64, stereo_snapshot.average64);
    EXPECT_EQ(mono_snapshot.left64, mono_snapshot.average64);
    EXPECT_EQ(mono_snapshot.right64, mono_snapshot.average64);
}

} // namespace
} // namespace wallpaper::audio
