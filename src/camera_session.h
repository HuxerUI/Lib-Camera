#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <huxerui/app.h>
#include <huxerui/camera.h>
#include <huxerui/state.h>
#include <huxerui/task.h>

namespace huxerui::camera::detail {

class CameraBackend;
class FramePipeline;
class VideoRecording;
struct PhotoRequest;
struct CameraControlRequest;
struct FocusRequest;
struct TorchRequest;

struct CameraSessionData : std::enable_shared_from_this<CameraSessionData> {
  State<CameraStatus> status;
  TaskScope tasks;
  ApplicationHandle application;
  std::shared_ptr<CameraBackend> backend;
  CameraOptions desired;
  CameraOptions opened;
  Rect preview_source_crop;
  std::shared_ptr<FramePipeline> frames;
  std::shared_ptr<VideoRecording> recording;
  std::chrono::microseconds completed_recording_duration{};
  std::optional<CameraError> failure;
  TaskHandle permission_check;
  std::uint64_t generation = 0;
  bool background = false;
  bool engaged = false;
  bool closed = false;
  std::shared_ptr<PhotoRequest> photo;
  bool flash_photo_pending = false;
  std::uint64_t control_generation = 0;
  std::shared_ptr<CameraControlRequest> active_zoom;
  std::shared_ptr<CameraControlRequest> pending_zoom;
  std::shared_ptr<CameraControlRequest> active_exposure_compensation;
  std::shared_ptr<CameraControlRequest> pending_exposure_compensation;
  std::shared_ptr<FocusRequest> focus_request;
  std::shared_ptr<TorchRequest> torch_request;

  CameraSessionData(State<CameraStatus> value, TaskScope scope, ApplicationHandle app);
  void InterruptPhoto();
  void InterruptControls();
  void ReadZoom();
  void ApplyZoom();
  void ReadExposureCompensation();
  void ApplyExposureCompensation();
  void ObserveLighting();
  std::chrono::microseconds RecordedDuration() const noexcept;
  void SetRecordingPaused(bool paused);
  void StopRecording();
  void BeginPhoto(PhotoOptions options, const std::shared_ptr<PhotoRequest>& request);
  void Publish(SessionState state);
  void Update(CameraOptions options, bool is_background);
  void Reconcile();
  void Settle();
  void Close();
};

Task<CameraResult<ImageAsset>> CapturePhoto(std::shared_ptr<CameraSessionData> owner, PhotoOptions options);
Task<CameraResult<void>> SetZoomRatio(std::shared_ptr<CameraSessionData> owner, float ratio);
Task<CameraResult<void>> SetExposureCompensation(std::shared_ptr<CameraSessionData> owner, float ev);
Task<CameraResult<void>> SetTorchEnabled(std::shared_ptr<CameraSessionData> owner, bool enabled);
Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, File destination,
                                    std::optional<audio::AudioCaptureSession> microphone = std::nullopt);
Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, RandomAccessStream destination,
                                    std::optional<audio::AudioCaptureSession> microphone = std::nullopt);
Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, OutputStream destination,
                                    std::optional<audio::AudioCaptureSession> microphone = std::nullopt);

} // namespace huxerui::camera::detail
