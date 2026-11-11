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

struct CameraSessionData : std::enable_shared_from_this<CameraSessionData> {
  State<CameraStatus> status;
  TaskScope tasks;
  ApplicationHandle application;
  std::shared_ptr<CameraBackend> backend;
  CameraOptions desired;
  std::optional<Facing> opened_facing;
  std::shared_ptr<const FrameProcessor> opened_processor;
  std::shared_ptr<FramePipeline> frames;
  std::shared_ptr<VideoRecording> recording;
  std::optional<CameraError> failure;
  TaskHandle permission_check;
  std::uint64_t generation = 0;
  bool background = false;
  bool engaged = false;
  bool closed = false;
  std::shared_ptr<PhotoRequest> photo;

  CameraSessionData(State<CameraStatus> value, TaskScope scope, ApplicationHandle app);
  void InterruptPhoto();
  void StopRecording();
  void BeginPhoto(PhotoOptions options, const std::shared_ptr<PhotoRequest>& request);
  void Publish(SessionState state);
  void Update(CameraOptions options, bool is_background);
  void Reconcile();
  void Settle();
  void Close();
};

Task<CameraResult<ImageAsset>> CapturePhoto(std::shared_ptr<CameraSessionData> owner, PhotoOptions options);
Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, File destination);
Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, RandomAccessStream destination);
Task<CameraResult<void>> RecordVideo(std::shared_ptr<CameraSessionData> owner, OutputStream destination);

} // namespace huxerui::camera::detail
