#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <iostream>

#include "camera_recording_audio.h"
#include "capture_run.h"

#include "camera_recording_test_fixture.h"

using namespace huxerui;
using namespace huxerui::camera;
using namespace huxerui::camera::test;

namespace {

struct MicrophoneTestContext {
  std::shared_ptr<audio::detail::CaptureRun> run;
  std::optional<audio::AudioCaptureSession> session;
};

MicrophoneTestContext* active_microphone = nullptr;

[[huxerui::composable]]
View TestMicrophone(MicrophoneTestContext* context, std::shared_ptr<const audio::PcmProcessorFactory> processor) {
  context->session = audio::UseAudioCapture({.active = true, .pcm_processor = processor});
  return Canvas([](PaintContext&, huxerui::Size) {});
}

class BufferedMicrophoneProcessor final : public audio::PcmProcessor {
public:
  explicit BufferedMicrophoneProcessor(std::shared_ptr<std::atomic<unsigned>> finishes)
      : finishes_(std::move(finishes)) {}

  audio::PcmTiming Timing() const noexcept override { return audio::PcmTiming::CaptureTime; }

  audio::CaptureResult<void> Process(std::span<const std::int16_t> pcm) override {
    ready_ = std::move(tail_);
    tail_ = audio::PcmBlock(pcm.begin(), pcm.end());
    return audio::CaptureResult<void>::Success();
  }

  audio::CaptureResult<std::optional<audio::PcmBlock>> TryRead() override {
    return audio::CaptureResult<std::optional<audio::PcmBlock>>::Success(std::exchange(ready_, std::nullopt));
  }

  audio::CaptureResult<void> Finish() override {
    ready_ = std::move(tail_);
    ++*finishes_;
    return audio::CaptureResult<void>::Success();
  }

private:
  std::shared_ptr<std::atomic<unsigned>> finishes_;
  std::optional<audio::PcmBlock> ready_;
  std::optional<audio::PcmBlock> tail_;
};

template <class Destination>
void TestSoundRecording(PhotoTestFixture& fixture, const MicrophoneTestContext& input,
                        const File& file, Destination destination,
                        media::ContainerLayout layout, bool stop_paused) {
  using namespace huxerui::media;
  using namespace std::chrono_literals;
  const auto owner = fixture.context.owner;
  Check(input.session && input.run, "The synthetic microphone did not start.");
  auto microphone = *input.session;
  std::optional<CameraResult<void>> result;
  auto task = fixture.context.callers->Launch(
      [owner, microphone, destination = std::move(destination), &result]() mutable -> Task<void> {
        result = co_await camera::detail::RecordVideo(owner, std::move(destination), std::optional{microphone});
      });
  fixture.Drain();
  Check(owner->status->recording_video, "The microphone recording did not start.");
  Check(!microphone.OpenReader().Succeeded(), "The recording must own the microphone reader.");

  const auto run = input.run;
  const auto video = SolidTexture(0, 255);
  camera::detail::FrameProducer producer;
  std::array<std::int16_t, 480> pcm;
  pcm.fill(2000);
  auto capture_time = camera::detail::RecordingClockNow();
  const auto start = capture_time;
  const auto feed = [&](int blocks, bool offer_video) {
    for (int index = 0; index < blocks; ++index) {
      while (camera::detail::RecordingClockNow() < capture_time + 10ms) std::this_thread::sleep_for(1ms);
      Check(run->Publish(audio::detail::mono_48k, audio::CaptureClock::CoreMediaHostTime,
                         {capture_time.count(), camera::detail::capture_timescale}, pcm),
            "The synthetic microphone could not deliver PCM.");
      if (offer_video && index % 3 == 0) {
        TakeRecordingResult(owner->frames->Process(producer, {.texture = video}, capture_time, 64, 64, capture_time));
      }
      capture_time += 10ms;
      fixture.Drain();
      if (result && !result->Succeeded()) throw std::runtime_error(result->Error().message);
    }
  };
  feed(10, true);
  owner->SetRecordingPaused(true);
  feed(10, true);
  if (!stop_paused) {
    owner->SetRecordingPaused(false);
    feed(10, true);
    // The final PCM block must arrive after StopRecording records the cutoff.
    std::this_thread::sleep_for(5ms);
  }
  const auto wall_time = camera::detail::RecordingClockNow() - start;
  owner->StopRecording();
  feed(3, false);
  fixture.platform.Await(fixture.runtime, [&] { return result.has_value(); });
  TakeRecordingResult(std::move(*result));
  fixture.Drain();
  Check(!owner->status->recording_video && !owner->status->recording_paused && !run->StopToken().stop_requested(),
        "The recording must finish without stopping the microphone.");
  Check(microphone.OpenReader().Succeeded(), "The finalized recording must release its reader.");
  const auto recorded = owner->RecordedDuration();
  Check(recorded >= 80ms && recorded < wall_time - 50ms, "The shared recording timeline must omit the pause.");

  auto demuxer = TakeRecordingResult(MediaDemuxer::Open(TakeRecordingResult(file.OpenRandomAccess())));
  Check(demuxer.Layout() == layout && demuxer.Tracks().size() == 2, "The muxer must write two MP4 tracks.");
  const auto& picture = demuxer.Tracks()[0];
  const auto& sound = demuxer.Tracks()[1];
  Check(picture.kind == TrackKind::Video && picture.descriptions.front().codec == CodecId::H264 &&
            sound.kind == TrackKind::Audio && sound.descriptions.front().codec == CodecId::Aac,
        "Unexpected video or microphone codec.");
  std::array<unsigned, 2> packets{};
  while (const auto item = TakeRecordingResult(demuxer.Read())) {
    if (const auto* packet = std::get_if<ContainerPacket>(&*item)) ++packets.at(packet->track_id - 1);
  }
  Check(packets[0] > 1 && packets[1] > 1, "Both tracks must contain media.");
  const auto video_range = demuxer.PresentationRange(picture.id);
  const auto audio_range = demuxer.PresentationRange(sound.id);
  Check(video_range && audio_range, "Both tracks must have a presentation range.");
  const auto video_end = video_range->end.Rescale(1'000'000)->ticks;
  const auto audio_end = audio_range->end.Rescale(1'000'000)->ticks;
  const auto limit = layout == ContainerLayout::Fragmented ? 75'000 : 5'000;
  Check(std::abs(video_end - audio_end) < limit, "Microphone and video must reach the same cutoff.");
}

void TestRecordingsWithSound() {
  struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("huxerui-camera-audio-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
  } directory;
  Check(std::filesystem::create_directory(directory.path), "Cannot create the audio recording test directory.");
  auto finishes = std::make_shared<std::atomic<unsigned>>(0);
  auto processor = std::make_shared<const audio::PcmProcessorFactory>([finishes](const media::AudioFormat&) {
    return std::make_unique<BufferedMicrophoneProcessor>(finishes);
  });
  MicrophoneTestContext microphone;
  active_microphone = &microphone;
  PhotoTestFixture fixture(Facing::Front, [](ApplicationContext& app) {
    app.RegisterPlatformModule<audio::detail::CaptureBackend>(audio::detail::capture_module, [](Runtime&) {
      return audio::detail::CaptureBackend{
          .start = [](const std::shared_ptr<audio::detail::CaptureRun>& run, audio::detail::CaptureCompleted complete)
              -> std::function<void()> {
            active_microphone->run = run;
            return [complete = std::move(complete)] { complete(std::nullopt); };
          },
          .use_sdk_permission = false};
    });
  }, TestMicrophone(&microphone, std::move(processor)));
  fixture.context.owner->frames = std::make_shared<camera::detail::FramePipeline>(CameraOptions{});
  const File file((directory.path / "audio.mp4").u8string());
  const File seekable((directory.path / "audio-seekable.mp4").u8string());
  const File sequential((directory.path / "audio-fragmented.mp4").u8string());
  TestSoundRecording(fixture, microphone, file, file, media::ContainerLayout::Default, false);
  TestSoundRecording(fixture, microphone, seekable,
      TakeRecordingResult(seekable.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew)),
      media::ContainerLayout::Default, false);
  TestSoundRecording(fixture, microphone, sequential, TakeRecordingResult(sequential.OpenWrite()),
                     media::ContainerLayout::Fragmented, true);
  Check(*finishes == 3, "Every recording must drain the microphone processor through EOF.");
  active_microphone = nullptr;
}

} // namespace

int main() {
  try {
    TestRecordingsWithSound();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
