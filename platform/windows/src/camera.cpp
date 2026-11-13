#include "camera_windows.h"

#include <huxerui/windows/external_texture.h>

#include <windows.h>
#include <cfgmgr32.h>
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

std::optional<double> ReadFrameRate(IMFMediaType& type, REFGUID key) {
  UINT32 numerator = 0;
  UINT32 denominator = 0;
  if (FAILED(MFGetAttributeRatio(&type, key, &numerator, &denominator)) || !numerator || !denominator) return {};
  return double(numerator) / denominator;
}

std::optional<FrameRateRange> FixedFrameRate(IMFMediaType& type) {
  if (const auto fps = ReadFrameRate(type, MF_MT_FRAME_RATE)) return FrameRateRange{*fps, *fps};
  return {};
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
  CameraBackend::StatusCallback changed;

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
    CameraBackend::StatusCallback callback;
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
                .error = error ? std::move(*error) : Error(result, CameraErrorCode::CaptureFailed)}, full_texture_crop);
  }

  void Publish(CameraStatus status) {
    CameraBackend::StatusCallback callback;
    {
      std::lock_guard lock(mutex_);
      if (!reported_ && !silent_) callback = changed;
    }
    if (callback) callback(std::move(status), full_texture_crop);
  }

  void Silence() {
    std::lock_guard lock(mutex_);
    silent_ = true;
  }

  void ObserveDeviceRemoval(std::wstring link) {
    if (link.empty()) return;
    device_link_ = std::move(link);
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
    if (CM_Register_Notification(&filter, this, DeviceNotification, &device_notification_) != CR_SUCCESS) {
      device_notification_ = nullptr;
    }
  }

  void StopObservingDeviceRemoval() {
    if (const auto notification = std::exchange(device_notification_, nullptr)) {
      CM_Unregister_Notification(notification);
    }
  }

private:
  static DWORD CALLBACK DeviceNotification(HCMNOTIFICATION, PVOID context, CM_NOTIFY_ACTION action,
                                           PCM_NOTIFY_EVENT_DATA data, DWORD) noexcept {
    auto& events = *static_cast<CaptureEvents*>(context);
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL && data->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE &&
        _wcsicmp(events.device_link_.c_str(), data->u.DeviceInterface.SymbolicLink) == 0) {
      try {
        events.Fail(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED));
      } catch (...) {
        // Platform notification callbacks cannot propagate exceptions.
      }
    }
    return ERROR_SUCCESS;
  }

  std::wstring device_link_;
  HCMNOTIFICATION device_notification_ = nullptr;
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

  void Configure(CaptureFormat capture_format, LONG stride, PreviewRotation rotation, std::optional<Facing> facing,
                 std::optional<std::string> device_id, std::shared_ptr<FramePipeline> pipeline) {
    const auto width = capture_format.pixel_width;
    const auto height = capture_format.pixel_height;
    width_ = width;
    height_ = height;
    stride_ = stride;
    rotation_ = rotation;
    facing_ = facing;
    device_id_ = std::move(device_id);
    capture_format_ = capture_format;
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
        } else if (processed.Value().output.texture && processed.Value().output != preview_) {
          preview_ = std::move(processed).Value().output;
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
    events->Publish({.state = SessionState::Running, .actual_facing = facing_, .preview = preview_,
                     .capture_format = capture_format_, .actual_device_id = device_id_});
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
  std::optional<CaptureFormat> capture_format_;
  std::optional<Facing> facing_;
  std::optional<std::string> device_id_;
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

std::wstring DeviceAttribute(IMFActivate& device, REFGUID attribute) {
  UINT32 length = 0;
  if (FAILED(device.GetStringLength(attribute, &length))) return {};
  std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
  if (FAILED(device.GetString(attribute, value.data(), length + 1, nullptr))) return {};
  value.resize(length);
  return value;
}

std::optional<Facing> DeviceFacing(const std::wstring& link) {
  if (link.empty()) return {};
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

struct CaptureDevices {
  IMFActivate** items = nullptr;
  UINT32 count = 0;

  CaptureDevices() {
    ComPtr<IMFAttributes> attributes;
    Require(MFCreateAttributes(&attributes, 1));
    Require(attributes->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID));
    const auto result = MFEnumDeviceSources(attributes.Get(), &items, &count);
    if (result != MF_E_NO_CAPTURE_DEVICES_AVAILABLE) Require(result);
  }

  CaptureDevices(const CaptureDevices&) = delete;
  CaptureDevices& operator=(const CaptureDevices&) = delete;

  ~CaptureDevices() {
    for (UINT32 index = 0; index < count; ++index) items[index]->Release();
    CoTaskMemFree(items);
  }
};

ComPtr<IMFActivate> SelectDevice(std::optional<Facing> facing, const std::optional<std::string>& requested_id,
                                std::optional<Facing>& actual_facing, std::wstring& link) {
  const CaptureDevices devices;
  for (UINT32 index = 0; index < devices.count; ++index) {
    auto* device = devices.items[index];
    auto candidate_link = DeviceAttribute(*device, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    if (requested_id && (candidate_link.empty() || winrt::to_string(candidate_link) != *requested_id)) continue;
    const auto candidate_facing = DeviceFacing(candidate_link);
    if (facing && facing != candidate_facing) continue;
    actual_facing = candidate_facing;
    link = std::move(candidate_link);
    return ComPtr<IMFActivate>(device);
  }
  return {};
}

CameraDevicesResult EnumerateCameras() {
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return CameraDevicesResult::Failure(Error(initialized, CameraErrorCode::CaptureFailed));
  struct Apartment {
    ~Apartment() { CoUninitialize(); }
  } apartment;
  const auto started = MFStartup(MF_VERSION);
  if (FAILED(started)) return CameraDevicesResult::Failure(Error(started, CameraErrorCode::CaptureFailed));
  struct MediaFoundation {
    ~MediaFoundation() { MFShutdown(); }
  } media_foundation;
  try {
    const CaptureDevices devices;
    std::vector<CameraDevice> result;
    result.reserve(devices.count);
    for (UINT32 index = 0; index < devices.count; ++index) {
      auto& device = *devices.items[index];
      auto link = DeviceAttribute(device, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
      if (link.empty()) continue;
      const auto name = DeviceAttribute(device, MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
      result.push_back({winrt::to_string(link), name.empty() ? "Camera" : winrt::to_string(name), DeviceFacing(link)});
    }
    return CameraDevicesResult::Success(std::move(result));
  } catch (HRESULT result) {
    return CameraDevicesResult::Failure(Error(result, CameraErrorCode::CaptureFailed));
  }
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

  void Start(std::optional<Facing> facing, const std::optional<std::string>& requested_id, CapturePreferences preferred,
             CameraBackend::StatusCallback changed, std::shared_ptr<FramePipeline> pipeline) {
    changed_ = std::move(changed);
    try {
      Require(com_status_);
      Require(MFStartup(MF_VERSION));
      started_ = true;
      std::wstring link;
      auto device = SelectDevice(facing, requested_id, facing_, link);
      if (!device) {
        changed_({.state = SessionState::Failed,
                  .error = CameraError{CameraErrorCode::DeviceNotFound, "No camera matches the requested selection."}},
                 full_texture_crop);
        return;
      }
      if (!link.empty()) device_id_ = winrt::to_string(link);
      ComPtr<IMFCaptureEngineClassFactory> factory;
      Require(CoCreateInstance(CLSID_MFCaptureEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
      Require(factory->CreateInstance(CLSID_MFCaptureEngine, IID_PPV_ARGS(&engine_)));
      events_ = Make<CaptureEvents>();
      events_->changed = changed_;
      events_->ObserveDeviceRemoval(std::move(link));
      Require(device->ActivateObject(IID_PPV_ARGS(&device_)));
      ComPtr<IMFAttributes> attributes;
      Require(MFCreateAttributes(&attributes, 1));
      Require(attributes->SetUINT32(MF_CAPTURE_ENGINE_USE_VIDEO_DEVICE_ONLY, TRUE));
      Require(engine_->Initialize(events_.Get(), attributes.Get(), nullptr, device_.Get()));
      Require(events_->Wait(MF_CAPTURE_ENGINE_INITIALIZED));
      Require(engine_->GetSource(&source_));
      ConfigurePreview(preferred, std::move(pipeline));
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
        changed_({.state = SessionState::Failed, .error = std::move(error)}, full_texture_crop);
    } catch (const std::exception& error) {
      changed_({.state = SessionState::Failed, .error = CameraError{CameraErrorCode::ConfigurationFailed, error.what()}},
               full_texture_crop);
    }
  }

  void Stop(std::function<void()> completed) {
    if (const auto torch = ReadTorch(); torch && torch->enabled) {
      try {
        ApplyTorch(false);
      } catch (HRESULT) {
        // Shutting down the device releases the torch if the control is no longer reachable.
      }
    }
    lighting_changed_ = {};
    running_ = false;
    changed_ = {};
    if (events_) {
      events_->Silence();
      events_->StopObservingDeviceRemoval();
    }
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
    device_id_.reset();
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

  void ObserveLighting(std::function<void(PhotoFlash, std::optional<Torch>)> changed) {
    lighting_changed_ = std::move(changed);
    PublishLighting();
  }

  CameraResult<void> SetTorchEnabled(bool enabled) {
    if (!running_) {
      return CameraResult<void>::Failure({CameraErrorCode::NotReady, "The camera is not running."});
    }
    try {
      if (!ApplyTorch(enabled)) {
        return CameraResult<void>::Failure({CameraErrorCode::Unavailable, "The camera torch is unavailable."});
      }
      PublishLighting();
      return CameraResult<void>::Success();
    } catch (HRESULT result) {
      return CameraResult<void>::Failure(Error(result, CameraErrorCode::CaptureFailed));
    }
  }

  void Photo(PhotoOptions options, std::function<void(CameraResult<ImageAsset>)> completed) {
    if (!running_) {
      completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::NotReady, "Start the camera before taking a photo."}));
      return;
    }
    ComPtr<IMFCapturePhotoSink> photo;
    ComPtr<IMFExtendedCameraControl> flash;
    const auto reset_flash = [&] {
      if (flash && options.flash_mode != FlashMode::Off &&
          SUCCEEDED(flash->SetFlags(KSCAMERA_EXTENDEDPROP_FLASH_OFF))) {
        flash->CommitSettings();
      }
      flash.Reset();
    };
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
      flash = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_FLASHMODE);
      if (options.flash_mode != FlashMode::Off) {
        const auto flags = options.flash_mode == FlashMode::Auto ? KSCAMERA_EXTENDEDPROP_FLASH_AUTO
                                                               : KSCAMERA_EXTENDEDPROP_FLASH_ON;
        if (!flash || !(flash->GetCapabilities() & flags)) {
          flash.Reset();
          photo->RemoveAllStreams();
          completed(CameraResult<ImageAsset>::Failure(
              {CameraErrorCode::Unavailable, "The requested flash mode is unavailable."}));
          return;
        }
        Require(flash->SetFlags(flags));
        Require(flash->CommitSettings());
      } else if (flash && flash->GetFlags() != KSCAMERA_EXTENDEDPROP_FLASH_OFF) {
        Require(flash->SetFlags(KSCAMERA_EXTENDEDPROP_FLASH_OFF));
        Require(flash->CommitSettings());
      }
      Require(engine_->TakePhoto());
      const HRESULT captured = events_->Wait(MF_CAPTURE_ENGINE_PHOTO_TAKEN);
      reset_flash();
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
      reset_flash();
      if (photo)
        photo->RemoveAllStreams();
      completed(CameraResult<ImageAsset>::Failure(Error(result, CameraErrorCode::CaptureFailed)));
    } catch (const std::exception& error) {
      reset_flash();
      if (photo)
        photo->RemoveAllStreams();
      completed(CameraResult<ImageAsset>::Failure({CameraErrorCode::CaptureFailed, error.what()}));
    }
  }

private:
  std::optional<Torch> ReadTorch() {
    const auto control = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_TORCHMODE);
    if (!control || !(control->GetCapabilities() & KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON)) return {};
    return Torch{.available = true, .enabled = control->GetFlags() != KSCAMERA_EXTENDEDPROP_VIDEOTORCH_OFF};
  }

  bool ApplyTorch(bool enabled) {
    const auto control = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_TORCHMODE);
    if (!control || !(control->GetCapabilities() & KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON)) return false;
    Require(control->SetFlags(enabled ? KSCAMERA_EXTENDEDPROP_VIDEOTORCH_ON : KSCAMERA_EXTENDEDPROP_VIDEOTORCH_OFF));
    Require(control->CommitSettings());
    return true;
  }

  void PublishLighting() {
    if (!lighting_changed_) return;
    PhotoFlash flash;
    ComPtr<IMFMediaType> type;
    if (source_ && SUCCEEDED(source_->GetCurrentDeviceMediaType(MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_PHOTO,
                                                                &type))) {
      if (const auto control = OpenCameraControl(KSPROPERTY_CAMERACONTROL_EXTENDED_FLASHMODE)) {
        const auto flags = control->GetCapabilities();
        flash.supports_auto = (flags & KSCAMERA_EXTENDEDPROP_FLASH_AUTO) != 0;
        flash.supports_on = (flags & KSCAMERA_EXTENDEDPROP_FLASH_ON) != 0;
      }
    }
    lighting_changed_(flash, ReadTorch());
  }

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

  void SelectCaptureFormat(DWORD stream, const CapturePreferences& preferred) {
    if (!preferred.pixel_width && !preferred.fps) return;
    struct Candidate {
      ComPtr<IMFMediaType> type;
      std::tuple<double, double, double> rank;
    };
    std::vector<Candidate> candidates;
    for (DWORD index = 0;; ++index) {
      ComPtr<IMFMediaType> type;
      if (FAILED(source_->GetAvailableDeviceMediaType(stream, index, &type))) break;
      UINT32 width = 0, height = 0;
      if (FAILED(MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height)) || !width || !height) continue;
      CaptureFormat format{width, height, FixedFrameRate(*type.Get())};
      if (preferred.fps) {
        const auto minimum = ReadFrameRate(*type.Get(), MF_MT_FRAME_RATE_RANGE_MIN);
        const auto maximum = ReadFrameRate(*type.Get(), MF_MT_FRAME_RATE_RANGE_MAX);
        if (minimum && maximum && *minimum <= *maximum) {
          const auto fps = std::clamp(*preferred.fps, *minimum, *maximum);
          if ((!format.frame_rate || fps != format.frame_rate->max_fps) && fps >= 0.001 && fps <= 1'000'000) {
            ComPtr<IMFMediaType> adjusted;
            Require(MFCreateMediaType(&adjusted));
            Require(type->CopyAllItems(adjusted.Get()));
            Require(MFSetAttributeRatio(adjusted.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(std::llround(fps * 1000)), 1000));
            candidates.push_back({std::move(adjusted), CaptureFormatRank(preferred, {width, height, FrameRateRange{fps, fps}})});
          }
        }
      }
      candidates.push_back({std::move(type), CaptureFormatRank(preferred, format)});
    }
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const Candidate& left, const Candidate& right) { return left.rank < right.rank; });
    for (const auto& candidate : candidates) {
      if (SUCCEEDED(source_->SetCurrentDeviceMediaType(stream, candidate.type.Get()))) return;
    }
  }

  void ConfigurePreview(const CapturePreferences& preferred, std::shared_ptr<FramePipeline> pipeline) {
    constexpr DWORD stream = MF_CAPTURE_ENGINE_PREFERRED_SOURCE_STREAM_FOR_VIDEO_PREVIEW;
    if (preferred.pixel_width || preferred.fps) {
      DWORD stream_index = 0;
      if (SUCCEEDED(source_->GetStreamIndexFromFriendlyName(stream, &stream_index))) {
        SelectCaptureFormat(stream_index, preferred);
      }
    }
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
    frames_->Configure({width, height, FixedFrameRate(*negotiated.Get())}, stride, rotation, facing_, device_id_,
                       std::move(pipeline));
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
  std::optional<std::string> device_id_;
  CameraBackend::StatusCallback changed_;
  std::function<void(PhotoFlash, std::optional<Torch>)> lighting_changed_;
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

  Task<CameraDevicesResult> EnumerateDevices() override {
    co_return co_await RunWorker(&EnumerateCameras);
  }

  void Start(std::optional<Facing> facing, std::optional<std::string> device_id,
             CapturePreferences preferred, StatusCallback changed) override {
    capture_->Post([capture = capture_, facing, device_id = std::move(device_id), preferred,
                    changed = std::move(changed), pipeline = frame_pipeline_] {
      capture->Start(facing, device_id, preferred, changed, pipeline);
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

  void ObserveLighting(std::function<void(PhotoFlash, std::optional<Torch>)> changed) override {
    capture_->Post([capture = capture_, changed = std::move(changed)] { capture->ObserveLighting(changed); });
  }

  void SetTorchEnabled(bool enabled, std::function<void(CameraResult<void>)> completed) override {
    capture_->Post([capture = capture_, enabled, completed = std::move(completed)] {
      completed(capture->SetTorchEnabled(enabled));
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
