#include "qr_scan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <huxerui/qrcode/codec.h>

using namespace huxerui;
namespace qrcode = huxerui::qrcode;

#if defined(__ANDROID__)
#include <huxerui/android/platform_registry.h>
#elif defined(__linux__)
#include <gdk-pixbuf/gdk-pixbuf.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#elif defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#endif

namespace app {
namespace {

using qrcode::BarcodeFormat;
using qrcode::DecodeOptions;
using qrcode::ImageView;
using qrcode::PixelFormat;

struct DecodedPixels {
  std::vector<std::uint8_t> bytes;
  int width = 0;
  int height = 0;
  PixelFormat format = PixelFormat::RGBA8;
};

/// Keeps camera frames small enough for a fast scan without losing QR modules.
constexpr int max_scan_dimension = 1280;

int ScaledDimension(int value, double scale) {
  return std::max(1, static_cast<int>(std::lround(static_cast<double>(value) * scale)));
}

ScanOutcome DecodePixels(const DecodedPixels& pixels) {
  try {
    const auto results = qrcode::DecodeAll(
        ImageView{std::span<const std::uint8_t>(pixels.bytes), pixels.width, pixels.height, pixels.format,
                  pixels.width * 4},
        DecodeOptions{.formats = {BarcodeFormat::QRCode}, .max_symbols = 8});
    ScanOutcome outcome;
    outcome.candidates.reserve(results.size());
    for (const auto& result : results) {
      outcome.candidates.push_back({.text = result.text, .format = "QR Code"});
    }
    return outcome;
  } catch (const std::exception& error) {
    return {.error = error.what()};
  }
}

#if !defined(__ANDROID__)
std::optional<DecodedPixels> DecodeEncodedImage(const Bytes& encoded, std::string& error) {
#if defined(__linux__)
  GError* failure = nullptr;
  GdkPixbufLoader* loader = gdk_pixbuf_loader_new();
  const bool written = gdk_pixbuf_loader_write(loader, reinterpret_cast<const guchar*>(encoded.data()),
                                               encoded.size(), &failure) != FALSE;
  const bool closed = written && gdk_pixbuf_loader_close(loader, &failure) != FALSE;
  if (!closed) {
    error = failure ? failure->message : "Cannot decode the camera JPEG.";
    if (failure) g_error_free(failure);
    g_object_unref(loader);
    return {};
  }
  GdkPixbuf* source = gdk_pixbuf_loader_get_pixbuf(loader);
  if (!source) {
    error = "The camera JPEG contains no image.";
    g_object_unref(loader);
    return {};
  }
  const int source_width = gdk_pixbuf_get_width(source);
  const int source_height = gdk_pixbuf_get_height(source);
  const double scale = std::min(1.0, static_cast<double>(max_scan_dimension) /
                                         static_cast<double>(std::max(source_width, source_height)));
  GdkPixbuf* scaled = nullptr;
  GdkPixbuf* rgba = nullptr;
  if (scale < 1.0) {
    scaled = gdk_pixbuf_scale_simple(source, ScaledDimension(source_width, scale),
                                     ScaledDimension(source_height, scale), GDK_INTERP_BILINEAR);
    if (!scaled) {
      error = "Cannot resize the camera frame for scanning.";
      g_object_unref(loader);
      return {};
    }
    rgba = gdk_pixbuf_copy(scaled);
    g_object_unref(scaled);
  } else {
    rgba = gdk_pixbuf_copy(source);
  }
  if (!rgba) {
    error = "Cannot copy the camera frame for scanning.";
    g_object_unref(loader);
    return {};
  }
  gdk_pixbuf_add_alpha(rgba, FALSE, 0, 0, 0);
  const int width = gdk_pixbuf_get_width(rgba);
  const int height = gdk_pixbuf_get_height(rgba);
  const int row_stride = gdk_pixbuf_get_rowstride(rgba);
  const auto* pixels = gdk_pixbuf_get_pixels(rgba);
  DecodedPixels decoded;
  decoded.bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  for (int row = 0; row < height; ++row) {
    std::memcpy(decoded.bytes.data() + static_cast<std::size_t>(row) * static_cast<std::size_t>(width) * 4U,
                pixels + static_cast<std::size_t>(row) * static_cast<std::size_t>(row_stride),
                static_cast<std::size_t>(width) * 4U);
  }
  decoded.width = width;
  decoded.height = height;
  decoded.format = PixelFormat::RGBA8;
  g_object_unref(rgba);
  g_object_unref(loader);
  return decoded;
#elif defined(_WIN32)
  const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  const bool uninitialize = initialized == S_OK || initialized == S_FALSE;
  struct ComGuard {
    bool active;
    ~ComGuard() {
      if (active) CoUninitialize();
    }
  } com_guard{uninitialize};
  if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE) {
    error = "Cannot initialize the Windows imaging component.";
    return {};
  }
  using Microsoft::WRL::ComPtr;
  HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, encoded.size());
  if (!memory) {
    error = "Cannot allocate the camera JPEG.";
    return {};
  }
  if (void* destination = GlobalLock(memory)) {
    std::memcpy(destination, encoded.data(), encoded.size());
    GlobalUnlock(memory);
  }
  ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(memory, TRUE, &stream))) {
    GlobalFree(memory);
    error = "Cannot open the camera JPEG.";
    return {};
  }
  ComPtr<IWICImagingFactory> factory;
  HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory));
  if (FAILED(result)) {
    error = "Cannot create the Windows imaging component.";
    return {};
  }
  ComPtr<IWICBitmapDecoder> decoder;
  result = factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder);
  if (FAILED(result)) {
    error = "Cannot decode the camera JPEG.";
    return {};
  }
  ComPtr<IWICBitmapFrameDecode> frame;
  result = decoder->GetFrame(0, &frame);
  if (FAILED(result)) {
    error = "The camera JPEG contains no frame.";
    return {};
  }
  UINT width = 0;
  UINT height = 0;
  result = frame->GetSize(&width, &height);
  if (FAILED(result) || width == 0 || height == 0) {
    error = "The camera frame is empty or too large to scan.";
    return {};
  }
  ComPtr<IWICBitmapSource> scaled_source;
  if (std::max(width, height) > static_cast<UINT>(max_scan_dimension)) {
    ComPtr<IWICBitmapScaler> scaler;
    result = factory->CreateBitmapScaler(&scaler);
    if (FAILED(result)) {
      error = "Cannot resize the camera frame for scanning.";
      return {};
    }
    const double scale = static_cast<double>(max_scan_dimension) /
                         static_cast<double>(std::max(width, height));
    const UINT scaled_width = static_cast<UINT>(ScaledDimension(static_cast<int>(width), scale));
    const UINT scaled_height = static_cast<UINT>(ScaledDimension(static_cast<int>(height), scale));
    result = scaler->Initialize(frame.Get(), scaled_width, scaled_height, WICBitmapInterpolationModeLinear);
    if (FAILED(result)) {
      error = "Cannot resize the camera frame for scanning.";
      return {};
    }
    scaled_source = scaler.Get();
    width = scaled_width;
    height = scaled_height;
  }
  IWICBitmapSource* source = scaled_source ? static_cast<IWICBitmapSource*>(scaled_source.Get()) : frame.Get();
  ComPtr<IWICFormatConverter> converter;
  result = factory->CreateFormatConverter(&converter);
  if (FAILED(result)) {
    error = "Cannot prepare the camera frame for scanning.";
    return {};
  }
  result = converter->Initialize(source, GUID_WICPixelFormat32bppBGRA,
                                 WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
  if (FAILED(result)) {
    error = "Cannot convert the camera frame for scanning.";
    return {};
  }
  DecodedPixels decoded;
  decoded.width = static_cast<int>(width);
  decoded.height = static_cast<int>(height);
  decoded.format = PixelFormat::BGRA8;
  const UINT stride = width * 4U;
  decoded.bytes.resize(static_cast<std::size_t>(stride) * height);
  result = converter->CopyPixels(nullptr, stride, static_cast<UINT>(decoded.bytes.size()), decoded.bytes.data());
  if (FAILED(result)) {
    error = "Cannot read the camera frame for scanning.";
    return {};
  }
  return decoded;
#elif defined(__APPLE__)
  if (encoded.size() > static_cast<std::size_t>(std::numeric_limits<CFIndex>::max())) {
    error = "The camera JPEG is too large.";
    return {};
  }
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(encoded.data()),
                                static_cast<CFIndex>(encoded.size()));
  if (!data) {
    error = "Cannot allocate the camera JPEG.";
    return {};
  }
  CGImageSourceRef image_source = CGImageSourceCreateWithData(data, nullptr);
  CGImageRef image = image_source ? CGImageSourceCreateImageAtIndex(image_source, 0, nullptr) : nullptr;
  if (!image) {
    error = "Cannot decode the camera JPEG.";
    if (image_source) CFRelease(image_source);
    CFRelease(data);
    return {};
  }
  const int source_width = static_cast<int>(CGImageGetWidth(image));
  const int source_height = static_cast<int>(CGImageGetHeight(image));
  const double scale = std::min(1.0, static_cast<double>(max_scan_dimension) /
                                         static_cast<double>(std::max(source_width, source_height)));
  const int width = ScaledDimension(source_width, scale);
  const int height = ScaledDimension(source_height, scale);
  DecodedPixels decoded;
  decoded.width = width;
  decoded.height = height;
  decoded.format = PixelFormat::RGBA8;
  decoded.bytes.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U);
  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(
      decoded.bytes.data(), width, height, 8, static_cast<std::size_t>(width) * 4U, color_space,
      kCGImageAlphaNoneSkipLast | kCGBitmapByteOrder32Big);
  if (!context) {
    error = "Cannot prepare the camera frame for scanning.";
    if (color_space) CGColorSpaceRelease(color_space);
    CGImageRelease(image);
    CFRelease(image_source);
    CFRelease(data);
    return {};
  }
  CGContextSetInterpolationQuality(context, kCGInterpolationLow);
  CGContextTranslateCTM(context, 0.0, static_cast<CGFloat>(height));
  CGContextScaleCTM(context, 1.0, -1.0);
  CGContextDrawImage(context, CGRectMake(0, 0, static_cast<CGFloat>(width), static_cast<CGFloat>(height)), image);
  CGContextRelease(context);
  CGColorSpaceRelease(color_space);
  CGImageRelease(image);
  CFRelease(image_source);
  CFRelease(data);
  return decoded;
#else
  static_cast<void>(encoded);
  error = "QR scanning is unavailable on this platform.";
  return {};
#endif
}

ScanOutcome ScanEncodedImage(const Bytes& encoded) {
  std::string error;
  auto pixels = DecodeEncodedImage(encoded, error);
  if (!pixels) {
    return {.error = std::move(error)};
  }
  return DecodePixels(*pixels);
}
#endif // !defined(__ANDROID__)

} // namespace

#if defined(__ANDROID__)
class QrDecoder {
public:
  explicit QrDecoder(PlatformChannel channel) : channel_(std::move(channel)) {}
  ~QrDecoder() { channel_.Close(); }

  std::function<void()> Decode(Bytes jpeg, std::function<void(PlatformResult<Bytes>)> completed) {
    const auto request = channel_.Invoke<Bytes>("decode", std::move(jpeg), std::move(completed));
    return [channel = channel_, request] { channel.Cancel(request); };
  }

private:
  PlatformChannel channel_;
};
#endif

bool QrScanningAvailable() noexcept {
#if defined(__ANDROID__) || defined(__linux__) || defined(_WIN32) || defined(__APPLE__)
  return true;
#else
  return false;
#endif
}

QrDecoderHandle OpenQrDecoder() {
#if defined(__ANDROID__)
  try {
    return OpenPlatformModule<QrDecoderHandle>("example/qr-image-decoder");
  } catch (...) {
    return {};
  }
#else
  return {};
#endif
}

void InstallQrScanning(ApplicationContext& context) {
#if defined(__ANDROID__)
  android::JavaPlatformModuleFactory<QrDecoderHandle> factory;
  factory.class_name = "org.huxerui.lib.camera.preview.QrImageDecoderModule";
  factory.create = [](PlatformChannel channel) {
    return std::make_shared<QrDecoder>(std::move(channel));
  };
  context.RegisterPlatformModule<QrDecoderHandle>("example/qr-image-decoder", std::move(factory));
#else
  static_cast<void>(context);
#endif
}

Task<ScanOutcome> ScanQrFromJpeg(Bytes jpeg, QrDecoderHandle decoder) {
#if defined(__ANDROID__)
  return AwaitCallback<ScanOutcome>(
      [jpeg = std::move(jpeg), decoder](CallbackCompletion<ScanOutcome> completion) -> std::function<void()> {
        if (!decoder) {
          completion.Complete({.error = "QR scanning is unavailable."});
          return {};
        }
        auto cancel = decoder->Decode(
            std::move(jpeg), [completion](PlatformResult<Bytes> result) mutable {
              if (const auto* error = std::get_if<PlatformError>(&result)) {
                completion.Complete({.error = error->message});
                return;
              }
              const auto& payload = std::get<Bytes>(result);
              if (payload.size() < 8) {
                completion.Complete({.error = "The platform returned an invalid scan frame."});
                return;
              }
              const auto* bytes = reinterpret_cast<const std::uint8_t*>(payload.data());
              const auto read_u32 = [](const std::uint8_t* value) {
                return (static_cast<std::uint32_t>(value[0]) << 24U) |
                       (static_cast<std::uint32_t>(value[1]) << 16U) |
                       (static_cast<std::uint32_t>(value[2]) << 8U) | static_cast<std::uint32_t>(value[3]);
              };
              const std::uint32_t width = read_u32(bytes);
              const std::uint32_t height = read_u32(bytes + 4);
              const std::size_t required = 8U + static_cast<std::size_t>(width) * height * 4U;
              if (width == 0 || height == 0 || width > 65535 || height > 65535 || payload.size() < required) {
                completion.Complete({.error = "The platform returned an invalid scan frame."});
                return;
              }
              DecodedPixels pixels;
              pixels.width = static_cast<int>(width);
              pixels.height = static_cast<int>(height);
              pixels.format = PixelFormat::RGBA8;
              pixels.bytes.assign(bytes + 8, bytes + required);
              completion.Complete(DecodePixels(pixels));
            });
        return cancel;
      });
#else
  return RunWorker([jpeg = std::move(jpeg)] { return ScanEncodedImage(jpeg); });
#endif
}

} // namespace app
