#include "camera_internal.h"

#include <map>

#include <huxerui/android/external_texture.h>
#include <huxerui/android/platform_registry.h>

namespace huxerui::camera::detail {
namespace {

// The platform channel passes a per-run identifier. The frame producer acquires the pipeline through JNI.
std::mutex pipeline_mutex;
std::map<std::int64_t, std::weak_ptr<FramePipeline>> pipelines;
std::int64_t next_pipeline = 0;

std::shared_ptr<FramePipeline> FindPipeline(std::int64_t id) {
  if (id == 0) return {};
  std::lock_guard lock(pipeline_mutex);
  const auto found = pipelines.find(id);
  auto frames = found == pipelines.end() ? nullptr : found->second.lock();
  if (!frames) throw std::runtime_error("The camera frame pipeline is closed.");
  return frames;
}

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

Zoom DecodeZoom(const PlatformPayload& payload) {
  const auto& fields = payload.AsObject();
  return {.ratio = static_cast<float>(fields.at("ratio").AsDouble()),
          .min_ratio = static_cast<float>(fields.at("min").AsDouble()),
          .max_ratio = static_cast<float>(fields.at("max").AsDouble())};
}

ExposureCompensation DecodeExposureCompensation(const PlatformPayload& payload) {
  const auto& fields = payload.AsObject();
  return {.ev = static_cast<float>(fields.at("ev").AsDouble()),
          .min_ev = static_cast<float>(fields.at("min").AsDouble()),
          .max_ev = static_cast<float>(fields.at("max").AsDouble()),
          .step_ev = static_cast<float>(fields.at("step").AsDouble())};
}

class AndroidCamera final : public CameraBackend, public std::enable_shared_from_this<AndroidCamera> {
public:
  explicit AndroidCamera(PlatformChannel channel) : channel_(std::move(channel)) {}
  ~AndroidCamera() override {
    ReleasePipeline();
    channel_.Close();
  }

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
          status.can_focus_and_meter = fields.at("canFocusAndMeter").AsBoolean();
          const auto& output = fields.at("preview");
          if (!output.IsNull()) {
            const auto& preview = output.AsObject();
            const auto rotation = preview.at("rotation").AsInteger();
            if (rotation < 0 || rotation > 3) {
              throw std::invalid_argument("Invalid camera preview rotation.");
            }
            status.preview = PreviewOutput{
                .texture = preview.at("texture").AsExternalTexture(),
                .crop = {static_cast<float>(preview.at("x").AsDouble()), static_cast<float>(preview.at("y").AsDouble()),
                         static_cast<float>(preview.at("width").AsDouble()), static_cast<float>(preview.at("height").AsDouble())},
                .rotation = static_cast<PreviewRotation>(rotation),
                .mirrored = preview.at("mirrored").AsBoolean()};
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
    if (frame_pipeline_) {
      std::lock_guard lock(pipeline_mutex);
      pipeline_ = ++next_pipeline;
      pipelines.emplace(pipeline_, frame_pipeline_);
    }
    auto weak = weak_from_this();
    PlatformPayload::Object arguments{{"run", run}, {"frames", pipeline_},
        {"facing", facing ? PlatformPayload(*facing == Facing::Front ? "front" : "back") : PlatformPayload{}}};
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
    ReleasePipeline();
    channel_.Invoke<std::monostate>("stop", [completed = std::move(completed)](PlatformResult<std::monostate>) {
      if (completed) completed();
    });
  }

  void ReadZoom(std::function<void(std::optional<Zoom>)> completed) override {
    channel_.Invoke("readZoom", PlatformPayload(PlatformPayload::Object{{"run", run_}}),
        [completed = std::move(completed)](PlatformResult<PlatformPayload> result) {
          std::optional<Zoom> zoom;
          try {
            if (const auto* payload = std::get_if<PlatformPayload>(&result); payload && !payload->IsNull()) {
              zoom = DecodeZoom(*payload);
            }
          } catch (const std::exception&) {
            // A failed control query leaves the capability unavailable.
          }
          completed(zoom);
        });
  }

  bool CanReplaceZoomRequest() const noexcept override { return true; }

  void SetZoomRatio(float ratio, std::function<void(CameraResult<Zoom>)> completed) override {
    channel_.Invoke("setZoomRatio",
        PlatformPayload(PlatformPayload::Object{{"run", run_}, {"ratio", static_cast<double>(ratio)}}),
        [completed = std::move(completed)](PlatformResult<PlatformPayload> result) {
          if (const auto* error = std::get_if<PlatformError>(&result)) {
            completed(CameraResult<Zoom>::Failure({DecodeError(error->code), error->message}));
            return;
          }
          try {
            completed(CameraResult<Zoom>::Success(DecodeZoom(std::get<PlatformPayload>(result))));
          } catch (const std::exception& error) {
            completed(CameraResult<Zoom>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
          }
        });
  }

  void ReadExposureCompensation(std::function<void(std::optional<ExposureCompensation>)> completed) override {
    channel_.Invoke("readExposureCompensation", PlatformPayload(PlatformPayload::Object{{"run", run_}}),
        [completed = std::move(completed)](PlatformResult<PlatformPayload> result) {
          std::optional<ExposureCompensation> exposure;
          try {
            if (const auto* payload = std::get_if<PlatformPayload>(&result); payload && !payload->IsNull()) {
              exposure = DecodeExposureCompensation(*payload);
            }
          } catch (const std::exception&) {
            // A failed control query leaves the capability unavailable.
          }
          completed(exposure);
        });
  }

  bool CanReplaceExposureCompensationRequest() const noexcept override { return true; }

  void SetExposureCompensation(float ev, std::function<void(CameraResult<ExposureCompensation>)> completed) override {
    channel_.Invoke("setExposureCompensation",
        PlatformPayload(PlatformPayload::Object{{"run", run_}, {"ev", static_cast<double>(ev)}}),
        [completed = std::move(completed)](PlatformResult<PlatformPayload> result) {
          if (const auto* error = std::get_if<PlatformError>(&result)) {
            completed(CameraResult<ExposureCompensation>::Failure({DecodeError(error->code), error->message}));
            return;
          }
          try {
            completed(CameraResult<ExposureCompensation>::Success(
                DecodeExposureCompensation(std::get<PlatformPayload>(result))));
          } catch (const std::exception& error) {
            completed(CameraResult<ExposureCompensation>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
          }
        });
  }

  void FocusAndMeter(Point point, std::function<void(CameraResult<void>)> completed) override {
    channel_.Invoke("focusAndMeter", PlatformPayload(PlatformPayload::Object{
        {"run", run_}, {"x", double(point.x)}, {"y", double(point.y)}}),
        [completed = std::move(completed)](PlatformResult<PlatformPayload> result) {
          if (const auto* error = std::get_if<PlatformError>(&result)) {
            completed(CameraResult<void>::Failure({DecodeError(error->code), error->message}));
          } else {
            completed(CameraResult<void>::Success());
          }
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
  void ReleasePipeline() {
    std::lock_guard lock(pipeline_mutex);
    pipelines.erase(pipeline_);
    pipeline_ = 0;
  }

  void Fail(CameraErrorCode code, std::string message) {
    changed_({.state = SessionState::Failed, .error = CameraError{code, std::move(message)}});
  }

  PlatformChannel channel_;
  std::function<void(CameraStatus)> changed_;
  std::int64_t run_ = 0;
  std::int64_t pipeline_ = 0;
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

using namespace huxerui;
using namespace huxerui::camera;

struct CameraOutput {
  std::shared_ptr<android::GlTexture> texture;
  std::shared_ptr<camera::detail::FramePipeline> frames;
  int width;
  int height;
  PreviewRotation rotation;
  bool mirrored;
  bool realtime_timestamps;
  std::optional<PreviewOutput> published;
  camera::detail::FrameProducer producer;
};

void ThrowJava(JNIEnv* env, const std::exception& error) {
  if (env->ExceptionCheck()) return;
  huxerui::android::LocalRef<jclass> type(env, env->FindClass("java/lang/IllegalStateException"));
  if (type.Get()) env->ThrowNew(type.Get(), error.what());
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_org_huxerui_lib_camera_CameraSurface_createOutput(JNIEnv* env, jclass, jlong pipeline, jintArray render_size,
                                                       jint rotation, jboolean mirrored, jboolean realtime_timestamps) {
  try {
    jint dimensions[2]{};
    env->GetIntArrayRegion(render_size, 0, 2, dimensions);
    if (env->ExceptionCheck()) return 0;
    auto& [width, height] = dimensions;
    if (width <= 0 || height <= 0 || rotation < 0 || rotation > 270 || rotation % 90 != 0) {
      throw std::invalid_argument("Invalid camera output geometry.");
    }
    auto frames = camera::detail::FindPipeline(pipeline);
    if (frames) {
      const auto source_width = static_cast<std::uint32_t>(width);
      const auto source_height = static_cast<std::uint32_t>(height);
      const auto [target_width, target_height] = frames->ProducerSize(source_width, source_height);
      if (target_width != source_width || target_height != source_height) {
        if (!camera::detail::SupportsGlOutputSize(target_width, target_height)) {
          throw std::invalid_argument("The requested camera output exceeds OpenGL ES limits.");
        }
        width = static_cast<jint>(target_width);
        height = static_cast<jint>(target_height);
        env->SetIntArrayRegion(render_size, 0, 2, dimensions);
        if (env->ExceptionCheck()) return 0;
      }
    }
    auto output = std::make_unique<CameraOutput>(
        std::make_shared<android::GlTexture>(Size{static_cast<float>(width), static_cast<float>(height)}),
        std::move(frames), width, height, static_cast<PreviewRotation>(rotation / 90),
        mirrored == JNI_TRUE, realtime_timestamps == JNI_TRUE);
    return reinterpret_cast<jlong>(output.release());
  } catch (const std::exception& error) {
    ThrowJava(env, error);
    return 0;
  }
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_huxerui_lib_camera_CameraSurface_processFrame(JNIEnv* env, jclass, jlong handle, jint texture, jlong time) {
  try {
    auto& output = *reinterpret_cast<CameraOutput*>(handle);
    output.texture->PublishCurrent({.texture_name = static_cast<std::uint32_t>(texture),
        .pixel_width = output.width, .pixel_height = output.height, .alpha = android::GlTexture::Alpha::Opaque});
    PreviewOutput preview{.texture = output.texture, .rotation = output.rotation, .mirrored = output.mirrored};
    if (output.frames) {
      const auto capture_time = std::chrono::nanoseconds(time);
      const auto synchronized_time = output.realtime_timestamps ? std::optional{capture_time} : std::nullopt;
      auto processed = output.frames->Process(output.producer, std::move(preview), capture_time,
          static_cast<std::uint32_t>(output.width), static_cast<std::uint32_t>(output.height), synchronized_time);
      if (!processed.Succeeded()) throw std::runtime_error(processed.Error().message);
      preview = std::move(processed).Value();
    }
    if (!preview.texture || preview == output.published) return nullptr;
    output.published = preview;
    return android::PlatformPayloadToJava(env, PlatformPayload(PlatformPayload::Object{
        {"texture", preview.texture}, {"x", double(preview.crop.x)}, {"y", double(preview.crop.y)},
        {"width", double(preview.crop.width)}, {"height", double(preview.crop.height)},
        {"rotation", static_cast<std::int64_t>(preview.rotation)}, {"mirrored", preview.mirrored}})).Release();
  } catch (const std::exception& error) {
    ThrowJava(env, error);
    return nullptr;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_org_huxerui_lib_camera_CameraSurface_finishOutput(JNIEnv*, jclass, jlong handle) {
  const std::unique_ptr<CameraOutput> output(reinterpret_cast<CameraOutput*>(handle));
  if (output) {
    output->producer.processor = {};
    output->producer.resizer.Reset();
    output->texture->Finish();
  }
}
