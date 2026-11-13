#include <huxerui/camera.h>
#include <huxerui/platform_registry.h>
#include <huxerui/system.h>

#include "camera_internal.h"

namespace huxerui::camera::detail {

CameraError CameraPermissionError(PermissionStatus permission) {
  if (permission == PermissionStatus::NotDetermined) {
    return {CameraErrorCode::PermissionRequired, "Camera access has not been authorized."};
  }
  if (permission == PermissionStatus::Unavailable) {
    return {CameraErrorCode::Unavailable, "Camera access is unavailable."};
  }
  return {CameraErrorCode::PermissionDenied, "Camera access is denied or restricted."};
}

} // namespace huxerui::camera::detail

namespace huxerui::camera {

void Install(ApplicationContext& context) {
  detail::InstallPlatformCamera(context);
}

Task<CameraResult<std::vector<CameraDevice>>> EnumerateDevicesAsync() {
  try {
    const auto permission = co_await UseApplication().CheckPermissionAsync(Permission::Camera);
    if (permission != PermissionStatus::Granted) {
      co_return detail::CameraDevicesResult::Failure(detail::CameraPermissionError(permission));
    }
    const auto backend = OpenPlatformModule<std::shared_ptr<detail::CameraBackend>>(detail::camera_module_name);
    co_return co_await backend->EnumerateDevices();
  } catch (const std::exception& error) {
    co_return detail::CameraDevicesResult::Failure({CameraErrorCode::CaptureFailed, error.what()});
  }
}

} // namespace huxerui::camera
