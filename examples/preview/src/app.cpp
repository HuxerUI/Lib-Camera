#include <huxerui/huxerui.h>
#include <huxerui/audiocapture.h>
#include <huxerui/camera.h>
#include <app_resources.h>

#if defined(__ANDROID__)
#include <huxerui/android/platform_registry.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>

#define CAMERA_DISCARD(expression) static_cast<void>(expression)

using namespace huxerui;

namespace {

const Color background = Color::Rgb(15, 18, 22);
const Color surface = Color::Rgb(24, 28, 33);
const Color control_background = Color::Rgb(34, 40, 47);
const Color overlay_background = Color::Rgb(15, 18, 22, 0.88F);
const Color outline = Color::Rgb(48, 55, 63);
const Color foreground = Color::Rgb(242, 245, 247);
const Color secondary = Color::Rgb(155, 166, 178);
const Color accent = Color::Rgb(111, 226, 190);
const Color record_red = Color::Rgb(255, 78, 89);

enum class CaptureMode { Photo, Video };
enum class VideoPhase { Idle, Authorizing, Starting, Recording, Finishing };

Text Label(StringVariant text, float size = 13.0F, Color color = secondary,
           FontWeight weight = FontWeight::Regular) {
  return Text(text).Style({Font::System(size).WithWeight(weight), color});
}

ThemeSpec CameraTokens() {
  auto tokens = FlatDarkThemeSpec();
  tokens.colors.primary = accent;
  tokens.colors.on_primary = background;
  tokens.colors.primary_container = Color::Rgb(30, 61, 53);
  tokens.colors.on_primary_container = accent;
  tokens.colors.background = background;
  tokens.colors.surface = surface;
  tokens.colors.surface_container_low = surface;
  tokens.colors.surface_container = surface;
  tokens.colors.surface_container_high = control_background;
  tokens.colors.surface_container_highest = outline;
  tokens.colors.on_surface = foreground;
  tokens.colors.on_surface_variant = secondary;
  tokens.colors.outline = outline;
  return tokens;
}

const char* StateName(camera::SessionState state) {
  switch (state) {
    case camera::SessionState::Stopped: return "STANDBY";
    case camera::SessionState::Starting: return "CONNECTING";
    case camera::SessionState::Running: return "LIVE";
    case camera::SessionState::Stopping: return "STOPPING";
    case camera::SessionState::Suspended: return "PAUSED";
    case camera::SessionState::Failed: return "UNAVAILABLE";
    case camera::SessionState::Closed: return "CLOSED";
  }
}

View StatusBadge(StringVariant text, Color color) {
  return Row {
    Stack {}.With(Frame{.width = 6.0F, .height = 6.0F}, Background(color), CornerRadius(3.0F)),
    Label(text, 11.0F, color, FontWeight::SemiBold),
  }.With(
      Padding(EdgeInsets::Symmetric(12.0F, 8.0F)),
      Spacing(7.0F),
      CrossAlign(CrossAxisAlignment::Center),
      Background(overlay_background),
      Border(outline, 1.0F),
      CornerRadius(20.0F)
  );
}

[[huxerui::composable]]
View RecordingBadge(camera::CameraSession session, bool recording, bool paused) {
  using namespace std::chrono_literals;
  auto elapsed = UseState(0s);
  auto tasks = UseTaskScope();
  Lifecycle([session, elapsed, tasks] {
    tasks.Launch([session, elapsed]() -> Task<void> {
      for (;;) {
        elapsed = std::chrono::duration_cast<std::chrono::seconds>(session.RecordedDuration());
        co_await Delay(250ms);
      }
    });
  });

  const auto seconds = elapsed.Get().count();
  char time[32];
  std::snprintf(time, sizeof(time), "%02lld:%02lld",
                static_cast<long long>(seconds / 60), static_cast<long long>(seconds % 60));
  const char* label = paused ? "PAUSED " : recording ? "REC " : "FINALIZING ";
  return StatusBadge(std::string(label) + time, recording && !paused ? record_red : secondary);
}

struct ZoomControlState {
  bool expanded = false;
  bool adjusting = false;
  bool applying = false;
  std::optional<float> proposal;
  std::uint64_t request_id = 0;
  std::string error;

  bool operator==(const ZoomControlState&) const = default;
};

void RequestZoom(camera::CameraSession session, TaskScope tasks, State<ZoomControlState> state, float ratio) {
  const auto id = state->request_id + 1;
  state.Update([&](ZoomControlState& value) {
    value.request_id = id;
    value.applying = true;
    value.proposal = ratio;
    value.error.clear();
  });
  tasks.Launch([session, ratio, id, state]() -> Task<void> {
    const auto result = co_await session.SetZoomRatioAsync(ratio);
    if (state->request_id != id) co_return;
    state.Update([&](ZoomControlState& value) {
      value.applying = false;
      if (!value.adjusting) value.proposal.reset();
      if (!result.Succeeded() && result.Error().code != camera::CameraErrorCode::Interrupted) {
        value.error = result.Error().message;
      }
    });
  });
}

void SetZoomAdjusting(State<ZoomControlState> state, bool adjusting) {
  state.Update([adjusting](ZoomControlState& value) {
    value.adjusting = adjusting;
    if (!adjusting && !value.applying) value.proposal.reset();
  });
}

View ZoomControls(camera::CameraSession session, camera::ZoomState zoom, TaskScope tasks,
                  State<ZoomControlState> state) {
  const auto set_zoom = [session, tasks, state](float ratio) {
    RequestZoom(session, tasks, state, ratio);
  };
  const float ratio = std::clamp(state->proposal.value_or(zoom.ratio), zoom.min_ratio, zoom.max_ratio);
  char label[24];
  std::snprintf(label, sizeof(label), "%.1f×", static_cast<double>(ratio));
  const auto supports = [zoom](float value) { return zoom.min_ratio <= value && value <= zoom.max_ratio; };

  return Column {
    state->expanded ? View(Column {
      Row {
        supports(1.0F) ? View(Button("1×").OnClick([set_zoom] { set_zoom(1.0F); })) : View(),
        supports(2.0F) ? View(Button("2×").OnClick([set_zoom] { set_zoom(2.0F); })) : View(),
      }.With(Spacing(8.0F), CrossAlign(CrossAxisAlignment::Center)),
      Slider(ratio).Range(zoom.min_ratio, zoom.max_ratio)
          .OnStarted([state](float) { SetZoomAdjusting(state, true); })
          .OnChanged(set_zoom)
          .OnCommitted([state](float) { SetZoomAdjusting(state, false); })
          .OnCanceled([state] { SetZoomAdjusting(state, false); })
          .Key("zoom-slider"),
      state->error.empty() ? View() : View(Label(state->error, 12.0F, foreground)),
    }.With(Frame{.width = 216.0F}, Padding(10.0F), Spacing(6.0F), CrossAlign(CrossAxisAlignment::Stretch),
           Background(overlay_background), CornerRadius(16.0F))) : View(),
    Button(std::string(label)).OnClick([state] {
      SetZoomAdjusting(state, false);
      state.Update([](ZoomControlState& value) { value.expanded = !value.expanded; });
    }).Key("zoom-toggle"),
  }.With(Spacing(6.0F), CrossAlign(CrossAxisAlignment::Center),
         Align(HorizontalAlignment::Center, VerticalAlignment::Center), Padding(8.0F));
}

[[huxerui::composable]]
View CaptureModePicker(State<CaptureMode> mode, bool enabled) {
  static constexpr float tab_width = 88.0F;
  auto drag = UseState(std::optional<float>{});
  const CaptureMode selected = mode;
  const bool video = selected == CaptureMode::Video;
  const float centered = video ? -tab_width / 2.0F : tab_width / 2.0F;
  const auto displacement = drag.Get();
  auto select = [mode, drag](CaptureMode value) {
    mode = value;
    drag = std::nullopt;
  };
  auto tab = [selected, select](CaptureMode value, const char* title) {
    const bool current = selected == value;
    return Stack {
      Label(title, 13.0F, current ? accent : secondary, FontWeight::SemiBold),
    }.With(
        Frame{.width = tab_width, .height = 44.0F},
        Align(HorizontalAlignment::Center, VerticalAlignment::Center),
        Focusable(),
        PointerCursor(PointerCursorKind::Hand),
        Semantics{.role = SemanticRole::Tab, .label = title, .selected = current,
                  .descendants = SemanticDescendantPolicy::Exclude}
    ).OnClick([select, value] { select(value); });
  };
  return Stack {
    Row {
      tab(CaptureMode::Photo, "PHOTO"),
      tab(CaptureMode::Video, "VIDEO"),
    }.With(displacement ? Offset(Point{centered + *displacement, 0.0F})
        : Offset(AnimateTo(Point{centered, 0.0F}, TweenSpec{0.18, Easing::EaseOut}))),
  }.With(
      Align(HorizontalAlignment::Center, VerticalAlignment::Center),
      Semantics{.role = SemanticRole::TabList, .label = "Capture mode"},
      Enabled(enabled),
      DragGesture{.axis = Axis::Horizontal}
  ).On<DragEvents::Changed>([drag, video](const DragEvent& event) {
    drag = video ? std::clamp(event.translation.x, 0.0F, tab_width)
                 : std::clamp(event.translation.x, -tab_width, 0.0F);
  }).On<DragEvents::Ended>([selected, video, select](const DragEvent& event) {
    const float travel = video ? event.translation.x : -event.translation.x;
    select(travel >= tab_width * 0.4F ? (video ? CaptureMode::Photo : CaptureMode::Video) : selected);
  }).On<DragEvents::Canceled>([drag](const DragEvent&) { drag = std::nullopt; });
}

View ShutterButton(const char* label, bool video, bool recording, bool enabled) {
  const float size = recording ? 28.0F : 52.0F;
  View mark = Stack {}.With(Frame{.width = size, .height = size}, Background(video ? record_red : foreground),
                            CornerRadius(recording ? 6.0F : size / 2.0F));
  return Stack {
    mark,
  }.With(
      Frame{.width = 68.0F, .height = 68.0F},
      Align(HorizontalAlignment::Center, VerticalAlignment::Center),
      Border(foreground, 3.0F),
      CornerRadius(34.0F),
      Focusable(),
      PointerCursor(PointerCursorKind::Hand),
      Semantics{.role = SemanticRole::Button, .label = label},
      Enabled(enabled)
  );
}

[[huxerui::composable]]
View CameraSettings(State<std::optional<camera::Facing>> facing, State<ImageFit> fit,
                    State<camera::MirrorMode> mirror, State<bool> grid, State<bool> comparison) {
  const std::optional<camera::Facing> selected_facing = facing;
  const ImageFit selected_fit = fit;
  const camera::MirrorMode selected_mirror = mirror;
  const std::size_t facing_index = !selected_facing ? 0 : *selected_facing == camera::Facing::Front ? 1 : 2;
  const std::size_t mirror_index = selected_mirror == camera::MirrorMode::Auto ? 0
      : selected_mirror == camera::MirrorMode::Off ? 1 : 2;
  return Column {
    Column {
      Label("Camera source", 13.0F, foreground, FontWeight::Medium),
      SegmentedButton({"Auto", "Front", "Rear"}, facing_index).OnChanged([facing](std::size_t index) {
        facing = index == 0 ? std::nullopt
            : std::optional(index == 1 ? camera::Facing::Front : camera::Facing::Back);
      }),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    Column {
      Label("Frame fit", 13.0F, foreground, FontWeight::Medium),
      SegmentedButton({"Fill", "Fit"}, selected_fit == ImageFit::Cover ? 0 : 1)
          .OnChanged([fit](std::size_t index) { fit = index == 0 ? ImageFit::Cover : ImageFit::Contain; }),
      Label("Fill crops to the viewfinder. Fit shows the full frame.", 12.0F),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    Column {
      Label("Mirror preview", 13.0F, foreground, FontWeight::Medium),
      SegmentedButton({"Auto", "Off", "On"}, mirror_index).OnChanged([mirror](std::size_t index) {
        mirror = index == 0 ? camera::MirrorMode::Auto
            : index == 1 ? camera::MirrorMode::Off : camera::MirrorMode::On;
      }),
      Label("Auto mirrors the front camera only.", 12.0F),
    }.With(Spacing(10.0F), CrossAlign(CrossAxisAlignment::Stretch)),
    Divider(),
    Switch("Composition grid", grid).OnChanged([grid](bool value) { grid = value; }),
    Switch("Comparison preview", comparison).OnChanged([comparison](bool value) { comparison = value; }),
    Label("Compare with the full, unmirrored frame from the same camera.", 12.0F),
  }.With(Spacing(22.0F), CrossAlign(CrossAxisAlignment::Stretch));
}

View CompositionGrid() {
  return Canvas([](PaintContext& paint, Size size) {
    const Color line = Color::Rgb(255, 255, 255, 0.24F);
    for (int division = 1; division < 3; ++division) {
      const float x = size.width * static_cast<float>(division) / 3.0F;
      const float y = size.height * static_cast<float>(division) / 3.0F;
      paint.DrawLine({x, 0.0F}, {x, size.height}, line, StrokeStyle{.width = 1.0F});
      paint.DrawLine({0.0F, y}, {size.width, y}, line, StrokeStyle{.width = 1.0F});
    }
  });
}

View EmptyPreview(const camera::CameraStatus& status, bool authorizing, const std::string& permission_error) {
  const bool pending = authorizing || status.state == camera::SessionState::Starting
      || status.state == camera::SessionState::Running || status.state == camera::SessionState::Stopping;
  std::string title = "Camera stopped";
  std::string message;
  if (pending) {
    title = authorizing ? "Camera access" : "Connecting to camera";
    if (authorizing) message = "Allow camera access to continue.";
    if (status.state == camera::SessionState::Stopping) {
      title = "Stopping camera";
    }
  } else if (!permission_error.empty()) {
    title = "Camera access needed";
    message = permission_error;
  } else if (status.error) {
    title = "Camera unavailable";
    message = status.error->message;
  } else if (status.state == camera::SessionState::Suspended) {
    title = "Preview paused";
    message = "The preview resumes when the camera is available.";
  }
  return Column {
    pending ? View(ProgressCircle().With(Frame{.width = 32.0F, .height = 32.0F}))
        : View(Image(app::images::camera).Tint(secondary).With(Frame{.width = 36.0F, .height = 36.0F})),
    Label(title, 20.0F, foreground, FontWeight::SemiBold).Align(TextAlign::Center),
    message.empty() ? View() : View(Label(message, 13.0F).Align(TextAlign::Center).With(Frame{.max_width = 300.0F})),
  }.With(
      Padding(24.0F),
      SafeAreaPadding{.bottom = false},
      Spacing(16.0F),
      MainAlign(MainAxisAlignment::Center),
      CrossAlign(CrossAxisAlignment::Center),
      Background(Color::Rgb(10, 13, 16))
  );
}

#if defined(__ANDROID__)
class PhotoThumbnail {
public:
  explicit PhotoThumbnail(PlatformChannel channel) : channel_(std::move(channel)) {}
  ~PhotoThumbnail() { channel_.Close(); }

  std::function<void()> Load(ImageAsset photo, std::function<void(ImageAsset, std::string)> completed) {
    const auto encoded = photo.EncodedBytes();
    const auto request = channel_.Invoke<Bytes>(
        "decode", Bytes(encoded.begin(), encoded.end()),
        [completed = std::move(completed)](PlatformResult<Bytes> result) {
          if (const auto* error = std::get_if<PlatformError>(&result)) {
            completed({}, error->message);
            return;
          }
          ImageAsset thumbnail;
          try {
            thumbnail = ImageAsset::FromEncoded(std::move(std::get<Bytes>(result)));
          } catch (const std::exception& error) {
            completed({}, error.what());
            return;
          }
          completed(std::move(thumbnail), {});
        });
    return [channel = channel_, request] { channel.Cancel(request); };
  }

private:
  PlatformChannel channel_;
};
#endif

#if defined(__ANDROID__)
[[huxerui::composable]]
View PhotoImage(ImageAsset photo) {
  auto image = UseState(ImageAsset{});
  auto error = UseState(std::string{});
  Lifecycle([photo, image, error] {
    image = ImageAsset{};
    error = std::string{};
    auto thumbnails = OpenPlatformModule<std::shared_ptr<PhotoThumbnail>>("example/photo-thumbnail");
    auto cancel = thumbnails->Load(photo, [image, error](ImageAsset thumbnail, std::string failure) {
      image = std::move(thumbnail);
      error = std::move(failure);
    });
    return [thumbnails, cancel = std::move(cancel)] { cancel(); };
  }, photo);
  if (!image->HasValue()) {
    return Column {
      error->empty() ? View(ProgressCircle()) : View(Label(error.Get(), 12.0F).Align(TextAlign::Center)),
    }.With(MainAlign(MainAxisAlignment::Center), CrossAlign(CrossAxisAlignment::Center));
  }
  return Image(image.Get()).Fit(ImageFit::Contain);
}
#else
View PhotoImage(ImageAsset photo) {
  return Image(photo).Fit(ImageFit::Contain);
}
#endif

[[huxerui::composable]]
View PhotoReview(ImageAsset photo, BottomSheetContext sheet, TaskScope tasks, State<bool> saving) {
  auto application = UseApplication();
  auto picker = UseService<FilePicker>();
  auto message = UseState(std::string{});
  auto save = [tasks, application, picker, photo, saving, message] {
    if (saving) return;
    saving = true;
    message = std::string{};
    tasks.Launch([application, picker, photo, saving, message]() -> Task<void> {
      static std::atomic_uint64_t sequence{0};
      const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
      const auto filename = "camera-" + std::to_string(timestamp) + "-" + std::to_string(++sequence) + ".jpg";
      const File directory = application.Directories().temporary_directory.Child("camera-preview-exports");
      const File temporary = directory.Child(filename);
      try {
        const bool prepared = co_await RunWorker([directory] {
          static std::once_flag cleanup;
          // Sweep previous runs only once so another window cannot delete an active export.
          std::call_once(cleanup, [&] { CAMERA_DISCARD(directory.DeleteRecursively()); });
          return directory.CreateDirectories();
        });
        const auto encoded = photo.EncodedBytes();
        if (prepared && co_await temporary.WriteBytesAsync(Bytes(encoded.begin(), encoded.end()))) {
          SaveFileOptions options{
              .suggested_name = "photo.jpg",
              .filter = {.name = "JPEG image", .extensions = {"jpg"}, .content_types = {"image/jpeg"}},
          };
          const bool saved = co_await picker->SaveFileAsync(temporary, options);
          message = std::string(saved ? "Photo saved." : "Photo was not saved.");
        } else {
          message = std::string("Could not prepare the photo for saving.");
        }
      } catch (const std::exception& error) {
        message = std::string(error.what());
      }
      co_await temporary.DeleteAsync();
      saving = false;
    });
  };
  return ScrollView(Column {
    Row {
      Label("Your photo", 20.0F, foreground, FontWeight::SemiBold),
      Spacer(),
      IconButton(app::images::close, "Close photo").OnClick([sheet] { sheet.Dismiss(); }),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    PhotoImage(photo)
        .With(Frame{.height = 280.0F}, Background(Color::Black()), CornerRadius(16.0F), ClipChildren()),
    Label(std::to_string(photo.PixelWidth()) + " × " + std::to_string(photo.PixelHeight()) + " · JPEG", 12.0F),
    Button(saving ? "Saving photo" : "Save photo").OnClick(save)
        .With(Enabled(!saving && picker->CanSaveFiles())),
    Label(!picker->CanSaveFiles() ? "File export is unavailable on this device." : message.Get(), 12.0F),
  }.With(Padding(20.0F), Spacing(14.0F), CrossAlign(CrossAxisAlignment::Stretch)))
      .With(Frame{.max_height = 560.0F});
}

[[huxerui::composable]]
View CameraWorkspace() {
  auto active = UseState(false);
  auto mode = UseState(CaptureMode::Photo);
  auto facing = UseState(std::optional<camera::Facing>{});
  auto fit = UseState(ImageFit::Cover);
  auto mirror = UseState(camera::MirrorMode::Auto);
  auto grid = UseState(false);
  auto comparison = UseState(false);
  auto authorizing = UseState(false);
  auto permission_error = UseState(std::string{});
  auto photo = UseState(ImageAsset{});
  auto photo_error = UseState(std::string{});
  auto saving_photo = UseState(false);
  auto video_phase = UseState(VideoPhase::Idle);
  auto video_file = UseState(std::optional<File>{});
  auto saving_video = UseState(false);
  auto video_message = UseState(std::string{});
  auto zoom_controls = UseState(ZoomControlState{});
  auto application = UseApplication();
  auto picker = UseService<FilePicker>();
  auto tasks = UseTaskScope();
  auto sheets = UseBottomSheet();
  auto session = camera::UseCamera({.facing = facing, .active = active});
  const VideoPhase phase = video_phase;
  const auto microphone = audio::UseAudioCapture({
      .active = phase == VideoPhase::Starting || phase == VideoPhase::Recording || phase == VideoPhase::Finishing,
      .sample_rate = 48'000,
      .channel_layout = media::AudioChannelLayout::Mono,
  });
  const auto status = session.Status();
  const auto microphone_status = microphone.Status();
  const bool zoom_available = status.zoom.has_value();
  Lifecycle([zoom_controls, zoom_available] {
    if (!zoom_available) zoom_controls = ZoomControlState{.request_id = zoom_controls->request_id + 1};
  }, zoom_available);
  const ImageFit selected_fit = fit;
  const bool live = status.state == camera::SessionState::Running && status.preview.has_value();
  const bool failed = status.state == camera::SessionState::Failed;
  const bool stopping = status.state == camera::SessionState::Stopping;
  const bool video = mode == CaptureMode::Video;
  const bool video_busy = video_phase != VideoPhase::Idle;
  const bool recording = status.recording_video && video_phase == VideoPhase::Recording;
  const bool paused = recording && status.recording_paused;

  auto show_settings = [sheets, facing, fit, mirror, grid, comparison] {
    sheets.Show([facing, fit, mirror, grid, comparison](BottomSheetContext sheet) {
      return Column {
        Row {
          Label("Preview settings", 20.0F, foreground, FontWeight::SemiBold),
          Spacer(),
          IconButton(app::images::close, "Close settings").OnClick([sheet] { sheet.Dismiss(); }),
        }.With(CrossAlign(CrossAxisAlignment::Center)),
        ScrollView(CameraSettings(facing, fit, mirror, grid, comparison)).With(Grow()),
      }.With(Padding(20.0F), Spacing(16.0F), Frame{.max_height = 580.0F}, CrossAlign(CrossAxisAlignment::Stretch));
    });
  };

  auto start_camera = [active, authorizing, permission_error, application, tasks, session] {
    if (authorizing) return;
    authorizing = true;
    permission_error = std::string{};
    tasks.Launch([active, authorizing, permission_error, application, session]() -> Task<void> {
      try {
        auto permission = co_await application.CheckPermissionAsync(Permission::Camera);
        if (permission != PermissionStatus::Granted) {
          permission = co_await application.RequestPermissionAsync(Permission::Camera);
        }
        if (permission == PermissionStatus::Granted) {
          if (active) session.Retry();
          else active = true;
        } else {
          active = false;
          permission_error = std::string("Allow camera access in system settings, then try again.");
        }
      } catch (const std::exception& error) {
        active = false;
        permission_error = std::string(error.what());
      }
      authorizing = false;
    });
  };
  Lifecycle(start_camera);

  auto take_photo = [session, tasks, photo, photo_error] {
    if (session.Status().capturing_photo) return;
    photo_error = std::string{};
    tasks.Launch([session, photo, photo_error]() -> Task<void> {
      try {
        auto result = co_await session.CapturePhotoAsync();
        if (result.Succeeded()) photo = std::move(result).Value();
        else photo_error = result.Error().message;
      } catch (const std::exception& error) {
        photo_error = std::string(error.what());
      }
    });
  };
  auto show_photo = [sheets, photo, tasks, saving_photo] {
    const ImageAsset image = photo;
    if (!image.HasValue()) return;
    sheets.Show([image, tasks, saving_photo](BottomSheetContext sheet) {
      return PhotoReview(image, sheet, tasks, saving_photo);
    });
  };

  auto toggle_recording = [session, tasks, application, active, video_phase, saving_video, video_message] {
    if (video_phase != VideoPhase::Idle) {
      if (video_phase == VideoPhase::Recording && session.Status().recording_video) {
        video_phase = VideoPhase::Finishing;
        session.StopRecording();
      }
      return;
    }
    if (saving_video) return;
    video_phase = VideoPhase::Authorizing;
    video_message = std::string{};
    tasks.Launch([application, active, video_phase, video_message]() -> Task<void> {
      try {
        auto permission = co_await application.CheckPermissionAsync(Permission::Microphone);
        if (permission != PermissionStatus::Granted && active) {
          permission = co_await application.RequestPermissionAsync(Permission::Microphone);
        }
        if (permission == PermissionStatus::Granted && active) {
          video_phase = VideoPhase::Starting;
          co_return;
        }
        if (active) video_message = "Allow microphone access to record video.";
      } catch (const std::exception& error) {
        video_message = std::string(error.what());
      }
      video_phase = VideoPhase::Idle;
    });
  };

  Lifecycle([session, microphone, tasks, application, active, status, microphone_status,
             video_phase, video_file, video_message] {
    if (video_phase != VideoPhase::Starting) return;
    if (!active || status.state == camera::SessionState::Failed) {
      video_phase = VideoPhase::Idle;
      return;
    }
    if (microphone_status.error) {
      video_message = microphone_status.error->message;
      video_phase = VideoPhase::Idle;
      return;
    }
    if (status.state != camera::SessionState::Running || microphone_status.state != audio::SessionState::Running) {
      return;
    }
    video_phase = VideoPhase::Recording;
    tasks.Launch([session, microphone, application, video_phase, video_file, video_message]() -> Task<void> {
      try {
        static std::atomic_uint64_t sequence{0};
        const auto timestamp = std::chrono::system_clock::now().time_since_epoch().count();
        const auto name = "camera-video-" + std::to_string(timestamp) + "-" + std::to_string(++sequence) + ".mp4";
        const auto output = application.Directories().temporary_directory.Child(name);
        auto result = co_await session.RecordVideoAsync(output, microphone);
        if (result.Succeeded()) {
          video_file = std::optional{output};
          video_message = "Recorded " + name;
        } else {
          video_message = result.Error().message;
        }
      } catch (const std::exception& error) {
        video_message = std::string(error.what());
      }
      video_phase = VideoPhase::Idle;
    });
  }, phase, active.Get(), status.state, microphone_status);

  auto save_video = [tasks, picker, video_phase, video_file, saving_video, video_message] {
    const auto source = video_file.Get();
    if (!source || saving_video || video_phase != VideoPhase::Idle || !picker->CanSaveFiles()) return;
    saving_video = true;
    tasks.Launch([picker, source = *source, video_file, saving_video, video_message]() -> Task<void> {
      bool saved = false;
      try {
        SaveFileOptions options{
            .filter = {.name = "MP4 video", .extensions = {"mp4"}, .content_types = {"video/mp4"}},
        };
        saved = co_await picker->SaveFileAsync(source, options);
        video_message = std::string(saved ? "Video saved." : "Video was not saved.");
        if (saved && video_file.Get() == source) video_file = std::nullopt;
      } catch (const std::exception& error) {
        video_message = std::string(error.what());
      }
      saving_video = false;
      if (saved) co_await source.DeleteAsync();
    });
  };

  const Color status_color = status.state == camera::SessionState::Running ? accent : secondary;
  View badge = recording || video_phase == VideoPhase::Finishing
      ? RecordingBadge(session, recording && live, paused)
      : StatusBadge(StateName(status.state), status_color);

  View preview = Stack {
    camera::CameraPreview(session, {.fit = selected_fit, .mirror = mirror}).Key("main-preview"),
    live && grid ? CompositionGrid() : View(),
  }.With(Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch));
  if (live && zoom_available) {
    const auto finish_zoom = [zoom_controls](const TransformEvent&) { SetZoomAdjusting(zoom_controls, false); };
    preview = std::move(preview).With(TransformGesture{})
        .On<TransformEvents::Started>([zoom_controls](const TransformEvent&) { SetZoomAdjusting(zoom_controls, true); })
        .On<TransformEvents::Changed>([session, tasks, zoom_controls](const TransformEvent& event) {
          if (!std::isfinite(event.scale) || event.scale <= 0.0F || event.scale == 1.0F) return;
          const auto zoom = session.Status().zoom;
          if (!zoom) return;
          const auto current = zoom_controls->proposal.value_or(zoom->ratio);
          const auto target = std::clamp(current * event.scale, zoom->min_ratio, zoom->max_ratio);
          if (target != current) RequestZoom(session, tasks, zoom_controls, target);
        })
        .On<TransformEvents::Ended>(finish_zoom)
        .On<TransformEvents::Canceled>(finish_zoom);
  }

  View viewfinder = Stack {
    std::move(preview).Key("preview-surface"),
    live ? View() : EmptyPreview(status, authorizing, permission_error),
    Column {
      Row {
        badge,
        Spacer(),
        live ? View(IconButton(app::images::stop, "Stop camera").OnClick([active] { active = false; })
            .With(Background(overlay_background), CornerRadius(14.0F))) : View(),
        IconButton(app::images::settings, "Preview settings").OnClick(show_settings)
            .With(Background(overlay_background), CornerRadius(14.0F)),
      }.With(
          Padding(12.0F),
          SafeAreaPadding{.bottom = false},
          Spacing(8.0F),
          CrossAlign(CrossAxisAlignment::Center),
          Background(LinearGradient{
              .start = {0.0F, 0.0F},
              .end = {0.0F, 1.0F},
              .stops = {{0.0F, Color::Rgb(0, 0, 0, 0.65F)}, {1.0F, Color::Rgb(0, 0, 0, 0.0F)}},
          })
      ),
      Spacer(),
      live && comparison ? View(Row {
        Spacer(),
        Column {
          camera::CameraPreview(session, {.fit = ImageFit::Contain, .mirror = camera::MirrorMode::Off})
              .With(Frame{.width = 144.0F, .height = 96.0F}, Background(Color::Black()), ClipChildren()),
          Label("FULL FRAME / MIRROR OFF", 9.0F, foreground, FontWeight::Medium).With(Padding(8.0F)),
        }.With(Background(background), Border(outline, 1.0F), CornerRadius(12.0F), ClipChildren()),
      }.With(Padding(16.0F), SafeAreaPadding{.top = false, .bottom = false})) : View(),
      zoom_available ? ZoomControls(session, *status.zoom, tasks, zoom_controls).Key("camera-zoom") : View(),
    }.With(CrossAlign(CrossAxisAlignment::Stretch)),
  }.With(
      Grow(),
      Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
      Background(Color::Black()),
      ClipChildren()
  );

  const char* photo_action = status.capturing_photo ? "Taking photo" : "Take photo";
  const char* video_action = recording ? "Stop recording" : phase == VideoPhase::Authorizing ? "Requesting microphone access"
      : phase == VideoPhase::Starting ? "Starting microphone" : video_busy ? "Processing video" : "Record video";
  const bool shutter_enabled = video ? recording || (!video_busy && !saving_video) : !status.capturing_photo;
  View primary = live || video_busy
      ? ShutterButton(video ? video_action : photo_action, video, video_busy, shutter_enabled)
          .OnClick([video, take_photo, toggle_recording] {
            if (video) toggle_recording();
            else take_photo();
          })
      : Button(authorizing ? "Requesting access" : stopping ? "Stopping camera"
                           : failed || !permission_error->empty() ? "Try again"
                           : active ? "Stop camera" : "Start camera")
          .OnClick([active, failed, start_camera] {
            if (active && !failed) active = false;
            else start_camera();
          }).With(Enabled(!authorizing && !stopping));

  View left_control = (recording && live
      ? IconButton(paused ? app::images::resume : app::images::pause,
                   paused ? "Resume recording" : "Pause recording").OnClick([session] {
          if (session.Status().recording_paused) session.ResumeRecording();
          else session.PauseRecording();
        })
      : IconButton(app::images::photo, "View last photo").OnClick(show_photo).With(Enabled(photo->HasValue())))
      .With(Background(control_background), CornerRadius(14.0F));

  View controls = Column {
    video_file->has_value() && picker->CanSaveFiles() ? View(Button(saving_video ? "Saving video" : "Save video")
        .OnClick(save_video).With(Enabled(!saving_video && !video_busy))) : View(),
    video_message->empty() ? View() : View(Label(video_message.Get(), 12.0F)),
    photo_error->empty() ? View() : View(Label(photo_error.Get(), 12.0F, Color::Rgb(255, 162, 156))),
    CaptureModePicker(mode, !video_busy && !status.capturing_photo),
    Stack {
      Row {
        left_control,
        Spacer(),
        primary,
        Spacer(),
        IconButton(app::images::flip, "Switch front and rear camera").OnClick([facing, status] {
          const auto current = status.actual_facing ? status.actual_facing : facing.Get();
          facing = current == camera::Facing::Front ? camera::Facing::Back : camera::Facing::Front;
        }).With(Enabled(!authorizing && !stopping), Background(control_background), CornerRadius(14.0F)),
      }.With(Frame{.width = 288.0F}, CrossAlign(CrossAxisAlignment::Center)),
    }.With(Frame{.height = 68.0F}, Align(HorizontalAlignment::Center, VerticalAlignment::Center)),
  }.With(
      Spacing(8.0F),
      Padding(EdgeInsets::Symmetric(16.0F, 8.0F)),
      SafeAreaPadding{.top = false},
      CrossAlign(CrossAxisAlignment::Stretch)
  );

  return Column {
    viewfinder,
    controls,
  }.With(
      CrossAlign(CrossAxisAlignment::Stretch),
      Background(background),
      SystemBarsAppearance{
          .status_bar_background = background,
          .navigation_bar_background = background,
          .status_bar_content = SystemBarContentBrightness::Light,
          .navigation_bar_content = SystemBarContentBrightness::Light,
      }
  );
}

[[huxerui::composable]]
View StyledWorkspace() {
  ThemeDefinition theme;

  auto button = UseEnvironment<ButtonStyle>();
  button.background = foreground;
  button.label_style = {Font::System(15.0F).WithWeight(FontWeight::SemiBold), background};
  button.minimum_height = 50.0F;
  button.corner_radii = CornerRadii{14.0F};
  theme.Set(button);

  auto icon = UseEnvironment<IconButtonStyle>();
  icon.foreground = foreground;
  icon.disabled_foreground = Color::Rgb(100, 112, 125);
  icon.minimum_interactive_size = 48.0F;
  icon.state_layer_size = 44.0F;
  icon.icon_size = 22.0F;
  icon.corner_radius = 12.0F;
  theme.Set(icon);

  auto segmented = UseEnvironment<SegmentedButtonStyle>();
  segmented.minimum_height = 44.0F;
  segmented.selected_background = outline;
  segmented.selected_label = foreground;
  segmented.selected_border = Border(outline, 1.0F);
  segmented.corner_radii = CornerRadii{10.0F};
  theme.Set(segmented);

  return Theme(theme, CameraWorkspace());
}

} // namespace

View App() {
  return FlatTheme(CameraTokens(), StyledWorkspace());
}

const Application application{
    App,
    {
        .window = {
            .title = "Camera Preview",
            .initial_size = {1180.0F, 820.0F},
            .minimum_size = Size{360.0F, 480.0F},
            .content_mode = WindowContentMode::EdgeToEdge,
        },
        .show_debug_overlay = false,
        .application_hooks = {
            huxerui::camera::Install,
            huxerui::audio::Install,
#if defined(__ANDROID__)
            [](ApplicationContext& context) {
              android::JavaPlatformModuleFactory<std::shared_ptr<PhotoThumbnail>> factory;
              factory.class_name = "org.huxerui.lib.camera.preview.PhotoThumbnailModule";
              factory.create = [](PlatformChannel channel) {
                return std::make_shared<PhotoThumbnail>(std::move(channel));
              };
              context.RegisterPlatformModule<std::shared_ptr<PhotoThumbnail>>("example/photo-thumbnail", std::move(factory));
            },
#endif
        },
    }
};
