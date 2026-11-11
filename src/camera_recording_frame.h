#pragma once

#include <huxerui/camera.h>
#include <huxerui/media_codec.h>

namespace huxerui::camera::detail {

/// @brief Acquires a retained frame in a format accepted by the platform video encoder.
///
/// Call on the frame queue before the next texture publication. The retained frame can be moved to the
/// recording worker and submitted after capture advances.
/// @param texture Texture with a completed publication for this frame.
/// @param pts Presentation timestamp associated with the publication.
/// @return A retained encoder input frame or a codec error describing why acquisition failed.
media::CodecResult<media::VideoFrame> AcquireRecordingFrame(
    const std::shared_ptr<ExternalTexture>& texture, media::MediaTimestamp pts);

} // namespace huxerui::camera::detail
