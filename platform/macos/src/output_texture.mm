#import <CoreImage/CoreImage.h>
#import <CoreVideo/CoreVideo.h>

#include <stdexcept>
#include <type_traits>

#include <huxerui/macos/external_texture.h>

#include "camera_internal.h"

namespace huxerui::camera::detail {
namespace {

using PixelBuffer = std::unique_ptr<std::remove_pointer_t<CVPixelBufferRef>, decltype(&CVPixelBufferRelease)>;

} // namespace

struct TextureResizer::State {
  CIContext* context = [CIContext contextWithOptions:@{kCIContextCacheIntermediates: @NO}];
  CGColorSpaceRef color_space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CVPixelBufferPoolRef pool = nullptr;
  std::shared_ptr<macos::PixelBufferTexture> texture;

  ~State() {
    if (texture) texture->Finish();
    if (pool) CVPixelBufferPoolRelease(pool);
    if (color_space) CGColorSpaceRelease(color_space);
  }

  void Initialize(std::uint32_t width, std::uint32_t height) {
    const auto attributes = @{(id)kCVPixelBufferWidthKey: @(width),
                              (id)kCVPixelBufferHeightKey: @(height),
                              (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
                              (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
                              (id)kCVPixelBufferMetalCompatibilityKey: @YES};
    if (CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr, (__bridge CFDictionaryRef)attributes, &pool) !=
        kCVReturnSuccess) {
      throw CameraError{CameraErrorCode::ConfigurationFailed, "Cannot configure camera output pixel buffers."};
    }
    texture = std::make_shared<macos::PixelBufferTexture>(Size{static_cast<float>(width), static_cast<float>(height)});
  }
};

TextureResizer::TextureResizer() = default;
TextureResizer::~TextureResizer() = default;
void TextureResizer::Reset() { state_.reset(); }

bool TextureResizer::Resize(PreviewOutput& frame, std::uint32_t width, std::uint32_t height) {
  @autoreleasepool {
    const auto* source = dynamic_cast<const macos::PixelBufferTexture*>(frame.texture.get());
    const PixelBuffer input(source ? source->AcquireFrame() : nullptr, CVPixelBufferRelease);
    if (!input) {
      throw CameraError{CameraErrorCode::ConfigurationFailed,
          "Expected a published PixelBufferTexture for camera output sizing."};
    }
    const auto source_width = static_cast<std::uint32_t>(CVPixelBufferGetWidth(input.get()));
    const auto source_height = static_cast<std::uint32_t>(CVPixelBufferGetHeight(input.get()));
    if (width == source_width && height == source_height && frame.crop == full_texture_crop) return true;
    const auto crop = CropForOutput(frame.crop, source_width, source_height, width, height);

    if (!state_) {
      state_ = std::make_unique<State>();
      state_->Initialize(width, height);
    }
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, state_->pool, &buffer) != kCVReturnSuccess) {
      throw std::runtime_error("Cannot allocate a camera output pixel buffer.");
    }
    const PixelBuffer output(buffer, CVPixelBufferRelease);
    CIImage* image = [CIImage imageWithCVPixelBuffer:input.get()];
    if (!image) throw std::runtime_error("Cannot import the camera frame into Core Image.");

    // Convert the normalized top-left crop to Core Image's bottom-left coordinates.
    const auto x = static_cast<CGFloat>(source_width) * crop.x;
    const auto y = static_cast<CGFloat>(source_height) * (1.0 - crop.y - crop.height);
    const auto scale_x = width / (static_cast<CGFloat>(source_width) * crop.width);
    const auto scale_y = height / (static_cast<CGFloat>(source_height) * crop.height);
    const auto transform = CGAffineTransformMake(scale_x, 0, 0, scale_y, -x * scale_x, -y * scale_y);
    image = [[image imageByClampingToExtent] imageByApplyingTransform:transform];
    [state_->context render:image toCVPixelBuffer:output.get()
        bounds:CGRectMake(0, 0, width, height) colorSpace:state_->color_space];
    state_->texture->Publish(output.get());
    frame.texture = state_->texture;
    frame.crop = full_texture_crop;
    return true;
  }
}

} // namespace huxerui::camera::detail
