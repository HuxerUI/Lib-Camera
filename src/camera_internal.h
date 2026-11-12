#pragma once

#include <functional>
#include <mutex>
#include <utility>

#include <huxerui/camera.h>

#if defined(_WIN32)
struct ID3D11DeviceContext;
#endif

namespace huxerui::camera::detail {

inline constexpr char camera_module_name[] = "camera/Session";

class VideoRecording;

inline constexpr Rect full_texture_crop{0, 0, 1, 1};

/// Finds the centered region with the requested pixel aspect ratio inside a normalized source crop.
Rect CropForOutput(Rect crop, std::uint32_t source_width, std::uint32_t source_height,
                   std::uint32_t output_width, std::uint32_t output_height);

#if defined(__ANDROID__)
bool SupportsGlOutputSize(std::uint32_t width, std::uint32_t height);
#endif

/// Sizes processed frames on the capture producer's queue and retains one output texture identity.
class TextureResizer {
public:
  TextureResizer();
  ~TextureResizer();
  /// Replaces the texture and crop in place. Returns false if the frame is temporarily unavailable.
  bool Resize(PreviewOutput& frame, std::uint32_t width, std::uint32_t height);
  void Reset();
#if defined(_WIN32)
  void SetContext(ID3D11DeviceContext& context) { context_ = &context; }
#endif

private:
  struct State;
  std::unique_ptr<State> state_;
#if defined(_WIN32)
  ID3D11DeviceContext* context_ = nullptr;
#endif
};

struct FrameProducer {
  FrameProcessor processor;
  TextureResizer resizer;
};

/// Processes and sizes one shared output per frame for preview and recording.
class FramePipeline {
public:
  explicit FramePipeline(const CameraOptions& options);
  /// Selects the initial render size when uniform scaling can produce the configured output directly.
  std::pair<std::uint32_t, std::uint32_t> ProducerSize(std::uint32_t width, std::uint32_t height) const noexcept;
  /// Acquires the recording snapshot before return. A success without a texture skips the frame.
  CameraResult<PreviewOutput> Process(FrameProducer& producer, PreviewOutput input,
      std::chrono::nanoseconds capture_time, std::uint32_t pixel_width, std::uint32_t pixel_height,
      std::optional<std::chrono::nanoseconds> synchronized_time = {});
  /// Called on the UI thread. An empty pointer detaches the recorder from subsequent frames.
  void SetRecording(std::shared_ptr<VideoRecording> recording);

private:
  void OfferRecordingFrame(const PreviewOutput& output, std::chrono::nanoseconds capture_time,
                           std::optional<std::chrono::nanoseconds> synchronized_time);

  std::shared_ptr<const FrameProcessorFactory> factory_;
  std::uint32_t output_width_;
  std::uint32_t output_height_;
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
  /// Reads the active capture configuration's zoom range and applied ratio.
  virtual void ReadZoom(std::function<void(std::optional<ZoomState>)> completed) { completed({}); }
  /// Indicates that a new zoom submission can supersede an unconfirmed device adjustment.
  virtual bool CanReplaceZoomRequest() const noexcept { return false; }
  /// Applies a ratio on the active device and returns its confirmed state.
  virtual void SetZoomRatio(float, std::function<void(CameraResult<ZoomState>)> completed) {
    completed(CameraResult<ZoomState>::Failure({CameraErrorCode::Unavailable, "Camera zoom is unavailable."}));
  }
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
