#include "camera_internal.h"

#if !defined(__ANDROID__) && !defined(__APPLE__) && !defined(_WIN32)
namespace huxerui::camera::detail {

struct TextureResizer::State {};

TextureResizer::TextureResizer() = default;
TextureResizer::~TextureResizer() = default;
void TextureResizer::Reset() {}

bool TextureResizer::Resize(PreviewOutput&, std::uint32_t, std::uint32_t) {
  throw CameraError{CameraErrorCode::Unavailable, "Sizing camera textures is not implemented on this platform."};
}

namespace {

class UnavailableCamera final : public CameraBackend {
public:
  void Start(std::optional<Facing>, std::function<void(CameraStatus)> changed) override {
    changed({.state = SessionState::Failed,
             .error = CameraError{CameraErrorCode::Unavailable, "Camera capture is not implemented on this platform."}});
  }

  void Stop(std::function<void()> completed) override {
    if (completed) {
      completed();
    }
  }
};

} // namespace

void InstallPlatformCamera(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, [](UiWindow&) -> std::shared_ptr<CameraBackend> {
        return std::make_shared<UnavailableCamera>();
      }
  );
}

} // namespace huxerui::camera::detail
#endif
