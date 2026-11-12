#include "camera_recording_audio.h"

#include <algorithm>
#include <limits>

#include <huxerui/camera.h>

#if defined(__ANDROID__)
#include <time.h>
#elif defined(__APPLE__)
#include <CoreMedia/CoreMedia.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace huxerui::camera::detail {
namespace {

using std::chrono::nanoseconds;

std::uint32_t SampleIndex(nanoseconds offset, std::uint32_t count, std::uint32_t rate, bool round_up) {
  if (offset.count() <= 0) return 0;
  const auto limit = static_cast<std::uint64_t>(count) * capture_timescale;
  const auto time = static_cast<std::uint64_t>(offset.count());
  if (time > limit / rate) return count;
  const auto scaled = time * rate;
  const auto rounding = round_up ? capture_timescale - 1 : 0;
  return static_cast<std::uint32_t>((scaled + rounding) / capture_timescale);
}

} // namespace

nanoseconds RecordingClockNow() {
#if defined(__ANDROID__)
  timespec time{};
  if (clock_gettime(CLOCK_BOOTTIME, &time) == 0) {
    return std::chrono::seconds(time.tv_sec) + nanoseconds(time.tv_nsec);
  }
#elif defined(__APPLE__)
  const auto time = CMTimeConvertScale(CMClockGetTime(CMClockGetHostTimeClock()), capture_timescale,
                                     kCMTimeRoundingMethod_Default);
  if (CMTIME_IS_NUMERIC(time)) return nanoseconds(time.value);
#elif defined(_WIN32)
  LARGE_INTEGER counter{}, rate{};
  if (QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&rate) &&
      rate.QuadPart > 0 && rate.QuadPart <= std::numeric_limits<std::uint32_t>::max()) {
    const auto time = media::MediaTimestamp{counter.QuadPart, static_cast<std::uint32_t>(rate.QuadPart)}
                          .Rescale(capture_timescale);
    if (time) return nanoseconds(time->ticks);
  }
#endif
  throw CameraError{CameraErrorCode::Unavailable, "The shared camera and microphone clock is unavailable."};
}

audio::CaptureClock RecordingCaptureClock() noexcept {
#if defined(__ANDROID__)
  return audio::CaptureClock::BootTime;
#elif defined(__APPLE__)
  return audio::CaptureClock::CoreMediaHostTime;
#elif defined(_WIN32)
  return audio::CaptureClock::QueryPerformanceCounter;
#endif
}

RecordingTimeline::RecordingTimeline(nanoseconds start) : intervals_{{start, {}, {}}} {}

void RecordingTimeline::Pause(nanoseconds time) { intervals_.back().end = time; }

void RecordingTimeline::Resume(nanoseconds time) {
  intervals_.push_back({time, {}, Duration()});
}

nanoseconds RecordingTimeline::Stop(nanoseconds time) {
  if (!intervals_.back().end) Pause(time);
  return *intervals_.back().end;
}

std::optional<nanoseconds> RecordingTimeline::Map(nanoseconds time) const {
  for (auto it = intervals_.rbegin(); it != intervals_.rend(); ++it) {
    if (time < it->start) continue;
    if (it->end && time >= *it->end) return {};
    return it->recorded_start + (time - it->start);
  }
  return {};
}

nanoseconds RecordingTimeline::Duration() const {
  const auto& last = intervals_.back();
  return last.recorded_start + (*last.end - last.start);
}

void RecordingTimeline::RetainAudio(const media::AudioFrame& frame, std::deque<media::AudioFrame>& output) const {
  const auto pts = frame.pts.Rescale(capture_timescale);
  if (!pts || pts->ticks < 0 || !frame.Validate() || frame.planes.size() != 1) {
    throw CameraError{CameraErrorCode::CaptureFailed, "The microphone returned invalid timestamped PCM."};
  }
  const auto start = nanoseconds(pts->ticks);
  const auto rate = frame.format.sample_rate;
  const auto duration = nanoseconds((static_cast<std::uint64_t>(frame.sample_count) * capture_timescale + rate - 1) /
                                    rate);
  const auto bytes_per_frame = frame.format.ChannelCount() * frame.format.BytesPerSample();
  const auto first_interval = std::lower_bound(intervals_.begin(), intervals_.end(), start,
      [](const Interval& interval, nanoseconds time) { return interval.end && *interval.end <= time; });
  for (auto it = first_interval; it != intervals_.end() && it->start < start + duration; ++it) {
    const auto first = SampleIndex(it->start - start, frame.sample_count, rate, true);
    const auto end = it->end ? SampleIndex(*it->end - start, frame.sample_count, rate, false) : frame.sample_count;
    if (end <= first) continue;
    const auto offset = nanoseconds(media::MediaTimestamp{first, rate}.Rescale(capture_timescale)->ticks);
    const auto time = it->recorded_start + (start + offset - it->start);
    output.push_back({.format = frame.format, .pts = {time.count(), capture_timescale}, .sample_count = end - first,
        .planes = {frame.planes.front().Slice(static_cast<std::size_t>(first) * bytes_per_frame,
                                             static_cast<std::size_t>(end - first) * bytes_per_frame)}});
  }
}

} // namespace huxerui::camera::detail
