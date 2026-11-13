#pragma once

#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <huxerui/huxerui.h>

#include "camera_internal.h"
#include "camera_session.h"

namespace huxerui::camera::test {

inline void Check(bool condition, const char* message) {
  if (!condition) {
    throw std::runtime_error(message);
  }
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

class PhotoBackend final : public camera::detail::CameraBackend {
public:
  std::function<void(camera::CameraResult<ImageAsset>)> pending;
  std::function<void()> stopped;
  std::function<void(camera::CameraResult<void>)> pending_torch;
  camera::PhotoOptions options;
  int captures = 0;

  void Start(std::optional<camera::Facing>, camera::detail::CapturePreferences, StatusCallback) override {}
  void Stop(std::function<void()> completed) override { stopped = std::move(completed); }
  void SetTorchEnabled(bool, std::function<void(camera::CameraResult<void>)> completed) override {
    pending_torch = std::move(completed);
  }
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
  View content;
};

class PhotoTestFixture {
public:
  PhotoTestContext context;
  Application application;
  TestPlatform platform;
  UiWindow& runtime = platform;

  explicit PhotoTestFixture(Facing facing = Facing::Front,
                            std::function<void(ApplicationContext&)> install = {}, View content = {});
  ~PhotoTestFixture();

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

} // namespace huxerui::camera::test
