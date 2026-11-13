#pragma once

#include <functional>
#include <mutex>
#include <tuple>
#include <utility>

#include <huxerui/camera.h>

#if defined(_WIN32)
struct ID3D11DeviceContext;
#endif

namespace huxerui::camera::detail {

inline constexpr char camera_module_name[] = "camera/Session";

class VideoRecording;

struct CapturePreferences {
  std::uint32_t pixel_width = 0;
  std::uint32_t pixel_height = 0;
  std::optional<double> fps;
};

/// Orders formats by the preferred size, then by the closest available frame rate.
std::tuple<double, double, double> CaptureFormatRank(const CapturePreferences& preferred, const CaptureFormat& format);

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
  /// Replaces the texture and crop in place and returns the region sampled from the input texture.
  /// An empty result means the frame is temporarily unavailable.
  std::optional<Rect> Resize(PreviewOutput& frame, std::uint32_t width, std::uint32_t height);
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

struct ProcessedFrame {
  PreviewOutput output;
  /// For point control without a custom processor, the source region covered by the entire output texture.
  Rect source_crop = full_texture_crop;

  bool operator==(const ProcessedFrame&) const = default;
};

/// Processes and sizes one shared output per frame for preview and recording.
class FramePipeline {
public:
  explicit FramePipeline(const CameraOptions& options);
  /// Selects the initial render size when uniform scaling can produce the configured output directly.
  std::pair<std::uint32_t, std::uint32_t> ProducerSize(std::uint32_t width, std::uint32_t height) const noexcept;
  /// Acquires the recording snapshot before return. A success without a texture skips the frame.
  CameraResult<ProcessedFrame> Process(FrameProducer& producer, PreviewOutput input,
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
  /// Publishes the session status and the source crop used for point control.
  using StatusCallback = std::function<void(CameraStatus, Rect)>;

  virtual ~CameraBackend() = default;
  void SetFramePipeline(std::shared_ptr<FramePipeline> frames) { frame_pipeline_ = std::move(frames); }
  virtual void Start(std::optional<Facing> facing, CapturePreferences preferred, StatusCallback changed) = 0;
  /// Completion follows producer shutdown and texture Finish. An empty callback is allowed.
  virtual void Stop(std::function<void()> completed) = 0;
  /// Reads the active capture configuration's zoom range and applied ratio.
  virtual void ReadZoom(std::function<void(std::optional<Zoom>)> completed) { completed({}); }
  /// Indicates that a new zoom submission can supersede an unconfirmed device adjustment.
  virtual bool CanReplaceZoomRequest() const noexcept { return false; }
  /// Applies a ratio on the active device and returns its confirmed state.
  virtual void SetZoomRatio(float, std::function<void(CameraResult<Zoom>)> completed) {
    completed(CameraResult<Zoom>::Failure({CameraErrorCode::Unavailable, "Camera zoom is unavailable."}));
  }
  /// Reads the active capture configuration's exposure compensation range and applied EV.
  virtual void ReadExposureCompensation(std::function<void(std::optional<ExposureCompensation>)> completed) {
    completed({});
  }
  /// Indicates that a new submission can supersede an unconfirmed exposure compensation adjustment.
  virtual bool CanReplaceExposureCompensationRequest() const noexcept { return false; }
  /// Applies an exposure compensation in EV and returns the device's confirmed value and range.
  virtual void SetExposureCompensation(float, std::function<void(CameraResult<ExposureCompensation>)> completed) {
    completed(CameraResult<ExposureCompensation>::Failure(
        {CameraErrorCode::Unavailable, "Camera exposure compensation is unavailable."}));
  }
  /// Sets a point expressed in normalized capture source texture coordinates with a top-left origin.
  virtual void FocusAndMeter(Point, std::function<void(CameraResult<void>)> completed) {
    completed(CameraResult<void>::Failure({CameraErrorCode::Unavailable, "Point focus and metering are unavailable."}));
  }
  /// Reports the current light controls and subsequent changes until capture stops.
  virtual void ObserveLighting(std::function<void(PhotoFlash, std::optional<Torch>)> changed) { changed({}, {}); }
  /// Changes the continuous light and publishes its observed status before completing the request.
  virtual void SetTorchEnabled(bool, std::function<void(CameraResult<void>)> completed) {
    completed(CameraResult<void>::Failure({CameraErrorCode::Unavailable, "The camera torch is unavailable."}));
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
std::optional<Point> PreviewTexturePoint(const PreviewOutput& output, PreviewOptions options,
                                        std::optional<Facing> facing, Size bounds, Point position,
                                        Rect source_crop = full_texture_crop);

} // namespace huxerui::camera::detail
