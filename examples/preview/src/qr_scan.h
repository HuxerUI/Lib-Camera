#pragma once

#include <memory>
#include <string>
#include <vector>

#include <huxerui/huxerui.h>

namespace app {

/// One QR code decoded from a camera frame.
struct ScanCandidate {
  std::string text;
  std::string format;

  bool operator==(const ScanCandidate&) const = default;
};

/// Result of decoding one encoded camera frame.
/// An empty candidate list with an empty error means no QR code was present.
struct ScanOutcome {
  std::vector<ScanCandidate> candidates;
  std::string error;
};

/// Opaque platform decoder module. Only Android needs a platform image decoder;
/// desktop and Apple platforms decode in-process.
class QrDecoder;
using QrDecoderHandle = std::shared_ptr<QrDecoder>;

/// Reports whether this platform can turn a camera JPEG into QR payload bytes.
[[nodiscard]] bool QrScanningAvailable() noexcept;

/// Opens the platform decoder module. Returns an empty handle where decoding is local.
[[nodiscard]] QrDecoderHandle OpenQrDecoder();

/// Registers the platform decoder module when the platform needs one.
void InstallQrScanning(huxerui::ApplicationContext& context);

/// Decodes every QR code found in an encoded PNG/JPEG camera frame.
///
/// The candidate list is empty when no symbol is present. Decoding runs off
/// the UI thread except on Android, where the platform module delivers raw
/// pixels and the codec runs on the UI thread.
[[nodiscard]] huxerui::Task<ScanOutcome> ScanQrFromJpeg(huxerui::Bytes jpeg, QrDecoderHandle decoder);

} // namespace app
