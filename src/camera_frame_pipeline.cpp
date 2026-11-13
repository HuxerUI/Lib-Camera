#include "camera_internal.h"

#include <algorithm>
#include <cmath>

namespace huxerui::camera::detail {

Rect CropForOutput(Rect crop, std::uint32_t source_width, std::uint32_t source_height,
                   std::uint32_t output_width, std::uint32_t output_height) {
  crop = crop.Intersection(full_texture_crop);
  if (crop.IsEmpty()) {
    throw CameraError{CameraErrorCode::ConfigurationFailed, "The camera output crop is empty."};
  }
  const auto aspect = (static_cast<double>(output_width) * source_height) /
                      (static_cast<double>(output_height) * source_width);
  const auto width = std::min(crop.width, static_cast<float>(crop.height * aspect));
  const auto height = std::min(crop.height, static_cast<float>(crop.width / aspect));
  return {crop.x + (crop.width - width) / 2, crop.y + (crop.height - height) / 2, width, height};
}

FramePipeline::FramePipeline(const CameraOptions& options)
    : factory_(options.frame_processor_factory),
      output_width_(options.output_pixel_width), output_height_(options.output_pixel_height) {}

std::pair<std::uint32_t, std::uint32_t> FramePipeline::ProducerSize(std::uint32_t width, std::uint32_t height) const noexcept {
  if (!factory_ && output_width_ && output_height_ &&
      static_cast<std::uint64_t>(width) * output_height_ == static_cast<std::uint64_t>(height) * output_width_) {
    return {output_width_, output_height_};
  }
  return {width, height};
}

CameraResult<ProcessedFrame> FramePipeline::Process(
    FrameProducer& producer, PreviewOutput input, std::chrono::nanoseconds capture_time,
    std::uint32_t pixel_width, std::uint32_t pixel_height,
    std::optional<std::chrono::nanoseconds> synchronized_time) {
  try {
    auto& processor = producer.processor;
    if (factory_) {
      if (!processor) {
        processor = (*factory_)(pixel_width, pixel_height);
        if (!processor) {
          return CameraResult<ProcessedFrame>::Failure(
              {CameraErrorCode::CaptureFailed, "The camera frame processor factory returned an empty processor."});
        }
      }
      auto result = processor(input, capture_time);
      if (!result.Succeeded()) return CameraResult<ProcessedFrame>::Failure(result.Error());
      input = std::move(result).Value();
    }
    if (!input.texture || !std::isfinite(input.crop.x) || !std::isfinite(input.crop.y) ||
        !std::isfinite(input.crop.width) || !std::isfinite(input.crop.height) ||
        input.crop.IsEmpty() || input.rotation < PreviewRotation::R0 || input.rotation > PreviewRotation::R270) {
      return CameraResult<ProcessedFrame>::Failure({CameraErrorCode::ConfigurationFailed, "Invalid processed camera frame."});
    }
    Rect source_crop = full_texture_crop;
    if (output_width_) {
      const auto crop = producer.resizer.Resize(input, output_width_, output_height_);
      if (!crop) return CameraResult<ProcessedFrame>::Success({});
      if (!factory_) source_crop = *crop;
    }
    OfferRecordingFrame(input, capture_time, synchronized_time);
    return CameraResult<ProcessedFrame>::Success({std::move(input), source_crop});
  } catch (const CameraError& error) {
    return CameraResult<ProcessedFrame>::Failure(error);
  } catch (const std::exception& error) {
    return CameraResult<ProcessedFrame>::Failure({CameraErrorCode::CaptureFailed, error.what()});
  } catch (...) {
    return CameraResult<ProcessedFrame>::Failure({CameraErrorCode::CaptureFailed, "The camera frame processor failed."});
  }
}

void FramePipeline::SetRecording(std::shared_ptr<VideoRecording> recording) {
  std::lock_guard lock(mutex_);
  recording_ = std::move(recording);
}

} // namespace huxerui::camera::detail
