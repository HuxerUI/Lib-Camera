#pragma once

#include <CoreVideo/CoreVideo.h>

#include <huxerui/macos/external_texture.h>
#include <huxerui/media_codec.h>
#include <huxerui/media_container.h>

#include "camera_test_fixture.h"

namespace huxerui::camera::test {

template <class T, class E> T TakeRecordingResult(Result<T, E> result) {
  if (!result.Succeeded()) throw std::runtime_error(result.Error().message);
  if constexpr (!std::is_void_v<T>) return std::move(result).Value();
}

inline std::shared_ptr<ExternalTexture> SolidTexture(std::uint8_t red, std::uint8_t green,
                                                     int width = 64, int height = 64) {
  CVPixelBufferRef pixels = nullptr;
  Check(CVPixelBufferCreate(nullptr, width, height, kCVPixelFormatType_32BGRA, nullptr, &pixels) == kCVReturnSuccess,
        "Cannot allocate a test camera frame.");
  const std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>
      owner(pixels, CVPixelBufferRelease);
  Check(CVPixelBufferLockBaseAddress(pixels, 0) == kCVReturnSuccess, "Cannot lock a test camera frame.");
  for (int row = 0; row < height; ++row) {
    auto* pixel = static_cast<std::uint8_t*>(CVPixelBufferGetBaseAddress(pixels)) + row * CVPixelBufferGetBytesPerRow(pixels);
    for (int column = 0; column < width; ++column) {
      *pixel++ = 0;
      *pixel++ = green;
      *pixel++ = red;
      *pixel++ = 255;
    }
  }
  CVPixelBufferUnlockBaseAddress(pixels, 0);
  auto texture = std::make_shared<macos::PixelBufferTexture>(Size{static_cast<float>(width), static_cast<float>(height)});
  texture->Publish(pixels);
  return texture;
}

} // namespace huxerui::camera::test
