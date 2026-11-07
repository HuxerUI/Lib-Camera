#include <huxerui/huxerui.h>
#include <huxerui/camera.h>
#include <app_resources.h>

#if defined(__ANDROID__)
#include <huxerui/android/platform_registry.h>
#endif

#include <atomic>
#include <chrono>
#include <mutex>

#define CAMERA_DISCARD(expression) static_cast<void>(expression)

using namespace huxerui;

namespace {

const Color background = Color::Rgb(15, 18, 22);
const Color surface = Color::Rgb(24, 28, 33);
const Color control_background = Color::Rgb(34, 40, 47);
const Color outline = Color::Rgb(48, 55, 63);
const Color foreground = Color::Rgb(242, 245, 247);
const Color secondary = Color::Rgb(155, 166, 178);
const Color accent = Color::Rgb(111, 226, 190);

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

const char* FacingName(std::optional<camera::Facing> facing) {
  if (!facing) return "Default camera";
  return *facing == camera::Facing::Front ? "Front camera" : "Rear camera";
}

View StatusBadge(camera::SessionState state) {
  const Color color = state == camera::SessionState::Running ? accent : secondary;
  return Row {
    Stack {}.With(Frame{.width = 6.0F, .height = 6.0F}, Background(color), CornerRadius(3.0F)),
    Label(StateName(state), 11.0F, color, FontWeight::SemiBold),
  }.With(
      Padding(EdgeInsets::Symmetric(12.0F, 8.0F)),
      Spacing(7.0F),
      CrossAlign(CrossAxisAlignment::Center),
      Background(Color::Rgb(15, 18, 22, 0.88F)),
      Border(outline, 1.0F),
      CornerRadius(20.0F)
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
      || status.state == camera::SessionState::Stopping;
  std::string title = "Your view starts here";
  std::string message = "Start the camera to open a live preview.";
  if (pending) {
    title = authorizing ? "Camera access" : "Connecting to camera";
    message = authorizing ? "Allow camera access to continue." : "Your preview will appear shortly.";
    if (status.state == camera::SessionState::Stopping) {
      title = "Stopping camera";
      message = "Finishing the current session.";
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
    Label(message, 13.0F).Align(TextAlign::Center).With(Frame{.max_width = 300.0F}),
  }.With(
      Padding(24.0F),
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
  auto application = UseApplication();
  auto tasks = UseTaskScope();
  auto sheets = UseBottomSheet();
  auto session = camera::UseCamera({.facing = facing, .active = active});
  const auto status = session.Status();
  const ImageFit selected_fit = fit;
  const bool expanded = UseViewportClass() == ViewportClass::Expanded;
  const bool live = status.state == camera::SessionState::Running && status.preview.has_value();
  const bool failed = status.state == camera::SessionState::Failed;
  const bool stopping = status.state == camera::SessionState::Stopping;

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

  auto toggle_camera = [active, authorizing, permission_error, application, tasks, session, failed] {
    if (active && !failed) {
      active = false;
      return;
    }
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

  View viewfinder = Stack {
    camera::CameraPreview(session, {.fit = selected_fit, .mirror = mirror}).Key("main-preview"),
    live ? View() : EmptyPreview(status, authorizing, permission_error),
    live && grid ? CompositionGrid() : View(),
    Column {
      Row {
        StatusBadge(status.state),
        Spacer(),
        Label("PREVIEW", 10.0F, foreground, FontWeight::SemiBold)
            .With(Padding(8.0F), Background(Color::Rgb(15, 18, 22, 0.88F)), CornerRadius(8.0F)),
      }.With(CrossAlign(CrossAxisAlignment::Center)),
      Spacer(),
      live && comparison ? View(Row {
        Spacer(),
        Column {
          camera::CameraPreview(session, {.fit = ImageFit::Contain, .mirror = camera::MirrorMode::Off})
              .With(Frame{.width = 144.0F, .height = 96.0F}, Background(Color::Black()), ClipChildren()),
          Label("FULL FRAME / MIRROR OFF", 9.0F, foreground, FontWeight::Medium).With(Padding(8.0F)),
        }.With(Background(background), Border(outline, 1.0F), CornerRadius(12.0F), ClipChildren()),
      }) : View(),
    }.With(Padding(16.0F)),
  }.With(
      Grow(),
      Align(HorizontalAlignment::Stretch, VerticalAlignment::Stretch),
      Background(Color::Black()),
      Border(outline, 1.0F),
      CornerRadius(20.0F),
      ClipChildren()
  );

  View controls = Column {
    Row {
      Column {
        Label(FacingName(status.actual_facing ? status.actual_facing : facing.Get()),
              14.0F, foreground, FontWeight::Medium),
        Label(selected_fit == ImageFit::Cover ? "Fill frame" : "Fit frame", 12.0F),
      }.With(Spacing(4.0F)),
      Spacer(),
      IconButton(app::images::photo, "View last photo").OnClick(show_photo)
          .With(Enabled(photo->HasValue()), Background(control_background), CornerRadius(14.0F)),
    }.With(CrossAlign(CrossAxisAlignment::Center)),
    Row {
      IconButton(app::images::flip, "Switch front and rear camera").OnClick([facing, status] {
        const auto current = status.actual_facing ? status.actual_facing : facing.Get();
        facing = current == camera::Facing::Front ? camera::Facing::Back : camera::Facing::Front;
      }).With(Enabled(!authorizing && !stopping), Background(control_background), CornerRadius(14.0F)),
      Button(authorizing ? "Requesting access" : stopping ? "Stopping camera"
          : status.capturing_photo ? "Taking photo" : live ? "Take photo"
          : failed || !permission_error->empty() ? "Try again"
          : active ? "Stop camera" : "Start camera")
          .OnClick([live, take_photo, toggle_camera] { if (live) take_photo(); else toggle_camera(); })
          .With(Grow(), Enabled(!authorizing && !stopping && !status.capturing_photo)),
      live ? View(IconButton(app::images::stop, "Stop camera").OnClick([active] { active = false; })
          .With(Background(control_background), CornerRadius(14.0F))) : View(),
      expanded ? View() : View(IconButton(app::images::settings, "Preview settings").OnClick(show_settings)
          .With(Background(control_background), CornerRadius(14.0F))),
    }.With(Spacing(12.0F), CrossAlign(CrossAxisAlignment::Center)),
    photo_error->empty() ? View() : View(Label(photo_error.Get(), 12.0F, Color::Rgb(255, 162, 156))),
  }.With(Spacing(18.0F), Padding(EdgeInsets::Symmetric(4.0F, 8.0F)), CrossAlign(CrossAxisAlignment::Stretch));

  View preview = Column {
    viewfinder,
    controls,
  }.With(Grow(), Spacing(16.0F), CrossAlign(CrossAxisAlignment::Stretch));

  View workspace = preview;
  if (expanded) {
    workspace = Row {
      preview,
      Column {
        Label("Preview settings", 18.0F, foreground, FontWeight::SemiBold),
        Label("Make the frame your own.", 13.0F),
        Divider(),
        ScrollView(CameraSettings(facing, fit, mirror, grid, comparison)).With(Grow()),
      }.With(
          Frame{.width = 300.0F},
          Padding(22.0F),
          Spacing(18.0F),
          CrossAlign(CrossAxisAlignment::Stretch),
          Background(surface),
          Border(outline, 1.0F),
          CornerRadius(20.0F)
      ),
    }.With(Spacing(24.0F), CrossAlign(CrossAxisAlignment::Stretch));
  }

  return Column {
    TopAppBar("Camera", Image(app::images::camera).Tint(accent).With(Frame{.width = 24.0F, .height = 24.0F}), {
      Label("HUXERUI", 11.0F, secondary, FontWeight::SemiBold),
    }),
    View(workspace).With(Grow(), Padding(EdgeInsets{.top = 0.0F, .right = 16.0F, .bottom = 16.0F, .left = 16.0F})),
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

  theme.Set(TopAppBarStyle{
      .background = background,
      .title_style = {Font::System(20.0F).WithWeight(FontWeight::SemiBold), foreground},
      .height = 68.0F,
      .horizontal_padding = 16.0F,
      .title_inset = 0.0F,
      .title_spacing = 12.0F,
      .action_spacing = 8.0F,
  });
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
        },
        .show_debug_overlay = false,
        .application_hooks = {
            huxerui::camera::Install,
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
