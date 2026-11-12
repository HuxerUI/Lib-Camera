#import <Accelerate/Accelerate.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>

#include <stdexcept>
#include <type_traits>
#include <utility>

#include <huxerui/ios/external_texture.h>

#include "camera_recording_frame.h"

namespace huxerui::camera::detail {
namespace {

using PixelBuffer = std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>;

PixelBuffer ConvertBgra(CVPixelBufferRef input, std::uint32_t width, std::uint32_t height) {
  static const auto conversion = [] {
    vImage_ARGBToYpCbCr info{};
    // Match the video-range NV12 buffer and the BT.709 metadata supplied to the encoder.
    const vImage_YpCbCrPixelRange range{16, 128, 235, 240, 235, 16, 240, 16};
    if (vImageConvert_ARGBToYpCbCr_GenerateConversion(kvImage_ARGBToYpCbCrMatrix_ITU_R_709_2,
          &range, &info, kvImageARGB8888, kvImage420Yp8_CbCr8, kvImageNoFlags) != kvImageNoError) {
      throw std::runtime_error("Cannot configure BGRA to NV12 conversion.");
    }
    return info;
  }();

  CVPixelBufferRef output = nullptr;
  const auto attributes = @{(id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                            (id)kCVPixelBufferMetalCompatibilityKey: @YES};
  if (CVPixelBufferCreate(kCFAllocatorDefault, width, height, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
          (__bridge CFDictionaryRef)attributes, &output) != kCVReturnSuccess) {
    throw std::runtime_error("Cannot allocate an NV12 recording buffer.");
  }
  PixelBuffer converted(output, CVPixelBufferRelease);
  if (CVPixelBufferLockBaseAddress(input, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
    throw std::runtime_error("Cannot read the processed camera pixels.");
  }
  const auto unlock_input = [](CVPixelBufferRef buffer) { CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly); };
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(unlock_input)> input_lock(input, unlock_input);
  if (CVPixelBufferLockBaseAddress(output, 0) != kCVReturnSuccess) {
    throw std::runtime_error("Cannot write the NV12 recording buffer.");
  }
  const auto unlock_output = [](CVPixelBufferRef buffer) { CVPixelBufferUnlockBaseAddress(buffer, 0); };
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(unlock_output)> output_lock(output, unlock_output);
  const vImage_Buffer source{CVPixelBufferGetBaseAddress(input), height, width, CVPixelBufferGetBytesPerRow(input)};
  const auto plane = [output](std::size_t index) {
    return vImage_Buffer{CVPixelBufferGetBaseAddressOfPlane(output, index), CVPixelBufferGetHeightOfPlane(output, index),
                         CVPixelBufferGetWidthOfPlane(output, index), CVPixelBufferGetBytesPerRowOfPlane(output, index)};
  };
  const auto y = plane(0);
  const auto uv = plane(1);
  // Map the input's BGRA byte order to the ARGB channels expected by vImage.
  constexpr std::uint8_t bgra[] = {3, 2, 1, 0};
  if (vImageConvert_ARGB8888To420Yp8_CbCr8(&source, &y, &uv, &conversion, bgra, kvImageNoFlags) != kvImageNoError) {
    throw std::runtime_error("Cannot convert the processed camera pixels to NV12.");
  }
  CVBufferSetAttachment(output, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                        kCVAttachmentMode_ShouldPropagate);
  return converted;
}

} // namespace

media::CodecResult<media::VideoFrame> AcquireRecordingFrame(
    const std::shared_ptr<ExternalTexture>& texture, media::MediaTimestamp pts) {
  using namespace media;
  using R = CodecResult<VideoFrame>;
  const auto* source = dynamic_cast<const ios::PixelBufferTexture*>(texture.get());
  const PixelBuffer buffer(source ? source->AcquireFrame() : nullptr, CVPixelBufferRelease);
  if (!buffer) return R::Failure({CodecErrorCode::Unsupported, "Expected a published pixel buffer texture."});
  const auto width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(buffer.get()));
  const auto height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(buffer.get()));
  if (width < 2 || height < 2 || width % 2 || height % 2) {
    return R::Failure({CodecErrorCode::Unsupported, "Recording requires even pixel dimensions."});
  }
  const auto type = CVPixelBufferGetPixelFormatType(buffer.get());
  VideoFormat format{.width = width, .height = height, .visible = {0, 0, width, height},
      .pixel_format = VideoPixelFormat::NV12,
      .range = type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange ? ColorRange::Full : ColorRange::Limited};
  if (type != kCVPixelFormatType_32BGRA) return MakeVideoFrame(texture, format, pts);

  // Give the encoder its own NV12 snapshot while preview retains the published BGRA texture.
  @autoreleasepool {
    const auto converted = ConvertBgra(buffer.get(), width, height);
    auto encoding_texture = std::make_shared<ios::PixelBufferTexture>(Size{static_cast<float>(width), static_cast<float>(height)});
    encoding_texture->Publish(converted.get());
    encoding_texture->Finish();
    format.matrix = ColorMatrix::BT709;
    return MakeVideoFrame(std::move(encoding_texture), format, pts);
  }
}

} // namespace huxerui::camera::detail
