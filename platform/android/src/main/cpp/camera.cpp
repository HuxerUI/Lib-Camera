#include "camera_internal.h"

#include <huxerui/android/external_texture.h>
#include <huxerui/android/platform_registry.h>

namespace huxerui::camera::detail {
namespace {

CameraErrorCode DecodeError(std::string_view code) {
  if (code == "permission-denied") return CameraErrorCode::PermissionDenied;
  if (code == "device-not-found") return CameraErrorCode::DeviceNotFound;
  if (code == "device-busy") return CameraErrorCode::DeviceBusy;
  if (code == "device-disconnected") return CameraErrorCode::DeviceDisconnected;
  if (code == "configuration-failed") return CameraErrorCode::ConfigurationFailed;
  if (code == "capture-failed") return CameraErrorCode::CaptureFailed;
  if (code == "not-ready")
    return CameraErrorCode::NotReady;
  if (code == "operation-in-progress")
    return CameraErrorCode::OperationInProgress;
  if (code == "interrupted")
    return CameraErrorCode::Interrupted;
  return CameraErrorCode::Unavailable;
}

class AndroidCamera final : public CameraBackend, public std::enable_shared_from_this<AndroidCamera> {
public:
  explicit AndroidCamera(PlatformChannel channel) : channel_(std::move(channel)) {}
  ~AndroidCamera() override { channel_.Close(); }

  void Connect() {
    auto weak = weak_from_this();
    channel_.On("snapshot", [weak](const PlatformPayload& payload) {
      if (auto self = weak.lock(); self && self->changed_) {
        try {
          const auto& fields = payload.AsObject();
          if (fields.at("run").AsInteger() != self->run_) return;
          CameraStatus status{.state = SessionState::Running};
          const auto& error = fields.at("error");
          if (!error.IsNull()) {
            status.state = SessionState::Failed;
            status.error = CameraError{DecodeError(error.AsObject().at("code").AsString()),
                                         std::string(error.AsObject().at("message").AsString())};
          }
          const auto& facing = fields.at("facing");
          if (!facing.IsNull()) status.actual_facing = facing.AsString() == "front" ? Facing::Front : Facing::Back;
          const auto& texture = fields.at("texture");
          if (!texture.IsNull()) {
            const auto rotation = fields.at("rotation").AsInteger();
            if (rotation < 0 || rotation > 270 || rotation % 90 != 0) {
              throw std::invalid_argument("Invalid camera preview rotation.");
            }
            status.preview = PreviewOutput{
                .texture = texture.AsExternalTexture(),
                .rotation = static_cast<PreviewRotation>(rotation / 90),
                .mirrored = fields.at("mirrored").AsBoolean()};
          }
          self->changed_(std::move(status));
        } catch (const std::exception& error) {
          self->Fail(CameraErrorCode::ConfigurationFailed, error.what());
        }
      }
    });
  }

  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) override {
    changed_ = std::move(changed);
    const auto run = ++run_;
    auto weak = weak_from_this();
    PlatformPayload::Object arguments{{"run", run}, {"facing", facing ? PlatformPayload(*facing == Facing::Front ? "front" : "back") : PlatformPayload{}}};
    channel_.Invoke("start", PlatformPayload(std::move(arguments)), [weak, run](PlatformResult<PlatformPayload> result) {
      if (auto self = weak.lock(); self && self->run_ == run && self->changed_) {
        if (auto* error = std::get_if<PlatformError>(&result)) {
          self->Fail(CameraErrorCode::Unavailable, error->message);
        }
      }
    });
  }

  void Stop(std::function<void()> completed) override {
    ++run_;
    changed_ = {};
    channel_.Invoke<std::monostate>("stop", [completed = std::move(completed)](PlatformResult<std::monostate>) {
      if (completed) completed();
    });
  }

  void CapturePhoto(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) override {
    PlatformPayload::Object arguments{
        {"run", run_}, {"quality", options.jpeg_quality}, {"mirror", options.mirror == MirrorMode::On}};
    channel_.Invoke(
        "capturePhoto", PlatformPayload(std::move(arguments)),
        [completed = std::move(completed)](PlatformResult<PlatformPayload> result) {
          if (auto* error = std::get_if<PlatformError>(&result)) {
            completed(CameraResult<ImageAsset>::Failure({DecodeError(error->code), error->message}));
            return;
          }
          try {
            const auto bytes = std::get<PlatformPayload>(result).AsBytes();
            completed(CameraResult<ImageAsset>::Success(ImageAsset::FromEncoded(Bytes(bytes.begin(), bytes.end()))));
          } catch (const std::exception& error) {
            completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
          }
        });
  }

private:
  void Fail(CameraErrorCode code, std::string message) {
    changed_({.state = SessionState::Failed, .error = CameraError{code, std::move(message)}});
  }

  PlatformChannel channel_;
  std::function<void(CameraStatus)> changed_;
  std::int64_t run_ = 0;
};

} // namespace

void InstallPlatformCamera(ApplicationContext& context) {
  android::JavaPlatformModuleFactory<std::shared_ptr<CameraBackend>> factory;
  factory.class_name = "org.huxerui.lib.camera.CameraModule";
  factory.create = [](PlatformChannel channel) {
    auto backend = std::make_shared<AndroidCamera>(std::move(channel));
    backend->Connect();
    return backend;
  };
  context.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(camera_module_name, std::move(factory));
}

} // namespace huxerui::camera::detail

namespace {

void ThrowJava(JNIEnv* env, const std::exception& error) {
  if (env->ExceptionCheck()) return;
  huxerui::android::LocalRef<jclass> type(env, env->FindClass("java/lang/IllegalStateException"));
  if (type.Get()) env->ThrowNew(type.Get(), error.what());
}

std::shared_ptr<huxerui::android::SurfaceStreamTexture> Texture(JNIEnv* env, jobject payload) {
  auto texture = std::dynamic_pointer_cast<huxerui::android::SurfaceStreamTexture>(
      huxerui::android::JavaPlatformPayloadToCpp(env, payload).AsExternalTexture());
  if (!texture) throw std::invalid_argument("Expected a camera Surface stream texture.");
  return texture;
}

} // namespace

extern "C" JNIEXPORT jobject JNICALL
Java_org_huxerui_lib_camera_CameraModule_createOutput(JNIEnv* env, jclass, jint width, jint height, jboolean swap) {
  try {
    huxerui::Size intrinsic{static_cast<float>(swap ? height : width), static_cast<float>(swap ? width : height)};
    auto texture = huxerui::android::SurfaceStreamTexture::Create(env, intrinsic, width, height);
    return huxerui::android::PlatformPayloadToJava(env, huxerui::PlatformPayload(std::move(texture))).Release();
  } catch (const std::exception& error) {
    ThrowJava(env, error);
    return nullptr;
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_huxerui_lib_camera_CameraModule_outputSurface(JNIEnv* env, jclass, jobject payload) {
  try {
    return Texture(env, payload)->Surface(env).Release();
  } catch (const std::exception& error) {
    ThrowJava(env, error);
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_lib_camera_CameraModule_finishOutput(JNIEnv* env, jclass, jobject payload) {
  try {
    Texture(env, payload)->Finish();
  } catch (const std::exception& error) {
    ThrowJava(env, error);
  }
}
