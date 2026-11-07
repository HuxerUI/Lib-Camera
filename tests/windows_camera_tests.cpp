#include "camera_windows.h"

#include <windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Security.Authorization.AppCapabilityAccess.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace huxerui;
using namespace huxerui::camera;
using Microsoft::WRL::ComPtr;

namespace {

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

void Require(HRESULT result) {
  Check(SUCCEEDED(result), "Windows image test operation failed.");
}

Bytes MarkerBitmap() {
  constexpr unsigned width = 80, height = 48;
  Bytes data(54 + width * height * 3);
  const auto write = [&](std::size_t offset, unsigned value, unsigned count) {
    for (unsigned index = 0; index < count; ++index)
      data[offset + index] = std::byte((value >> (index * 8)) & 255);
  };
  data[0] = std::byte{'B'};
  data[1] = std::byte{'M'};
  write(2, static_cast<unsigned>(data.size()), 4);
  write(10, 54, 4);
  write(14, 40, 4);
  write(18, width, 4);
  write(22, height, 4);
  write(26, 1, 2);
  write(28, 24, 2);
  for (unsigned y = 0; y < height; ++y) {
    for (unsigned x = 0; x < width; ++x) {
      const auto offset = 54 + ((height - 1 - y) * width + x) * 3;
      data[offset] = std::byte(y >= height / 2 ? 240 : 10);
      data[offset + 1] = std::byte(x >= width / 2 ? 240 : 10);
      data[offset + 2] = std::byte(y < height / 2 ? 240 : 10);
    }
  }
  return data;
}

void CheckPhoto(const ImageAsset& photo, bool swapped, int top_left, int top_right) {
  ComPtr<IWICImagingFactory> factory;
  Require(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)));
  const auto bytes = photo.EncodedBytes();
  Check(bytes[0] == std::byte{0xff} && bytes[1] == std::byte{0xd8}, "Output must be JPEG.");
  ComPtr<IWICStream> stream;
  Require(factory->CreateStream(&stream));
  Require(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<std::byte*>(bytes.data())),
                                       static_cast<DWORD>(bytes.size())));
  ComPtr<IWICBitmapDecoder> decoder;
  Require(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder));
  ComPtr<IWICBitmapFrameDecode> frame;
  Require(decoder->GetFrame(0, &frame));
  UINT width = 0, height = 0;
  Require(frame->GetSize(&width, &height));
  Check(width == (swapped ? 48U : 80U) && height == (swapped ? 80U : 48U),
        "Rotation must produce upright photo dimensions.");
  ComPtr<IWICFormatConverter> converter;
  Require(factory->CreateFormatConverter(&converter));
  Require(converter->Initialize(frame.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone,
                                nullptr, 0, WICBitmapPaletteTypeCustom));
  const int corners[] = {top_left, top_right, 3 - top_right, 3 - top_left};
  for (int index = 0; index < 4; ++index) {
    BYTE pixel[3]{};
    WICRect region{index % 2 ? static_cast<INT>(width) - 6 : 5,
                   index / 2 ? static_cast<INT>(height) - 6 : 5, 1, 1};
    Require(converter->CopyPixels(&region, 3, 3, pixel));
    const int corner = corners[index];
    Check((pixel[0] > 128) == (corner >= 2) && (pixel[1] > 128) == (corner % 2 == 1) &&
          (pixel[2] > 128) == (corner < 2), "Photo rotation or final mirroring moved the wrong corner.");
  }
}

Bytes WithExifOrientation(std::span<const std::byte> jpeg, unsigned orientation) {
  // A little-endian EXIF IFD containing only the orientation tag.
  const unsigned char app1[] = {
      0xff, 0xe1, 0x00, 0x22, 'E', 'x', 'i', 'f', 0, 0,
      'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0,
      0x12, 0x01, 3, 0, 1, 0, 0, 0, static_cast<unsigned char>(orientation), 0, 0, 0,
      0, 0, 0, 0};
  Bytes result(jpeg.begin(), jpeg.begin() + 2);
  for (const auto value : app1)
    result.push_back(std::byte{value});
  result.insert(result.end(), jpeg.begin() + 2, jpeg.end());
  return result;
}

void TestPhotos() {
  const auto original = MarkerBitmap();
  const int top_left[] = {0, 2, 3, 1};
  const int top_right[] = {1, 0, 2, 3};
  for (int rotation = 0; rotation < 4; ++rotation) {
    for (bool mirror : {false, true}) {
      const auto photo = camera::detail::EncodeWindowsPhoto(original, {.jpeg_quality = 98,
          .mirror = mirror ? MirrorMode::On : MirrorMode::Off}, static_cast<PreviewRotation>(rotation));
      CheckPhoto(photo, rotation % 2 != 0, mirror ? top_right[rotation] : top_left[rotation],
                  mirror ? top_left[rotation] : top_right[rotation]);
    }
  }
  const auto jpeg = camera::detail::EncodeWindowsPhoto(original, {.jpeg_quality = 98}, PreviewRotation::R0);
  const int exif_left[] = {0, 1, 3, 2, 0, 2, 3, 1};
  const int exif_right[] = {1, 0, 2, 3, 2, 0, 1, 3};
  for (unsigned orientation = 1; orientation <= 8; ++orientation) {
    const auto input = WithExifOrientation(jpeg.EncodedBytes(), orientation);
    for (int rotation = 0; rotation < 4; ++rotation) {
      for (bool mirror : {false, true}) {
        const auto photo = camera::detail::EncodeWindowsPhoto(input, {.jpeg_quality = 98,
            .mirror = mirror ? MirrorMode::On : MirrorMode::Off}, static_cast<PreviewRotation>(rotation));
        const auto index = orientation - 1;
        try {
          CheckPhoto(photo, orientation >= 5, mirror ? exif_right[index] : exif_left[index],
                      mirror ? exif_left[index] : exif_right[index]);
        } catch (const std::exception& error) {
          throw std::runtime_error("EXIF " + std::to_string(orientation) + ", stream rotation " +
              std::to_string(rotation * 90) + ", mirror " + std::to_string(mirror) + ": " + error.what());
        }
      }
    }
  }
  bool rejected = false;
  try {
    camera::detail::EncodeWindowsPhoto({}, {}, PreviewRotation::R0);
  } catch (const std::exception&) {
    rejected = true;
  }
  Check(rejected, "Invalid photo bytes must be rejected.");
  std::cout << "Windows JPEG encoding, stream rotations, eight EXIF orientations and final mirroring passed.\n";
}

struct DeviceProbe {
  std::mutex mutex;
  std::condition_variable ready;
  CameraStatus status;
  std::optional<CameraResult<ImageAsset>> photo;
  bool stopped = false;

  template <class Predicate> void Await(Predicate predicate, const char* message) {
    std::unique_lock lock(mutex);
    Check(ready.wait_for(lock, std::chrono::seconds(30), predicate), message);
  }
};

void TestDevice() {
  using namespace std::chrono_literals;
  const auto permission = winrt::Windows::Security::Authorization::AppCapabilityAccess::AppCapability::Create(L"webcam");
  std::cout << "Windows webcam CheckAccess status: " << static_cast<int>(permission.CheckAccess()) << '\n';
  auto camera = camera::detail::CreateWindowsCamera();
  const auto begin = [&](std::optional<Facing> facing) {
    auto probe = std::make_shared<DeviceProbe>();
    camera->Start(facing, [probe](CameraStatus value) {
      std::lock_guard lock(probe->mutex);
      probe->status = std::move(value);
      probe->ready.notify_all();
    });
    return probe;
  };
  const auto request_stop = [&](const std::shared_ptr<DeviceProbe>& probe) {
    camera->Stop([probe] {
      std::lock_guard lock(probe->mutex);
      probe->stopped = true;
      probe->ready.notify_all();
    });
  };
  const auto stop = [&](const std::shared_ptr<DeviceProbe>& probe) {
    request_stop(probe);
    probe->Await([&] { return probe->stopped; }, "Windows camera did not stop.");
  };
  std::optional<Facing> actual_facing;
  for (int run = 0; run < 2; ++run) {
    auto probe = begin(actual_facing);
    probe->Await([&] { return probe->status.state == SessionState::Running || probe->status.error.has_value(); },
                 "Windows camera did not start.");
    std::shared_ptr<ExternalTexture> texture;
    {
      std::lock_guard lock(probe->mutex);
      if (probe->status.error)
        throw std::runtime_error(probe->status.error->message);
      Check(probe->status.preview && probe->status.preview->texture, "Running camera must expose its preview.");
      texture = probe->status.preview->texture;
      actual_facing = probe->status.actual_facing;
      std::cout << "Windows preview: " << texture->IntrinsicSize().width << 'x' << texture->IntrinsicSize().height
                << ", facing=" << (probe->status.actual_facing ? (*probe->status.actual_facing == Facing::Front ? "front" : "back") : "unknown") << '\n';
    }
    const auto revision = texture->Revision();
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (texture->Revision() <= revision + 2 && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(20ms);
    Check(texture->Revision() > revision + 2, "Windows preview did not publish frames.");
    camera->CapturePhoto({.mirror = run == 0 ? MirrorMode::Off : MirrorMode::On}, [probe](CameraResult<ImageAsset> result) {
      std::lock_guard lock(probe->mutex);
      probe->photo = std::move(result);
      probe->ready.notify_all();
    });
    if (run == 1)
      request_stop(probe);
    probe->Await([&] { return probe->photo.has_value(); }, "Windows photo did not complete.");
    {
      std::lock_guard lock(probe->mutex);
      if (probe->photo->Succeeded())
        std::cout << "Windows photo: " << probe->photo->Value().PixelWidth() << 'x' << probe->photo->Value().PixelHeight() << '\n';
      else if (probe->photo->Error().code == CameraErrorCode::Unavailable)
        std::cout << "Windows photo unavailable on this device: " << probe->photo->Error().message << '\n';
      else
        throw std::runtime_error(probe->photo->Error().message);
    }
    if (run == 0)
      stop(probe);
    else
      probe->Await([&] { return probe->stopped; }, "Stop must finish after photo processing.");
    const auto final_revision = texture->Revision();
    std::this_thread::sleep_for(100ms);
    Check(texture->Revision() == final_revision, "Preview published after stop completion.");
    if (probe->photo->Succeeded())
      Check(!probe->photo->Value().EncodedBytes().empty(), "A photo must remain usable after stop.");
  }
  auto starting = begin({});
  stop(starting);
  for (const auto facing : {Facing::Front, Facing::Back}) {
    auto probe = begin(facing);
    probe->Await([&] { return probe->status.state == SessionState::Running || probe->status.error.has_value(); },
                 "Facing selection did not settle.");
    {
      std::lock_guard lock(probe->mutex);
      if (probe->status.error)
        Check(probe->status.error->code == CameraErrorCode::DeviceNotFound, "Missing facing must report DeviceNotFound.");
      else
        Check(probe->status.actual_facing == facing, "Explicit facing must not select a different camera facing.");
    }
    stop(probe);
  }
  auto closing = begin({});
  stop(closing);
  camera.reset();
  std::cout << "Windows preview, photos, pending-operation stops, facing selection and restart passed.\n";
}
} // namespace

int main(int argc, char** argv) {
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized))
    return 1;
  int result = 0;
  try {
    TestPhotos();
    if (argc > 1 && std::string_view(argv[1]) == "--device")
      TestDevice();
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  CoUninitialize();
  return result;
}
