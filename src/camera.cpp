#include <huxerui/camera.h>

#include "camera_internal.h"

namespace huxerui::camera {

void Install(ApplicationContext& context) {
  detail::InstallPlatformCamera(context);
}

} // namespace huxerui::camera
