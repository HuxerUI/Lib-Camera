#include <huxerui/camera.h>

#include <algorithm>
#include <cmath>

#include "camera_internal.h"

namespace huxerui::camera::detail {

std::optional<PreviewPlacement> PlacePreview(
    Size intrinsic, const PreviewOutput& output, PreviewOptions options, std::optional<Facing> facing, Size bounds
) {
  const auto positive = [](float value) { return std::isfinite(value) && value > 0.0F; };
  if (!positive(intrinsic.width) || !positive(intrinsic.height) ||
      !positive(bounds.width) || !positive(bounds.height) ||
      !std::isfinite(output.crop.x) || !std::isfinite(output.crop.y) ||
      !positive(output.crop.width) || !positive(output.crop.height)) {
    return std::nullopt;
  }
  const float left = std::clamp(output.crop.x, 0.0F, 1.0F);
  const float top = std::clamp(output.crop.y, 0.0F, 1.0F);
  const float right = std::clamp(output.crop.x + output.crop.width, 0.0F, 1.0F);
  const float bottom = std::clamp(output.crop.y + output.crop.height, 0.0F, 1.0F);
  const Rect source{left * intrinsic.width, top * intrinsic.height,
                    (right - left) * intrinsic.width, (bottom - top) * intrinsic.height};
  if (source.IsEmpty()) {
    return std::nullopt;
  }
  Transform2D rotation;
  Size upright{source.width, source.height};
  switch (output.rotation) {
    case PreviewRotation::R0: break;
    case PreviewRotation::R90:
      rotation = {0.0F, 1.0F, -1.0F, 0.0F, source.height, 0.0F};
      upright = {source.height, source.width};
      break;
    case PreviewRotation::R180:
      rotation = {-1.0F, 0.0F, 0.0F, -1.0F, source.width, source.height};
      break;
    case PreviewRotation::R270:
      rotation = {0.0F, -1.0F, 1.0F, 0.0F, 0.0F, source.width};
      upright = {source.height, source.width};
      break;
  }
  float sx = bounds.width / upright.width;
  float sy = bounds.height / upright.height;
  switch (options.fit) {
    case ImageFit::None: sx = sy = 1.0F; break;
    case ImageFit::Contain: sx = sy = std::min(sx, sy); break;
    case ImageFit::Cover: sx = sy = std::max(sx, sy); break;
    case ImageFit::Fill: break;
    case ImageFit::ScaleDown: sx = sy = std::min({1.0F, sx, sy}); break;
  }
  const bool desired_mirror = options.mirror == MirrorMode::On ||
      (options.mirror == MirrorMode::Auto && facing == Facing::Front);
  if (output.mirrored != desired_mirror) {
    rotation.m11 = -rotation.m11;
    rotation.m21 = -rotation.m21;
    rotation.translate_x = upright.width - rotation.translate_x;
  }
  rotation.m11 *= sx;
  rotation.m21 *= sx;
  rotation.m12 *= sy;
  rotation.m22 *= sy;
  rotation.translate_x = rotation.translate_x * sx + (bounds.width - upright.width * sx) * 0.5F;
  rotation.translate_y = rotation.translate_y * sy + (bounds.height - upright.height * sy) * 0.5F;
  return PreviewPlacement{source, {0.0F, 0.0F, source.width, source.height}, rotation};
}

std::optional<Point> PreviewTexturePoint(const PreviewOutput& output, PreviewOptions options,
                                        std::optional<Facing> facing, Size bounds, Point position) {
  if (!output.texture || !Rect{0.0F, 0.0F, bounds.width, bounds.height}.Contains(position)) return {};
  const auto intrinsic = output.texture->IntrinsicSize();
  const auto placement = PlacePreview(intrinsic, output, options, facing, bounds);
  if (!placement) return {};
  const auto local = placement->transform.Inverse(position);
  if (!local || !placement->destination.Contains(*local)) return {};
  return Point{(placement->source.x + local->x) / intrinsic.width,
               (placement->source.y + local->y) / intrinsic.height};
}

} // namespace huxerui::camera::detail

namespace huxerui::camera {

[[huxerui::composable]]
View CameraPreview(CameraSession session, PreviewOptions options) {
  const auto status = session.Status();
  return Canvas([output = status.preview, facing = status.actual_facing, options](PaintContext& paint, Size size) {
    if (!output || !output->texture) {
      return;
    }
    const auto placement = detail::PlacePreview(output->texture->IntrinsicSize(), *output, options, facing, size);
    if (!placement) {
      return;
    }
    paint.PushClip({0.0F, 0.0F, size.width, size.height});
    paint.PushTransform(placement->transform);
    paint.DrawImageRect(output->texture, placement->source, placement->destination);
    paint.PopTransform();
    paint.PopClip();
  });
}

} // namespace huxerui::camera
