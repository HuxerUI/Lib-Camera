#include <filesystem>
#include <iostream>

#include "camera_recording_test_fixture.h"

using namespace huxerui;
using namespace huxerui::camera;
using namespace huxerui::camera::test;

namespace {

void TestRecordingPause(PhotoTestFixture& fixture, const File& file) {
  using namespace std::chrono_literals;
  using namespace huxerui::media;
  const auto owner = fixture.context.owner;
  owner->frames = std::make_shared<camera::detail::FramePipeline>(CameraOptions{});
  std::optional<CameraResult<void>> result;
  auto task = fixture.context.callers->Launch([owner, file, &result]() -> Task<void> {
    result = co_await camera::detail::RecordVideo(owner, file);
  });
  fixture.Drain();
  Check(owner->status->recording_video, "Recording did not start.");

  camera::detail::FrameProducer producer;
  owner->SetRecordingPaused(true);
  owner->SetRecordingPaused(true);
  Check(owner->status->recording_paused && owner->status->recording_video,
        "Pausing must keep the recording in progress.");
  const auto unpublished = std::make_shared<macos::PixelBufferTexture>(huxerui::Size{64.0F, 64.0F});
  const auto preview = TakeRecordingResult(owner->frames->Process(producer, {.texture = unpublished}, 1s, 64, 64));
  Check(preview.output.texture == unpublished && preview.source_crop == camera::detail::full_texture_crop,
        "Preview must continue at the original geometry while recording is paused.");
  owner->SetRecordingPaused(false);
  Check(!owner->status->recording_paused, "Resuming must clear the pause state.");

  const auto input = SolidTexture(255, 0);
  std::chrono::microseconds timestamp = 17s;
  const auto capture_until = [&](std::chrono::microseconds duration, std::chrono::milliseconds step) {
    fixture.platform.Await(fixture.runtime, [&] {
      if (result || owner->RecordedDuration() > duration) return true;
      TakeRecordingResult(owner->frames->Process(producer, {.texture = input}, timestamp, 64, 64));
      timestamp += step;
      return false;
    });
    Check(!result, "Recording ended before it was stopped.");
  };
  capture_until(0us, 1ms);
  const auto before_pause = timestamp - 17s;
  owner->SetRecordingPaused(true);
  timestamp += 30s;
  owner->SetRecordingPaused(false);
  capture_until(before_pause, 10ms);

  owner->SetRecordingPaused(true);
  owner->StopRecording();
  owner->SetRecordingPaused(false);
  Check(!owner->status->recording_paused, "Stopping a paused recording must clear the pause state.");
  fixture.platform.Await(fixture.runtime, [&] { return result.has_value(); });
  TakeRecordingResult(std::move(*result));
  fixture.Drain();
  Check(!owner->status->recording_video, "A paused recording must finalize on stop.");

  const auto duration = owner->RecordedDuration();
  Check(duration > before_pause && duration < 30s, "The recording timeline must exclude the pause gap.");
  auto demuxer = TakeRecordingResult(MediaDemuxer::Open(TakeRecordingResult(file.OpenRandomAccess())));
  const auto written_duration = demuxer.Duration();
  Check(written_duration && *written_duration == MediaTimestamp{duration.count(), 1'000'000},
        "Reported duration must match the MP4 after resuming.");
}

constexpr std::uint32_t recording_width = 96;
constexpr std::uint32_t recording_height = 64;

template <class Destination>
std::shared_ptr<ExternalTexture> TestRecordedFrame(PhotoTestFixture& fixture, camera::detail::FrameProducer& producer,
    const File& file, Destination destination, media::ContainerLayout layout, bool unmount = false) {
  using namespace huxerui::media;
  std::optional<CameraResult<void>> result;
  const auto owner = fixture.context.owner;
  auto task = fixture.context.callers->Launch(
      [owner, &result, destination = std::move(destination)]() mutable -> Task<void> {
        result = co_await camera::detail::RecordVideo(owner, std::move(destination));
      });
  fixture.Drain();
  Check(owner->status->recording_video, "Recording did not start.");
  Check(owner->RecordedDuration().count() == 0, "A new recording must start at zero duration.");

  const auto input = SolidTexture(255, 0);
  const auto processed = TakeRecordingResult(
      owner->frames->Process(producer, {.texture = input}, std::chrono::seconds(17), 64, 64));
  const auto& preview = processed.output;
  Check(preview.rotation == PreviewRotation::R90 && preview.mirrored && preview.crop == huxerui::Rect{0, 0, 1, 1},
        "Sizing must consume the processed crop and preserve rotation and mirroring.");
  const auto* texture = dynamic_cast<const macos::PixelBufferTexture*>(preview.texture.get());
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
      pixels(texture ? texture->AcquireFrame() : nullptr, CVPixelBufferRelease);
  Check(pixels && CVPixelBufferGetWidth(pixels.get()) == recording_width &&
            CVPixelBufferGetHeight(pixels.get()) == recording_height,
        "Preview texture must have the requested physical dimensions.");
  if (unmount) {
    fixture.context.visible = false;
    fixture.Drain();
  } else {
    owner->StopRecording();
  }
  fixture.platform.Await(fixture.runtime, [&] { return result.has_value(); });
  TakeRecordingResult(std::move(*result));
  fixture.Drain();
  Check(!owner->status->recording_video, "Recording remained busy after finalization.");

  auto demuxer = TakeRecordingResult(MediaDemuxer::Open(TakeRecordingResult(file.OpenRandomAccess())));
  Check(demuxer.Layout() == layout && demuxer.Tracks().size() == 1, "Unexpected recording layout or track count.");
  const auto& track = demuxer.Tracks().front();
  const auto& format = track.descriptions.front();
  Check(track.kind == TrackKind::Video && track.rotation == VideoRotation::Clockwise90 && format.codec == CodecId::H264,
        "Recording lost the processed frame geometry.");
  const auto visible = format.visible.value_or(VideoRectangle{0, 0, *format.coded_width, *format.coded_height});
  Check(visible.width == recording_width && visible.height == recording_height,
        "The visible video frame must have the requested preview texture dimensions.");
  auto decoder = TakeRecordingResult(VideoDecoder::Create({.initialization_data = format.initialization_data,
      .width = *format.coded_width, .height = *format.coded_height}));
  Check(TakeRecordingResult(demuxer.Read()).has_value(), "Missing video track configuration.");
  auto item = TakeRecordingResult(demuxer.Read());
  Check(item && std::holds_alternative<ContainerPacket>(*item), "Recording has no frame.");
  const auto& packet = std::get<ContainerPacket>(*item);
  Check(packet.pts && packet.pts->ticks == 0, "Recording did not start at a relative timestamp of zero.");
  const auto duration = owner->RecordedDuration();
  if (unmount) {
    Check(duration.count() == 0, "A closed session must report zero recorded duration.");
  } else {
    Check(packet.duration && MediaTimestamp{duration.count(), 1'000'000} == *packet.duration,
          "The final recorded duration must match the MP4 sample duration.");
  }
  fixture.platform.Await(fixture.runtime, [&] {
    return TakeRecordingResult(decoder.TrySubmit({packet.data, *packet.pts, packet.dts, packet.duration,
                                                 packet.sync_sample.value_or(false)})) == SubmitStatus::Accepted;
  });
  TakeRecordingResult(decoder.Finish());
  fixture.platform.Await(fixture.runtime, [&] {
    while (auto output = TakeRecordingResult(decoder.TryRead())) {
      if (auto* frame = std::get_if<VideoFrame>(&*output)) {
        const auto luma = std::to_integer<int>(frame->planes.front().data.AsBytes().front());
        Check(luma > 140, "The encoded video contains the red source frame instead of the green processed frame.");
        return true;
      }
    }
    return false;
  });
  return preview.texture;
}

void TestSizedPreviewMetering() {
  using namespace camera::detail;
  FramePipeline pipeline({.output_pixel_width = 64, .output_pixel_height = 64});
  FrameProducer producer;
  const auto input = SolidTexture(255, 0, 128, 64);
  const auto frame = TakeRecordingResult(pipeline.Process(
      producer, {.texture = input, .rotation = PreviewRotation::R90}, std::chrono::seconds(1), 128, 64));
  Check(frame.output.crop == full_texture_crop && frame.source_crop == huxerui::Rect{0.25F, 0, 0.5F, 1},
        "The sized preview must report the source region that it displays.");
  const huxerui::Size bounds{128, 96};
  const PreviewOptions options{.fit = ImageFit::Cover};
  const auto point = PreviewTexturePoint(frame.output, options, Facing::Front, bounds, {96, 16}, frame.source_crop);
  Check(point && *point == huxerui::Point{0.375F, 0.75F},
        "Metering must undo cover placement, rotation, mirroring, and the output crop.");
}

void TestRecording() {
  struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("huxerui-camera-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
  } directory;
  Check(std::filesystem::create_directory(directory.path), "Cannot create the recording test directory.");
  int factory_calls = 0;
  PhotoTestFixture fixture;
  TestRecordingPause(fixture, File((directory.path / "paused.mp4").u8string()));
  auto factory = std::make_shared<const FrameProcessorFactory>([&](std::uint32_t width, std::uint32_t height) -> FrameProcessor {
    Check(width == 64 && height == 64, "The recording factory must receive the source frame size.");
    ++factory_calls;
    return [green = SolidTexture(0, 255, 128, 96)](const PreviewOutput&, std::chrono::nanoseconds timestamp) {
      Check(timestamp == std::chrono::seconds(17), "The frame processor received the wrong capture timestamp.");
      return CameraResult<PreviewOutput>::Success({.texture = green, .crop = {0.125F, 0.125F, 0.75F, 0.75F},
                                                  .rotation = PreviewRotation::R90, .mirrored = true});
    };
  });
  fixture.context.owner->frames = std::make_shared<camera::detail::FramePipeline>(
      CameraOptions{.frame_processor_factory = std::move(factory), .output_pixel_width = recording_width,
                    .output_pixel_height = recording_height});
  camera::detail::FrameProducer producer;
  const File file((directory.path / "file.mp4").u8string());
  const File seekable((directory.path / "seekable.mp4").u8string());
  const File sequential((directory.path / "sequential.mp4").u8string());
  const auto first = TestRecordedFrame(fixture, producer, file, file, media::ContainerLayout::Default);
  const auto second = TestRecordedFrame(fixture, producer, seekable,
      TakeRecordingResult(seekable.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew)),
      media::ContainerLayout::Default);
  const auto third = TestRecordedFrame(fixture, producer, sequential, TakeRecordingResult(sequential.OpenWrite()),
                                       media::ContainerLayout::Fragmented, true);
  Check(first == second && first == third, "Sized frames must reuse the preview texture identity.");
  Check(factory_calls == 1, "Recording starts and stops must share the producer's processor.");
}

} // namespace

int main() {
  try {
    TestSizedPreviewMetering();
    TestRecording();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
