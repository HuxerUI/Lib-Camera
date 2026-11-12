#pragma once

#include <chrono>
#include <deque>
#include <optional>
#include <vector>

#include <huxerui/audiocapture.h>

namespace huxerui::camera::detail {

inline constexpr std::uint32_t capture_timescale = 1'000'000'000;

/// The host clock shared by camera capture and the platform microphone.
std::chrono::nanoseconds RecordingClockNow();
audio::CaptureClock RecordingCaptureClock() noexcept;

/// Maps both tracks through the same set of active capture intervals.
/// Access is serialized by the recording worker's mutex.
class RecordingTimeline {
public:
  explicit RecordingTimeline(std::chrono::nanoseconds start);
  void Pause(std::chrono::nanoseconds time);
  void Resume(std::chrono::nanoseconds time);
  std::chrono::nanoseconds Stop(std::chrono::nanoseconds time);
  std::optional<std::chrono::nanoseconds> Map(std::chrono::nanoseconds time) const;
  std::chrono::nanoseconds Duration() const;
  void RetainAudio(const media::AudioFrame& frame, std::deque<media::AudioFrame>& output) const;

private:
  struct Interval {
    std::chrono::nanoseconds start;
    std::optional<std::chrono::nanoseconds> end;
    std::chrono::nanoseconds recorded_start;
  };
  std::vector<Interval> intervals_;
};

} // namespace huxerui::camera::detail
