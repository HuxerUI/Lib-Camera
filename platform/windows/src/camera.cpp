#include "camera_windows.h"

#include <huxerui/windows/external_texture.h>

#include <d3d11.h>
#include <ks.h>
#include <ksmedia.h>
#include <mfapi.h>
#include <mfcaptureengine.h>
#include <mferror.h>
#include <mfidl.h>
#include <wincodec.h>
#include <wrl.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>

#include <chrono>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace huxerui::camera::detail {
namespace {

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Make;
template <class Interface>
using Callback = Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Interface>;

void Require(HRESULT result) {
  if (FAILED(result))
    throw result;
}

CameraError Error(HRESULT result, CameraErrorCode fallback) {
  auto code = fallback;
  if (result == E_ACCESSDENIED)
    code = CameraErrorCode::PermissionDenied;
  else if (result == MF_E_NO_CAPTURE_DEVICES_AVAILABLE)
    code = CameraErrorCode::DeviceNotFound;
  else if (result == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || result == HRESULT_FROM_WIN32(ERROR_BUSY))
    code = CameraErrorCode::DeviceBusy;
  else if (result == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED || result == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED))
    code = CameraErrorCode::DeviceDisconnected;
  return {code, "Windows camera operation failed (HRESULT " + std::to_string(result) + ")."};
}

constexpr double zoom_scale = 65'536.0;

std::optional<Zoom> DriverZoom(IMFExtendedCameraControl& control, std::optional<float> target = {}) {
  BYTE* payload = nullptr;
  ULONG size = 0;
  Require(control.LockPayload(&payload, &size));
  KSCAMERA_EXTENDEDPROP_VIDEOPROCSETTING setting{};
  const bool has_payload = payload && size == sizeof(setting);
  if (has_payload) std::memcpy(&setting, payload, sizeof(setting));
  const bool supported = has_payload && setting.Min > 0 && setting.Max > setting.Min &&
      setting.VideoProc.Value.ul >= static_cast<ULONG>(setting.Min) &&
      setting.VideoProc.Value.ul <= static_cast<ULONG>(setting.Max);
  if (supported && target) {
    const auto value = std::clamp(static_cast<double>(*target) * zoom_scale,
        static_cast<double>(setting.Min), static_cast<double>(setting.Max));
    const auto step = std::max(1L, setting.Step);
    const auto index = std::min(std::llround((value - setting.Min) / step),
        static_cast<long long>((setting.Max - setting.Min) / step));
    setting.VideoProc.Value.ul = static_cast<ULONG>(setting.Min + index * step);
    std::memcpy(payload, &setting, sizeof(setting));
  }
  Require(control.UnlockPayload());
  if (!supported) return {};
  return Zoom{.ratio = static_cast<float>(setting.VideoProc.Value.ul / zoom_scale),
              .min_ratio = static_cast<float>(setting.Min / zoom_scale),
              .max_ratio = static_cast<float>(setting.Max / zoom_scale)};
}

int ExposureStepsPerEV(ULONGLONG flags) {
  switch (flags) {
    case KSCAMERA_EXTENDEDPROP_EVCOMP_SIXTHSTEP: return 6;
    case KSCAMERA_EXTENDEDPROP_EVCOMP_QUARTERSTEP: return 4;
    case KSCAMERA_EXTENDEDPROP_EVCOMP_THIRDSTEP: return 3;
    case KSCAMERA_EXTENDEDPROP_EVCOMP_HALFSTEP: return 2;
    case KSCAMERA_EXTENDEDPROP_EVCOMP_FULLSTEP: return 1;
    default: return 0;
  }
}

std::optional<ExposureCompensation> DriverExposureCompensation(IMFExtendedCameraControl& control,
                                                                std::optional<float> target = {}) {
  const auto steps_per_ev = ExposureStepsPerEV(control.GetFlags());
  if (!steps_per_ev) return {};
  BYTE* payload = nullptr;
  ULONG size = 0;
  Require(control.LockPayload(&payload, &size));
  KSCAMERA_EXTENDEDPROP_EVCOMPENSATION setting{};
  const bool has_payload = payload && size == sizeof(setting);
  if (has_payload) std::memcpy(&setting, payload, sizeof(setting));
  const auto min_index = static_cast<long long>(setting.Min) * steps_per_ev;
  const auto max_index = static_cast<long long>(setting.Max) * steps_per_ev;
  const bool supported = has_payload && max_index > min_index && min_index >= std::numeric_limits<LONG>::min() &&
      max_index <= std::numeric_limits<LONG>::max() && setting.Value >= min_index && setting.Value <= max_index;
  if (supported && target) {
    const auto index = std::clamp(std::llround(static_cast<double>(*target) * steps_per_ev), min_index, max_index);
    setting.Mode = 0;
    setting.Value = static_cast<LONG>(index);
    setting.Reserved = 0;
    std::memcpy(payload, &setting, sizeof(setting));
  }
  Require(control.UnlockPayload());
  if (!supported) return {};
  return ExposureCompensation{.ev = static_cast<float>(setting.Value) / steps_per_ev,
                             .min_ev = static_cast<float>(setting.Min),
                             .max_ev = static_cast<float>(setting.Max),
                             .step_ev = 1.0F / steps_per_ev};
}

class CaptureEvents final : public Callback<IMFCaptureEngineOnEventCallback> {
public:
  std::function<void(CameraStatus)> changed;

  HRESULT STDMETHODCALLTYPE OnEvent(IMFMediaEvent* event) override {
    GUID type{};
    HRESULT status = S_OK;
    if (!event || FAILED(event->GetExtendedType(&type)) || FAILED(event->GetStatus(&status)))
      return E_INVALIDARG;
    {
      std::lock_guard lock(mutex_);
      if (type == MF_CAPTURE_ENGINE_ERROR)
        failure_ = FAILED(status) ? status : E_FAIL;
      else if (type == MF_CAPTURE_ENGINE_INITIALIZED || type == MF_CAPTURE_ENGINE_PREVIEW_STARTED ||
               type == MF_CAPTURE_ENGINE_PREVIEW_STOPPED || type == MF_CAPTURE_ENGINE_PHOTO_TAKEN)
        events_.emplace_back(type, status);
    }
    ready_.notify_all();
    if (type == MF_CAPTURE_ENGINE_ERROR)
      Fail(FAILED(status) ? status : E_FAIL);
    return S_OK;
  }

  HRESULT Wait(REFGUID type) {
    std::unique_lock lock(mutex_);
    const auto found = [&] {
      return std::find_if(events_.begin(), events_.end(), [&](const auto& event) { return event.first == type; });
    };
    if (!ready_.wait_for(lock, std::chrono::seconds(15), [&] { return FAILED(failure_) || found() != events_.end(); }))
      return HRESULT_FROM_WIN32(WAIT_TIMEOUT);
    if (FAILED(failure_))
      return failure_;
    const auto event = found();
    const auto result = event->second;
    events_.erase(event);
    return result;
  }

  void Fail(HRESULT result, std::optional<CameraError> error = {}) {
    std::function<void(CameraStatus)> callback;
    {
      std::lock_guard lock(mutex_);
      failure_ = result;
      if (!reported_ && !silent_) {
        reported_ = true;
        callback = changed;
      }
    }
    ready_.notify_all();
    if (callback)
      callback({.state = SessionState::Failed,
                .error = error ? std::move(*error) : Error(result, CameraErrorCode::CaptureFailed)});
  }

  void Publish(CameraStatus status) {
    std::function<void(CameraStatus)> callback;
    {
      std::lock_guard lock(mutex_);
      if (!reported_ && !silent_) callback = changed;
    }
    if (callback) callback(std::move(status));
  }

  void Silence() {
    std::lock_guard lock(mutex_);
    silent_ = true;
  }

private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<std::pair<GUID, HRESULT>> events_;
  HRESULT failure_ = S_OK;
  bool reported_ = false;
  bool silent_ = false;
};

class PreviewFrames final : public Callback<IMFCaptureEngineOnSampleCallback> {
public:
  std::shared_ptr<windows::D3D11Texture> texture;
  ComPtr<CaptureEvents> events;

  void Configure(UINT width, UINT height, LONG stride, PreviewRotation rotation, std::optional<Facing> facing,
                 std::shared_ptr<FramePipeline> pipeline) {
    width_ = width;
    height_ = height;
    stride_ = stride;
    rotation_ = rotation;
    facing_ = facing;
    pipeline_ = std::move(pipeline);
    if (!width || !height || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION || height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION)
      throw E_INVALIDARG;
    Require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &device_, nullptr, &context_));
    producer_.resizer.SetContext(*context_.Get());
    D3D11_TEXTURE2D_DESC description{};
    description.Width = width;
    description.Height = height;
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Require(device_->CreateTexture2D(&description, nullptr, &upload_));
    texture = std::make_shared<windows::D3D11Texture>(Size{static_cast<float>(width), static_cast<float>(height)});
    if (!pipeline_) preview_ = PreviewOutput{.texture = texture, .rotation = rotation_};
  }

  HRESULT STDMETHODCALLTYPE OnSample(IMFSample* sample) override {
    if (!sample)
      return S_OK;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || finished_)
      return S_OK;
    try {
      ComPtr<IMFMediaBuffer> buffer;
      Require(sample->ConvertToContiguousBuffer(&buffer));
      ComPtr<IMF2DBuffer2> image;
      BYTE* bytes = nullptr;
      LONG stride = stride_;
      if (SUCCEEDED(buffer.As(&image))) {
        BYTE* allocation = nullptr;
        DWORD length = 0;
        Require(image->Lock2DSize(MF2DBuffer_LockFlags_Read, &bytes, &stride, &allocation, &length));
        const auto row_size = static_cast<std::size_t>(std::abs(static_cast<long long>(stride)));
        const auto offset = static_cast<std::size_t>(bytes - allocation);
        const auto tail = row_size * (height_ - 1);
        const bool valid = row_size >= static_cast<std::size_t>(width_) * 4 && offset <= length &&
            (stride < 0 ? offset >= tail && length - offset >= static_cast<std::size_t>(width_) * 4
                        : length - offset >= tail + static_cast<std::size_t>(width_) * 4);
        try {
          if (valid)
            Upload(bytes, stride);
        } catch (...) {
          image->Unlock2D();
          throw;
        }
        image->Unlock2D();
        if (!valid)
          throw E_INVALIDARG;
      } else {
        DWORD length = 0;
        Require(buffer->Lock(&bytes, nullptr, &length));
        const auto row = static_cast<std::size_t>(std::abs(static_cast<long long>(stride)));
        const bool valid = row >= static_cast<std::size_t>(width_) * 4 &&
                           length >= row * (height_ - 1) + static_cast<std::size_t>(width_) * 4;
        try {
          if (valid)
            Upload(stride < 0 ? bytes + row * (height_ - 1) : bytes, stride);
        } catch (...) {
          buffer->Unlock();
          throw;
        }
        buffer->Unlock();
        if (!valid)
          throw E_INVALIDARG;
      }
      texture->Publish({upload_.Get(), windows::D3D11Texture::Alpha::Opaque});
      if (pipeline_) {
        LONGLONG time = 0;
        Require(sample->GetSampleTime(&time));
        const auto capture_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>(time));
        std::optional<std::chrono::nanoseconds> synchronized_time;
        // DeviceTimestamp uses the QPC epoch also used by WASAPI capture.
        UINT64 device_time = 0;
        if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &device_time)) &&
            device_time <= static_cast<UINT64>(std::chrono::nanoseconds::max().count() / 100)) {
          synchronized_time = std::chrono::nanoseconds(device_time * 100);
        }
        auto processed = pipeline_->Process(producer_, {.texture = texture, .rotation = rotation_},
            capture_time, width_, height_, synchronized_time);
        if (!processed.Succeeded()) {
          finished_ = true;
          events->Fail(E_FAIL, processed.Error());
        } else if (processed.Value().texture && processed.Value() != preview_) {
          preview_ = std::move(processed).Value();
          if (running_) Publish();
        }
      }
    } catch (HRESULT result) {
      events->Fail(result);
    } catch (const std::exception&) {
      events->Fail(E_FAIL);
    }
    return S_OK;
  }

  void StartPublishing() {
    std::lock_guard lock(mutex_);
    running_ = true;
    Publish();
  }

  void Finish() {
    // Publication and Finish share this lock, including the source device's immediate context.
    std::lock_guard lock(mutex_);
    finished_ = true;
    producer_.processor = {};
    producer_.resizer.Reset();
    if (texture)
      texture->Finish();
    preview_.reset();
    pipeline_.reset();
  }

private:
  void Publish() {
    events->Publish({.state = SessionState::Running, .actual_facing = facing_, .preview = preview_});
  }
  void Upload(const BYTE* first, LONG stride) {
    if (stride < 0) {
      pixels_.resize(static_cast<std::size_t>(width_) * height_ * 4);
      for (UINT row = 0; row < height_; ++row)
        std::memcpy(pixels_.data() + static_cast<std::size_t>(row) * width_ * 4,
                    first + static_cast<std::ptrdiff_t>(row) * stride, static_cast<std::size_t>(width_) * 4);
      first = pixels_.data();
      stride = static_cast<LONG>(width_ * 4);
    }
    context_->UpdateSubresource(upload_.Get(), 0, nullptr, first, static_cast<UINT>(stride), 0);
  }

  std::mutex mutex_;
  bool finished_ = false;
  bool running_ = false;
  std::shared_ptr<FramePipeline> pipeline_;
  std::optional<PreviewOutput> preview_;
  std::optional<Facing> facing_;
  PreviewRotation rotation_ = PreviewRotation::R0;
  UINT width_ = 0;
  UINT height_ = 0;
  LONG stride_ = 0;
  std::vector<BYTE> pixels_;
  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11Texture2D> upload_;
  FrameProducer producer_;
};

std::optional<Facing> DeviceFacing(IMFActivate* device) {
  UINT32 length = 0;
  if (FAILED(device->GetStringLength(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &length)))
    return {};
  std::wstring link(static_cast<std::size_t>(length) + 1, L'\0');
  if (FAILED(device->GetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, link.data(), length + 1, nullptr)))
    return {};
  link.resize(length);
  try {
    using namespace winrt::Windows::Devices::Enumeration;
    const auto info = DeviceInformation::CreateFromIdAsync(link).get();
    if (const auto location = info.EnclosureLocation()) {
      if (location.Panel() == Panel::Front)
        return Facing::Front;
      if (location.Panel() == Panel::Back)
        return Facing::Back;
    }
  } catch (const winrt::hresult_error&) {
  }
  return {};
}

ComPtr<IMFActivate> SelectDevice(std::optional<Facing> requested, std::optional<Facing>& actual) {
  ComPtr<IMFAttributes> attributes;
  Require(MFCreateAttributes(&attributes, 1));
  Require(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
  IMFActivate** devices = nullptr;
  UINT32 count = 0;
  Require(MFEnumDeviceSources(attributes.Get(), &devices, &count));
  ComPtr<IMFActivate> selected;
  for (UINT32 index = 0; index < count; ++index) {
    if (!selected) {
      const auto facing = DeviceFacing(devices[index]);
      if (!requested || requested == facing) {
        selected = devices[index];
        actual = facing;
      }
    }
    devices[index]->Release();
  }
  CoTaskMemFree(devices);
  return selected;
}

PreviewRotation Rotation(IMFMediaType* type) {
  UINT32 rotation = 0;
  if (SUCCEEDED(type->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation))) {
    if (rotation > 270 || rotation % 90 != 0)
      throw E_INVALIDARG;
  }
  return static_cast<PreviewRotation>(rotation / 90);
}

class WindowsCapture final : public std::enable_shared_from_this<WindowsCapture> {
public:
  void Post(std::function<void()> operation) {
    std::lock_guard lock(queue_mutex_);
    queue_.push_back(std::move(operation));
    if (scheduled_)
      return;
    auto owner = std::make_unique<std::shared_ptr<WindowsCapture>>(shared_from_this());
    if (!TrySubmitThreadpoolCallback([](PTP_CALLBACK_INSTANCE, void* context) {
          std::unique_ptr<std::shared_ptr<WindowsCapture>> owner(static_cast<std::shared_ptr<WindowsCapture>*>(context));
          const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
          (*owner)->Drain(initialized);
          owner.reset();
          if (SUCCEEDED(initialized))
            CoUninitialize();
        }, owner.get(), nullptr)) {
      queue_.pop_back();
      throw std::runtime_error("Cannot schedule Windows camera work.");
    }
    owner.release();
    scheduled_ = true;
  }

  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed, std::shared_ptr<FramePipeline> pipeline) {
    changed_ = std::move(changed);
    try {
      Require(com_status_);
      Require(MFStartup(MF_VERSION));
      started_ = true;
      auto device = SelectDevice(facing, facing_);
      if (!device) {
        changed_({.state = SessionState::Failed,
                  .error = CameraError{CameraErrorCode::DeviceNotFound, "No camera matches the requested facing."}});
        return;
      }
      Require(device->ActivateObject(IID_PPV_ARGS(&device_)));
      ComPtr<IMFCaptureEngineClassFactory> factory;
      Require(CoCreateInstance(CLSID_MFCaptureEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
      Require(factory->CreateInstance(CLSID_MFCaptureEngine, IID_PPV_ARGS(&engine_)));
      events_ = Make<CaptureEvents>();
      events_->changed = changed_;
      ComPtr<IMFAttributes> attributes;
      Require(MFCreateAttributes(&attributes, 1));
      Require(attributes->SetUINT32(MF_CAPTURE_ENGINE_USE_VIDEO_DEVICE_ONLY, TRUE));
      Require(engine_->Initialize(events_.Get(), attributes.Get(), nullptr, device_.Get()));
      Require(events_->Wait(MF_CAPTURE_ENGINE_INITIALIZED));
      Require(engine_->GetSource(&source_));
      ConfigurePreview(std::move(pipeline));
      Require(engine_->StartPreview());
      preview_started_ = true;
      Require(events_->Wait(MF_CAPTURE_ENGINE_PREVIEW_STARTED));
      running_ = true;
      frames_->StartPublishing();
    } catch (HRESULT result) {
      auto error = Error(result, CameraErrorCode::ConfigurationFailed);
      if (events_)
        events_->Fail(result, std::move(error));
      else
        changed_({.state = SessionState::Failed, .error = std::move(error)});
    } catch (const std::exception& error) {
      changed_({.state = SessionState::Failed, .error = CameraError{CameraErrorCode::ConfigurationFailed, error.what()}});
    }
  }

  void Stop(std::function<void()> completed) {
    running_ = false;
    changed_ = {};
    if (events_)
      events_->Silence();
    if (engine_ && preview_started_ && SUCCEEDED(engine_->StopPreview()))
      events_->Wait(MF_CAPTURE_ENGINE_PREVIEW_STOPPED);
    // Shutdown also covers failed initialization and a device that no longer sends stop events.
    if (device_)
      device_->Shutdown();
    if (frames_)
      frames_->Finish();
    preview_.Reset();
    source_.Reset();
    engine_.Reset();
    device_.Reset();
    frames_.Reset();
    events_.Reset();
    preview_started_ = false;
    facing_.reset();
    if (started_) {
      MFShutdown();
      started_ = false;
    }
    if (completed)
      completed();
  }

  std::optional<Zoom> ReadZoom() {
    try {
      const auto control = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_ZOOM);
      return control ? DriverZoom(*control.Get()) : std::nullopt;
    } catch (HRESULT) {
      return {};
    }
  }

  CameraResult<Zoom> SetZoomRatio(float ratio) {
    if (!running_) {
      return CameraResult<Zoom>::Failure({CameraErrorCode::NotReady, "The camera is not running."});
    }
    try {
      const auto control = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_ZOOM);
      if (!control || !DriverZoom(*control.Get(), ratio)) {
        return CameraResult<Zoom>::Failure({CameraErrorCode::Unavailable, "Camera zoom is unavailable."});
      }
      Require(control->SetFlags((control->GetCapabilities() & KSCAMERA_EXTENDEDPROP_ZOOM_DIRECT)
          ? KSCAMERA_EXTENDEDPROP_ZOOM_DIRECT : KSCAMERA_EXTENDEDPROP_ZOOM_DEFAULT));
      Require(control->CommitSettings());
      if (const auto applied = ReadZoom()) return CameraResult<Zoom>::Success(*applied);
      return CameraResult<Zoom>::Failure({CameraErrorCode::CaptureFailed, "Cannot read the applied camera zoom."});
    } catch (HRESULT result) {
      return CameraResult<Zoom>::Failure(Error(result, CameraErrorCode::CaptureFailed));
    }
  }

  std::optional<ExposureCompensation> ReadExposureCompensation() {
    try {
      const auto control = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_EVCOMPENSATION);
      return control ? DriverExposureCompensation(*control.Get()) : std::nullopt;
    } catch (HRESULT) {
      return {};
    }
  }

  CameraResult<ExposureCompensation> SetExposureCompensation(float ev) {
    if (!running_) {
      return CameraResult<ExposureCompensation>::Failure({CameraErrorCode::NotReady, "The camera is not running."});
    }
    try {
      const auto control = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_EVCOMPENSATION);
      if (!control || !DriverExposureCompensation(*control.Get(), ev)) {
        return CameraResult<ExposureCompensation>::Failure(
            {CameraErrorCode::Unavailable, "Camera exposure compensation is unavailable."});
      }
      Require(control->CommitSettings());
      if (const auto applied = ReadExposureCompensation()) return CameraResult<ExposureCompensation>::Success(*applied);
      return CameraResult<ExposureCompensation>::Failure(
          {CameraErrorCode::CaptureFailed, "Cannot read the applied camera exposure compensation."});
    } catch (HRESULT result) {
      return CameraResult<ExposureCompensation>::Failure(Error(result, CameraErrorCode::CaptureFailed));
    }
  }

  void Photo(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) {
    if (!running_) {
      completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::NotReady, "Start the camera before taking a photo."}));
      return;
    }
    ComPtr<IMFCapturePhotoSink> photo;
    try {
      Require(com_status_);
      constexpr DWORD stream = MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_PHOTO;
      ComPtr<IMFMediaType> source_type;
      const HRESULT available = source_->GetCurrentDeviceMediaType(stream, &source_type);
      if (available == MF_E_INVALIDSTREAMNUMBER) {
        completed(CameraResult<ImageAsset>::Failure(
            {CameraErrorCode::Unavailable, "This camera does not expose a still-photo output."}));
        return;
      }
      Require(available);
      const auto rotation = Rotation(source_type.Get());
      ComPtr<IMFMediaType> type;
      Require(MFCreateMediaType(&type));
      Require(type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Image));
      Require(type->SetGUID(MF_MT_SUBTYPE, GUID_ContainerFormatJpeg));
      UINT32 width = 0, height = 0;
      Require(MFGetAttributeSize(source_type.Get(), MF_MT_FRAME_SIZE, &width, &height));
      Require(MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height));
      ComPtr<IMFCaptureSink> sink;
      Require(engine_->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PHOTO, &sink));
      Require(sink.As(&photo));
      Require(photo->RemoveAllStreams());
      DWORD output_index = 0;
      Require(photo->AddStream(stream, type.Get(), nullptr, &output_index));
      ComPtr<IStream> storage;
      Require(CreateStreamOnHGlobal(nullptr, TRUE, &storage));
      ComPtr<IMFByteStream> bytes;
      Require(MFCreateMFByteStreamOnStream(storage.Get(), &bytes));
      Require(photo->SetOutputByteStream(bytes.Get()));
      Require(engine_->TakePhoto());
      const HRESULT captured = events_->Wait(MF_CAPTURE_ENGINE_PHOTO_TAKEN);
      if (captured == HRESULT_FROM_WIN32(WAIT_TIMEOUT)) {
        // An unacknowledged request cannot safely share the engine with another photo.
        events_->Fail(captured);
        photo.Reset();
        Stop({});
      }
      Require(captured);
      Require(bytes->Flush());
      STATSTG stat{};
      Require(storage->Stat(&stat, STATFLAG_NONAME));
      if (!stat.cbSize.QuadPart || stat.cbSize.QuadPart > std::numeric_limits<ULONG>::max())
        throw E_INVALIDARG;
      Bytes jpeg(static_cast<std::size_t>(stat.cbSize.QuadPart));
      Require(storage->Seek({}, STREAM_SEEK_SET, nullptr));
      ULONG read = 0;
      Require(storage->Read(jpeg.data(), static_cast<ULONG>(jpeg.size()), &read));
      if (read != jpeg.size())
        throw E_FAIL;
      auto image = EncodeWindowsPhoto(jpeg, options, rotation);
      photo->RemoveAllStreams();
      completed(CameraResult<ImageAsset>::Success(std::move(image)));
    } catch (HRESULT result) {
      if (photo)
        photo->RemoveAllStreams();
      completed(CameraResult<ImageAsset>::Failure(Error(result, CameraErrorCode::CaptureFailed)));
    } catch (const std::exception& error) {
      if (photo)
        photo->RemoveAllStreams();
      completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
    }
  }

private:
  ComPtr<IMFExtendedCameraControl> OpenCameraControl(ULONG property) {
    ComPtr<IMFExtendedCameraController> controller;
    ComPtr<IMFExtendedCameraControl> control;
    if (running_ && device_ && SUCCEEDED(MFGetService(device_.Get(), GUID_NULL, IID_PPV_ARGS(&controller)))) {
      controller->GetExtendedCameraControl(MF_CAPTURE_ENGINE_MEDIASOURCE, property, &control);
    }
    return control;
  }

  void Drain(HRESULT initialized) noexcept {
    com_status_ = initialized;
    while (true) {
      std::function<void()> operation;
      {
        std::lock_guard lock(queue_mutex_);
        if (queue_.empty()) {
          scheduled_ = false;
          return;
        }
        operation = std::move(queue_.front());
        queue_.pop_front();
      }
      try {
        operation();
      } catch (...) {
        if (events_)
          events_->Fail(E_FAIL);
      }
    }
  }

  void ConfigurePreview(std::shared_ptr<FramePipeline> pipeline) {
    constexpr DWORD stream = MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_VIDEO_PREVIEW;
    ComPtr<IMFMediaType> source_type;
    Require(source_->GetCurrentDeviceMediaType(stream, &source_type));
    const auto rotation = Rotation(source_type.Get());
    ComPtr<IMFMediaType> type;
    Require(MFCreateMediaType(&type));
    Require(source_type->CopyAllItems(type.Get()));
    Require(type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32));
    type->DeleteItem(MF_MT_DEFAULT_STRIDE);
    type->DeleteItem(MF_MT_SAMPLE_SIZE);
    ComPtr<IMFCaptureSink> sink;
    Require(engine_->GetSink(MF_CAPTURE_ENGINE_SINK_TYPE_PREVIEW, &sink));
    Require(sink.As(&preview_));
    DWORD output = 0;
    Require(preview_->AddStream(stream, type.Get(), nullptr, &output));
    ComPtr<IMFMediaType> negotiated;
    Require(preview_->GetOutputMediaType(output, &negotiated));
    UINT32 width = 0, height = 0;
    Require(MFGetAttributeSize(negotiated.Get(), MF_MT_FRAME_SIZE, &width, &height));
    UINT32 stride_value = 0;
    LONG stride = 0;
    if (SUCCEEDED(negotiated->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride_value)))
      stride = static_cast<LONG>(stride_value);
    else
      Require(MFGetStrideForBitmapInfoHeader(MFVideoFormat_RGB32.Data1, width, &stride));
    frames_ = Make<PreviewFrames>();
    frames_->events = events_;
    frames_->Configure(width, height, stride, rotation, facing_, std::move(pipeline));
    Require(preview_->SetSampleCallback(output, frames_.Get()));
  }

  std::mutex queue_mutex_;
  std::deque<std::function<void()>> queue_;
  bool scheduled_ = false;
  bool started_ = false;
  bool running_ = false;
  bool preview_started_ = false;
  HRESULT com_status_ = S_OK;
  std::optional<Facing> facing_;
  std::function<void(CameraStatus)> changed_;
  ComPtr<IMFMediaSource> device_;
  ComPtr<IMFCaptureEngine> engine_;
  ComPtr<IMFCaptureSource> source_;
  ComPtr<IMFCapturePreviewSink> preview_;
  ComPtr<CaptureEvents> events_;
  ComPtr<PreviewFrames> frames_;
};

class WindowsCamera final : public CameraBackend {
public:
  WindowsCamera() : capture_(std::make_shared<WindowsCapture>()) {}
  ~WindowsCamera() override {
    if (needs_stop_)
      capture_->Post([capture = capture_] { capture->Stop({}); });
  }

  void Start(std::optional<Facing> facing, std::function<void(CameraStatus)> changed) override {
    capture_->Post([capture = capture_, facing, changed = std::move(changed), pipeline = frame_pipeline_] {
      capture->Start(facing, changed, pipeline);
    });
    needs_stop_ = true;
  }

  void Stop(std::function<void()> completed) override {
    capture_->Post([capture = capture_, completed = std::move(completed)] { capture->Stop(completed); });
    needs_stop_ = false;
  }

  void CapturePhoto(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) override {
    capture_->Post([capture = capture_, options, completed = std::move(completed)] { capture->Photo(options, completed); });
  }

  void ReadZoom(std::function<void(std::optional<Zoom>)> completed) override {
    capture_->Post([capture = capture_, completed = std::move(completed)] { completed(capture->ReadZoom()); });
  }

  void SetZoomRatio(float ratio, std::function<void(CameraResult<Zoom>)> completed) override {
    capture_->Post([capture = capture_, ratio, completed = std::move(completed)] {
      completed(capture->SetZoomRatio(ratio));
    });
  }

  void ReadExposureCompensation(std::function<void(std::optional<ExposureCompensation>)> completed) override {
    capture_->Post([capture = capture_, completed = std::move(completed)] {
      completed(capture->ReadExposureCompensation());
    });
  }

  void SetExposureCompensation(float ev, std::function<void(CameraResult<ExposureCompensation>)> completed) override {
    capture_->Post([capture = capture_, ev, completed = std::move(completed)] {
      completed(capture->SetExposureCompensation(ev));
    });
  }

private:
  std::shared_ptr<WindowsCapture> capture_;
  bool needs_stop_ = false;
};

} // namespace

std::shared_ptr<CameraBackend> CreateWindowsCamera() {
  return std::make_shared<WindowsCamera>();
}

void InstallPlatformCamera(ApplicationContext& context) {
  context.RegisterPlatformModule<std::shared_ptr<CameraBackend>>(
      camera_module_name, [](UiWindow&) { return CreateWindowsCamera(); });
}

} // namespace huxerui::camera::detail
