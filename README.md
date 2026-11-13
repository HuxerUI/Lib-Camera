# HuxerUI Camera

Camera preview, frame processing, JPEG photo capture, and video recording with optional microphone audio for HuxerUI applications on Android, iOS, macOS, and Windows.

Use `UseCamera` to control a camera session, `CameraPreview` to display it, and `CapturePhotoAsync` to take photos. Multiple previews can share one session with independent fitting and mirroring. Preview frames update without recomposing the UI on every frame.

Linux and Web capture are not supported; capture requests report `CameraErrorCode::Unavailable`.

## Installation

In an application created with the installed HuxerUI SDK, add the library to your application target:

```cmake
huxerui_use_library(my_app
        TARGET HuxerUI::Camera
        PATH "/path/to/Lib-Camera"
)
```

Camera requires `HuxerUI::Media` and `HuxerUI::AudioCapture`. Use existing CMake targets or place `Lib-Media`
and `Lib-AudioCapture` beside `Lib-Camera`. Set `HUXERUI_CAMERA_MEDIA_PATH` and
`HUXERUI_CAMERA_AUDIO_CAPTURE_PATH` to use other source checkouts. AudioCapture must provide
`AudioReader::FinishInput()`.

Include the public headers and register the camera module in your application's `application_hooks`:

```cpp
#include <huxerui/camera.h>
#include <huxerui/huxerui.h>

using namespace huxerui;

const Application application{
    App,
    {
        .application_hooks = {
            camera::Install,
        },
    }
};
```

Here, `App` is your application's root view function. Registration does not open the camera or request permission.

## Permissions and Preview

The application owns camera authorization. Use `ApplicationHandle::CheckPermissionAsync(Permission::Camera)` to check access and `RequestPermissionAsync(Permission::Camera)` from a user action to request it. Set capture intent to active only after authorization is granted.

- **Android:** the library declares the camera permission and optional camera hardware features. Your application must still request runtime permission.
- **iOS and macOS:** add `NSCameraUsageDescription` to the application's Info.plist with a description of how the camera is used.
- **Windows:** allow camera access for desktop applications in Windows privacy settings. Packaged applications also need the `webcam` capability in their package manifest.

This component assumes the caller has obtained permission before setting `active` to true:

```cpp
[[huxerui::composable]]
View CameraContent(bool active) {
  auto session = camera::UseCamera({.active = active});

  return camera::CameraPreview(session, {
      .fit = ImageFit::Cover,
      .mirror = camera::MirrorMode::Auto,
  }).With(Frame{.width = 320.0F, .height = 240.0F});
}
```

Configure capture through `CameraOptions`:

| Option | Default | Behavior |
| --- | --- | --- |
| `active` | `true` | Starts or stops capture asynchronously. Capture suspends in the background. |
| `facing` | Empty | Selects the default camera. `Facing::Front` or `Facing::Back` requires that facing, without falling back to the opposite one. |
| `output_pixel_width`, `output_pixel_height` | Both `0` | Set both to positive pixel counts to size the shared preview and recording texture before rotation. Changing the pair reopens active capture and finishes any current recording. |
| `preferred_capture_pixel_width`, `preferred_capture_pixel_height` | Both `0` | Request the pixel dimensions of the source video stream. Matching ignores orientation. Set both to zero for the platform default. |
| `preferred_capture_fps` | Empty | Request a positive, finite capture frame rate. The platform can choose a different rate or leave it under automatic control. |

Capture selection favors a working preview and photo combination, then the preferred video resolution,
then the closest frame rate. Read `session.Status().capture_format` for the delivered source dimensions
before frame processing or output sizing, and the configured rate range when available. Frame timestamps
describe actual delivery timing. Changing capture preferences reopens capture and finalizes an active recording.

Configure each preview through `PreviewOptions`:

| Option | Default | Behavior |
| --- | --- | --- |
| `fit` | `ImageFit::Cover` | Fills and crops to the view's bounds. Use `Contain` to show the complete frame. |
| `mirror` | `MirrorMode::Auto` | Mirrors confirmed front cameras. `Off` and `On` select the final unmirrored or mirrored appearance. |

`session.Status()` observes lifecycle state, failure details, and photo activity. When a session fails, inspect `status.error->code` for recovery decisions and `status.error->message` for diagnostics. After resolving the cause, call `session.Retry()`; it does not request permission or change `active`.

The component calling `UseCamera` owns the session lifetime. Removing that component closes capture, even if another object retains a session handle. Removing an individual `CameraPreview` does not stop its shared session.

## Adjust Zoom

Read `session.Status().zoom` for the active camera's applied zoom ratio and supported range. When it is
available, call `session.SetZoomRatioAsync(ratio)` from a UI task to adjust preview, photos, and recording
together. Adjusting zoom keeps the camera open and continues any active recording. Zoom is available on
supported Android and iOS cameras and on Windows cameras exposing extended zoom control.

The example shows supported zoom shortcuts in the viewfinder. Drag the multiplier strip horizontally or pinch
the viewfinder to change zoom continuously during preview or recording.

## Adjust Exposure Compensation

Read `session.Status().exposure_compensation` for the active camera's applied EV, supported range, and optional
step. When it is available, call `session.SetExposureCompensationAsync(ev)` from a UI task to adjust the
automatic exposure target for preview, photos, and recording. The device constrains the request to its supported
values. Read the status again for the applied EV. Pass `0.0F` to remove compensation. Exposure compensation
is available on supported Android and iOS cameras and on Windows cameras exposing extended EV control.

Tap the viewfinder to reveal the exposure slider on supported cameras. Tap the sun below the slider to reset to zero.

## Focus and Meter at a Point

When `session.Status().can_focus_and_meter` is true, call `session.FocusAndMeterAsync(point, size, options)` with
the point and size in preview-local logical coordinates and the same presentation options as `CameraPreview`.
The call accounts for fitting, rotation, mirroring, and the crop applied for an explicitly sized output. It
controls autofocus or metering as supported by the active camera on Android, iOS, and macOS. Point control is
unavailable with a custom frame processor.

## Capture a Photo

Launch `CapturePhotoAsync` from a UI event or task. It returns `CameraResult<ImageAsset>`, an alias for `Result<ImageAsset, CameraError>`.

Set `PhotoOptions::flash_mode` to `FlashMode::Auto` or `FlashMode::On` when the corresponding capability is
reported by `session.Status().photo_flash`. The default is `FlashMode::Off`. Turn off the continuous light
before requesting a flash photo.

The following component stores the captured image in state supplied by its caller:

```cpp
[[huxerui::composable]]
View PhotoCapture(camera::CameraSession session, State<ImageAsset> photo) {
  auto tasks = UseTaskScope();
  auto message = UseState(std::string{});
  const auto status = session.Status();

  return Column {
    camera::CameraPreview(session).With(Frame{.height = 240.0F}),
    Button("Take photo").OnClick([tasks, session, photo, message] {
      tasks.Launch([session, photo, message]() -> Task<void> {
        auto result = co_await session.CapturePhotoAsync({
            .jpeg_quality = 90,
            .mirror = camera::MirrorMode::Off,
        });
        if (result.Succeeded()) {
          photo = result.Value();
          message = std::string{"Photo captured."};
        } else {
          message = result.Error().message;
        }
      });
    }).With(Enabled(status.state == camera::SessionState::Running && !status.capturing_photo)),
    Text(message),
  };
}
```

The result is an upright JPEG whose resolution is selected by the platform camera output. Preview size, fitting, and cropping do not affect the photo. Photo mirroring is independent of preview mirroring and defaults to `Off`. JPEG quality defaults to 90 and must be between 1 and 100; invalid options throw `std::invalid_argument` before returning the task.

Keep the original `ImageAsset` for saving. For photo review on Android, decode a separate thumbnail before displaying it: constraining an `Image` view's layout does not reduce the original bitmap's decoded dimensions. The [example](examples/preview/src/app.cpp) displays a thumbnail with a maximum edge of 2048 pixels while preserving the original JPEG for export.

Only one photo request can be in progress per session. Additional requests return `OperationInProgress`; a session that is not ready returns `NotReady`. `capturing_photo` stays true until platform processing finishes, including after the caller cancels waiting. Stopping, switching cameras, backgrounding, or session failure interrupts a pending photo with `Interrupted`.

A photo failure does not stop an otherwise healthy preview. A captured `ImageAsset` remains usable after the session closes. Devices that cannot provide still-photo output report `Unavailable` for photo requests. RAW, HDR, maximum sensor resolution, and metadata preservation are outside the current API's guarantees.

## Save a Photo

Capture does not write a file or add an item to the system photo library. Save the JPEG bytes with the SDK's `File` API:

```cpp
Task<bool> WritePhoto(ImageAsset photo, File destination) {
  const auto encoded = photo.EncodedBytes();
  co_return co_await destination.WriteBytesAsync(Bytes(encoded.begin(), encoded.end()));
}
```

The destination must be writable and its parent directory must exist. Writing creates or truncates the file. Obtain application directories with `UseService<FileSystem>()` during composition.

To let the user choose a destination, write a temporary JPEG and export it with `FilePicker`:

```cpp
Task<bool> ExportPhoto(ImageAsset photo, File prepared_file, std::shared_ptr<FilePicker> picker) {
  if (!picker->CanSaveFiles()) co_return false;
  if (!co_await WritePhoto(photo, prepared_file)) co_return false;

  SaveFileOptions options{
      .suggested_name = "photo.jpg",
      .filter = {.name = "JPEG image", .extensions = {"jpg"}, .content_types = {"image/jpeg"}},
  };
  co_return co_await picker->SaveFileAsync(prepared_file, options);
}
```

Obtain the picker with `UseService<FilePicker>()` during composition and pass a unique temporary file as `prepared_file`. The task returns false when saving is unavailable, dismissed, or unsuccessful. File operations use SDK file results rather than `CameraError`.

Your application owns temporary-file cleanup. Keep the file available until writing and platform export finish; canceling the caller does not make it safe to delete immediately. The example keeps saving in the camera page's task scope so dismissing photo review does not cancel the export. Saving directly to the system photo library is not provided by this module.

## Process and Record Video

Pass a retained `FrameProcessorFactory` to `CameraOptions::frame_processor_factory`. Each frame producer calls the factory with the source texture's pixel width and height on its first usable frame. It retains the returned processor until the producer retires or the input dimensions change. The processor receives the published source texture, its geometry, and the capture timestamp. Return a published texture and its matching geometry. Preview and recording use the same result. Reuse the output texture identity across frames with stable geometry so frame updates only redraw the texture. Keep the factory identity stable during capture and update synchronized effect parameters to change an effect while recording.

```cpp
static const auto factory = std::make_shared<const camera::FrameProcessorFactory>(
    [](std::uint32_t, std::uint32_t) -> camera::FrameProcessor {
      return [](const camera::PreviewOutput& input, std::chrono::nanoseconds) {
        auto output = input;
        output.crop = {0.125F, 0.125F, 0.75F, 0.75F};
        return camera::CameraResult<camera::PreviewOutput>::Success(std::move(output));
      };
    });

auto session = camera::UseCamera({
    .active = active,
    .frame_processor_factory = factory,
    .output_pixel_width = 1280,
    .output_pixel_height = 720,
});
```

With an output size configured, Camera center-crops within the processed frame's crop and scales to the requested physical pixel dimensions. The resulting texture has a full-frame crop and retains the frame's rotation and mirroring metadata. Both zero values use the processed frame at its original size. Still photos use their independent output, and video sizes must satisfy the recording encoder's requirements.

Run `session.RecordVideoAsync(destination)` in a task and call `session.StopRecording()` from the stop action. The task returns after the encoder drains and the muxer closes the output. `CameraStatus::recording_video` stays true during recording, pauses, and finalization. Stopping capture, switching cameras, or backgrounding finalizes a recording with captured frames. The file contains one H.264 video track. The recording uses the initial frame's dimensions and stores its crop and rotation as MP4 presentation metadata. Preview fitting and mirroring remain per-view settings.

Use `session.PauseRecording()` to skip new frames while preview and frame processing continue. `session.ResumeRecording()` appends to the same MP4 and joins the recording intervals without the pause gap. Read `CameraStatus::recording_paused` for the pause state. The stop action can finalize a paused recording.

Poll `session.RecordedDuration()` on the UI thread for the current recording duration in microseconds. It advances as the MP4 muxer accepts video samples, including frames that drain after a pause or stop. After success, the final duration remains available until the next recording starts or the session closes.

| Destination | Output |
| --- | --- |
| `File` | Creates a new regular MP4 file. |
| `RandomAccessStream` | Transfers an empty writable stream and writes regular MP4 from offset zero. |
| `OutputStream` | Transfers a fresh writable sequential stream and writes fragmented MP4. |

Android recording requires API 28 or later.

## Use the Torch

Read `session.Status().torch` for the active camera's continuous light capability and observed state. Call
`session.SetTorchEnabledAsync(true)` from a UI task to turn on the light during preview or recording.
The light belongs to the camera session and stays on when recording pauses or ends. Switching cameras
or closing the session turns it off. The example shows a torch button in video mode and turns off the
light when switching to photo mode.

## Record with a Microphone

Register `huxerui::audio::Install` in the application's hooks alongside `camera::Install`. After granting microphone
permission, activate `audio::UseAudioCapture()` in the owning composition. Pass the resulting session as the second
argument to `RecordVideoAsync(destination, microphone)` from a UI task. All three destinations accept an
optional microphone session. The default is `std::nullopt` for silent recording. Camera attaches the microphone's sole
reader when the task starts. Configure microphone effects through `audio::CaptureOptions::pcm_processor` with
`PcmTiming::CaptureTime` to preserve the source timestamps. Use interleaved S16 mono or stereo at 44.1 or 48 kHz.

Pause and resume the recording through the Camera session. The recorder maps both sources to a shared capture clock
and excludes the same pause intervals from both tracks. On stop, Camera seals the audio reader at the recording cutoff,
drains its processor and both encoders, and closes the MP4. Keep microphone capture active until the recording task
completes. The recorder releases the reader so a later task can use the same capture session. A device that cannot
provide camera timestamps on the shared clock reports `Unavailable` for microphone recording.

`File` and `RandomAccessStream` write regular MP4 with known AAC priming and padding represented as presentation
trim. `OutputStream` writes fragmented MP4 with the same audio and video timing and complete AAC access units,
including any encoder priming and padding.

For iOS, use an SDK that discovers the Swift packages at `platform/ios/Camera/` and `platform/ios/Media/`. Register `HuxerUI::Camera` on the application target. The SDK includes Camera's Media dependency in both the generated Swift package integration and the application core archive.

For recording, publish a `GlTexture` on Android, a `PixelBufferTexture` with an NV12 or BGRA buffer on iOS or macOS, or an opaque `D3D11Texture` on Windows. Camera snapshots the processed frame before the next publication. The iOS and macOS backends convert BGRA to NV12 for the encoder.

## Run the Example

The [preview example](examples/preview) includes permission handling, camera switching, fitting and mirroring controls, a composition grid, simultaneous previews, photo review, and file export.

From `examples/preview`, build and run for your target platform:

```sh
huxerui build android --profile debug
huxerui run android --profile debug
```

Replace `android` with `ios`, `macos`, or `windows` as needed. For Android, use `--java-home /path/to/jdk17` if your default Java installation is unsuitable. Running on an iOS device requires your own signing configuration. The example already includes the Apple camera and microphone usage descriptions.

The example opens in **PHOTO** and starts the preview automatically after camera access is granted. The viewfinder fills the window above the capture controls, with **Preview settings** in the upper corner. Tap or swipe the **PHOTO** and **VIDEO** labels to choose a capture mode. In **PHOTO**, press the white shutter to capture and use the photo icon to review or save the latest image. In **VIDEO**, press the red shutter and allow microphone access to start recording. The recording badge shows the recorded video duration in whole seconds. Use the control to the left of the shutter to pause or resume. Press the shutter again to finalize the MP4 with microphone audio, then select **Save video** to export it to a location you choose in the system file picker. You can retry if you dismiss the picker. A failed camera session offers **Try again**.

On Windows, the backend uses Media Foundation capture and D3D11 preview textures. Camera facing comes from device enclosure metadata; external cameras may have unknown facing and should use the default selection. Still photos use the Capture Engine photo output and WIC to normalize orientation and JPEG quality.

## Tests

Configure with `-DHUXERUI_CAMERA_BUILD_TESTS=ON` and run `ctest --test-dir <build-directory> --output-on-failure`.
On macOS, recording tests cover the three output types. The microphone tests use a synthetic input from the
AudioCapture source checkout. On Windows, the suite includes JPEG corner checks for all stream rotations,
all eight EXIF orientations, and final mirroring without opening a camera.

Run `<build-directory>/camera_windows_tests.exe --device` explicitly to check live preview, photos, facing selection, stopping during pending operations, and restart. A camera and camera access are required. The Windows backend was tested with an ACER HD User Facing camera at 1280×720, and the example was manually verified. Device removal, permission revocation during capture, and sustained performance remain unverified.

See the [public API reference](include/huxerui/camera.h) for complete options and contracts, or the [design document](docs/camera-design.md) for implementation details.
