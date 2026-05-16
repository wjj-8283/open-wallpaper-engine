#include "Audio/AudioResponseService.h"
#include "AudioResponseAnalyzerVdsp.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace wallpaper::audio
{
namespace
{

constexpr uint32_t kAnalysisSampleRate = 12000;
constexpr uint32_t kFftSize = 1024;
constexpr uint32_t kHopSize = 200;
constexpr size_t kInterleavedChannels = 2u;
constexpr size_t kMaxFifoSamples = static_cast<size_t>(kAnalysisSampleRate) * 2u;

struct AudioResponseState
{
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<float> fifo;
    std::jthread worker;
    bool worker_started { false };
    AudioSpectrumSnapshot snapshot {};
    std::chrono::steady_clock::time_point last_submit_time {};
};

AudioResponseState g_state {};

bool SetError(std::string* error, std::string message)
{
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

bool ValidateSubmitInput(
    uint32_t sample_rate,
    uint32_t frame_count,
    const float* pcm_frames,
    std::string* error)
{
    if (sample_rate == 0) {
        return SetError(error, "sample_rate must be greater than zero");
    }
    if (sample_rate != kAnalysisSampleRate) {
        return SetError(error, "sample_rate must be 12000 Hz for audio response analysis");
    }
    if (frame_count == 0) {
        return SetError(error, "frame_count must be greater than zero");
    }
    if (pcm_frames == nullptr) {
        return SetError(error, "pcm_frames must not be null");
    }
    return true;
}

void WorkerMain(std::stop_token stop_token)
{
    while (!stop_token.stop_requested()) {
        std::array<float, kFftSize> block {};
        AudioSpectrumSnapshot next_snapshot {};
        bool analyze_block { false };
        bool decay_snapshot { false };

        {
            std::unique_lock<std::mutex> lock(g_state.mutex);
            g_state.condition.wait_for(lock, std::chrono::milliseconds(16), [&] {
                return stop_token.stop_requested() ||
                       g_state.fifo.size() >= block.size();
            });

            if (stop_token.stop_requested()) {
                break;
            }

            if (g_state.fifo.size() >= block.size()) {
                std::copy_n(g_state.fifo.begin(), block.size(), block.begin());
                g_state.fifo.erase(g_state.fifo.begin(), g_state.fifo.begin() + kHopSize);
                next_snapshot = g_state.snapshot;
                analyze_block = true;
            } else if (g_state.snapshot.generation > 0 &&
                       (std::chrono::steady_clock::now() - g_state.last_submit_time) > std::chrono::milliseconds(180)) {
                next_snapshot = g_state.snapshot;
                decay_snapshot = true;
            } else {
                continue;
            }
        }

        if (analyze_block) {
            AnalyzeAudioResponseMonoBlock(block.data(), kFftSize, &next_snapshot);
            next_snapshot.generation += 1u;
            next_snapshot.sample_rate = kAnalysisSampleRate;
        } else if (decay_snapshot) {
            DecayAudioResponseSnapshot(&next_snapshot);
        }

        std::lock_guard<std::mutex> lock(g_state.mutex);
        next_snapshot.last_submit_sample_rate = g_state.snapshot.last_submit_sample_rate;
        next_snapshot.accepted_frame_count = g_state.snapshot.accepted_frame_count;
        g_state.snapshot = next_snapshot;
    }
}

void EnsureWorkerStartedLocked()
{
    if (g_state.worker_started) {
        return;
    }

    g_state.worker = std::jthread(WorkerMain);
    g_state.worker_started = true;
}

} // namespace

bool SubmitMonoAudioFrames(
    uint32_t sample_rate,
    uint32_t frame_count,
    const float* pcm_frames,
    std::string* error)
{
    if (!ValidateSubmitInput(sample_rate, frame_count, pcm_frames, error)) {
        return false;
    }

    const size_t sample_count = static_cast<size_t>(frame_count);

    std::lock_guard<std::mutex> lock(g_state.mutex);
    EnsureWorkerStartedLocked();

    if (g_state.fifo.size() + sample_count > kMaxFifoSamples) {
        const size_t overflow = (g_state.fifo.size() + sample_count) - kMaxFifoSamples;
        if (overflow >= g_state.fifo.size()) {
            g_state.fifo.clear();
        } else {
            g_state.fifo.erase(g_state.fifo.begin(), g_state.fifo.begin() + static_cast<std::ptrdiff_t>(overflow));
        }
    }

    g_state.fifo.insert(g_state.fifo.end(), pcm_frames, pcm_frames + sample_count);
    g_state.last_submit_time = std::chrono::steady_clock::now();
    g_state.snapshot.last_submit_sample_rate = sample_rate;
    g_state.snapshot.accepted_frame_count += frame_count;
    g_state.condition.notify_one();
    return true;
}

bool SubmitAudioFrames(
    uint32_t sample_rate,
    uint32_t frame_count,
    const float* pcm_frames,
    std::string* error)
{
    if (!ValidateSubmitInput(sample_rate, frame_count, pcm_frames, error)) {
        return false;
    }

    std::vector<float> mono(static_cast<size_t>(frame_count), 0.0f);
    for (uint32_t frame = 0; frame < frame_count; ++frame) {
        mono[frame] = 0.5f * (pcm_frames[frame * 2u] + pcm_frames[(frame * 2u) + 1u]);
    }

    return SubmitMonoAudioFrames(sample_rate, frame_count, mono.data(), error);
}

AudioSpectrumSnapshot CurrentAudioSpectrumSnapshot()
{
    std::lock_guard<std::mutex> lock(g_state.mutex);
    return g_state.snapshot;
}

void ResetAudioResponseServiceForTesting()
{
    std::jthread worker;
    {
        std::lock_guard<std::mutex> lock(g_state.mutex);
        worker = std::move(g_state.worker);
        g_state.worker_started = false;
        g_state.fifo.clear();
        g_state.snapshot = {};
        g_state.snapshot.sample_rate = kAnalysisSampleRate;
        g_state.last_submit_time = {};
    }

    if (worker.joinable()) {
        worker.request_stop();
        g_state.condition.notify_all();
        worker.join();
    }
}

} // namespace wallpaper::audio
