#include "camera_session.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <utility>

#include <huxerui/app.h>
#include <huxerui/lifecycle.h>
#include <huxerui/state.h>
#include <huxerui/system.h>
#include <huxerui/task.h>

#include "camera_internal.h"

namespace huxerui::camera::detail {

namespace {

bool ValidZoom(const ZoomState& zoom) {
  return zoom.min_ratio > 0.0F && zoom.min_ratio < zoom.max_ratio && std::isfinite(zoom.max_ratio) &&
      zoom.ratio >= zoom.min_ratio && zoom.ratio <= zoom.max_ratio;
}

} // namespace

struct ZoomRequest {
  float ratio;
  CallbackCompletion<CameraResult<void>> completion;
};

struct PhotoRequest {
  std::mutex mutex;
  std::condition_variable ready;
  std::optional<CameraResult<ImageAsset>> result;

  void Complete(CameraResult<ImageAsset> value) {
    {
      std::lock_guard lock(mutex);
      if (result)
        return;
      result = std::move(value);
    }
    ready.notify_all();
  }

  CameraResult<ImageAsset> Wait() {
    std::unique_lock lock(mutex);
    ready.wait(lock, [this] { return result.has_value(); });
    return *result;
  }
};

CameraSessionData::CameraSessionData(State<CameraStatus> value, TaskScope scope, ApplicationHandle app)
    : status(value), tasks(scope), application(app) {}

void CameraSessionData::InterruptZoom() {
  ++zoom_generation;
  auto active = std::exchange(active_zoom, {});
  auto pending = std::exchange(pending_zoom, {});
  for (const auto& request : {active, pending}) {
    if (request) request->completion.Complete(
        CameraResult<void>::Failure({CameraErrorCode::Interrupted, "Camera zoom was interrupted."}));
  }
}

void CameraSessionData::ReadZoom() {
  const auto update = [weak = weak_from_this(), scope = tasks, token = zoom_generation](std::optional<ZoomState> zoom) {
    if (!zoom || !ValidZoom(*zoom)) return;
    scope.Post([weak, token, zoom] {
      if (auto owner = weak.lock(); owner && token == owner->zoom_generation &&
          owner->status->state == SessionState::Running) {
        CameraStatus value = owner->status;
        value.zoom = zoom;
        owner->status = std::move(value);
      }
    });
  };
  try {
    backend->ReadZoom(update);
  } catch (const std::exception&) {
    // A failed control query leaves the capability unavailable.
  }
}

void CameraSessionData::ApplyZoom() {
  if (active_zoom || !pending_zoom) return;
  active_zoom = std::exchange(pending_zoom, {});
  const auto request = active_zoom;
  const auto& zoom = *status->zoom;
  const auto ratio = std::clamp(request->ratio, zoom.min_ratio, zoom.max_ratio);
  auto complete = [weak = weak_from_this(), request, scope = tasks](CameraResult<ZoomState> result) {
    scope.Post([weak, request, result = std::move(result)]() mutable {
      auto owner = weak.lock();
      if (!owner || owner->active_zoom != request) return;
      owner->active_zoom.reset();
      if (!result.Succeeded()) {
        request->completion.Complete(CameraResult<void>::Failure(result.Error()));
      } else if (!ValidZoom(result.Value())) {
        request->completion.Complete(CameraResult<void>::Failure(
            {CameraErrorCode::CaptureFailed, "The camera returned an invalid zoom range or ratio."}));
      } else {
        CameraStatus value = owner->status;
        value.zoom = result.Value();
        owner->status = std::move(value);
        request->completion.Complete(CameraResult<void>::Success());
      }
      owner->ApplyZoom();
    });
  };
  try {
    backend->SetZoomRatio(ratio, complete);
  } catch (const std::exception& error) {
    complete(CameraResult<ZoomState>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
  }
}

void CameraSessionData::InterruptPhoto() {
  if (photo) {
    photo->Complete(
        CameraResult<ImageAsset>::Failure({CameraErrorCode::Interrupted, "Photo capture was interrupted."}));
  }
}

void CameraSessionData::BeginPhoto(PhotoOptions options, const std::shared_ptr<PhotoRequest>& request) {
  photo = request;
  CameraStatus value = status;
  value.capturing_photo = true;
  status = value;
  options.mirror =
      options.mirror == MirrorMode::On || (options.mirror == MirrorMode::Auto && value.actual_facing == Facing::Front)
          ? MirrorMode::On
          : MirrorMode::Off;
  auto weak = weak_from_this();
  auto complete = [weak, request, scope = tasks](CameraResult<ImageAsset> result) {
    scope.Post([weak, request, result = std::move(result)]() mutable {
      auto owner = weak.lock();
      if (!owner || owner->photo != request)
        return;
      owner->photo.reset();
      CameraStatus value = owner->status;
      value.capturing_photo = false;
      owner->status = std::move(value);
      request->Complete(std::move(result));
    });
  };
  try {
    backend->CapturePhoto(options, complete);
  } catch (const std::exception& error) {
    complete(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
  }
}

void CameraSessionData::Publish(SessionState state) {
  status = CameraStatus{.state = state,
                        .error = state == SessionState::Failed ? failure : std::nullopt,
                        .capturing_photo = photo && state != SessionState::Closed,
                        .recording_video = recording && state != SessionState::Closed};
}

void CameraSessionData::Update(CameraOptions options, bool is_background) {
  if (closed) {
    return;
  }
  if (options != desired || (background && !is_background)) {
    failure.reset();
  }
  desired = options;
  background = is_background;
  Reconcile();
}

void CameraSessionData::Reconcile() {
  if (closed || !backend || status->state == SessionState::Stopping) {
    return;
  }
  if (desired.active && !background && !failure &&
      ((desired.output_pixel_width == 0) != (desired.output_pixel_height == 0))) {
    failure = CameraError{CameraErrorCode::ConfigurationFailed, "Specify both camera output dimensions."};
  }
  const bool configuration_changed = desired != opened;
  if (!desired.active || background || failure ||
      ((engaged || status->state == SessionState::Starting) && configuration_changed)) {
    const auto stopping_generation = ++generation;
    permission_check.Cancel();
    InterruptPhoto();
    InterruptZoom();
    StopRecording();
    if (engaged) {
      engaged = false;
      Publish(SessionState::Stopping);
      const auto weak = weak_from_this();
      backend->Stop([weak, scope = tasks, stopping_generation] {
        scope.Post([weak, stopping_generation] {
          if (auto self = weak.lock(); self && !self->closed && self->generation == stopping_generation) {
            self->Settle();
            self->Reconcile();
          }
        });
      });
    } else {
      Settle();
      if (desired.active && !background && !failure) {
        Reconcile();
      }
    }
    return;
  }
  if (engaged || status->state == SessionState::Starting) {
    return;
  }
  opened = desired;
  Publish(SessionState::Starting);
  const auto opening_generation = ++generation;
  const auto weak = weak_from_this();
  permission_check = tasks.Launch([weak, app = application, opening_generation]() -> Task<void> {
    try {
      const auto permission = co_await app.CheckPermissionAsync(Permission::Camera);
      auto self = weak.lock();
      if (!self || self->closed || self->generation != opening_generation) {
        co_return;
      }
      if (permission != PermissionStatus::Granted) {
        CameraErrorCode code = CameraErrorCode::PermissionDenied;
        if (permission == PermissionStatus::NotDetermined) {
          code = CameraErrorCode::PermissionRequired;
        } else if (permission == PermissionStatus::Unavailable) {
          code = CameraErrorCode::Unavailable;
        }
        self->failure = CameraError{code, "Camera access is not authorized or available."};
        self->Settle();
        co_return;
      }
      self->frames = std::make_shared<FramePipeline>(self->opened);
      self->backend->SetFramePipeline(self->frames);
      self->engaged = true;
      self->backend->Start(self->opened.facing, [weak, scope = self->tasks, opening_generation](CameraStatus value) {
        scope.Post([weak, opening_generation, value = std::move(value)]() mutable {
          auto owner = weak.lock();
          if (!owner || owner->closed || owner->generation != opening_generation) {
            return;
          }
          if (value.error) {
            owner->failure = std::move(value.error);
            owner->Reconcile();
          } else {
            const auto previous = owner->status->state;
            if (value.state == SessionState::Suspended) {
              owner->InterruptPhoto();
              owner->InterruptZoom();
              owner->StopRecording();
            }
            value.capturing_photo = static_cast<bool>(owner->photo);
            value.recording_video = static_cast<bool>(owner->recording);
            value.recording_paused = value.recording_video && owner->status->recording_paused;
            if (value.state == SessionState::Running && previous == SessionState::Running) {
              value.zoom = owner->status->zoom;
            }
            const bool read_zoom = value.state == SessionState::Running && previous != SessionState::Running;
            owner->status = std::move(value);
            if (read_zoom) owner->ReadZoom();
          }
        });
      });
    } catch (const std::exception& error) {
      if (auto self = weak.lock(); self && !self->closed && self->generation == opening_generation) {
        self->failure = CameraError{CameraErrorCode::CaptureFailed, error.what()};
        self->Reconcile();
      }
    }
  });
}

void CameraSessionData::Settle() {
  if (backend) backend->SetFramePipeline({});
  frames.reset();
  opened = {};
  Publish(failure ? SessionState::Failed
                  : (desired.active && background ? SessionState::Suspended : SessionState::Stopped));
}

void CameraSessionData::Close() {
  if (closed) {
    return;
  }
  closed = true;
  ++generation;
  permission_check.Cancel();
  InterruptPhoto();
  InterruptZoom();
  StopRecording();
  Publish(SessionState::Closed);
  if (backend) {
    backend->Stop({});
    backend.reset();
  }
  frames.reset();
  opened = {};
  desired.frame_processor_factory.reset();
}

Task<CameraResult<ImageAsset>> CapturePhoto(std::shared_ptr<CameraSessionData> owner, PhotoOptions options) {
  if (owner->closed || !owner->engaged || !owner->desired.active || owner->background ||
      owner->status->state != SessionState::Running) {
    co_return CameraResult<ImageAsset>::Failure({CameraErrorCode::NotReady, "Start the camera before taking a photo."});
  }
  if (owner->photo) {
    co_return CameraResult<ImageAsset>::Failure(
        {CameraErrorCode::OperationInProgress, "A photo capture is already in progress."});
  }
  auto request = std::make_shared<PhotoRequest>();
  owner->BeginPhoto(options, request);
  // Let the SDK own coroutine resumption and cancellation; platform callbacks never resume a frame directly.
  co_return co_await RunWorker([request] { return request->Wait(); });
}

Task<CameraResult<void>> SetZoomRatio(std::shared_ptr<CameraSessionData> owner, float ratio) {
  return AwaitCallback<CameraResult<void>>(
      [owner = std::move(owner), ratio](CallbackCompletion<CameraResult<void>> complete) -> std::function<void()> {
        if (owner->closed || !owner->engaged || owner->status->state != SessionState::Running) {
          complete.Complete(CameraResult<void>::Failure(
              {CameraErrorCode::NotReady, "Start the camera before adjusting zoom."}));
          return {};
        }
        if (!owner->status->zoom) {
          complete.Complete(CameraResult<void>::Failure(
              {CameraErrorCode::Unavailable, "Camera zoom is unavailable."}));
          return {};
        }
        auto request = std::make_shared<ZoomRequest>(ZoomRequest{ratio, complete});
        auto pending = std::exchange(owner->pending_zoom, request);
        auto active = owner->backend->CanReplaceZoomRequest() ? std::exchange(owner->active_zoom, {}) : nullptr;
        for (const auto& superseded : {pending, active}) {
          if (superseded) superseded->completion.Complete(CameraResult<void>::Failure(
              {CameraErrorCode::Interrupted, "A newer zoom request superseded this adjustment."}));
        }
        owner->ApplyZoom();
        return [weak = owner->weak_from_this(), request] {
          if (auto owner = weak.lock(); owner && owner->pending_zoom == request) owner->pending_zoom.reset();
        };
      });
}

} // namespace huxerui::camera::detail

namespace huxerui::camera {

CameraSession::CameraSession(std::shared_ptr<detail::CameraSessionData> data) : data_(std::move(data)) {}

CameraStatus CameraSession::Status() const {
  return data_->status;
}

void CameraSession::Retry() const {
  if (!data_->closed && data_->status->state == SessionState::Failed) {
    data_->failure.reset();
    data_->Reconcile();
  }
}

Task<CameraResult<void>> CameraSession::SetZoomRatioAsync(float ratio) const {
  if (!std::isfinite(ratio) || ratio <= 0.0F) {
    throw std::invalid_argument("Camera zoom requires a positive finite ratio.");
  }
  return detail::SetZoomRatio(data_, ratio);
}

Task<CameraResult<ImageAsset>> CameraSession::CapturePhotoAsync(PhotoOptions options) const {
  if (options.jpeg_quality < 1 || options.jpeg_quality > 100 ||
      (options.mirror != MirrorMode::Auto && options.mirror != MirrorMode::Off && options.mirror != MirrorMode::On)) {
    throw std::invalid_argument("Invalid photo quality or mirror mode.");
  }
  return detail::CapturePhoto(data_, options);
}

Task<CameraResult<void>> CameraSession::RecordVideoAsync(
    File destination, std::optional<audio::AudioCaptureSession> microphone) const {
  return detail::RecordVideo(data_, std::move(destination), std::move(microphone));
}

Task<CameraResult<void>> CameraSession::RecordVideoAsync(
    RandomAccessStream destination, std::optional<audio::AudioCaptureSession> microphone) const {
  return detail::RecordVideo(data_, std::move(destination), std::move(microphone));
}

Task<CameraResult<void>> CameraSession::RecordVideoAsync(
    OutputStream destination, std::optional<audio::AudioCaptureSession> microphone) const {
  return detail::RecordVideo(data_, std::move(destination), std::move(microphone));
}

std::chrono::microseconds CameraSession::RecordedDuration() const noexcept { return data_->RecordedDuration(); }

void CameraSession::PauseRecording() const { data_->SetRecordingPaused(true); }

void CameraSession::ResumeRecording() const { data_->SetRecordingPaused(false); }

void CameraSession::StopRecording() const { data_->StopRecording(); }

CameraSession UseCamera(CameraOptions options, const std::source_location& location) {
  auto status = UseState(CameraStatus{}, location);
  auto tasks = UseTaskScope();
  auto application = UseApplication();
  auto retained = UseState(std::make_shared<detail::CameraSessionData>(status, tasks, application), location);
  std::shared_ptr<detail::CameraSessionData> data = retained;
  const bool background = application.LifecycleState() == ApplicationLifecycleState::Background;
  Lifecycle([data] {
    data->backend = OpenPlatformModule<std::shared_ptr<detail::CameraBackend>>(detail::camera_module_name);
    return [data] { data->Close(); };
  }, data);
  Lifecycle([data, options, background] {
    data->Update(options, background);
    return [] {};
  }, data, options, background);
  return CameraSession(data);
}

} // namespace huxerui::camera
