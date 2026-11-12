#include "camera_internal.h"
#include "camera_session.h"

#include <algorithm>
#include <cmath>
#include <utility>

#if defined(HUXERUI_CAMERA_RECORDING)
#include <atomic>
#include <condition_variable>
#include <span>
#include <thread>

#include <huxerui/media_container.h>

#include "camera_recording_frame.h"

#if defined(__ANDROID__)
#include <huxerui/android/external_texture.h>
#elif defined(_WIN32)
#include <huxerui/windows/external_texture.h>
#endif
#endif

namespace huxerui::camera::detail {

#if defined(HUXERUI_CAMERA_RECORDING)
using namespace huxerui::media;

#if defined(__ANDROID__) || defined(_WIN32)
CodecResult<VideoFrame> AcquireRecordingFrame(const std::shared_ptr<ExternalTexture>& texture, MediaTimestamp pts) {
#if defined(__ANDROID__)
  using Texture = android::GlTexture;
  constexpr auto pixel_format = VideoPixelFormat::Opaque;
#else
  using Texture = windows::D3D11Texture;
  constexpr auto pixel_format = VideoPixelFormat::NV12;
#endif
  const auto* source = dynamic_cast<const Texture*>(texture.get());
  const auto snapshot = source ? source->AcquireFrame() : nullptr;
  if (!snapshot) {
    return CodecResult<VideoFrame>::Failure({CodecErrorCode::Unsupported, "Expected a published GPU video texture."});
  }
  const auto width = static_cast<std::uint32_t>(snapshot->PixelWidth());
  const auto height = static_cast<std::uint32_t>(snapshot->PixelHeight());
  return MakeVideoFrame(texture, {.width = width, .height = height, .visible = {0, 0, width, height},
                                 .pixel_format = pixel_format}, pts);
}
#endif

namespace {

constexpr std::uint32_t recording_timescale = 1'000'000;
constexpr std::int64_t nominal_frame_duration = recording_timescale / 30;

struct RecordingFrame {
  VideoFrame video;
  Rect crop;
  PreviewRotation rotation;
  std::uint64_t generation;
};

using OpenMuxer = std::function<MediaMuxer(const MediaTrack&)>;
using RecordingCompletion = std::function<void(std::shared_ptr<VideoRecording>, CameraResult<void>)>;

template <class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) {
    auto code = CameraErrorCode::CaptureFailed;
    if constexpr (std::is_same_v<E, CodecError>) {
      if (result.Error().code == CodecErrorCode::Unsupported) code = CameraErrorCode::Unavailable;
    }
    throw CameraError{code, result.Error().message};
  }
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

VideoRectangle VisibleRectangle(VideoRectangle visible, const Rect& crop) {
  const auto coordinate = [](float value, std::uint32_t size) {
    return static_cast<std::uint32_t>(std::lround(std::clamp(value, 0.0F, 1.0F) * size));
  };
  const auto left = coordinate(crop.x, visible.width);
  const auto top = coordinate(crop.y, visible.height);
  const auto right = coordinate(crop.x + crop.width, visible.width);
  const auto bottom = coordinate(crop.y + crop.height, visible.height);
  if (right <= left || bottom <= top) {
    throw CameraError{CameraErrorCode::ConfigurationFailed, "The recording crop is empty."};
  }
  return {visible.x + left, visible.y + top, right - left, bottom - top};
}

OpenMuxer MakeOutput(File destination) {
  return [destination = std::move(destination)](const MediaTrack& track) {
    auto stream = Require(destination.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew));
    return Require(MediaMuxer::Open(std::move(stream), ContainerFormat::Mp4, std::span(&track, 1)));
  };
}

template <class Stream> OpenMuxer MakeOutput(Stream destination) {
  return [stream = std::make_shared<Stream>(std::move(destination))](const MediaTrack& track) {
    constexpr auto layout = std::is_same_v<Stream, OutputStream> ? ContainerLayout::Fragmented : ContainerLayout::Default;
    return Require(MediaMuxer::Open(std::move(*stream), ContainerFormat::Mp4, std::span(&track, 1), {.layout = layout}));
  };
}

} // namespace

class VideoRecording : public std::enable_shared_from_this<VideoRecording> {
public:
  VideoRecording(OpenMuxer output, RecordingCompletion complete)
      : output_(std::move(output)), complete_(std::move(complete)) {}

  ~VideoRecording() {
    if (!worker_.joinable()) return;
    // The worker retains the recording until finalization, even if its UI owner closes.
    if (worker_.get_id() == std::this_thread::get_id()) worker_.detach();
    else worker_.join();
  }

  void Start() {
    worker_ = std::thread([self = shared_from_this()] {
      auto result = self->Run();
      {
        std::lock_guard lock(self->mutex_);
        self->stopping_ = true;
        self->pending_.reset();
      }
      self->output_ = {};
      self->finished_ = true;
      self->complete_(self, std::move(result));
    });
  }

  bool Finished() const noexcept { return finished_; }

  std::chrono::microseconds Duration() const noexcept {
    return std::chrono::microseconds(duration_ticks_.load(std::memory_order_relaxed));
  }

  bool SetPaused(bool paused) {
    std::lock_guard lock(mutex_);
    if (stopping_ || paused_ == paused) return false;
    paused_ = paused;
    if (!paused) ++generation_;
    return true;
  }

  void Stop() noexcept {
    std::lock_guard lock(mutex_);
    stopping_ = true;
    Signal();
  }

  void Offer(const PreviewOutput& output, std::chrono::nanoseconds capture_time) {
    std::uint64_t generation;
    {
      std::lock_guard lock(mutex_);
      // Leave the producer free to deliver the next preview frame when the recorder falls behind.
      if (stopping_ || paused_ || pending_) return;
      generation = generation_;
    }
    try {
      const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(capture_time).count();
      auto frame = Require(AcquireRecordingFrame(output.texture, {ticks, recording_timescale}));
      std::lock_guard lock(mutex_);
      // A pause and resume can both occur while acquiring a snapshot.
      if (stopping_ || paused_ || generation != generation_ || pending_) return;
      pending_ = RecordingFrame{std::move(frame), output.crop, output.rotation, generation};
      Signal();
    } catch (const CameraError& error) {
      Fail(error, generation);
    } catch (const std::exception& error) {
      Fail({CameraErrorCode::CaptureFailed, error.what()}, generation);
    }
  }

private:
  void Signal() {
    ready_ = true;
    work_.notify_one();
  }

  void Wake() {
    std::lock_guard lock(mutex_);
    Signal();
  }

  void Fail(CameraError error, std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (stopping_ || paused_ || generation != generation_) return;
    failure_ = std::move(error);
    stopping_ = true;
    Signal();
  }

  CameraResult<void> Run() {
    try {
      Encode();
      return CameraResult<void>::Success();
    } catch (const CameraError& error) {
      return CameraResult<void>::Failure(error);
    } catch (const std::exception& error) {
      return CameraResult<void>::Failure({CameraErrorCode::CaptureFailed, error.what()});
    }
  }

  void WritePacket(MediaMuxer& muxer, const EncodedPacket& packet, std::int64_t duration) {
    // H.264 Baseline has no reordered frames, so decode time equals presentation time.
    if (Require(muxer.TryWrite({1, packet.data, packet.pts, packet.pts,
                                MediaTimestamp{duration, recording_timescale}, packet.key_frame}))) {
      throw std::logic_error("The single-track MP4 writer requested another track.");
    }
    duration_ticks_.store(packet.pts.ticks + duration, std::memory_order_relaxed);
  }

  void Encode() {
    std::optional<VideoEncoder> encoder;
    std::optional<MediaMuxer> muxer;
    std::optional<VideoEncodeFormat> encoded_format;
    std::optional<RecordingFrame> input;
    std::optional<EncodedPacket> packet;
    VideoFormat input_format;
    Rect crop;
    PreviewRotation rotation = PreviewRotation::R0;
    std::int64_t timeline_origin = 0;
    std::int64_t last_input = -1;
    std::int64_t duration = nominal_frame_duration;
    std::uint64_t input_generation = 0;
    bool draining = false;

    for (;;) {
      {
        std::unique_lock lock(mutex_);
        work_.wait(lock, [&] { return ready_; });
        ready_ = false;
      }
      bool progress;
      do {
        bool stopping;
        {
          std::lock_guard lock(mutex_);
          if (failure_) throw *failure_;
          if (!input) input = std::exchange(pending_, std::nullopt);
          stopping = stopping_;
        }
        progress = false;
        if (encoder) {
          while (auto output = Require(encoder->TryRead())) {
            progress = true;
            if (auto* format = std::get_if<VideoEncodeFormat>(&*output)) {
              if (encoded_format && *encoded_format != *format) {
                throw CameraError{CameraErrorCode::ConfigurationFailed, "The video encoder changed its output format."};
              }
              encoded_format = std::move(*format);
            } else if (auto* next = std::get_if<EncodedPacket>(&*output)) {
              if (!muxer) {
                if (!encoded_format) throw std::logic_error("The video encoder returned a packet before its format.");
                const auto& format = *encoded_format;
                const MediaTrack track{.id = 1, .kind = TrackKind::Video, .timescale = recording_timescale,
                    .descriptions = {{.codec = format.codec, .framing = PacketFraming::LengthPrefixedNals,
                        .initialization_data = format.initialization_data, .coded_width = format.width,
                        .coded_height = format.height, .visible = VisibleRectangle(format.visible, crop)}},
                    .rotation = static_cast<VideoRotation>(rotation)};
                muxer.emplace(output_(track));
              }
              if (packet) {
                // Look ahead one packet so skipped capture frames extend the previous sample's duration.
                duration = next->pts.ticks - packet->pts.ticks;
                WritePacket(*muxer, *packet, duration);
              }
              packet = std::move(*next);
            } else {
              if (!packet || !muxer) {
                throw CameraError{CameraErrorCode::Interrupted, "Recording stopped before a video frame was encoded."};
              }
              WritePacket(*muxer, *packet, duration);
              Require(muxer->Finish());
              return;
            }
          }
        }
        if (input) {
          auto& frame = input->video;
          if (!encoder) {
            timeline_origin = frame.pts.ticks;
            input_generation = input->generation;
            input_format = frame.format;
            crop = input->crop;
            rotation = input->rotation;
            encoder.emplace(Require(VideoEncoder::Create({.input = input_format, .profile = H264Profile::Baseline,
                .wakeup = [weak = weak_from_this()] { if (auto self = weak.lock()) self->Wake(); },
                .input_storage = frame.Storage()})));
          }
          if (frame.format != input_format) {
            Stop();
            input.reset();
            progress = true;
            continue;
          }
          auto relative = frame;
          relative.pts.ticks -= timeline_origin;
          if (relative.pts.ticks <= last_input) {
            input.reset();
            progress = true;
            continue;
          }
          const bool resumed = input->generation != input_generation;
          if (resumed) {
            // Limit the sample spanning a pause to one nominal frame interval.
            relative.pts.ticks = last_input + std::min(relative.pts.ticks - last_input, nominal_frame_duration);
          }
          // Keep the same retained input across WouldBlock until the encoder signals readiness.
          if (Require(encoder->TrySubmit(relative)) == SubmitStatus::Accepted) {
            if (resumed) {
              timeline_origin = frame.pts.ticks - relative.pts.ticks;
              input_generation = input->generation;
            }
            last_input = relative.pts.ticks;
            input.reset();
            progress = true;
          }
        } else if (stopping && !draining) {
          if (!encoder) throw CameraError{CameraErrorCode::Interrupted, "Recording stopped before the first video frame."};
          Require(encoder->Finish());
          draining = true;
          progress = true;
        }
      } while (progress);
    }
  }

  OpenMuxer output_;
  RecordingCompletion complete_;
  std::mutex mutex_;
  std::condition_variable work_;
  std::optional<RecordingFrame> pending_;
  std::optional<CameraError> failure_;
  std::uint64_t generation_ = 0;
  bool ready_ = false;
  bool stopping_ = false;
  bool paused_ = false;
  std::atomic<std::int64_t> duration_ticks_ = 0;
  std::atomic<bool> finished_ = false;
  std::thread worker_;
};

template <class Destination>
Task<CameraResult<void>> RecordVideoTask(std::shared_ptr<CameraSessionData> owner, Destination destination) {
  return AwaitCallback<CameraResult<void>>(
      [owner = std::move(owner), destination = std::move(destination)](CallbackCompletion<CameraResult<void>> complete)
          mutable -> std::function<void()> {
        if (owner->closed || !owner->engaged || !owner->frames || owner->status->state != SessionState::Running) {
          complete.Complete(CameraResult<void>::Failure({CameraErrorCode::NotReady, "Start the camera before recording."}));
          return {};
        }
        if (owner->recording && !owner->recording->Finished()) {
          complete.Complete(CameraResult<void>::Failure({CameraErrorCode::OperationInProgress,
                                                          "A video recording is already in progress."}));
          return {};
        }
        try {
          auto recording = std::make_shared<VideoRecording>(MakeOutput(std::move(destination)),
              [weak = owner->weak_from_this(), scope = owner->tasks, complete](auto finished, auto result) {
                const auto duration = result.Succeeded() ? finished->Duration() : std::chrono::microseconds{};
                // Update shared session state even when the calling task was canceled.
                scope.Post([weak, finished = std::move(finished), duration] {
                  if (auto owner = weak.lock(); owner && owner->recording == finished) {
                    owner->completed_recording_duration = duration;
                    owner->recording.reset();
                    if (owner->frames) owner->frames->SetRecording({});
                    CameraStatus value = owner->status;
                    value.recording_video = false;
                    value.recording_paused = false;
                    owner->status = std::move(value);
                  }
                });
                complete.Complete(std::move(result));
              });
          std::function<void()> cancel = [weak = std::weak_ptr(recording)] {
            if (auto recording = weak.lock()) recording->Stop();
          };
          recording->Start();
          owner->recording = recording;
          owner->frames->SetRecording(recording);
          CameraStatus value = owner->status;
          value.recording_video = true;
          value.recording_paused = false;
          owner->status = std::move(value);
          return cancel;
        } catch (const std::exception& error) {
          owner->StopRecording();
          complete.Complete(CameraResult<void>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
          return {};
        }
      });
}
#else
template <class Destination>
Task<CameraResult<void>> RecordVideoTask(std::shared_ptr<CameraSessionData>, Destination) {
  co_return CameraResult<void>::Failure({CameraErrorCode::Unavailable, "Video recording requires Lib-Media support."});
}
#endif

Rect CropForOutput(Rect crop, std::uint32_t source_width, std::uint32_t source_height,
                   std::uint32_t output_width, std::uint32_t output_height) {
  crop = crop.Intersection(full_texture_crop);
  if (crop.IsEmpty()) {
    throw CameraError{CameraErrorCode::ConfigurationFailed, "The camera output crop is empty."};
  }
  const auto aspect = (static_cast<double>(output_width) * source_height) /
                      (static_cast<double>(output_height) * source_width);
  const auto width = std::min(crop.width, static_cast<float>(crop.height * aspect));
  const auto height = std::min(crop.height, static_cast<float>(crop.width / aspect));
  return {crop.x + (crop.width - width) / 2, crop.y + (crop.height - height) / 2, width, height};
}

FramePipeline::FramePipeline(const CameraOptions& options)
    : factory_(options.frame_processor_factory),
      output_width_(options.output_pixel_width), output_height_(options.output_pixel_height) {}

std::pair<std::uint32_t, std::uint32_t> FramePipeline::ProducerSize(std::uint32_t width, std::uint32_t height) const noexcept {
  if (!factory_ && output_width_ && output_height_ &&
      static_cast<std::uint64_t>(width) * output_height_ == static_cast<std::uint64_t>(height) * output_width_) {
    return {output_width_, output_height_};
  }
  return {width, height};
}

CameraResult<PreviewOutput> FramePipeline::Process(
    FrameProducer& producer, PreviewOutput input, std::chrono::nanoseconds capture_time,
    std::uint32_t pixel_width, std::uint32_t pixel_height) {
  try {
    auto& processor = producer.processor;
    if (factory_) {
      if (!processor) {
        processor = (*factory_)(pixel_width, pixel_height);
        if (!processor) {
          return CameraResult<PreviewOutput>::Failure(
              {CameraErrorCode::CaptureFailed, "The camera frame processor factory returned an empty processor."});
        }
      }
      auto result = processor(input, capture_time);
      if (!result.Succeeded()) return result;
      input = std::move(result).Value();
    }
    if (!input.texture || !std::isfinite(input.crop.x) || !std::isfinite(input.crop.y) ||
        !std::isfinite(input.crop.width) || !std::isfinite(input.crop.height) ||
        input.crop.IsEmpty() || input.rotation < PreviewRotation::R0 || input.rotation > PreviewRotation::R270) {
      return CameraResult<PreviewOutput>::Failure({CameraErrorCode::ConfigurationFailed, "Invalid processed camera frame."});
    }
    if (output_width_ && !producer.resizer.Resize(input, output_width_, output_height_)) {
      return CameraResult<PreviewOutput>::Success({});
    }
#if defined(HUXERUI_CAMERA_RECORDING)
    std::shared_ptr<VideoRecording> recording;
    {
      std::lock_guard lock(mutex_);
      recording = recording_;
    }
    if (recording) recording->Offer(input, capture_time);
#endif
    return CameraResult<PreviewOutput>::Success(std::move(input));
  } catch (const CameraError& error) {
    return CameraResult<PreviewOutput>::Failure(error);
  } catch (const std::exception& error) {
    return CameraResult<PreviewOutput>::Failure({CameraErrorCode::CaptureFailed, error.what()});
  } catch (...) {
    return CameraResult<PreviewOutput>::Failure({CameraErrorCode::CaptureFailed, "The camera frame processor failed."});
  }
}

void FramePipeline::SetRecording(std::shared_ptr<VideoRecording> recording) {
  std::lock_guard lock(mutex_);
  recording_ = std::move(recording);
}

std::chrono::microseconds CameraSessionData::RecordedDuration() const noexcept {
  if (closed) return {};
#if defined(HUXERUI_CAMERA_RECORDING)
  if (recording) return recording->Duration();
#endif
  return completed_recording_duration;
}

void CameraSessionData::SetRecordingPaused(bool paused) {
#if defined(HUXERUI_CAMERA_RECORDING)
  if (closed || !recording || !recording->SetPaused(paused)) return;
  CameraStatus value = status;
  value.recording_paused = paused;
  status = std::move(value);
#else
  (void)paused;
#endif
}

void CameraSessionData::StopRecording() {
#if defined(HUXERUI_CAMERA_RECORDING)
  if (recording) recording->Stop();
  if (status->recording_paused) {
    CameraStatus value = status;
    value.recording_paused = false;
    status = std::move(value);
  }
#endif
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, File destination) {
  return RecordVideoTask(std::move(owner), std::move(destination));
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, RandomAccessStream destination) {
  return RecordVideoTask(std::move(owner), std::move(destination));
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, OutputStream destination) {
  return RecordVideoTask(std::move(owner), std::move(destination));
}

} // namespace huxerui::camera::detail

namespace huxerui::camera {

Task<CameraResult<void>> CameraSession::RecordVideoAsync(File destination) const {
  return detail::RecordVideo(data_, std::move(destination));
}

Task<CameraResult<void>> CameraSession::RecordVideoAsync(RandomAccessStream destination) const {
  return detail::RecordVideo(data_, std::move(destination));
}

Task<CameraResult<void>> CameraSession::RecordVideoAsync(OutputStream destination) const {
  return detail::RecordVideo(data_, std::move(destination));
}

std::chrono::microseconds CameraSession::RecordedDuration() const noexcept { return data_->RecordedDuration(); }

void CameraSession::PauseRecording() const { data_->SetRecordingPaused(true); }

void CameraSession::ResumeRecording() const { data_->SetRecordingPaused(false); }

void CameraSession::StopRecording() const { data_->StopRecording(); }

} // namespace huxerui::camera
