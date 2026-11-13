#include <huxerui/huxerui.h>

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <stdexcept>

#include "camera_test_fixture.h"
#include "photo_fixture.h"

#define CAMERA_DISCARD(expression) static_cast<void>(expression)

using namespace huxerui;
using namespace huxerui::camera;
using namespace huxerui::camera::test;

namespace {

using huxerui::Point;
using huxerui::Rect;
using huxerui::Size;

void Near(float actual, float expected) {
  Check(std::abs(actual - expected) < 0.001F, "Unexpected preview coordinate.");
}

void TestGeometry() {
  struct TestTexture final : ExternalTexture {
    explicit TestTexture(Size size) : ExternalTexture(size) {}
  };
  const Size source{400.0F, 200.0F};
  PreviewOutput output{.texture = std::make_shared<TestTexture>(source)};
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
      const auto metering = camera::detail::PreviewTexturePoint(output, {.fit = ImageFit::Contain}, Facing::Front,
          bounds, front->transform.Apply({100.0F, 120.0F}));
      Check(metering.has_value(), "A visible point must map to the preview texture.");
      Near(metering->x, 0.25F);
      Near(metering->y, 0.6F);
    }
    output.mirrored = false;
  }
  output.rotation = camera::PreviewRotation::R0;
  const auto contain = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Contain}, {}, {200.0F, 200.0F});
  Near(contain->transform.Apply({0.0F, 0.0F}).y, 50.0F);
  const auto cover = camera::detail::PlacePreview(source, output, {.fit = ImageFit::Cover}, {}, {200.0F, 200.0F});
  Near(cover->transform.Apply({0.0F, 0.0F}).x, -100.0F);
  Check(!camera::detail::PreviewTexturePoint(output, {.fit = ImageFit::Contain}, {}, {200.0F, 200.0F}, {40.0F, 20.0F}),
        "The letterbox must not produce a metering point.");
  const auto covered_point = camera::detail::PreviewTexturePoint(output, {.fit = ImageFit::Cover}, {},
      {200.0F, 200.0F}, {20.0F, 100.0F});
  Check(covered_point.has_value(), "A visible point in a cropped preview must map to the source.");
  Near(covered_point->x, 0.3F);
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
  Check(output.Succeeded() && output.Value().output.rotation == PreviewRotation::R90 && factory_calls == 1,
        "The first frame must create and use the processor.");
  const auto first_instance = last_instance;
  output = pipeline.Process(first, input, timestamp, width, height);
  Check(output.Succeeded() && output.Value().output.rotation == PreviewRotation::R180 && factory_calls == 1,
        "Successive frames must retain the producer's processor state.");
  camera::detail::FrameProducer next;
  expected_dimensions = {height, width};
  output = pipeline.Process(next, input, timestamp, height, width);
  Check(output.Succeeded() && output.Value().output.rotation == PreviewRotation::R90 && factory_calls == 2,
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

class TestBackend final : public camera::detail::CameraBackend {
public:
  int starts = 0;
  int stops = 0;
  void Start(std::optional<Facing>, StatusCallback) override { ++starts; }
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

void TestZoomRequests() {
  class ZoomBackend final : public camera::detail::CameraBackend {
  public:
    void Start(std::optional<Facing>, StatusCallback) override { ++starts; }
    void Stop(std::function<void()> completed) override {
      ++stops;
      if (completed) completed();
    }
    void ReadZoom(std::function<void(std::optional<Zoom>)> completed) override {
      completed(Zoom{.ratio = 1.0F, .min_ratio = 1.0F, .max_ratio = 4.0F});
    }
    bool CanReplaceZoomRequest() const noexcept override { return replace_active; }
    void SetZoomRatio(float ratio, std::function<void(CameraResult<Zoom>)> completed) override {
      if (completion) {
        Check(replace_active && !replaced, "Only a replaceable zoom adjustment may be superseded.");
        replaced = std::move(completion);
      }
      targets.push_back(ratio);
      completion = std::move(completed);
    }
    void Complete(float ratio) {
      auto complete = std::exchange(completion, {});
      complete(CameraResult<Zoom>::Success({.ratio = ratio, .min_ratio = 1.0F, .max_ratio = 4.0F}));
    }
    int starts = 0;
    int stops = 0;
    bool replace_active = false;
    std::vector<float> targets;
    std::function<void(CameraResult<Zoom>)> completion;
    std::function<void(CameraResult<Zoom>)> replaced;
  };

  PhotoTestFixture fixture;
  const auto owner = fixture.context.owner;
  auto backend = std::make_shared<ZoomBackend>();
  owner->backend = backend;
  owner->ReadZoom();
  fixture.Drain();
  const auto launch = [&](float ratio, std::optional<CameraResult<void>>& result) {
    return fixture.context.callers->Launch([owner, ratio, &result]() -> Task<void> {
      result = co_await camera::detail::SetZoomRatio(owner, ratio);
    });
  };

  std::optional<CameraResult<void>> first;
  std::optional<CameraResult<void>> superseded;
  std::optional<CameraResult<void>> last;
  launch(0.5F, first);
  fixture.Drain();
  launch(2.0F, superseded);
  launch(8.0F, last);
  fixture.Drain();
  Check(backend->targets == std::vector{1.0F}, "Zoom must clamp the lower bound and keep the newest pending request.");
  Check(superseded && superseded->Error().code == CameraErrorCode::Interrupted,
        "A superseded zoom request must complete with Interrupted.");

  backend->Complete(1.0F);
  fixture.Drain();
  Check(first && first->Succeeded() && backend->targets == std::vector{1.0F, 4.0F},
        "The latest pending zoom must be clamped and applied next.");
  backend->Complete(3.75F);
  fixture.Drain();
  Check(last && last->Succeeded() && owner->status->zoom && owner->status->zoom->ratio == 3.75F,
        "Zoom status must report the ratio confirmed by the device.");
  Check(backend->starts == 0 && backend->stops == 0, "Zoom must keep the current capture running.");

  backend->replace_active = true;
  std::optional<CameraResult<void>> replaced;
  std::optional<CameraResult<void>> latest;
  launch(2.0F, replaced);
  fixture.Drain();
  launch(3.0F, latest);
  fixture.Drain();
  Check(replaced && replaced->Error().code == CameraErrorCode::Interrupted && backend->targets.back() == 3.0F,
        "A replaceable backend must receive the latest zoom before the previous result arrives.");
  auto stale = std::exchange(backend->replaced, {});
  stale(CameraResult<Zoom>::Success({.ratio = 2.0F, .min_ratio = 1.0F, .max_ratio = 4.0F}));
  fixture.Drain();
  Check(owner->status->zoom && owner->status->zoom->ratio == 3.75F && !latest,
        "An obsolete result must not change the applied zoom or complete the latest request.");
  backend->Complete(3.0F);
  fixture.Drain();
  Check(latest && latest->Succeeded() && owner->status->zoom && owner->status->zoom->ratio == 3.0F,
        "The latest confirmed ratio must update the status.");

  std::optional<CameraResult<void>> interrupted;
  launch(2.0F, interrupted);
  fixture.Drain();
  owner->Close();
  fixture.Drain();
  Check(interrupted && interrupted->Error().code == CameraErrorCode::Interrupted,
        "Closing capture must interrupt its active zoom request.");
  backend->Complete(2.0F);
  fixture.Drain();
  Check(owner->status->state == SessionState::Closed && !owner->status->zoom,
        "A late zoom response must not restore closed camera state.");
}

void TestExposureCompensationRequests() {
  class ExposureBackend final : public camera::detail::CameraBackend {
  public:
    void Start(std::optional<Facing>, StatusCallback) override { ++starts; }
    void Stop(std::function<void()> completed) override {
      ++stops;
      if (completed) completed();
    }
    void ReadExposureCompensation(std::function<void(std::optional<ExposureCompensation>)> completed) override {
      completed({{.ev = 0.0F, .min_ev = -2.0F, .max_ev = 2.0F, .step_ev = 0.5F}});
    }
    void SetExposureCompensation(float ev, std::function<void(CameraResult<ExposureCompensation>)> completed) override {
      targets.push_back(ev);
      completion = std::move(completed);
    }
    void Complete(float ev) {
      auto complete = std::exchange(completion, {});
      complete(CameraResult<ExposureCompensation>::Success(
          {.ev = ev, .min_ev = -2.0F, .max_ev = 2.0F, .step_ev = 0.5F}));
    }

    int starts = 0;
    int stops = 0;
    std::vector<float> targets;
    std::function<void(CameraResult<ExposureCompensation>)> completion;
  };

  PhotoTestFixture fixture;
  const auto owner = fixture.context.owner;
  auto backend = std::make_shared<ExposureBackend>();
  owner->backend = backend;
  const auto launch = [&](float ev, std::optional<CameraResult<void>>& result) {
    fixture.context.callers->Launch([owner, ev, &result]() -> Task<void> {
      result = co_await camera::detail::SetExposureCompensation(owner, ev);
    });
    fixture.Drain();
  };

  std::optional<CameraResult<void>> unavailable;
  launch(0.0F, unavailable);
  Check(unavailable && unavailable->Error().code == CameraErrorCode::Unavailable,
        "Exposure compensation requires an available device range.");
  owner->ReadExposureCompensation();
  fixture.Drain();
  Check(owner->status->exposure_compensation && owner->status->exposure_compensation->step_ev == 0.5F,
        "The exposure compensation range includes the device step.");

  std::optional<CameraResult<void>> first;
  std::optional<CameraResult<void>> superseded;
  std::optional<CameraResult<void>> reset;
  launch(-4.0F, first);
  launch(0.8F, superseded);
  launch(0.0F, reset);
  Check(backend->targets == std::vector{-2.0F} && superseded &&
        superseded->Error().code == CameraErrorCode::Interrupted,
        "Exposure compensation must clamp the request and keep the newest pending value.");
  backend->Complete(-1.5F);
  fixture.Drain();
  Check(first && first->Succeeded() && owner->status->exposure_compensation->ev == -1.5F &&
        backend->targets == std::vector{-2.0F, 0.0F},
        "The confirmed EV must be published before applying the pending reset.");
  backend->Complete(0.0F);
  fixture.Drain();
  Check(reset && reset->Succeeded() && owner->status->exposure_compensation->ev == 0.0F &&
        backend->starts == 0 && backend->stops == 0, "Resetting EV must keep capture running.");

  std::optional<CameraResult<void>> interrupted;
  launch(1.0F, interrupted);
  owner->Close();
  backend->Complete(1.0F);
  fixture.Drain();
  Check(interrupted && interrupted->Error().code == CameraErrorCode::Interrupted &&
        !owner->status->exposure_compensation, "A late EV result must not restore a closed camera's capability.");
}

void TestFlashAndTorchRequests() {
  PhotoTestFixture fixture;
  const auto& owner = fixture.context.owner;
  const auto& backend = fixture.context.backend;
  auto status = owner->status.Get();
  status.photo_flash.supports_on = true;
  status.torch = Torch{.available = true};
  owner->status = status;
  const auto request_torch = [&](bool enabled, std::optional<CameraResult<void>>& result) {
    fixture.context.callers->Launch([owner, enabled, &result]() -> Task<void> {
      result = co_await camera::detail::SetTorchEnabled(owner, enabled);
    });
    fixture.Drain();
  };

  std::optional<CameraResult<ImageAsset>> photo;
  fixture.Launch(photo, {.flash_mode = FlashMode::Auto});
  fixture.Await(photo);
  Check(photo->Error().code == CameraErrorCode::Unavailable && backend->captures == 0,
        "An unsupported flash mode must not reach the camera.");

  std::optional<CameraResult<void>> torch;
  request_torch(true, torch);
  Check(backend->pending_torch && !torch, "The torch task must wait for platform completion.");
  photo.reset();
  fixture.Launch(photo, {.flash_mode = FlashMode::On});
  fixture.Await(photo);
  Check(photo->Error().code == CameraErrorCode::OperationInProgress && backend->captures == 0,
        "A pending torch adjustment must exclude flash photos.");
  status.torch->enabled = true;
  owner->status = status;
  std::exchange(backend->pending_torch, {})(CameraResult<void>::Success());
  fixture.Drain();
  Check(torch && torch->Succeeded(), "The torch task must report the platform result.");

  photo.reset();
  fixture.Launch(photo, {.flash_mode = FlashMode::On});
  fixture.Await(photo);
  Check(photo->Error().code == CameraErrorCode::Unavailable && backend->captures == 0,
        "The torch must be off before taking a flash photo.");
  photo.reset();
  fixture.Launch(photo);
  fixture.Drain();
  Check(backend->captures == 1 && backend->options.flash_mode == FlashMode::Off,
        "A photo with flash off can use the continuous light.");
  backend->Complete(CameraResult<ImageAsset>::Success(ImageAsset{}));
  fixture.Await(photo);

  status.torch->enabled = false;
  owner->status = status;
  photo.reset();
  fixture.Launch(photo, {.flash_mode = FlashMode::On});
  fixture.Drain();
  Check(backend->options.flash_mode == FlashMode::On, "The selected flash mode must reach the photo backend.");
  torch.reset();
  request_torch(true, torch);
  Check(torch && torch->Error().code == CameraErrorCode::OperationInProgress && !backend->pending_torch,
        "A pending flash photo must exclude torch adjustments.");
  backend->Complete(CameraResult<ImageAsset>::Success(ImageAsset{}));
  fixture.Await(photo);

  torch.reset();
  request_torch(true, torch);
  owner->Close();
  fixture.Drain();
  Check(torch && torch->Error().code == CameraErrorCode::Interrupted && !owner->status->torch,
        "Closing capture must interrupt an active torch adjustment and clear its status.");
  std::exchange(backend->pending_torch, {})(CameraResult<void>::Success());
  fixture.Drain();
  Check(!owner->status->torch, "A late torch result must not restore closed camera status.");
}

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

} // namespace

int main() {
  try {
    TestGeometry();
    TestFrameProcessorFactory();
    TestOutputCrop();
    TestProducerSize();
    TestSessionLifetime();
    TestZoomRequests();
    TestExposureCompensationRequests();
    TestFlashAndTorchRequests();
    TestPhotoRequests();
    std::cout << "Camera tests passed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
