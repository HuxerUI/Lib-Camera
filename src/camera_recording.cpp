#include "camera_internal.h"
#include "camera_session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <numeric>
#include <span>
#include <thread>
#include <utility>

#include <huxerui/media_container.h>

#include "camera_recording_frame.h"
#include "camera_recording_audio.h"

#if defined(__ANDROID__)
#include <huxerui/android/external_texture.h>
#elif defined(_WIN32)
#include <huxerui/windows/external_texture.h>
#endif

namespace huxerui::camera::detail {

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

using OpenMuxer = std::function<MediaMuxer(std::span<const MediaTrack>)>;
using RecordingCompletion = std::function<void(std::shared_ptr<VideoRecording>, CameraResult<void>)>;

CameraError MicrophoneError(const audio::CaptureError& error) {
  auto code = CameraErrorCode::CaptureFailed;
  switch (error.code) {
    case audio::CaptureErrorCode::PermissionRequired: code = CameraErrorCode::PermissionRequired; break;
    case audio::CaptureErrorCode::PermissionDenied: code = CameraErrorCode::PermissionDenied; break;
    case audio::CaptureErrorCode::Unavailable: code = CameraErrorCode::Unavailable; break;
    case audio::CaptureErrorCode::NotReady: code = CameraErrorCode::NotReady; break;
    case audio::CaptureErrorCode::ReaderInUse: code = CameraErrorCode::OperationInProgress; break;
    case audio::CaptureErrorCode::Interrupted:
    case audio::CaptureErrorCode::NoAudio: code = CameraErrorCode::Interrupted; break;
    default: break;
  }
  return {code, "Microphone: " + error.message};
}

template <class T, class E> T Require(Result<T, E> result) {
  if (!result.Succeeded()) {
    auto code = CameraErrorCode::CaptureFailed;
    if constexpr (std::is_same_v<E, CodecError>) {
      if (result.Error().code == CodecErrorCode::Unsupported) code = CameraErrorCode::Unavailable;
    }
    if constexpr (std::is_same_v<E, audio::CaptureError>) throw MicrophoneError(result.Error());
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
  return [destination = std::move(destination)](std::span<const MediaTrack> tracks) {
    auto stream = Require(destination.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew));
    return Require(MediaMuxer::Open(std::move(stream), ContainerFormat::Mp4, tracks));
  };
}

template <class Stream> OpenMuxer MakeOutput(Stream destination) {
  return [stream = std::make_shared<Stream>(std::move(destination))](std::span<const MediaTrack> tracks) {
    constexpr auto layout = std::is_same_v<Stream, OutputStream> ? ContainerLayout::Fragmented : ContainerLayout::Default;
    return Require(MediaMuxer::Open(std::move(*stream), ContainerFormat::Mp4, tracks, {.layout = layout}));
  };
}

struct RecordingTrack {
  std::optional<MediaTrack> description;
  std::optional<ContainerPacket> packet;
  std::optional<AudioEncodeSummary> audio_summary;
  bool drained = false;
  bool ended = false;
};

/// Retains the next packet for each track until the muxer accepts it.
class RecordingMuxer {
public:
  RecordingMuxer(const OpenMuxer& open, bool fragmented, std::size_t track_count)
      : open_(open), fragmented_(fragmented), track_count_(track_count) {}

  RecordingTrack video;
  RecordingTrack audio;
  bool finished = false;

  bool Write(std::atomic<std::int64_t>& duration, bool has_video_lookahead) {
    const std::array tracks{&video, &audio};
    const auto active = std::span(tracks).first(track_count_);
    bool progress = false;
    if (!muxer_) {
      for (const auto* track : active) {
        if (track->drained && !track->packet) {
          throw CameraError{CameraErrorCode::Interrupted, "Recording stopped before both requested tracks had media."};
        }
      }
      if (!video.description || (!video.packet && !has_video_lookahead)) return false;
      if (track_count_ == 2 && (!video.packet || !audio.description || !audio.packet)) return false;
      if (fragmented_ && track_count_ == 2) {
        const auto dts = *audio.packet->dts;
        if (dts.ticks < 0) {
          // Shift both tracks on a grid exactly representable by both MP4 track clocks.
          const auto scale = std::gcd(recording_timescale, dts.timescale);
          offset_ = {static_cast<std::int64_t>(std::ceil(-static_cast<long double>(dts.ticks) * scale / dts.timescale)),
                     scale};
        }
      }
      std::array<MediaTrack, 2> descriptions;
      for (std::size_t index = 0; index < track_count_; ++index) descriptions[index] = *active[index]->description;
      muxer_.emplace(open_(std::span(descriptions).first(track_count_)));
      progress = true;
    }
    for (auto* track : active) {
      if (track->packet) {
        auto packet = *track->packet;
        if (offset_.ticks) {
          packet.pts->ticks += offset_.Rescale(packet.pts->timescale)->ticks;
          packet.dts->ticks += offset_.Rescale(packet.dts->timescale)->ticks;
        }
        if (!Require(muxer_->TryWrite(packet))) {
          if (track == &video) {
            duration.store(track->packet->pts->ticks + track->packet->duration->ticks, std::memory_order_relaxed);
          }
          track->packet.reset();
          progress = true;
        }
      }
      if (track->drained && !track->packet && !track->ended) {
        const auto id = track->description->id;
        if (track->audio_summary && !fragmented_) {
          Require(muxer_->EndTrack(id, track->audio_summary->priming_samples, track->audio_summary->padding_samples));
        } else {
          Require(muxer_->EndTrack(id));
        }
        track->ended = true;
        progress = true;
      }
    }
    if (std::ranges::all_of(active, [](const auto* track) { return track->ended; })) {
      Require(muxer_->Finish());
      finished = true;
    }
    return progress;
  }

private:
  const OpenMuxer& open_;
  const bool fragmented_;
  const std::size_t track_count_;
  std::optional<MediaMuxer> muxer_;
  MediaTimestamp offset_{0, 1};
};

} // namespace

class VideoRecording : public std::enable_shared_from_this<VideoRecording> {
public:
  VideoRecording(OpenMuxer output, bool fragmented, RecordingCompletion complete,
                 std::optional<audio::AudioReader> microphone)
      : output_(std::move(output)), fragmented_(fragmented), complete_(std::move(complete)),
        microphone_(std::move(microphone)) {
    if (microphone_) timeline_.emplace(RecordingClockNow());
  }

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
        self->microphone_.reset();
        self->pending_audio_.reset();
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
    if (timeline_) {
      try {
        const auto time = RecordingClockNow();
        if (paused) timeline_->Pause(time);
        else timeline_->Resume(time);
      } catch (const CameraError& error) {
        failure_ = error;
        stopping_ = true;
        Signal();
        return false;
      } catch (const std::exception& error) {
        failure_ = CameraError{CameraErrorCode::CaptureFailed, error.what()};
        stopping_ = true;
        Signal();
        return false;
      }
    }
    paused_ = paused;
    if (!paused) ++generation_;
    return true;
  }

  void Stop() noexcept {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_ = true;
    if (microphone_) {
      try {
        const auto cutoff = timeline_->Stop(RecordingClockNow());
        audio_cutoff_ = cutoff;
        audio_finish_deadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        auto result = microphone_->FinishInput(MediaTimestamp{cutoff.count(), capture_timescale});
        if (!result.Succeeded() && result.Error().code == audio::CaptureErrorCode::InvalidArgument) {
          // A paused recording may already have read past its cutoff. The timeline still clips those samples.
          result = microphone_->FinishInput();
        }
        Require(std::move(result));
      } catch (const CameraError& error) {
        failure_ = error;
      } catch (const std::exception& error) {
        failure_ = CameraError{CameraErrorCode::CaptureFailed, error.what()};
      }
    }
    Signal();
  }

  void Offer(const PreviewOutput& output, std::chrono::nanoseconds capture_time,
             std::optional<std::chrono::nanoseconds> synchronized_time) {
    std::uint64_t generation;
    {
      std::lock_guard lock(mutex_);
      // Leave the producer free to deliver the next preview frame when the recorder falls behind.
      if (stopping_ || paused_ || pending_) return;
      if (timeline_) {
        if (!synchronized_time) {
          failure_ = CameraError{CameraErrorCode::Unavailable, "The camera does not supply a microphone-compatible capture clock."};
          stopping_ = true;
          Signal();
          return;
        }
        const auto time = timeline_->Map(*synchronized_time);
        if (!time) return;
        capture_time = *time;
      }
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
      if (microphone_) {
        std::jthread reader([this](std::stop_token stop) { ReadMicrophone(stop); });
        Encode();
        return CameraResult<void>::Success();
      }
      Encode();
      return CameraResult<void>::Success();
    } catch (const CameraError& error) {
      return CameraResult<void>::Failure(error);
    } catch (const std::exception& error) {
      return CameraResult<void>::Failure({CameraErrorCode::CaptureFailed, error.what()});
    }
  }

  void ReadMicrophone(std::stop_token stop) {
    try {
      while (!stop.stop_requested()) {
        auto block = Require(microphone_->Read(stop));
        std::unique_lock lock(mutex_);
        if (!audio_space_.wait(lock, stop, [&] { return !pending_audio_; })) return;
        if (!block) {
          if (!audio_cutoff_ || !audio_capture_end_ || *audio_capture_end_ + last_audio_sample_ < *audio_cutoff_) {
            throw CameraError{CameraErrorCode::Interrupted, "The microphone ended before the recording cutoff."};
          }
          audio_eof_ = true;
          Signal();
          return;
        }
        if (block->clock != RecordingCaptureClock()) {
          throw CameraError{CameraErrorCode::ConfigurationFailed, "Recording requires capture-timed PCM on the shared clock."};
        }
        const auto start = block->frame.pts.Rescale(capture_timescale);
        const auto duration = block->frame.Duration().Rescale(capture_timescale);
        if (!start || !duration) {
          throw CameraError{CameraErrorCode::CaptureFailed, "The microphone returned invalid capture timing."};
        }
        audio_capture_end_ = std::chrono::nanoseconds(start->ticks + duration->ticks);
        last_audio_sample_ = std::chrono::nanoseconds(capture_timescale / block->frame.format.sample_rate + 1);
        pending_audio_ = std::move(block->frame);
        Signal();
      }
    } catch (const CameraError& error) {
      if (!stop.stop_requested()) FailMicrophone(error);
    } catch (const std::exception& error) {
      if (!stop.stop_requested()) FailMicrophone({CameraErrorCode::CaptureFailed, error.what()});
    }
  }

  void FailMicrophone(CameraError error) {
    std::lock_guard lock(mutex_);
    if (!failure_) failure_ = std::move(error);
    stopping_ = true;
    Signal();
  }

  static ContainerPacket VideoPacket(const EncodedPacket& packet, std::int64_t duration) {
    // H.264 Baseline has no reordered frames, so decode time equals presentation time.
    return {1, packet.data, packet.pts, packet.pts, MediaTimestamp{duration, recording_timescale}, packet.key_frame};
  }

  void Encode() {
    const std::size_t track_count = microphone_ ? 2 : 1;
    std::optional<AudioEncoder> audio_encoder;
    std::optional<AudioFormat> audio_format;
    std::deque<AudioFrame> audio_input;
    std::optional<std::int64_t> next_audio_sample;
    bool audio_draining = false;
    RecordingMuxer muxer(output_, fragmented_, track_count);
    std::optional<VideoEncoder> encoder;
    std::optional<VideoEncodeFormat> encoded_format;
    std::optional<RecordingFrame> input;
    std::optional<EncodedPacket> lookahead;
    VideoFormat input_format;
    Rect crop;
    PreviewRotation rotation = PreviewRotation::R0;
    std::int64_t timeline_origin = 0;
    std::int64_t last_input = -1;
    std::int64_t duration = nominal_frame_duration;
    std::uint64_t input_generation = 0;
    bool draining = false;
    const auto wakeup = [weak = weak_from_this()] { if (auto self = weak.lock()) self->Wake(); };

    while (!muxer.finished) {
      {
        std::unique_lock lock(mutex_);
        while (!ready_) {
          if (audio_cutoff_ && !audio_eof_) {
            if (!work_.wait_until(lock, audio_finish_deadline_, [&] { return ready_; })) {
              throw CameraError{CameraErrorCode::Interrupted, "The microphone did not reach the recording cutoff."};
            }
          } else {
            work_.wait(lock, [&] { return ready_; });
          }
        }
        ready_ = false;
      }
      bool progress;
      do {
        progress = false;
        bool stopping;
        bool audio_eof;
        std::optional<std::int64_t> video_end;
        {
          std::lock_guard lock(mutex_);
          if (failure_) throw *failure_;
          if (!input) input = std::exchange(pending_, std::nullopt);
          stopping = stopping_;
          audio_eof = audio_eof_;
          if (audio_input.empty() && pending_audio_) {
            timeline_->RetainAudio(*pending_audio_, audio_input);
            pending_audio_.reset();
            audio_space_.notify_one();
            progress = true;
          }
          if (audio_cutoff_) {
            video_end = std::chrono::duration_cast<std::chrono::microseconds>(timeline_->Duration()).count();
          }
        }
        while (encoder && !muxer.video.packet && !muxer.video.drained) {
          auto output = Require(encoder->TryRead());
          if (!output) break;
          progress = true;
          if (auto* format = std::get_if<VideoEncodeFormat>(&*output)) {
            if (encoded_format && *encoded_format != *format) {
              throw CameraError{CameraErrorCode::ConfigurationFailed, "The video encoder changed its output format."};
            }
            if (!encoded_format) {
              muxer.video.description = MediaTrack{.id = 1, .kind = TrackKind::Video, .timescale = recording_timescale,
                  .descriptions = {{.codec = format->codec, .framing = PacketFraming::LengthPrefixedNals,
                      .initialization_data = format->initialization_data, .coded_width = format->width,
                      .coded_height = format->height, .visible = VisibleRectangle(format->visible, crop)}},
                  .rotation = static_cast<VideoRotation>(rotation)};
              encoded_format = std::move(*format);
            }
          } else if (auto* next = std::get_if<EncodedPacket>(&*output)) {
            if (!encoded_format) throw std::logic_error("The video encoder returned a packet before its format.");
            if (lookahead) {
              // Skipped capture frames extend the previous sample's presentation.
              duration = next->pts.ticks - lookahead->pts.ticks;
              muxer.video.packet = VideoPacket(*lookahead, duration);
            }
            lookahead = std::move(*next);
          } else {
            if (lookahead) {
              if (video_end) duration = std::max<std::int64_t>(1, *video_end - lookahead->pts.ticks);
              muxer.video.packet = VideoPacket(*lookahead, duration);
              lookahead.reset();
            }
            muxer.video.drained = true;
          }
        }
        while (audio_encoder && !muxer.audio.packet && !muxer.audio.drained) {
          auto output = Require(audio_encoder->TryRead());
          if (!output) break;
          progress = true;
          if (auto* format = std::get_if<AudioEncodeFormat>(&*output)) {
            muxer.audio.description = MediaTrack{.id = 2, .kind = TrackKind::Audio, .timescale = format->sample_rate,
                .descriptions = {{.codec = CodecId::Aac, .framing = PacketFraming::CodecPacket,
                    .initialization_data = std::move(format->initialization_data), .sample_rate = format->sample_rate,
                    .channel_count = audio_format->ChannelCount()}}};
          } else if (auto* packet = std::get_if<EncodedPacket>(&*output)) {
            muxer.audio.packet = ContainerPacket{.track_id = 2, .data = std::move(packet->data),
                .pts = packet->pts, .dts = packet->dts.value_or(packet->pts), .duration = packet->duration,
                .sync_sample = packet->key_frame};
          } else if (auto* summary = std::get_if<AudioEncodeSummary>(&*output)) {
            muxer.audio.audio_summary = std::move(*summary);
          } else {
            muxer.audio.drained = true;
          }
        }
        progress |= muxer.Write(duration_ticks_, lookahead.has_value());
        if (muxer.finished) return;
        if (input) {
          auto& frame = input->video;
          if (!encoder) {
            if (!timeline_) timeline_origin = frame.pts.ticks;
            input_generation = input->generation;
            input_format = frame.format;
            crop = input->crop;
            rotation = input->rotation;
            encoder.emplace(Require(VideoEncoder::Create({.input = input_format, .profile = H264Profile::Baseline,
                .wakeup = wakeup, .input_storage = frame.Storage()})));
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
          const bool resumed = !timeline_ && input->generation != input_generation;
          if (resumed) {
            // Limit the sample spanning a pause in a video-only recording to one nominal frame interval.
            relative.pts.ticks = last_input + std::min(relative.pts.ticks - last_input, nominal_frame_duration);
          }
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
        if (!audio_input.empty()) {
          auto& frame = audio_input.front();
          if (!audio_encoder) {
            audio_format = frame.format;
            audio_encoder.emplace(Require(AudioEncoder::Create({.input = *audio_format, .wakeup = wakeup})));
            next_audio_sample = frame.pts.Rescale(frame.format.sample_rate)->ticks;
          }
          // The first sample keeps its shared-clock offset. AAC then advances on a contiguous sample grid.
          frame.pts = {*next_audio_sample, audio_format->sample_rate};
          if (Require(audio_encoder->TrySubmit(frame)) == SubmitStatus::Accepted) {
            *next_audio_sample += frame.sample_count;
            audio_input.pop_front();
            progress = true;
          }
        } else if (audio_eof && !audio_draining) {
          if (!audio_encoder) {
            throw CameraError{CameraErrorCode::Interrupted, "Recording stopped before the first audio sample."};
          }
          Require(audio_encoder->Finish());
          audio_draining = true;
          progress = true;
        }
      } while (progress);
    }
  }

  OpenMuxer output_;
  const bool fragmented_;
  RecordingCompletion complete_;
  std::mutex mutex_;
  std::condition_variable work_;
  std::optional<RecordingFrame> pending_;
  std::optional<CameraError> failure_;
  std::optional<audio::AudioReader> microphone_;
  std::optional<RecordingTimeline> timeline_;
  std::condition_variable_any audio_space_;
  std::optional<AudioFrame> pending_audio_;
  std::optional<std::chrono::nanoseconds> audio_cutoff_;
  std::optional<std::chrono::nanoseconds> audio_capture_end_;
  std::chrono::nanoseconds last_audio_sample_{};
  std::chrono::steady_clock::time_point audio_finish_deadline_;
  bool audio_eof_ = false;
  std::uint64_t generation_ = 0;
  bool ready_ = false;
  bool stopping_ = false;
  bool paused_ = false;
  std::atomic<std::int64_t> duration_ticks_ = 0;
  std::atomic<bool> finished_ = false;
  std::thread worker_;
};

template <class Destination>
Task<CameraResult<void>> RecordVideoTask(std::shared_ptr<CameraSessionData> owner, Destination destination,
                                        std::optional<audio::AudioCaptureSession> microphone) {
  return AwaitCallback<CameraResult<void>>(
      [owner = std::move(owner), destination = std::move(destination), microphone = std::move(microphone)](
          CallbackCompletion<CameraResult<void>> complete) mutable -> std::function<void()> {
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
          std::optional<audio::AudioReader> reader;
          if (microphone) {
            const auto status = microphone->Status();
            if (status.error) throw MicrophoneError(*status.error);
            reader.emplace(Require(microphone->OpenReader()));
          }
          auto recording = std::make_shared<VideoRecording>(MakeOutput(std::move(destination)),
              std::is_same_v<Destination, OutputStream>,
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
              }, std::move(reader));
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
        } catch (const CameraError& error) {
          complete.Complete(CameraResult<void>::Failure(error));
          return {};
        } catch (const std::exception& error) {
          owner->StopRecording();
          complete.Complete(CameraResult<void>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
          return {};
        }
      });
}

void FramePipeline::OfferRecordingFrame(const PreviewOutput& input, std::chrono::nanoseconds capture_time,
                                        std::optional<std::chrono::nanoseconds> synchronized_time) {
  std::shared_ptr<VideoRecording> recording;
  {
    std::lock_guard lock(mutex_);
    recording = recording_;
  }
  if (recording) recording->Offer(input, capture_time, synchronized_time);
}

std::chrono::microseconds CameraSessionData::RecordedDuration() const noexcept {
  if (closed) return {};
  if (recording) return recording->Duration();
  return completed_recording_duration;
}

void CameraSessionData::SetRecordingPaused(bool paused) {
  if (closed || !recording || !recording->SetPaused(paused)) return;
  CameraStatus value = status;
  value.recording_paused = paused;
  status = std::move(value);
}

void CameraSessionData::StopRecording() {
  if (recording) recording->Stop();
  if (status->recording_paused) {
    CameraStatus value = status;
    value.recording_paused = false;
    status = std::move(value);
  }
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, File destination,
                                    std::optional<audio::AudioCaptureSession> microphone) {
  return RecordVideoTask(std::move(owner), std::move(destination), std::move(microphone));
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, RandomAccessStream destination,
                                    std::optional<audio::AudioCaptureSession> microphone) {
  return RecordVideoTask(std::move(owner), std::move(destination), std::move(microphone));
}

Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, OutputStream destination,
                                    std::optional<audio::AudioCaptureSession> microphone) {
  return RecordVideoTask(std::move(owner), std::move(destination), std::move(microphone));
}

} // namespace huxerui::camera::detail
