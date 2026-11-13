#pragma once

/// @file
/// @brief Camera sessions, live previews, frame processing, still photos, and video recording.
///
/// Include <huxerui/camera.h> and link HuxerUI::Camera. Register Install before composing UseCamera.
/// The owning composition controls capture. Use CameraPreview to present frames and CameraSession to
/// capture photos or record video. Recording uses the codec and container APIs from HuxerUI::Media.

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <vector>

#include <huxerui/external_texture.h>
#include <huxerui/data.h>
#include <huxerui/file.h>
#include <huxerui/resource.h>
#include <huxerui/app.h>
#include <huxerui/audiocapture.h>
#include <huxerui/task.h>
#include <huxerui/view.h>

namespace huxerui::camera {

/// @brief Camera position relative to the device's primary display.
///
/// Devices whose position is unknown report an empty CameraStatus::actual_facing.
enum class Facing {
  /// Camera facing the user on the display side of the device.
  Front,
  /// Camera facing away from the user on the rear of the device.
  Back,
};

/// @brief A video capture device currently selectable by the application.
/// @see EnumerateDevicesAsync(), CameraOptions::device_id
struct CameraDevice {
  /// Opaque UTF-8 identifier scoped to the platform and system. Revalidate saved IDs through discovery.
  std::string id;
  /// Display name supplied by the platform or backend. Names may repeat.
  std::string name;
  /// Camera position when known.
  std::optional<Facing> facing;

  bool operator==(const CameraDevice&) const = default;
};

/// @brief Selects the final horizontal mirror appearance of an upright preview or photo.
///
/// CameraPreview accounts for PreviewOutput::mirrored to avoid applying mirroring twice.
enum class MirrorMode {
  /// Mirror a confirmed Facing::Front camera. Rear and unknown-facing cameras appear unmirrored.
  Auto,
  /// Show an unmirrored result, undoing source mirroring when necessary.
  Off,
  /// Show a mirrored result, regardless of the camera's facing or source mirroring.
  On,
};

/// @brief Clockwise rotation required to orient a cropped source.
enum class PreviewRotation {
  /// No remaining rotation.
  R0,
  /// Rotate 90 degrees clockwise and exchange the cropped width and height.
  R90,
  /// Rotate 180 degrees clockwise.
  R180,
  /// Rotate 270 degrees clockwise and exchange the cropped width and height.
  R270,
};

/// @brief Observed lifecycle of a camera session.
///
/// Start, stop, and camera-switch operations settle asynchronously.
/// @see CameraOptions::active, CameraSession::Status()
enum class SessionState {
  /// No active capture. This is the initial state before capture intent is applied.
  Stopped,
  /// Checking existing authorization or preparing the selected camera.
  Starting,
  /// Capture has started. The first preview frame may still be pending.
  Running,
  /// Releasing capture resources before applying the latest capture intent.
  Stopping,
  /// Capture is temporarily paused by application backgrounding or a system interruption.
  Suspended,
  /// Capture could not proceed. CameraStatus::error describes the failure.
  Failed,
  /// The owning UseCamera composition was unmounted and the session is permanently closed.
  Closed,
};

/// @brief Portable failure categories for authorization, device selection, and capture.
///
/// Platform-specific details are provided by CameraError::message.
enum class CameraErrorCode {
  /// The application still needs to request camera authorization.
  PermissionRequired,
  /// Camera access is denied or restricted by the user or system policy.
  PermissionDenied,
  /// A capture backend or required platform capability is unavailable.
  Unavailable,
  /// No camera matches the requested ID or facing, or no default camera can be found.
  DeviceNotFound,
  /// The platform reports that another client or operation is using the camera.
  DeviceBusy,
  /// The selected camera was disconnected during capture.
  DeviceDisconnected,
  /// The requested capture input, output, format, or preview geometry could not be configured.
  ConfigurationFailed,
  /// Capture failed to start, resume, process a frame, or complete a photo or recording.
  CaptureFailed,
  /// The session is not running and ready to accept the capture request.
  NotReady,
  /// An operation of the same kind is still being captured, processed, or finalized by this session.
  OperationInProgress,
  /// An accepted request was interrupted by stopping, switching, backgrounding, or session failure.
  Interrupted,
};

/// @brief Diagnostic information for a failed camera session or individual operation.
struct CameraError {
  /// Stable category suitable for application recovery decisions.
  CameraErrorCode code;
  /// Human-readable diagnostic that may contain platform-provided text. Use code for recovery decisions.
  std::string message;

  bool operator==(const CameraError&) const = default;
};

/// @brief SDK result carrying an owned success value or a CameraError.
///
/// Check Succeeded() before accessing Value() or Error(). References from those accessors borrow the
/// result. Copy or move the stored value to retain it independently.
/// @tparam T Owned result payload, or void for operations without a payload.
template <class T> using CameraResult = Result<T, CameraError>;

/// @brief Hardware flash requested for one still photo.
enum class FlashMode {
  /// Capture without firing the flash.
  Off,
  /// Let the camera decide whether to fire the flash.
  Auto,
  /// Request a flash for this capture.
  On,
};

/// @brief Capture and encoding options for one still photo.
struct PhotoOptions {
  /// Final JPEG encoding quality in [1, 100]. Higher values generally produce larger files.
  int jpeg_quality = 90;
  /// Final horizontal mirroring after orientation is applied. Defaults to MirrorMode::Off.
  /// Auto mirrors only a confirmed front camera.
  MirrorMode mirror = MirrorMode::Off;
  /// Hardware flash for this request. Off also permits capture under an enabled torch.
  FlashMode flash_mode = FlashMode::Off;

  bool operator==(const PhotoOptions&) const = default;
};

/// @brief Flash modes supported by the active still-photo output.
struct PhotoFlash {
  /// Whether the photo output supports FlashMode::Auto.
  bool supports_auto = false;
  /// Whether the photo output supports FlashMode::On.
  bool supports_on = false;

  bool operator==(const PhotoFlash&) const = default;
};

/// @brief Observed continuous light capability of the active camera.
struct Torch {
  /// Whether the camera can currently turn on its torch.
  bool available = false;
  /// Whether the platform reports the torch on.
  bool enabled = false;

  bool operator==(const Torch&) const = default;
};

/// @brief Presentation options for one CameraPreview.
///
/// Each preview applies its own options to the shared session output.
struct PreviewOptions {
  /// Scaling applied after cropping and orientation, centered within the preview's bounds.
  /// Cover fills the bounds and clips excess content. Contain preserves the complete cropped frame.
  /// Fill stretches independently on each axis. None keeps intrinsic size. ScaleDown only reduces size.
  ImageFit fit = ImageFit::Cover;
  /// Desired final horizontal mirror appearance. Auto mirrors confirmed front cameras only.
  /// Changing this option affects presentation without reopening the shared camera.
  MirrorMode mirror = MirrorMode::Auto;

  bool operator==(const PreviewOptions&) const = default;
};

/// @brief Published frame texture with its crop, orientation, and mirror state.
///
/// CameraPreview applies the geometry when drawing. Apply it together when drawing the texture yourself.
/// The producer can publish new pixels to the same texture independently of UI recomposition.
/// @see FrameProcessor, CameraStatus::preview, CameraPreview()
struct PreviewOutput {
  /// Shared frame source. The owning composition controls capture lifetime.
  /// ExternalTexture::IntrinsicSize() gives the source geometry in logical units.
  /// Use the texture's platform acquisition API when you need a retained snapshot of its current pixels.
  std::shared_ptr<ExternalTexture> texture;
  /// Normalized source region in texture coordinates, before clockwise rotation and final mirroring.
  /// Coordinates span [0, 1] with the origin at the top left. The default selects the complete texture.
  /// CameraPreview intersects this rectangle with the texture bounds and draws nothing for an empty region.
  Rect crop = {0.0F, 0.0F, 1.0F, 1.0F};
  /// Remaining clockwise rotation after selecting crop.
  PreviewRotation rotation = PreviewRotation::R0;
  /// Whether the source already appears horizontally mirrored after applying rotation.
  /// CameraPreview flips the upright result only when this differs from PreviewOptions::mirror's desired result.
  bool mirrored = false;

  bool operator==(const PreviewOutput&) const = default;
};

/// @brief Processes a camera frame for both live preview and video recording.
///
/// The platform serializes frame callbacks. `input` holds the published source texture and
/// its geometry. `capture_time` is the frame timestamp in nanoseconds on the capture source's clock.
/// Read the source during the callback and acquire a snapshot if its pixels must outlive the call. Any
/// shared effect parameters read on the frame queue must be synchronized with writes from other threads.
///
/// Return a published texture and its matching crop, rotation, and mirror state. You can return the source
/// texture for a geometry-only change. Reuse the same texture object for successive frames with stable geometry
/// so updates can redraw without recomposing. When an output size is configured, Camera sizes this result
/// before sharing it with preview and recording. Camera acquires each accepted recording snapshot before the
/// next callback. Serialize other publications to the returned texture with this work.
///
/// Returning an error or throwing fails the capture session and exposes the diagnostic through CameraStatus::error.
/// @see FrameProcessorFactory, PreviewOutput
using FrameProcessor = std::function<CameraResult<PreviewOutput>(
    const PreviewOutput& input, std::chrono::nanoseconds capture_time)>;

/// @brief Creates a processor when a frame producer delivers its first usable frame.
///
/// `pixel_width` and `pixel_height` are the source texture dimensions in physical pixels,
/// before applying PreviewOutput::crop or PreviewOutput::rotation.
///
/// Return a callable with its own per-producer state. Camera uses it for the first frame and retains it until
/// the producer is replaced, capture stops, or the input texture dimensions change. Recording starts and
/// stops can share the same instance.
///
/// Creation, processing, and destruction for one producer are serialized by its backend. A shared factory can
/// be called concurrently by different sessions. On Android the capture EGL context is current on entry
/// to the factory, processor, and normal destruction. Restore that context if a callback changes it.
/// Processor cleanup must also handle context loss.
///
/// Throwing or returning an empty callable fails capture and reports the error through CameraStatus::error.
/// @see CameraOptions::frame_processor_factory, FrameProcessor
using FrameProcessorFactory = std::function<FrameProcessor(std::uint32_t pixel_width, std::uint32_t pixel_height)>;

/// @brief Configured capture frame rate bounds in frames per second.
struct FrameRateRange {
  /// Minimum configured rate in frames per second.
  double min_fps = 0;
  /// Maximum configured rate in frames per second.
  double max_fps = 0;

  bool operator==(const FrameRateRange&) const = default;
};

/// @brief Video frames delivered by the platform before Camera processes or sizes them.
struct CaptureFormat {
  /// Width in physical pixels, in the platform's delivered orientation.
  std::uint32_t pixel_width = 0;
  /// Height in physical pixels, in the platform's delivered orientation.
  std::uint32_t pixel_height = 0;
  /// Configured rate bounds, if known. The bounds can be equal for a fixed rate.
  /// Delivered frames can arrive at a lower rate. Use capture timestamps for frame timing.
  std::optional<FrameRateRange> frame_rate;

  bool operator==(const CaptureFormat&) const = default;
};

/// @brief Declarative capture intent applied by UseCamera on recomposition.
struct CameraOptions {
  /// Required camera facing. When both facing and device_id are omitted, select the platform default.
  /// An explicit facing requires a matching device. Changing it while active reopens capture.
  std::optional<Facing> facing;
  /// Whether capture is requested. Defaults to true.
  /// False stops capture asynchronously. True starts only with existing camera authorization.
  /// Backgrounding suspends capture without changing this intent. Foregrounding reevaluates it.
  bool active = true;
  /// Optional factory for the processor shared by preview and recording. An empty pointer uses source frames.
  /// Reuse the same factory across recompositions to keep capture open. Replacing it reopens active capture.
  /// Update synchronized effect parameters in place to change an effect during capture.
  std::shared_ptr<const FrameProcessorFactory> frame_processor_factory;
  /// Width in physical pixels of the texture shared by preview and recording, before rotation.
  ///
  /// Set both output dimensions to zero to use the frame at its original size. When both are positive,
  /// Camera center-crops within the frame's crop rectangle and scales to the requested size after processing.
  /// The published crop then covers the entire texture with the frame's rotation and mirroring metadata.
  /// An incomplete pair reports CameraErrorCode::ConfigurationFailed.
  /// Processors must return a published GlTexture on Android, PixelBufferTexture on Apple platforms, or
  /// D3D11Texture on Windows when sizing is enabled. The recording encoder's size requirements still apply.
  /// Changing the size while active reopens capture and finishes any current recording. Still photos use
  /// their independent capture path.
  /// @see output_pixel_height, FrameProcessor, PreviewOutput
  std::uint32_t output_pixel_width = 0;
  /// Height in physical pixels of the shared texture, before rotation. Specify it together with output_pixel_width.
  std::uint32_t output_pixel_height = 0;
  /// Preferred capture width and height in physical pixels. Set both to zero for the platform default.
  /// The match ignores orientation. The platform can choose a different size to keep capture available.
  /// An incomplete pair reports CameraErrorCode::ConfigurationFailed.
  /// Changing either dimension reopens active capture and finalizes any current recording.
  std::uint32_t preferred_capture_pixel_width = 0;
  std::uint32_t preferred_capture_pixel_height = 0;
  /// Preferred capture rate in frames per second. An empty value leaves frame rate selection to the platform.
  /// Specify a positive finite value. Capture prioritizes a working preview and photo combination, then size,
  /// then frame rate. Changing this preference reopens active capture and finalizes any current recording.
  /// An invalid value reports CameraErrorCode::ConfigurationFailed.
  std::optional<double> preferred_capture_fps;
  /// Required ID from EnumerateDevicesAsync(). An empty string or a combination with facing is invalid.
  /// Changing the ID reopens active capture and finalizes any current recording.
  /// An unavailable ID reports DeviceNotFound. Retry can reopen the device after it reconnects.
  std::optional<std::string> device_id;

  bool operator==(const CameraOptions&) const = default;
};

/// @brief The active camera's applied zoom ratio and supported range.
struct Zoom {
  /// Latest ratio confirmed by the camera.
  float ratio = 1.0F;
  /// Smallest zoom ratio for the active capture configuration.
  float min_ratio = 1.0F;
  /// Largest zoom ratio for the active capture configuration.
  float max_ratio = 1.0F;

  bool operator==(const Zoom&) const = default;
};

/// @brief The active camera's applied exposure compensation and supported range in EV.
struct ExposureCompensation {
  /// Latest exposure compensation confirmed by the camera.
  float ev = 0.0F;
  /// Smallest exposure compensation for the active capture configuration.
  float min_ev = 0.0F;
  /// Largest exposure compensation for the active capture configuration.
  float max_ev = 0.0F;
  /// Fixed increment reported by the device. Empty when the platform reports no fixed increment.
  std::optional<float> step_ev;

  bool operator==(const ExposureCompensation&) const = default;
};

/// @brief Current observed session state and optional preview metadata.
///
/// A returned status value can hold a texture that continues receiving frames.
/// @see CameraSession::Status(), CameraOptions::active
struct CameraStatus {
  /// Observed lifecycle state. The first frame may still be pending in SessionState::Running.
  SessionState state = SessionState::Stopped;
  /// Facing reported by the selected device, or empty when unavailable or unknown.
  /// This may differ from an unspecified CameraOptions::facing and determines automatic mirroring.
  std::optional<Facing> actual_facing;
  /// Failure details when state is SessionState::Failed. Empty for other states.
  std::optional<CameraError> error;
  /// Latest output texture and geometry, including any frame processing.
  /// Empty before the first output and after output is cleared. A suspended session can retain its last output.
  std::optional<PreviewOutput> preview;
  /// Whether an accepted photo request is still pending, including platform processing after caller cancellation.
  /// Capture normally remains Running during a photo. This flag is shared by all session handles.
  bool capturing_photo = false;
  /// Whether the session has a recording in progress, including pauses and finalization.
  /// Closing the session clears this flag. Pending finalization can continue after closure.
  bool recording_video = false;
  /// Whether the current recording is skipping new frames after PauseRecording().
  /// Preview and frame processing continue. Resuming or ending the recording clears this flag.
  bool recording_paused = false;
  /// Zoom control for the current camera. Empty while unavailable or before the range has been read.
  std::optional<Zoom> zoom;
  /// Exposure compensation for the current camera. Empty while unavailable or before the range has been read.
  std::optional<ExposureCompensation> exposure_compensation;
  /// Whether the current preview supports setting a focus or metering point.
  /// Available with the camera's default or explicitly sized output when the device supports point control.
  /// Empty previews and custom frame processors do not provide point control.
  bool can_focus_and_meter = false;
  /// Still-photo flash capability. Both modes are false while unavailable or before the capability is read.
  PhotoFlash photo_flash;
  /// Continuous light control. Empty while unsupported or before the capability is read.
  std::optional<Torch> torch;
  /// Current video capture format. Empty until the platform reports the source geometry and while capture is inactive.
  std::optional<CaptureFormat> capture_format;
  /// ID of the opened capture device when available, including automatic and facing-based selection.
  std::optional<std::string> actual_device_id;

  bool operator==(const CameraStatus&) const = default;
};

namespace detail {
struct CameraSessionData;
} // namespace detail

/// @brief Shared UI-thread handle for a camera session owned by a UseCamera call.
///
/// Copies refer to the same session and may be passed to multiple CameraPreview components.
/// Unmounting the owner closes capture even if a preview, callback, or another object retains a handle.
/// Read state and call session operations on the owning UI thread. The backend manages frame delivery.
class CameraSession {
public:
  /// @brief Reads the latest observed lifecycle state, failure, and preview metadata.
  ///
  /// Reading during composition subscribes to status and output geometry changes. Frames published to the
  /// same texture can redraw independently. The returned value keeps its metadata from the time of the call.
  /// @return The observed session status at the time of the call.
  [[nodiscard]] CameraStatus Status() const;

  /// @brief Retries a failed session using the latest options from its owning UseCamera call.
  ///
  /// Clears the failure and reevaluates capture intent. It does not set active to true or request permission UI.
  /// Calls in states other than SessionState::Failed, including SessionState::Closed, have no effect.
  /// Obtain any missing authorization before retrying and observe Status() for the asynchronous outcome.
  /// @code{.cpp}
  /// const auto status = session.Status();
  /// return Button("Retry")
  ///     .OnClick([session] { session.Retry(); })
  ///     .With(Enabled(status.state == camera::SessionState::Failed));
  /// @endcode
  void Retry() const;

  /// @brief Adjusts the running camera's zoom for preview, still capture, and recording.
  ///
  /// Start the lazy task on the owning UI thread. The camera stays open and the recording continues.
  /// The ratio is constrained to the device's current range and rounded to the nearest supported step.
  /// A newer request can supersede an earlier adjustment. A superseded request, camera switch,
  /// or capture shutdown returns Interrupted. Canceling a task withdraws a pending request and suppresses
  /// delivery. An adjustment already submitted to the camera can still update CameraStatus::zoom.
  /// @param ratio Positive finite zoom multiplier.
  /// @return Success when the device accepts the adjustment. Read CameraStatus::zoom for the applied value.
  /// NotReady means the camera is not running. Unavailable means it has no zoom control.
  /// @throws std::invalid_argument Before returning a task if the ratio is nonpositive or nonfinite.
  Task<CameraResult<void>> SetZoomRatioAsync(float ratio) const;

  /// @brief Adjusts the running camera's automatic exposure target for preview, still capture, and recording.
  ///
  /// Start the lazy task on the owning UI thread. The camera stays open and the recording continues.
  /// The value is constrained to the device's range and rounded to the nearest supported increment when needed.
  /// A newer request can supersede an earlier adjustment. A superseded request, camera switch, or capture shutdown
  /// returns Interrupted. Canceling a task withdraws a pending request and suppresses delivery. An adjustment
  /// already submitted to the camera can still update CameraStatus::exposure_compensation.
  /// @param ev Finite exposure compensation in EV. Zero applies no compensation, positive values raise the
  /// exposure target, and negative values lower it.
  /// @return Success when the device accepts the adjustment. Read CameraStatus::exposure_compensation for the
  /// applied value. NotReady means the camera is not running. Unavailable means exposure compensation is unsupported
  /// or has not been read yet.
  /// @throws std::invalid_argument Before returning a task if ev is nonfinite.
  Task<CameraResult<void>> SetExposureCompensationAsync(float ev) const;

  /// @brief Turns the active camera's continuous light on or off during preview or recording.
  ///
  /// Start the lazy task on the owning UI thread. Read CameraStatus::torch for the observed light status.
  /// The light belongs to the camera session and stays on across recording pause and stop. Switching cameras,
  /// suspending capture, or closing the session releases the light. The session does not restore it on restart.
  /// @param enabled Whether to turn on the continuous light.
  /// @return Unavailable when there is no torch or it cannot be turned on. OperationInProgress when a torch
  /// adjustment or a flash photo is pending. NotReady means the camera is not running.
  Task<CameraResult<void>> SetTorchEnabledAsync(bool enabled) const;

  /// @brief Sets a focus and metering point on the active camera.
  ///
  /// Pass the point and size in the same local logical coordinates as CameraPreview, with its presentation options.
  /// The session maps the point through preview placement and any configured output crop to the camera source.
  /// Points outside the displayed image return Unavailable. The camera applies the supported operations at the point.
  /// A successful result confirms the request, without implying that the lens acquired focus.
  /// Starting another request supersedes an earlier one. The camera stays open and any recording continues.
  /// @param position Point relative to the top left of the preview.
  /// @param preview_size Size of the preview in logical units.
  /// @param options The fit and mirroring used to display the preview.
  /// @return NotReady when there is no live preview, or Unavailable when point control is unsupported.
  Task<CameraResult<void>> FocusAndMeterAsync(Point position, Size preview_size, PreviewOptions options = {}) const;

  /// @brief Captures a JPEG photo through the platform still-photo output of the current camera.
  ///
  /// Start the lazy task from a UI event or Task. It checks readiness when it runs.
  /// Only one request is accepted at a time. Additional requests return OperationInProgress immediately.
  /// The target orientation and resolved mirror mode are fixed when the request is accepted.
  /// Success returns an independent ImageAsset with upright pixels, normal EXIF orientation, and scale 1.
  /// Photo dimensions are negotiated by the capture output, independently of the preview's layout or Cover crop.
  /// Stopping, switching, backgrounding, or session failure ends a pending request with Interrupted.
  /// An individual photo error does not set CameraStatus::error or stop otherwise healthy preview capture.
  /// Task cancellation suppresses delivery while platform cleanup continues. Cleanup must finish before
  /// another request is accepted.
  /// No file or photo-library entry is created. Save EncodedBytes() with the SDK File APIs when needed.
  /// @param options JPEG quality, final mirroring, and flash mode for this request only.
  /// @return A task yielding an owned JPEG image or CameraError. Unavailable indicates missing still-photo output,
  /// an unavailable flash mode, or a request for flash while the torch is on. A pending torch adjustment
  /// prevents a flash photo with OperationInProgress. FlashMode::On requests flash subject to platform availability.
  /// @throws std::invalid_argument Before returning a task if quality, mirror mode, or flash mode is invalid.
  /// @code{.cpp}
  /// auto result = co_await session.CapturePhotoAsync({.jpeg_quality = 90});
  /// if (result.Succeeded()) {
  ///   photo = result.Value();
  /// } else {
  ///   message = result.Error().message;
  /// }
  /// @endcode
  Task<CameraResult<ImageAsset>> CapturePhotoAsync(PhotoOptions options = {}) const;

  /// @brief Records H.264 video and optional AAC-LC microphone audio into a new MP4 file.
  ///
  /// Start the lazy task on the owning UI thread from a UI event or TaskScope. The session must be running.
  /// Each session accepts one recording at a time, including pauses and finalization. Call StopRecording()
  /// to stop accepting frames and finalize the output. Capture shutdown, including a camera switch or
  /// backgrounding, also requests finalization. Task cancellation requests finalization and suppresses
  /// completion delivery.
  ///
  /// The encoder receives retained snapshots of the same output used by preview. The track uses the
  /// initial dimensions and stores the initial crop and rotation as presentation metadata. Encoded
  /// pixels keep their source mirror appearance.
  /// A change in input video format ends the recording.
  /// When recording falls behind, Camera can skip input frames while preserving their elapsed capture time.
  ///
  /// Use a published GlTexture on Android, a PixelBufferTexture backed by NV12 or BGRA on iOS or macOS, or
  /// an opaque D3D11Texture on Windows. iOS and macOS require even pixel dimensions.
  ///
  /// With no microphone, the recording has one video track starting at the first submitted frame.
  /// With a microphone in Starting or Running, Camera opens its sole PCM reader and releases it after
  /// recording. Keep the microphone active until the task completes. The caller owns permission and
  /// capture intent. Use capture-timed PCM, including any configured capture-time processing, in
  /// interleaved S16 mono or stereo at 44.1 or 48 kHz. Both tracks use the same capture clock and pause
  /// intervals. On stop, Camera finishes input at the capture cutoff and drains the processor and encoders.
  ///
  /// An encoding or I/O failure may leave partial output.
  /// @param destination New file in a writable directory. Camera creates it when the first encoded packet
  /// for each requested track is ready to write and preserves an existing file at that path.
  /// @param microphone Optional active microphone session on the same owning UI thread.
  /// @return A task yielding success after finalization or a CameraError. NotReady means capture has
  /// not reached SessionState::Running or is stopping. OperationInProgress means a recording or a
  /// microphone reader is already active. Unavailable indicates missing platform, codec, or clock support.
  /// Interrupted means capture ended before the required media arrived.
  /// @see PauseRecording(), StopRecording(), RecordedDuration(), CameraOptions::frame_processor_factory
  Task<CameraResult<void>> RecordVideoAsync(
      File destination, std::optional<audio::AudioCaptureSession> microphone = std::nullopt) const;

  /// @brief Records video and optional microphone audio into a writable random-access stream as a regular MP4.
  ///
  /// The task owns the destination and writes from offset zero. The capture, cancellation, and finalization
  /// contract is shared with the file overload.
  /// @param destination Empty writable stream transferred to the task.
  /// @param microphone Optional active microphone session supplying capture-timed PCM.
  /// @return A task yielding success after the muxer finalizes the stream, or CameraError.
  /// @see RecordVideoAsync(File, std::optional<audio::AudioCaptureSession>), StopRecording()
  Task<CameraResult<void>> RecordVideoAsync(
      RandomAccessStream destination, std::optional<audio::AudioCaptureSession> microphone = std::nullopt) const;

  /// @brief Records video and optional microphone audio into a sequential stream as a fragmented MP4.
  ///
  /// The task owns the destination. The capture, cancellation, and finalization contract is shared with
  /// the file overload. With audio, the stream contains complete AAC access units and can include
  /// encoder priming and final padding.
  /// @param destination Fresh writable stream transferred to the task.
  /// @param microphone Optional active microphone session supplying capture-timed PCM.
  /// @return A task yielding success after the muxer finalizes the stream, or CameraError.
  /// @see RecordVideoAsync(File, std::optional<audio::AudioCaptureSession>), StopRecording()
  Task<CameraResult<void>> RecordVideoAsync(
      OutputStream destination, std::optional<audio::AudioCaptureSession> microphone = std::nullopt) const;

  /// @brief Reads the video duration on the MP4 timeline of the current or latest recording.
  ///
  /// The duration advances to the presentation end of each sample accepted by the muxer. It can
  /// lag capture while the encoder buffers frames and advance while queued frames drain after a pause or stop.
  /// The output timeline excludes the gaps between recording intervals.
  /// Read the snapshot on the owning UI thread. Poll from a UI task at the display refresh interval.
  /// @return The duration in microseconds. A new recording starts at zero and a successful final
  /// duration remains available until the next recording starts. Returns zero before any recording,
  /// after a failed recording settles, or after the session closes.
  /// @see RecordVideoAsync(File, std::optional<audio::AudioCaptureSession>), CameraStatus::recording_video
  std::chrono::microseconds RecordedDuration() const noexcept;

  /// @brief Pauses acceptance of new frames for the current recording on the owning UI thread.
  ///
  /// Preview and frame processing continue. The recording keeps its encoder and output open while accepted
  /// frames drain. Calling again while paused, during finalization, or with no recording has no effect.
  /// @see ResumeRecording(), StopRecording(), CameraStatus::recording_paused
  void PauseRecording() const;

  /// @brief Continues a paused recording into the same output on the owning UI thread.
  ///
  /// Video-only recording uses up to one nominal frame interval at the join. With microphone audio,
  /// both tracks exclude the same pause interval. Calling while already recording, during finalization,
  /// or with no recording has no effect.
  /// @see PauseRecording(), RecordedDuration(), CameraStatus::recording_paused
  void ResumeRecording() const;

  /// @brief Requests finalization of the current recording on the owning UI thread.
  ///
  /// A running or paused recording drains accepted frames through the encoder before the muxer finalizes
  /// the output. Await the original recording task for the outcome. Calling again during finalization or
  /// with no recording has no effect.
  /// @see RecordVideoAsync(File, std::optional<audio::AudioCaptureSession>), CameraStatus::recording_video
  void StopRecording() const;

private:
  explicit CameraSession(std::shared_ptr<detail::CameraSessionData> data);
  std::shared_ptr<detail::CameraSessionData> data_;

  friend CameraSession UseCamera(CameraOptions, const std::source_location&);
};

/// @brief Installs the camera platform module in the application catalog.
///
/// Register this function in AppOptions::application_hooks before composing content that calls UseCamera.
/// Registration neither opens a camera nor requests authorization. Unsupported capture platforms report
/// CameraErrorCode::Unavailable when capture is requested.
/// @param context Application context supplied by HuxerUI when it runs the registered hook.
/// @code{.cpp}
/// const huxerui::Application application{
///     App,
///     {
///         .application_hooks = {
///             huxerui::camera::Install,
///         },
///     }
/// };
/// @endcode
void Install(ApplicationContext& context);

/// @brief Queries a snapshot of the video devices selectable by the application.
///
/// Launch from a UI task after registering Install and obtaining Permission::Camera.
/// Discovery checks authorization and leaves the permission prompt to the application.
/// It can run with or without an active CameraSession. Devices can disappear between discovery and selection.
/// @return The current device list, an empty list when none are exposed, or a CameraError.
Task<CameraResult<std::vector<CameraDevice>>> EnumerateDevicesAsync();

/// @brief Retains a camera session in the current composition and applies the requested capture intent.
///
/// Call from an active composition on the UI thread, with Install registered for its application.
/// Recomposition at the same retained call site updates the existing session. Distinct calls own separate
/// sessions. Share a returned handle when multiple previews should use one device.
///
/// Authorization is checked before starting. The application requests
/// Permission::Camera through ApplicationHandle::RequestPermissionAsync when appropriate.
/// Missing authorization is reported through CameraStatus::error. Unmounting the owning composition closes
/// the session. Mark reusable functions that call this hook with [[huxerui::composable]].
/// @param options Capture intent and optional frame processing. Defaults to active capture from the default camera.
/// @param location Call-site metadata used to retain composition state. Use the default value for ordinary calls.
/// @return A shared handle for observing capture, displaying previews, taking photos, and recording video.
/// @code{.cpp}
/// #include <huxerui/camera.h>
/// #include <huxerui/huxerui.h>
///
/// using namespace huxerui;
///
/// [[huxerui::composable]]
/// View CameraScreen() {
///   auto active = UseState(false);
///   auto application = UseApplication();
///   auto tasks = UseTaskScope();
///   auto session = camera::UseCamera({.active = active});
///
///   return Column {
///     camera::CameraPreview(session).With(Frame{.width = 320.0F, .height = 240.0F}),
///     Button("Start camera").OnClick([application, tasks, active] {
///       tasks.Launch([application, active]() -> Task<void> {
///         const auto status = co_await application.RequestPermissionAsync(Permission::Camera);
///         active = status == PermissionStatus::Granted;
///       });
///     }),
///     Button("Stop camera").OnClick([active] { active = false; }),
///   }.With(Spacing(12.0F));
/// }
/// @endcode
[[nodiscard]] CameraSession UseCamera(
    CameraOptions options = {},
    const std::source_location& location = std::source_location::current()
);

/// @brief Displays a shared camera session with centered fitting, source cropping, rotation, and final mirroring.
///
/// Supply bounded layout space through the parent or a Frame modifier. Drawing requires usable preview
/// output and nonempty bounds. Live texture updates redraw independently of UI recomposition.
/// @param session Session returned by UseCamera. Multiple previews may share it to display the same camera.
/// @param options Appearance for this preview only. Defaults to ImageFit::Cover and MirrorMode::Auto.
/// @return A preview View whose lifetime does not control the shared capture session.
/// @code{.cpp}
/// return Row {
///   camera::CameraPreview(session).With(Frame{.width = 320.0F, .height = 240.0F}),
///   camera::CameraPreview(session, {
///       .fit = ImageFit::Contain,
///       .mirror = camera::MirrorMode::Off,
///   }).With(Frame{.width = 160.0F, .height = 120.0F}),
/// }.With(Spacing(12.0F));
/// @endcode
View CameraPreview(CameraSession session, PreviewOptions options = {});

} // namespace huxerui::camera
