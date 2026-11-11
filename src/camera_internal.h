#pragma once

#include <functional>
#include <mutex>
#include <utility>

#include <huxerui/camera.h>

namespace huxerui::camera::detail {

inline constexpr char camera_module_name[] = "camera/Session";

class VideoRecording;

/// Applies one processor per capture frame and forwards its output to the active recording.
class FramePipeline {
public:
  explicit FramePipeline(std::shared_ptr<const FrameProcessor> processor);
  /// Called on the serial frame queue. Acquires any recording snapshot before returning the preview output.
  CameraResult<PreviewOutput> Process(PreviewOutput input, std::chrono::nanoseconds capture_time);
  /// Called on the UI thread. An empty pointer detaches the recorder from subsequent frames.
  void SetRecording(std::shared_ptr<VideoRecording> recording);

private:
  std::shared_ptr<const FrameProcessor> processor_;
  std::mutex mutex_;
  std::shared_ptr<VideoRecording> recording_;
};

/// Operations enter on the UI thread; callbacks may originate on a producer queue.
class CameraBackend {
public:
  virtual ~CameraBackend() = default;
  void SetFramePipeline(std::shared_ptr<FramePipeline> frames) { frame_pipeline_ = std::move(frames); }
  virtual void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) = 0;
  /// Completion follows producer shutdown and texture Finish. An empty callback is allowed.
  virtual void Stop(std::function<void()> completed) = 0;
  /// Delivers one independent, upright JPEG. Stop must wait for accepted platform photo work to finish cleanup.
  virtual void CapturePhoto(PhotoOptions, std::function<void(CameraResult<ImageAsset>)> completed) {
    completed(CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::Unavailable, "Still-photo capture is unavailable on this camera."}));
  }

protected:
  std::shared_ptr<FramePipeline> frame_pipeline_;
};

void InstallPlatformCamera(ApplicationContext& context);

struct PreviewPlacement {
  Rect source;
  Rect destination;
  Transform2D transform;
};

[[nodiscard]] std::optional<PreviewPlacement> PlacePreview(
    Size intrinsic, const PreviewOutput& output, PreviewOptions options, std::optional<Facing> facing, Size bounds
);

} // namespace huxerui::camera::detail
