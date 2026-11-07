# HuxerUI Camera

Camera preview and JPEG photo capture for HuxerUI applications on Android, iOS, macOS, and Windows.

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

Configure each preview through `PreviewOptions`:

| Option | Default | Behavior |
| --- | --- | --- |
| `fit` | `ImageFit::Cover` | Fills and crops to the view's bounds. Use `Contain` to show the complete frame. |
| `mirror` | `MirrorMode::Auto` | Mirrors confirmed front cameras. `Off` and `On` select the final unmirrored or mirrored appearance. |

`session.Status()` observes lifecycle state, failure details, and photo activity. When a session fails, inspect `status.error->code` for recovery decisions and `status.error->message` for diagnostics. After resolving the cause, call `session.Retry()`; it does not request permission or change `active`.

The component calling `UseCamera` owns the session lifetime. Removing that component closes capture, even if another object retains a session handle. Removing an individual `CameraPreview` does not stop its shared session.

## Capture a Photo

Launch `CapturePhotoAsync` from a UI event or task. It returns `CameraResult<ImageAsset>`, an alias for `Result<ImageAsset, CameraError>`.

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
    Text(message.Get()),
  };
}
```

The result is an upright JPEG whose resolution is selected by the native camera output. Preview size, fitting, and cropping do not affect the photo. Photo mirroring is independent of preview mirroring and defaults to `Off`. JPEG quality defaults to 90 and must be between 1 and 100; invalid options throw `std::invalid_argument` before returning the task.

Keep the original `ImageAsset` for saving. For photo review on Android, decode a separate thumbnail before displaying it: constraining an `Image` view's layout does not reduce the original bitmap's decoded dimensions. The [example](examples/preview/src/app.cpp) displays a thumbnail with a maximum edge of 2048 pixels while preserving the original JPEG for export.

Only one photo request can be in progress per session. Additional requests return `OperationInProgress`; a session that is not ready returns `NotReady`. `capturing_photo` stays true until native processing finishes, including after the caller cancels waiting. Stopping, switching cameras, backgrounding, or session failure interrupts a pending photo with `Interrupted`.

A photo failure does not stop an otherwise healthy preview. A captured `ImageAsset` remains usable after the session closes. Devices that cannot provide still-photo output report `Unavailable` for photo requests. Video recording, RAW, HDR, maximum sensor resolution, and metadata preservation are outside the current API's guarantees.

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

Your application owns temporary-file cleanup. Keep the file available until writing and native export finish; canceling the caller does not make it safe to delete immediately. The example keeps saving in the camera page's task scope so dismissing photo review does not cancel the export. Saving directly to the system photo library is not provided by this module.

## Run the Example

The [preview example](examples/preview) includes permission handling, camera switching, fitting and mirroring controls, a composition grid, simultaneous previews, photo review, and file export.

From `examples/preview`, build and run for your target platform:

```sh
huxerui build android --profile debug
huxerui run android --profile debug
```

Replace `android` with `ios`, `macos`, or `windows` as needed. For Android, use `--java-home /path/to/jdk17` if your default Java installation is unsuitable. Running on an iOS device requires your own signing configuration. The example already includes the Apple camera usage descriptions.

Select **Start camera** to authorize and start the preview, then **Take photo** to capture. The photo icon opens the latest photo and its **Save photo** action. Use the adjacent controls to stop capture, switch cameras, or open preview settings. A failed session offers **Try again**.

On Windows, the backend uses Media Foundation capture and D3D11 preview textures. Camera facing comes from device enclosure metadata; external cameras may have unknown facing and should use the default selection. Still photos use the native photo output and WIC to normalize orientation and JPEG quality.

## Tests

Configure with `-DHUXERUI_CAMERA_BUILD_TESTS=ON` and run `ctest --test-dir <build-directory> --output-on-failure`. On Windows, this includes JPEG corner checks for all stream rotations, all eight EXIF orientations, and final mirroring without opening a camera.

Run `<build-directory>/camera_windows_tests.exe --device` explicitly to check live preview, photos, facing selection, stopping during pending operations, and restart. A camera and camera access are required. The Windows backend was tested with an ACER HD User Facing camera at 1280×720, and the example was manually verified. Device removal, permission revocation during capture, and sustained performance remain unverified.

See the [public API reference](include/huxerui/camera.h) for complete options and contracts, or the [design document](docs/camera-design.md) for implementation details.
