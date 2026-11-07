#pragma once

#include <memory>
#include <optional>
#include <source_location>
#include <string>

#include <huxerui/external_texture.h>
#include <huxerui/data.h>
#include <huxerui/resource.h>
#include <huxerui/app.h>
#include <huxerui/task.h>
#include <huxerui/view.h>

namespace huxerui::camera {

/// Camera position relative to the device's primary display.
/// Devices whose position is unknown report an empty CameraStatus::actual_facing.
enum class Facing {
  /// Camera facing the user on the display side of the device.
  Front,
  /// Camera facing away from the user on the rear of the device.
  Back,
};

/// Selects the final horizontal mirror appearance of an upright preview or photo.
/// CameraPreview accounts for PreviewOutput::mirrored to avoid applying mirroring twice.
enum class MirrorMode {
  /// Mirror a confirmed Facing::Front camera; leave rear or unknown-facing cameras unmirrored.
  Auto,
  /// Show an unmirrored result, undoing source mirroring when necessary.
  Off,
  /// Show a mirrored result, regardless of the camera's facing or source mirroring.
  On,
};

/// Clockwise rotation still required to orient a cropped source for the current preview.
enum class PreviewRotation {
  /// No remaining rotation.
  R0,
  /// Rotate 90 degrees clockwise; exchange the cropped width and height.
  R90,
  /// Rotate 180 degrees clockwise.
  R180,
  /// Rotate 270 degrees clockwise; exchange the cropped width and height.
  R270,
};

/// Observed capture lifecycle, distinct from CameraOptions::active capture intent.
/// Start, stop, and camera-switch operations settle asynchronously.
enum class SessionState {
  /// No active capture; also the initial state before capture intent is applied.
  Stopped,
  /// Checking existing authorization or preparing the selected camera.
  Starting,
  /// Capture has started; the first preview frame may not yet be available.
  Running,
  /// Releasing capture resources before applying the latest capture intent.
  Stopping,
  /// Capture is temporarily paused by application backgrounding or a reported platform interruption.
  Suspended,
  /// Capture could not proceed; CameraStatus::error describes the failure.
  Failed,
  /// The owning UseCamera composition was unmounted; this session cannot be restarted.
  Closed,
};

/// Portable failure categories for authorization, device selection, and capture.
/// Platform-specific details are provided by CameraError::message.
enum class CameraErrorCode {
  /// Camera authorization has not been requested; the application must request it explicitly.
  PermissionRequired,
  /// Camera access is denied or restricted by the user or system policy.
  PermissionDenied,
  /// The platform, capture backend, or required platform capability is unavailable.
  Unavailable,
  /// No camera matches the requested facing, or no default camera can be found.
  DeviceNotFound,
  /// The platform reports that another client or operation is using the camera.
  DeviceBusy,
  /// The selected camera was disconnected during capture.
  DeviceDisconnected,
  /// The requested capture input, output, format, or preview geometry could not be configured.
  ConfigurationFailed,
  /// Capture failed to start, resume, continue, or produce a photo for another reason.
  CaptureFailed,
  /// The session is not running and ready to accept a photo request.
  NotReady,
  /// Another photo request is still being captured or processed by this session.
  OperationInProgress,
  /// An accepted request was interrupted by stopping, switching, backgrounding, or session failure.
  Interrupted,
};

/// Diagnostic information for a failed camera session or individual operation.
struct CameraError {
  /// Stable category suitable for application recovery decisions.
  CameraErrorCode code;
  /// Human-readable diagnostic that may contain platform-provided text.
  /// Wording and language are not stable; use code rather than parsing this string.
  std::string message;

  bool operator==(const CameraError&) const = default;
};

/// Success value or a camera operation error.
/// @tparam T Owned result payload, or void for operations without a payload.
/// Check Succeeded() before accessing Value() or Error().
template <class T> using CameraResult = Result<T, CameraError>;

/// Encoding and final appearance of one still photo, independent of preview settings.
struct PhotoOptions {
  /// Final JPEG encoding quality in [1, 100]; higher values generally produce larger files.
  int jpeg_quality = 90;
  /// Final horizontal mirroring after orientation is applied; Off by default.
  /// Auto mirrors only a confirmed front camera. Preview fitting and mirroring do not affect this choice.
  MirrorMode mirror = MirrorMode::Off;

  bool operator==(const PhotoOptions&) const = default;
};

/// Declarative capture intent applied by UseCamera on recomposition.
/// These options control the shared device; per-preview appearance belongs to PreviewOptions.
struct CameraOptions {
  /// Required camera facing, or an empty value to select the platform's default video device.
  /// An explicit facing has no fallback to the opposite facing. Changing it while active reopens capture.
  std::optional<Facing> facing;
  /// Whether capture is requested; true by default.
  /// False stops capture asynchronously. True starts only with existing camera authorization.
  /// Backgrounding suspends capture without changing this intent; foregrounding reevaluates it.
  bool active = true;

  bool operator==(const CameraOptions&) const = default;
};

/// Presentation options for one CameraPreview, independent of other previews of the same session.
struct PreviewOptions {
  /// Scaling applied after cropping and orientation, centered within the preview's bounds.
  /// Cover fills the bounds and clips excess content. Contain preserves the complete cropped frame.
  /// Fill stretches independently on each axis; None keeps intrinsic size; ScaleDown only reduces size.
  ImageFit fit = ImageFit::Cover;
  /// Desired final horizontal mirror appearance; Auto mirrors confirmed front cameras only.
  /// Changing this option affects presentation without reopening the shared camera.
  MirrorMode mirror = MirrorMode::Auto;

  bool operator==(const PreviewOptions&) const = default;
};

/// Published texture and geometry needed to present a camera frame correctly.
/// Prefer CameraPreview unless custom drawing is needed. Custom consumers must interpret crop, rotation,
/// and mirrored together rather than drawing the texture as an ordinary, already-oriented image.
/// The producer updates texture contents independently of UI recomposition.
struct PreviewOutput {
  /// Shared frame source; retaining it does not keep the owning camera session capturing.
  /// Use ExternalTexture::IntrinsicSize() for source geometry in logical units, not physical pixel dimensions.
  std::shared_ptr<ExternalTexture> texture;
  /// Normalized source region in texture coordinates, before clockwise rotation and final mirroring.
  /// Coordinates span [0, 1], with the origin at the top left; the default selects the complete texture.
  /// CameraPreview intersects this rectangle with the texture bounds and draws nothing for an empty region.
  Rect crop = {0.0F, 0.0F, 1.0F, 1.0F};
  /// Remaining clockwise rotation after selecting crop; independent of the requested mirror mode.
  PreviewRotation rotation = PreviewRotation::R0;
  /// Whether the source already appears horizontally mirrored after applying rotation.
  /// CameraPreview flips the upright result only when this differs from PreviewOptions::mirror's desired result.
  bool mirrored = false;

  bool operator==(const PreviewOutput&) const = default;
};

/// Current observed session state and optional preview metadata.
/// This describes session status, not a frozen image: preview may retain a texture that continues receiving frames.
/// Do not infer capture intent from this value; CameraOptions::active remains the application's control.
struct CameraStatus {
  /// Observed lifecycle state; Running alone does not guarantee that preview is populated.
  SessionState state = SessionState::Stopped;
  /// Facing reported by the selected device, or empty when unavailable or unknown.
  /// This may differ from an unspecified CameraOptions::facing and determines automatic mirroring.
  std::optional<Facing> actual_facing;
  /// Failure details when state is SessionState::Failed; empty for other states.
  std::optional<CameraError> error;
  /// Available texture and presentation geometry, or empty before output is available or after it is cleared.
  /// A suspended platform session may retain output; its presence alone does not indicate live capture.
  std::optional<PreviewOutput> preview;
  /// Whether an accepted photo request is still pending, including native processing after caller cancellation.
  /// Capture normally remains Running during a photo; this flag is shared by all session handles.
  bool capturing_photo = false;

  bool operator==(const CameraStatus&) const = default;
};

namespace detail {
struct CameraSessionData;
} // namespace detail

/// A shared UI-thread handle whose capture lifetime belongs to the owning UseCamera call.
/// Copies refer to the same session and may be passed to multiple CameraPreview components.
/// Unmounting the owner closes capture even if a preview, callback, or another object retains a handle.
/// Read state and request retries on the owning UI thread; frame delivery is managed by the backend.
class CameraSession {
public:
  /// Reads the latest observed lifecycle state, failure, and preview metadata.
  /// Reading during composition subscribes to state and output-metadata changes, not individual frames.
  /// A previously returned status value does not refresh its metadata, although its shared texture may receive frames.
  /// @return The observed session status at the time of the call.
  [[nodiscard]] CameraStatus Status() const;

  /// Retries a failed session using the latest CameraOptions supplied to its owning UseCamera call.
  /// Clears the failure and reevaluates capture intent. It does not set active to true or request permission UI.
  /// Calls in states other than SessionState::Failed, including SessionState::Closed, have no effect.
  /// Observe Status() for the asynchronous outcome; obtain missing authorization before retrying.
  /// @code
  /// const auto status = session.Status();
  /// return Button("Retry")
  ///     .OnClick([session] { session.Retry(); })
  ///     .With(Enabled(status.state == camera::SessionState::Failed));
  /// @endcode
  void Retry() const;

  /// Captures a JPEG photo through the native still-photo output of the current camera.
  /// Start from a UI event or Task, not during composition. The lazy task checks readiness when it runs.
  /// Only one request is accepted at a time; additional requests return OperationInProgress without queuing.
  /// The target orientation and resolved mirror mode are fixed when the request is accepted.
  /// Success returns an independent ImageAsset with upright pixels, normal EXIF orientation, and scale 1.
  /// Photo dimensions are negotiated by the native output, independently of the preview's layout or Cover crop.
  /// Stopping, switching, backgrounding, or session failure ends a pending request with Interrupted.
  /// An individual photo error does not set CameraStatus::error or stop otherwise healthy preview capture.
  /// Task cancellation suppresses delivery; native cleanup continues before another request is accepted.
  /// No file or photo-library entry is created. Save EncodedBytes() with the SDK File APIs when needed.
  /// @param options JPEG quality and final mirroring for this request only.
  /// @return A task yielding an owned JPEG image or CameraError; unavailable still-photo output returns Unavailable.
  /// @throws std::invalid_argument Before returning a task if quality or mirror mode is invalid.
  /// @code
  /// auto result = co_await session.CapturePhotoAsync({.jpeg_quality = 90});
  /// if (result.Succeeded()) {
  ///   photo = result.Value();
  /// } else {
  ///   message = result.Error().message;
  /// }
  /// @endcode
  [[nodiscard]] Task<CameraResult<ImageAsset>> CapturePhotoAsync(PhotoOptions options = {}) const;

private:
  explicit CameraSession(std::shared_ptr<detail::CameraSessionData> data);
  std::shared_ptr<detail::CameraSessionData> data_;

  friend CameraSession UseCamera(CameraOptions, const std::source_location&);
};

/// Installs the camera platform module in the application catalog.
/// Register this function in AppOptions::application_hooks before composing content that calls UseCamera.
/// Registration neither opens a camera nor requests authorization. Unsupported capture platforms report
/// CameraErrorCode::Unavailable when capture is requested.
/// @param context Application context supplied by HuxerUI when it runs the registered hook.
/// @code
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

/// Retains a camera session in the current composition and applies the requested capture intent.
/// Call from an active composition on the UI thread, with Install registered for its application.
/// Recomposition at the same retained call site updates the existing session. Distinct calls own separate
/// sessions; share a returned handle when multiple previews should use one device.
///
/// Authorization is checked before starting, but no permission prompt is shown. The application requests
/// Permission::Camera through ApplicationHandle::RequestPermissionAsync when appropriate.
/// Missing authorization is reported through CameraStatus::error. Unmounting the owner closes the session;
/// merely unmounting a CameraPreview does not stop capture. Mark reusable functions that call this hook
/// with [[huxerui::composable]].
/// @param options Requested camera facing and active state. Defaults to the platform's default camera and active=true.
/// @param location Call-site metadata used to retain composition state; normally leave the default value.
/// @return A shared handle for observing state, requesting retries, and creating previews.
/// @code
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
///   auto session = camera::UseCamera({.active = active.Get()});
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

/// Displays a shared camera session with centered fitting, source cropping, rotation, and final mirroring.
/// Supply bounded layout space through the parent or a Frame modifier; the component does not choose
/// a capture resolution or provide controls, a background, permission prompts, or error placeholders.
/// No content is drawn when the session has no usable preview output or the drawing bounds are empty.
/// Live texture updates do not cause UI recomposition for each frame.
/// @param session Session returned by UseCamera; multiple previews may share it without opening another device.
/// @param options Appearance for this preview only. Defaults to ImageFit::Cover and MirrorMode::Auto.
/// @return A preview View whose lifetime does not control the shared capture session.
/// @code
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
