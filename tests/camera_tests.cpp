#include <huxerui/huxerui.h>

#include <array>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <stdexcept>

#include "camera_internal.h"
#include "camera_session.h"
#include "photo_fixture.h"

using namespace huxerui;
using namespace huxerui::camera;

namespace {

void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
}

void Near(float actual, float expected) {
  Check(std::abs(actual - expected) < 0.001F, "Unexpected preview coordinate.");
}

void TestGeometry() {
  PreviewOutput output;
  const Size source{400.0F, 200.0F};
  const std::array<camera::PreviewRotation, 4> rotations{camera::PreviewRotation::R0, camera::PreviewRotation::R90, camera::PreviewRotation::R180, camera::PreviewRotation::R270};
  const std::array<Point, 4> origin{{{0.0F, 0.0F}, {200.0F, 0.0F}, {400.0F, 200.0F}, {0.0F, 400.0F}}};
  const std::array<Point, 4> top_right{{{400.0F, 0.0F}, {200.0F, 400.0F}, {0.0F, 200.0F}, {0.0F, 0.0F}}};
  for (std::size_t index = 0; index < rotations.size(); ++index) {
    output.rotation = rotations[index];
    const Size bounds = index % 2 == 0 ? source : Size{200.0F, 400.0F};
    const auto placed = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Contain}, {}, bounds);
    Check(placed.has_value(), "Valid geometry was rejected.");
    const auto first = placed->transform.Apply({0.0F, 0.0F});
    const auto second = placed->transform.Apply({400.0F, 0.0F});
    Near(first.x, origin[index].x);
    Near(first.y, origin[index].y);
    Near(second.x, top_right[index].x);
    Near(second.y, top_right[index].y);
    const auto inverse = placed->transform.Inverse(second);
    Check(inverse.has_value(), "Preview transform must remain invertible.");
    Near(inverse->x, 400.0F);
    Near(inverse->y, 0.0F);
    for (bool already_mirrored : {false, true}) {
      output.mirrored = already_mirrored;
      const auto front = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Contain}, Facing::Front, bounds);
      const auto point = front->transform.Apply({0.0F, 0.0F});
      Near(point.x, already_mirrored ? origin[index].x : bounds.width - origin[index].x);
      Near(point.y, origin[index].y);
    }
    output.mirrored = false;
  }
  output.rotation = camera::PreviewRotation::R0;
  const auto contain = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Contain}, {}, {200.0F, 200.0F});
  Near(contain->transform.Apply({0.0F, 0.0F}).y, 50.0F);
  const auto cover = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Cover}, {}, {200.0F, 200.0F});
  Near(cover->transform.Apply({0.0F, 0.0F}).x, -100.0F);
  const auto fill = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Fill}, {}, {200.0F, 200.0F});
  Near(fill->transform.Apply({400.0F, 200.0F}).x, 200.0F);
  Near(fill->transform.Apply({400.0F, 200.0F}).y, 200.0F);
  const auto small = camera::detail::PlacePreview(source, output, {.fit = ImageFit::ScaleDown}, {}, {800.0F, 800.0F});
  Near(small->transform.m11, 1.0F);
  const auto none = camera::detail::PlacePreview(source, output, {.fit = ImageFit::None}, {}, {100.0F, 100.0F});
  Near(none->transform.m11, 1.0F);
  output.crop = {0.25F, 0.25F, 0.5F, 0.5F};
  const auto cropped = camera::detail::PlacePreview(source, output, {}, {}, {200.0F, 100.0F});
  Check(cropped->source == Rect{100.0F, 50.0F, 200.0F, 100.0F}, "Crop must use intrinsic source coordinates.");
  Check(!camera::detail::PlacePreview(source, output, {}, {}, {}), "Empty bounds must not draw.");
  output.crop.width = std::numeric_limits<float>::quiet_NaN();
  Check(!camera::detail::PlacePreview(source, output, {}, {}, source), "Non-finite geometry must not draw.");
}

class TestPlatform final : public UiWindow, public huxerui::Runtime {
public:
  TestPlatform()
      : huxerui::Runtime([this](std::function<void()> work) {
          std::lock_guard lock(mutex);
          pending.push_back(std::move(work));
        }) {}
  ~TestPlatform() override { UiWindow::Retire(); huxerui::Runtime::Retire(); }
  void Start(const Application& application) {
    InitializeApplication(application, LaunchActivation{}, ApplicationLifecycleState::Active);
    InitializeWindow(*this, application.root_factory, application.options.window,
                     std::nullopt, WindowLifecycleState::Active);
  }
  void OnRuntimeStopped() override {}
  void RequestFrameAt(double) override {}
  double Now() const noexcept override { return 0.0; }
  FontMetrics Metrics(const Font&) override { return {}; }
  TextRunMetrics MeasureRun(std::string_view, const TextStyle&, const TextShapingOptions&) override { return {}; }
  TextLayoutMetrics MeasureText(const AttributedText&, const TextStyle&, float, const TextLayoutOptions&) override {
    return {};
  }
  void Drain(UiWindow& runtime) {
    for (int turns = 0; turns < 30; ++turns) {
      while (RunOne()) {
      }
      runtime.BuildFrame();
      std::lock_guard lock(mutex);
      if (pending.empty())
        return;
    }
    throw std::runtime_error("UI dispatch did not settle.");
  }

  bool RunOne() {
    std::function<void()> work;
    {
      std::lock_guard lock(mutex);
      if (pending.empty())
        return false;
      work = std::move(pending.front());
      pending.pop_front();
    }
    work();
    return true;
  }

  void Await(UiWindow& runtime, const std::function<bool()>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (!done() && std::chrono::steady_clock::now() < deadline) {
      Drain(runtime);
      std::this_thread::sleep_for(std::chrono::milliseconds{1});
    }
    Check(done(), "Asynchronous photo operation did not complete.");
  }

  std::mutex mutex;
  std::deque<std::function<void()>> pending;
};

class TestBackend final : public camera::detail::CameraBackend {
public:
  int starts = 0;
  int stops = 0;
  void Start(std::optional<Facing>, std::function<void(CameraStatus)>) override { ++starts; }
  void Stop(std::function<void()> completed) override {
    ++stops;
    if (completed) {
      completed();
    }
  }
};

State<bool> visible;
State<CameraOptions> requested;
std::optional<CameraSession> retained_session;
std::vector<std::shared_ptr<TestBackend>> backends;

[[huxerui::composable]]
View TestOwner() {
  requested = UseState(CameraOptions{.active = false});
  auto session = UseCamera(requested.Get());
  retained_session = session;
  return Column {
    CameraPreview(session).With(Frame{.width = 80.0F, .height = 60.0F}),
    CameraPreview(session).With(Frame{.width = 80.0F, .height = 60.0F}),
  };
}

View TestRoot() {
  visible = UseState(true);
  return visible.Get() ? TestOwner() : Canvas([](PaintContext&, Size) {});
}

void TestSessionLifetime() {
  Application application(TestRoot, {.show_debug_overlay = false, .application_hooks = {
    [](ApplicationContext& context) {
      context.RegisterPlatformModule<std::shared_ptr<camera::detail::CameraBackend>>(
          camera::detail::camera_module_name, [](UiWindow&) -> std::shared_ptr<camera::detail::CameraBackend> {
            auto backend = std::make_shared<TestBackend>();
            backends.push_back(backend);
            return backend;
          }
      );
    },
  }});
  {
    TestPlatform platform;
    platform.Start(application);
    UiWindow& runtime = platform;
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(backends.size() == 1, "Two previews must share one backend.");
    Check(retained_session->Status().state == SessionState::Stopped, "Inactive session must start stopped.");
    for (int quality : {0, 101}) {
      bool rejected = false;
      try {
        (void)retained_session->CapturePhotoAsync({.jpeg_quality = quality});
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      Check(rejected, "Invalid JPEG quality must throw before a task is returned.");
    }
    requested = CameraOptions{.active = true};
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(retained_session->Status().state == SessionState::Failed, "Unavailable permission must fail.");
    Check(retained_session->Status().error->code == CameraErrorCode::Unavailable, "Permission capability error lost.");
    Check(backends.front()->starts == 0, "Unauthorized capture must not start.");
    retained_session->Retry();
    platform.Drain(runtime);
    Check(backends.front()->starts == 0, "Retry must not bypass authorization.");
    requested = CameraOptions{.active = false};
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(retained_session->Status().state == SessionState::Stopped, "Stop must clear a previous error.");
    visible = false;
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(retained_session->Status().state == SessionState::Closed, "Retained handles must close on owner unmount.");
    Check(backends.front()->stops == 1, "Owner unmount must release the backend exactly once.");
    retained_session->Retry();
    Check(retained_session->Status().state == SessionState::Closed, "Retry must not reopen a closed owner.");
  }
  backends.clear();
  retained_session.reset();
}

class PhotoBackend final : public camera::detail::CameraBackend {
public:
  std::function<void(camera::CameraResult<ImageAsset>)> pending;
  std::function<void()> stopped;
  camera::PhotoOptions options;
  int captures = 0;

  void Start(std::optional<camera::Facing>, std::function<void(camera::CameraStatus)>) override {}
  void Stop(std::function<void()> completed) override { stopped = std::move(completed); }
  void CapturePhoto(camera::PhotoOptions value,
                    std::function<void(camera::CameraResult<ImageAsset>)> completed) override {
    ++captures;
    options = value;
    pending = std::move(completed);
  }
  void Complete(camera::CameraResult<ImageAsset> result) {
    auto completed = std::exchange(pending, {});
    completed(std::move(result));
    if (auto stop = std::exchange(stopped, {}))
      stop();
  }
};

struct PhotoTestContext {
  std::shared_ptr<camera::detail::CameraSessionData> owner;
  std::shared_ptr<PhotoBackend> backend = std::make_shared<PhotoBackend>();
  std::optional<TaskScope> callers;
  State<bool> visible;
  Facing facing = Facing::Front;
};

[[huxerui::composable]]
View PhotoTestOwner(PhotoTestContext* context) {
  auto status = UseState(CameraStatus{.state = SessionState::Running, .actual_facing = context->facing});
  auto tasks = UseTaskScope();
  auto retained = UseState(std::make_shared<camera::detail::CameraSessionData>(status, tasks, UseApplication()));
  context->owner = retained.Get();
  Lifecycle([owner = context->owner, backend = context->backend] {
    owner->backend = backend;
    owner->engaged = true;
    return [owner] { owner->Close(); };
  }, context->owner);
  return Canvas([](PaintContext&, Size) {});
}

PhotoTestContext* active_photo_test = nullptr;

[[huxerui::composable]]
View PhotoTestContent(PhotoTestContext* context) {
  context->callers = UseTaskScope();
  context->visible = UseState(true);
  return context->visible.Get() ? PhotoTestOwner(context) : View{};
}

View PhotoTestRoot() { return PhotoTestContent(active_photo_test); }

class PhotoTestFixture {
public:
  PhotoTestContext context;
  Application application;
  TestPlatform platform;
  UiWindow& runtime = platform;

  explicit PhotoTestFixture(Facing facing = Facing::Front)
      : application(PhotoTestRoot, {.show_debug_overlay = false}) {
    Check(!active_photo_test, "Photo fixtures must not overlap.");
    active_photo_test = &context;
    context.facing = facing;
    platform.Start(application);
    runtime.SetWindowMetrics({.viewport = {320.0F, 240.0F}});
    Drain();
  }

  ~PhotoTestFixture() { active_photo_test = nullptr; }

  TaskHandle Launch(std::optional<CameraResult<ImageAsset>>& result, PhotoOptions options = {}) {
    return context.callers->Launch([&result, options, owner = context.owner]() -> Task<void> {
      result = co_await camera::detail::CapturePhoto(owner, options);
    });
  }

  void Drain() { platform.Drain(runtime); }
  void Await(const std::optional<CameraResult<ImageAsset>>& result) {
    platform.Await(runtime, [&] { return result.has_value(); });
  }
};

void TestPhotoRequests() {
  {
    PhotoTestFixture fixture;
    const auto& owner = fixture.context.owner;
    const auto& backend = fixture.context.backend;
    std::optional<CameraResult<ImageAsset>> first;
    std::optional<CameraResult<ImageAsset>> second;
    auto handle = fixture.Launch(first, {.jpeg_quality = 82, .mirror = MirrorMode::Auto});
    fixture.Drain();
    Check(owner->status->capturing_photo, "Accepted photo must publish busy state.");
    Check(backend->options.mirror == MirrorMode::On, "Auto photo mirror must lock actual front facing.");
    Check(backend->options.jpeg_quality == 82, "Photo quality must reach the native backend.");
    (void)fixture.Launch(second);
    fixture.Await(second);
    Check(second->Error().code == CameraErrorCode::OperationInProgress, "Concurrent photos must be rejected.");
    Check(backend->captures == 1, "Rejected photo must not reach the backend.");
    handle.Cancel();
    fixture.Drain();
    Check(owner->status->capturing_photo, "Canceling the caller must not release native capture ownership.");
    backend->Complete(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Injected capture failure."}));
    fixture.Drain();
    Check(!first && !owner->status->capturing_photo, "Canceled caller must not receive a late callback.");
    Check(owner->status->state == SessionState::Running && !owner->status->error,
          "Single-photo errors must not fail the preview session.");
  }
  {
    PhotoTestFixture fixture;
    std::optional<CameraResult<ImageAsset>> result;
    (void)fixture.Launch(result);
    fixture.Drain();
    fixture.context.backend->Complete(
        CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Injected encoding failure."}));
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::CaptureFailed, "Photo error must reach its caller.");
    Check(fixture.context.owner->status->state == SessionState::Running && !fixture.context.owner->status->error,
          "An encoding failure must leave preview running.");
  }
  {
    PhotoTestFixture fixture;
    const auto& owner = fixture.context.owner;
    std::optional<CameraResult<ImageAsset>> result;
    (void)fixture.Launch(result);
    fixture.Drain();
    owner->Update({.active = false}, false);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::Interrupted, "Stopping must interrupt the photo task.");
    Check(owner->status->state == SessionState::Stopping, "Stop must wait for native photo cleanup.");
    Check(owner->status->capturing_photo, "Native cleanup remains busy while stopping.");
    fixture.context.backend->Complete(
        CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Late native failure."}));
    fixture.Drain();
    Check(result->Error().code == CameraErrorCode::Interrupted, "Late callbacks must not replace interruption.");
    Check(owner->status->state == SessionState::Stopped, "Native completion must settle stop.");
    result.reset();
    (void)fixture.Launch(result);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::NotReady, "Stopped camera must reject photos.");
  }
  const ImageAsset image = PhotoFixture();
  ImageAsset retained_photo;
  {
    PhotoTestFixture fixture(Facing::Back);
    std::optional<CameraResult<ImageAsset>> result;
    (void)fixture.Launch(result, {.mirror = MirrorMode::Auto});
    fixture.Drain();
    Check(fixture.context.backend->options.mirror == MirrorMode::Off,
          "Auto photo mirror must leave rear cameras unmirrored.");
    std::thread producer([backend = fixture.context.backend, image] {
      backend->Complete(CameraResult<ImageAsset>::Success(image));
    });
    producer.join();
    fixture.Await(result);
    Check(result->Succeeded() && result->Value().PixelWidth() == 2 && result->Value().PixelHeight() == 1,
          "Photo image dimensions must survive the async result.");
    retained_photo = result->Value();
  }
  Check(retained_photo.EncodedBytes().size() == image.EncodedBytes().size(), "Photo bytes must outlive the camera.");
  for (bool background : {false, true}) {
    PhotoTestFixture fixture;
    const auto& owner = fixture.context.owner;
    std::optional<CameraResult<ImageAsset>> result;
    (void)fixture.Launch(result);
    fixture.Drain();
    Check(owner->engaged && !owner->failure && owner->desired.facing == owner->opened_facing,
          "Interruption scenarios must start with a healthy, matching camera configuration.");
    auto options = CameraOptions{};
    if (!background)
      options.facing = Facing::Back;
    owner->Update(options, background);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::Interrupted, "Switching and background must interrupt photos.");
    Check(owner->status->state == SessionState::Stopping, "Interruption must wait for native cleanup.");
    fixture.context.backend->Complete(CameraResult<ImageAsset>::Success(image));
    fixture.Drain();
    Check(!owner->status->capturing_photo, "Late successful capture must release busy state.");
    if (background)
      Check(owner->status->state == SessionState::Suspended && !owner->failure,
            "Background suspension must not be caused by an unrelated session failure.");
  }
  {
    PhotoTestFixture fixture;
    const auto owner = fixture.context.owner;
    std::optional<CameraResult<ImageAsset>> result;
    (void)fixture.Launch(result);
    fixture.Drain();
    fixture.context.visible = false;
    fixture.Drain();
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::Interrupted, "Owner unmount must resolve an external caller.");
    Check(!owner->backend, "Owner unmount must release its native backend reference.");
    fixture.context.backend->Complete(CameraResult<ImageAsset>::Success(image));
    fixture.Drain();
    Check(owner->status->state == SessionState::Closed, "Late capture must not reopen an unmounted owner.");
    result.reset();
    (void)fixture.Launch(result);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::NotReady, "Closed camera must reject photos.");
  }
}

} // namespace

int main() {
  try {
    TestGeometry();
    TestSessionLifetime();
    TestPhotoRequests();
    std::cout << "Camera geometry, session lifetime, and photo request tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
