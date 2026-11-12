#include <huxerui/huxerui.h>

#include <array>
#include <cmath>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <thread>
#include <stdexcept>

#if defined(HUXERUI_CAMERA_TEST_RECORDING)
#include <CoreVideo/CoreVideo.h>
#include <filesystem>
#include <huxerui/macos/external_texture.h>
#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>
#endif

#include "camera_internal.h"
#include "camera_session.h"
#include "photo_fixture.h"

#define CAMERA_DISCARD(expression) static_cast<void>(expression)

using namespace huxerui;
using namespace huxerui::camera;

namespace {

using huxerui::Point;
using huxerui::Rect;
using huxerui::Size;

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

void TestFrameProcessorFactory() {
  struct TestTexture final : ExternalTexture {
    TestTexture() : ExternalTexture({64.0F, 64.0F}) {}
  };
  const PreviewOutput input{.texture = std::make_shared<TestTexture>(), .rotation = PreviewRotation::R270};
  const auto timestamp = std::chrono::seconds(1);
  constexpr std::uint32_t width = 720;
  constexpr std::uint32_t height = 1280;
  std::array expected_dimensions{width, height};
  int factory_calls = 0;
  std::weak_ptr<int> last_instance;
  camera::detail::FramePipeline pipeline({.frame_processor_factory = std::make_shared<const FrameProcessorFactory>(
      [&](std::uint32_t pixel_width, std::uint32_t pixel_height) -> FrameProcessor {
        Check(pixel_width == expected_dimensions[0] && pixel_height == expected_dimensions[1],
              "The factory must receive the source texture's pixel dimensions.");
        ++factory_calls;
        auto count = std::make_shared<int>(0);
        last_instance = count;
        return [count = std::move(count)](const PreviewOutput& source, std::chrono::nanoseconds) {
          auto output = source;
          output.rotation = ++*count == 1 ? PreviewRotation::R90 : PreviewRotation::R180;
          return CameraResult<PreviewOutput>::Success(std::move(output));
        };
      })});
  camera::detail::FrameProducer first;
  Check(factory_calls == 0, "The factory must wait for a source frame.");
  auto output = pipeline.Process(first, input, timestamp, width, height);
  Check(output.Succeeded() && output.Value().rotation == PreviewRotation::R90 && factory_calls == 1,
        "The first frame must create and use the processor.");
  const auto first_instance = last_instance;
  output = pipeline.Process(first, input, timestamp, width, height);
  Check(output.Succeeded() && output.Value().rotation == PreviewRotation::R180 && factory_calls == 1,
        "Successive frames must retain the producer's processor state.");
  camera::detail::FrameProducer next;
  expected_dimensions = {height, width};
  output = pipeline.Process(next, input, timestamp, height, width);
  Check(output.Succeeded() && output.Value().rotation == PreviewRotation::R90 && factory_calls == 2,
        "A new producer must get independent processor state.");
  first.processor = {};
  Check(first_instance.expired() && !last_instance.expired(), "Retiring one producer must release its processor.");

  camera::detail::FramePipeline empty({.frame_processor_factory =
      std::make_shared<const FrameProcessorFactory>([](std::uint32_t, std::uint32_t) { return FrameProcessor{}; })});
  camera::detail::FrameProducer invalid;
  const auto missing = empty.Process(invalid, input, timestamp, width, height);
  Check(!missing.Succeeded() && missing.Error().code == CameraErrorCode::CaptureFailed,
        "An empty processor returned by the factory must fail capture.");
  camera::detail::FramePipeline throwing({.frame_processor_factory = std::make_shared<const FrameProcessorFactory>(
      [](std::uint32_t, std::uint32_t) -> FrameProcessor {
        throw std::runtime_error("Injected factory failure.");
      })});
  const auto failed = throwing.Process(invalid, input, timestamp, width, height);
  Check(!failed.Succeeded() && failed.Error().message == "Injected factory failure.",
        "The factory failure must reach the capture error.");
}

void TestOutputCrop() {
  using camera::detail::CropForOutput;
  const auto square = CropForOutput({0.125F, 0.25F, 0.75F, 0.5F}, 640, 480, 320, 320);
  Check(square == Rect{0.3125F, 0.25F, 0.375F, 0.5F}, "Square output must center-crop the source horizontally.");
  const auto wide = CropForOutput({-0.25F, 0.0F, 1.0F, 1.0F}, 640, 480, 640, 180);
  Check(wide == Rect{0.0F, 0.359375F, 0.75F, 0.28125F}, "Wide output must center-crop within the clipped source.");
}

void TestProducerSize() {
  using camera::detail::FramePipeline;
  CameraOptions options{.output_pixel_width = 360, .output_pixel_height = 480};
  FramePipeline direct(options);
  Check(direct.ProducerSize(720, 960) == std::pair{360u, 480u},
        "Uniform scaling can use the configured producer size.");
  Check(direct.ProducerSize(720, 1280) == std::pair{720u, 1280u},
        "A change in aspect ratio needs the original producer size.");
  options.frame_processor_factory = std::make_shared<const FrameProcessorFactory>();
  FramePipeline processed(options);
  Check(processed.ProducerSize(720, 960) == std::pair{720u, 960u},
        "A custom processor must receive the original producer size.");
  FramePipeline defaults(CameraOptions{});
  Check(defaults.ProducerSize(720, 960) == std::pair{720u, 960u},
        "The default output must use the original producer size.");
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
    Check(done(), "Asynchronous camera operation did not complete.");
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
  auto session = UseCamera(requested);
  retained_session = session;
  return Column {
    CameraPreview(session).With(Frame{.width = 80.0F, .height = 60.0F}),
    CameraPreview(session).With(Frame{.width = 80.0F, .height = 60.0F}),
  };
}

View TestRoot() {
  visible = UseState(true);
  return visible ? TestOwner() : Canvas([](PaintContext&, Size) {});
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
        CAMERA_DISCARD(retained_session->CapturePhotoAsync({.jpeg_quality = quality}));
      } catch (const std::invalid_argument&) {
        rejected = true;
      }
      Check(rejected, "Invalid JPEG quality must throw before a task is returned.");
    }
    requested = CameraOptions{.output_pixel_width = 640};
    runtime.BuildFrame();
    platform.Drain(runtime);
    Check(retained_session->Status().state == SessionState::Failed &&
              retained_session->Status().error->code == CameraErrorCode::ConfigurationFailed,
          "An incomplete output size must fail before opening capture.");
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
  context->owner = retained;
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
  return context->visible ? PhotoTestOwner(context) : View{};
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
    Check(backend->options.jpeg_quality == 82, "Photo quality must reach the platform backend.");
    fixture.Launch(second);
    fixture.Await(second);
    Check(second->Error().code == CameraErrorCode::OperationInProgress, "Concurrent photos must be rejected.");
    Check(backend->captures == 1, "Rejected photo must not reach the backend.");
    handle.Cancel();
    fixture.Drain();
    Check(owner->status->capturing_photo, "Canceling the caller must not release platform capture ownership.");
    backend->Complete(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Injected capture failure."}));
    fixture.Drain();
    Check(!first && !owner->status->capturing_photo, "Canceled caller must not receive a late callback.");
    Check(owner->status->state == SessionState::Running && !owner->status->error,
          "Single-photo errors must not fail the preview session.");
  }
  {
    PhotoTestFixture fixture;
    std::optional<CameraResult<ImageAsset>> result;
    fixture.Launch(result);
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
    fixture.Launch(result);
    fixture.Drain();
    owner->Update({.active = false}, false);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::Interrupted, "Stopping must interrupt the photo task.");
    Check(owner->status->state == SessionState::Stopping, "Stop must wait for platform photo cleanup.");
    Check(owner->status->capturing_photo, "Platform cleanup remains busy while stopping.");
    fixture.context.backend->Complete(
        CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, "Late platform failure."}));
    fixture.Drain();
    Check(result->Error().code == CameraErrorCode::Interrupted, "Late callbacks must not replace interruption.");
    Check(owner->status->state == SessionState::Stopped, "Platform completion must settle stop.");
    result.reset();
    fixture.Launch(result);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::NotReady, "Stopped camera must reject photos.");
  }
  const ImageAsset image = PhotoFixture();
  ImageAsset retained_photo;
  {
    PhotoTestFixture fixture(Facing::Back);
    std::optional<CameraResult<ImageAsset>> result;
    fixture.Launch(result, {.mirror = MirrorMode::Auto});
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
    fixture.Launch(result);
    fixture.Drain();
    Check(owner->engaged && !owner->failure && owner->desired.facing == owner->opened.facing,
          "Interruption scenarios must start with a healthy, matching camera configuration.");
    auto options = CameraOptions{};
    if (!background)
      options.facing = Facing::Back;
    owner->Update(options, background);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::Interrupted, "Switching and background must interrupt photos.");
    Check(owner->status->state == SessionState::Stopping, "Interruption must wait for platform cleanup.");
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
    fixture.Launch(result);
    fixture.Drain();
    fixture.context.visible = false;
    fixture.Drain();
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::Interrupted, "Owner unmount must resolve an external caller.");
    Check(!owner->backend, "Owner unmount must release its platform backend reference.");
    fixture.context.backend->Complete(CameraResult<ImageAsset>::Success(image));
    fixture.Drain();
    Check(owner->status->state == SessionState::Closed, "Late capture must not reopen an unmounted owner.");
    result.reset();
    fixture.Launch(result);
    fixture.Await(result);
    Check(result->Error().code == CameraErrorCode::NotReady, "Closed camera must reject photos.");
  }
}

#if defined(HUXERUI_CAMERA_TEST_RECORDING)
template <class T, class E> T TakeRecordingResult(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

std::shared_ptr<ExternalTexture> SolidTexture(std::uint8_t red, std::uint8_t green, int width = 64, int height = 64) {
  CVPixelBufferRef pixels = nullptr;
  Check(CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_32BGRA, nullptr, &pixels) == kCVReturnSuccess,
        "Cannot allocate a test camera frame.");
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
      owner(pixels, CVPixelBufferRelease);
  Check(CVPixelBufferLockBaseAddress(pixels, 0) == kCVReturnSuccess, "Cannot lock a test camera frame.");
  for (int row = 0; row < height; ++row) {
    auto* pixel = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels)) + row * CVPixelBufferGetBytesPerRow(pixels);
    for (int column = 0; column < width; ++column) {
      *pixel++ = 0;
      *pixel++ = green;
      *pixel++ = red;
      *pixel++ = 255;
    }
  }
  CVPixelBufferUnlockBaseAddress(pixels, 0);
  auto texture = std::make_shared<macos::PixelBufferTexture>(Size{static_cast<float>(width), static_cast<float>(height)});
  texture->Publish(pixels);
  return texture;
}

void TestRecordingPause(PhotoTestFixture& fixture, const File& file) {
  using namespace std::chrono_literals;
  using namespace huxerui::media;
  const auto owner = fixture.context.owner;
  owner->frames = std::make_shared<camera::detail::FramePipeline>(CameraOptions{});
  std::optional<CameraResult<void>> result;
  auto task = fixture.context.callers->Launch([owner, file, &result]() -> Task<void> {
    result = co_await camera::detail::RecordVideo(owner, file);
  });
  fixture.Drain();
  Check(owner->status->recording_video, "Recording did not start.");

  camera::detail::FrameProducer producer;
  owner->SetRecordingPaused(true);
  owner->SetRecordingPaused(true);
  Check(owner->status->recording_paused && owner->status->recording_video,
        "Pausing must keep the recording in progress.");
  const auto unpublished = std::make_shared<macos::PixelBufferTexture>(Size{64.0F, 64.0F});
  const auto preview = TakeRecordingResult(owner->frames->Process(producer, {.texture = unpublished}, 1s, 64, 64));
  Check(preview.texture == unpublished, "Preview must continue while recording is paused.");
  owner->SetRecordingPaused(false);
  Check(!owner->status->recording_paused, "Resuming must clear the pause state.");

  const auto input = SolidTexture(255, 0);
  std::chrono::microseconds timestamp = 17s;
  const auto capture_until = [&](std::chrono::microseconds duration, std::chrono::milliseconds step) {
    fixture.platform.Await(fixture.runtime, [&] {
      if (result || owner->RecordedDuration() > duration) return true;
      TakeRecordingResult(owner->frames->Process(producer, {.texture = input}, timestamp, 64, 64));
      timestamp += step;
      return false;
    });
    Check(!result, "Recording ended before it was stopped.");
  };
  capture_until(0us, 1ms);
  const auto before_pause = timestamp - 17s;
  owner->SetRecordingPaused(true);
  timestamp += 30s;
  owner->SetRecordingPaused(false);
  capture_until(before_pause, 10ms);

  owner->SetRecordingPaused(true);
  owner->StopRecording();
  owner->SetRecordingPaused(false);
  Check(!owner->status->recording_paused, "Stopping a paused recording must clear the pause state.");
  fixture.platform.Await(fixture.runtime, [&] { return result.has_value(); });
  TakeRecordingResult(std::move(*result));
  fixture.Drain();
  Check(!owner->status->recording_video, "A paused recording must finalize on stop.");

  const auto duration = owner->RecordedDuration();
  Check(duration > before_pause && duration < 30s, "The recording timeline must exclude the pause gap.");
  auto demuxer = TakeRecordingResult(MediaDemuxer::Open(TakeRecordingResult(file.OpenRandomAccess())));
  const auto written_duration = demuxer.Duration();
  Check(written_duration && *written_duration == MediaTimestamp{duration.count(), 1'000'000},
        "Reported duration must match the MP4 after resuming.");
}

constexpr std::uint32_t recording_width = 96;
constexpr std::uint32_t recording_height = 64;

template <class Destination>
std::shared_ptr<ExternalTexture> TestRecordedFrame(PhotoTestFixture& fixture, camera::detail::FrameProducer& producer,
    const File& file, Destination destination, media::ContainerLayout layout, bool unmount = false) {
  using namespace huxerui::media;
  std::optional<CameraResult<void>> result;
  const auto owner = fixture.context.owner;
  auto task = fixture.context.callers->Launch(
      [owner, &result, destination = std::move(destination)]() mutable -> Task<void> {
        result = co_await camera::detail::RecordVideo(owner, std::move(destination));
      });
  fixture.Drain();
  Check(owner->status->recording_video, "Recording did not start.");
  Check(owner->RecordedDuration().count() == 0, "A new recording must start at zero duration.");

  const auto input = SolidTexture(255, 0);
  const auto preview = TakeRecordingResult(
      owner->frames->Process(producer, {.texture = input}, std::chrono::seconds(17), 64, 64));
  Check(preview.rotation == PreviewRotation::R90 && preview.mirrored && preview.crop == Rect{0, 0, 1, 1},
        "Sizing must consume the processed crop and preserve rotation and mirroring.");
  const auto* texture = dynamic_cast<const macos::PixelBufferTexture*>(preview.texture.get());
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
      pixels(texture ? texture->AcquireFrame() : nullptr, CVPixelBufferRelease);
  Check(pixels && CVPixelBufferGetWidth(pixels.get()) == recording_width &&
            CVPixelBufferGetHeight(pixels.get()) == recording_height,
        "Preview texture must have the requested physical dimensions.");
  if (unmount) {
    fixture.context.visible = false;
    fixture.Drain();
  } else {
    owner->StopRecording();
  }
  fixture.platform.Await(fixture.runtime, [&] { return result.has_value(); });
  TakeRecordingResult(std::move(*result));
  fixture.Drain();
  Check(!owner->status->recording_video, "Recording remained busy after finalization.");

  auto demuxer = TakeRecordingResult(MediaDemuxer::Open(TakeRecordingResult(file.OpenRandomAccess())));
  Check(demuxer.Layout() == layout && demuxer.Tracks().size() == 1, "Unexpected recording layout or track count.");
  const auto& track = demuxer.Tracks().front();
  const auto& format = track.descriptions.front();
  Check(track.kind == TrackKind::Video && track.rotation == VideoRotation::Clockwise90 && format.codec == CodecId::H264,
        "Recording lost the processed frame geometry.");
  const auto visible = format.visible.value_or(VideoRectangle{0, 0, *format.coded_width, *format.coded_height});
  Check(visible.width == recording_width && visible.height == recording_height,
        "The visible video frame must have the requested preview texture dimensions.");
  auto decoder = TakeRecordingResult(VideoDecoder::Create({.initialization_data = format.initialization_data,
      .width = *format.coded_width, .height = *format.coded_height}));
  Check(TakeRecordingResult(demuxer.Read()).has_value(), "Missing video track configuration.");
  auto item = TakeRecordingResult(demuxer.Read());
  Check(item && std::holds_alternative<ContainerPacket>(*item), "Recording has no frame.");
  const auto& packet = std::get<ContainerPacket>(*item);
  Check(packet.pts && packet.pts->ticks == 0, "Recording did not start at a relative timestamp of zero.");
  const auto duration = owner->RecordedDuration();
  if (unmount) {
    Check(duration.count() == 0, "A closed session must report zero recorded duration.");
  } else {
    Check(packet.duration && MediaTimestamp{duration.count(), 1'000'000} == *packet.duration,
          "The final recorded duration must match the MP4 sample duration.");
  }
  fixture.platform.Await(fixture.runtime, [&] {
    return TakeRecordingResult(decoder.TrySubmit({packet.data, *packet.pts, packet.dts, packet.duration,
                                                 packet.sync_sample.value_or(false)})) == SubmitStatus::Accepted;
  });
  TakeRecordingResult(decoder.Finish());
  fixture.platform.Await(fixture.runtime, [&] {
    while (auto output = TakeRecordingResult(decoder.TryRead())) {
      if (auto* frame = std::get_if<VideoFrame>(&*output)) {
        const auto luma = std::to_integer<int>(frame->planes.front().data.AsBytes().front());
        Check(luma > 140, "The encoded video contains the red source frame instead of the green processed frame.");
        return true;
      }
    }
    return false;
  });
  return preview.texture;
}

void TestRecording() {
  struct Directory {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
        ("huxerui-camera-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~Directory() { std::error_code error; std::filesystem::remove_all(path, error); }
  } directory;
  Check(std::filesystem::create_directory(directory.path), "Cannot create the recording test directory.");
  int factory_calls = 0;
  PhotoTestFixture fixture;
  TestRecordingPause(fixture, File((directory.path / "paused.mp4").u8string()));
  auto factory = std::make_shared<const FrameProcessorFactory>([&](std::uint32_t width, std::uint32_t height) -> FrameProcessor {
    Check(width == 64 && height == 64, "The recording factory must receive the source frame size.");
    ++factory_calls;
    return [green = SolidTexture(0, 255, 128, 96)](const PreviewOutput&, std::chrono::nanoseconds timestamp) {
      Check(timestamp == std::chrono::seconds(17), "The frame processor received the wrong capture timestamp.");
      return CameraResult<PreviewOutput>::Success({.texture = green, .crop = {0.125F, 0.125F, 0.75F, 0.75F},
                                                  .rotation = PreviewRotation::R90, .mirrored = true});
    };
  });
  fixture.context.owner->frames = std::make_shared<camera::detail::FramePipeline>(
      CameraOptions{.frame_processor_factory = std::move(factory), .output_pixel_width = recording_width,
                    .output_pixel_height = recording_height});
  camera::detail::FrameProducer producer;
  const File file((directory.path / "file.mp4").u8string());
  const File seekable((directory.path / "seekable.mp4").u8string());
  const File sequential((directory.path / "sequential.mp4").u8string());
  const auto first = TestRecordedFrame(fixture, producer, file, file, media::ContainerLayout::Default);
  const auto second = TestRecordedFrame(fixture, producer, seekable,
      TakeRecordingResult(seekable.OpenRandomAccess(FileAccessMode::WriteOnly, FileOpenMode::CreateNew)),
      media::ContainerLayout::Default);
  const auto third = TestRecordedFrame(fixture, producer, sequential, TakeRecordingResult(sequential.OpenWrite()),
                                       media::ContainerLayout::Fragmented, true);
  Check(first == second && first == third, "Sized frames must reuse the preview texture identity.");
  Check(factory_calls == 1, "Recording starts and stops must share the producer's processor.");
}
#endif

} // namespace

int main() {
  try {
    TestGeometry();
    TestFrameProcessorFactory();
    TestOutputCrop();
    TestProducerSize();
    TestSessionLifetime();
    TestPhotoRequests();
#if defined(HUXERUI_CAMERA_TEST_RECORDING)
    TestRecording();
#endif
    std::cout << "Camera tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
