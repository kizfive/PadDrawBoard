#pragma once

#include "pdb/video/async_mft_state.h"
#include "pdb/video/types.h"

#include <mftransform.h>
#include <mfidl.h>

#include <chrono>
#include <span>
#include <string_view>
#include <vector>

namespace pdb::video {

struct H264EncoderConfig {
  Size size;
  std::uint32_t frame_rate{60};
  std::uint32_t bitrate_bits_per_second{80'000'000};
};

struct H264EncoderCandidateDiagnostics {
  std::wstring name;
  std::uint32_t enumeration_index{};
  HRESULT setup_status{S_OK};
  HRESULT benchmark_status{S_OK};
  std::chrono::microseconds measured_latency{};
  std::uint32_t measured_frames{};
  bool asynchronous{};
  bool usable{};
};

// Some hardware encoders expose optional CodecAPI properties but reject a
// SetValue call when the property is not implemented by that MFT. In that
// case the documented encoder default remains authoritative.
[[nodiscard]] constexpr bool IsOptionalH264CodecApiPropertyFailure(HRESULT hr) noexcept {
  return hr == E_INVALIDARG || hr == E_NOTIMPL || hr == E_NOINTERFACE ||
         hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

// Benchmarking may leave a selected MFT with stream types that are no longer
// valid after the formal reset. Keep the required re-negotiation order small,
// pure, and directly testable.
enum class H264EncoderStreamSetupStep : std::uint8_t {
  kSetD3DManager,
  kConfigureCodecApi,
  kSetOutputType,
  kSetInputType,
  kBeginStreaming,
  kStartOfStream,
};

class H264EncoderStreamSetupSequence final {
 public:
  [[nodiscard]] bool Advance(H264EncoderStreamSetupStep step) noexcept {
    if (static_cast<std::uint8_t>(step) != next_step_) return false;
    ++next_step_;
    return true;
  }

  [[nodiscard]] bool complete() const noexcept {
    return next_step_ == kStepCount;
  }

  [[nodiscard]] bool media_types_configured() const noexcept {
    return next_step_ >=
           static_cast<std::uint8_t>(H264EncoderStreamSetupStep::kBeginStreaming);
  }

 private:
  static constexpr std::uint8_t kStepCount =
      static_cast<std::uint8_t>(H264EncoderStreamSetupStep::kStartOfStream) + 1;
  std::uint8_t next_step_{};
};

// Pure deterministic ordering used after runtime benchmarking. Unusable
// candidates never win; equal measurements prefer friendly name and then the
// stable MFT enumeration index.
[[nodiscard]] bool PreferH264EncoderCandidate(const H264EncoderCandidateDiagnostics& candidate,
                                              const H264EncoderCandidateDiagnostics& current) noexcept;

// Detects an IDR NAL unit in an Annex-B H.264 access unit. This is used as a
// fallback when an encoder omits MFSampleExtension_CleanPoint metadata.
[[nodiscard]] bool AnnexBAccessUnitContainsIdr(
    std::span<const std::uint8_t> access_unit) noexcept;

// CODECAPI_AVEncVideoForceKeyFrame is a ULONG (VT_UI4). A non-zero value
// requests that the next ProcessInput produce a key frame.
[[nodiscard]] VARIANT MakeForceKeyFrameVariant() noexcept;

class HardwareH264Encoder final {
 public:
  HardwareH264Encoder() = default;
  ~HardwareH264Encoder();
  HardwareH264Encoder(const HardwareH264Encoder&) = delete;
  HardwareH264Encoder& operator=(const HardwareH264Encoder&) = delete;

  // Selects a hardware NV12 -> H.264 MFT. Both synchronous and asynchronous
  // Media Foundation transforms are supported.
  [[nodiscard]] HRESULT Initialize(ID3D11Device* device, const H264EncoderConfig& config);
  void Shutdown();
  // Delivers asynchronous MFT events and any access unit already produced by
  // the encoder. S_FALSE means that no output is available yet.
  [[nodiscard]] HRESULT Poll(EncodedAccessUnit* output);
  [[nodiscard]] bool CanAcceptFrame() const noexcept;
  [[nodiscard]] bool HasPendingInput() const noexcept {
    return asynchronous_ && async_state_.HasPendingInput();
  }
  [[nodiscard]] std::optional<std::uint64_t> pending_input_sequence() const noexcept {
    return asynchronous_ ? async_state_.pending_sequence() : std::nullopt;
  }
  [[nodiscard]] HRESULT Encode(ID3D11Texture2D* nv12_frame, std::uint64_t sequence,
                               LONGLONG sample_time_100ns, EncodedAccessUnit* output);
  [[nodiscard]] HRESULT ResetAndRequestIdr();
  // An asynchronous fatal event requires the owner to reset the AVC stream at
  // its transport boundary. This consumes exactly one request.
  [[nodiscard]] bool TakeFatalRestartRequest() noexcept;
  [[nodiscard]] bool initialized() const noexcept { return transform_ != nullptr; }
  [[nodiscard]] const std::wstring& selected_name() const noexcept { return selected_name_; }
  [[nodiscard]] const std::vector<H264EncoderCandidateDiagnostics>& candidate_diagnostics() const noexcept {
    return candidate_diagnostics_;
  }
  [[nodiscard]] std::chrono::microseconds selected_benchmark_latency() const noexcept {
    return selected_benchmark_latency_;
  }
  [[nodiscard]] std::string_view last_failure_stage() const noexcept {
    return last_failure_stage_;
  }

 private:
  [[nodiscard]] HRESULT ConfigureTransform(const H264EncoderConfig& config);
  [[nodiscard]] HRESULT ReconfigureSelectedTransformForStream();
  [[nodiscard]] HRESULT ConfigureCodecApi();
  [[nodiscard]] HRESULT ProcessOutput(std::uint64_t sequence, EncodedAccessUnit* output);
  [[nodiscard]] HRESULT PumpAsyncEvents();
  [[nodiscard]] HRESULT ProcessAsyncOutput(EncodedAccessUnit* output);
  [[nodiscard]] HRESULT BenchmarkCurrentCandidate(ID3D11Device* device,
                                                  const H264EncoderConfig& config,
                                                  std::chrono::microseconds* latency,
                                                  std::uint32_t* measured_frames);
  [[nodiscard]] HRESULT MarkAsyncFatal(HRESULT failure) noexcept;
  void ShutdownCurrentCandidate() noexcept;
  void BeginAsyncShutdown() noexcept;

  ComPtr<ID3D11Device> device_;
  ComPtr<IMFDXGIDeviceManager> device_manager_;
  UINT device_manager_reset_token_{};
  ComPtr<IMFTransform> transform_;
  ComPtr<IMFMediaEventGenerator> event_generator_;
  H264EncoderConfig config_{};
  std::wstring selected_name_;
  AsyncMftStateMachine async_state_;
  bool asynchronous_{};
  bool fatal_restart_requested_{};
  HRESULT last_async_fatal_{S_OK};
  bool mf_started_{};
  std::vector<H264EncoderCandidateDiagnostics> candidate_diagnostics_;
  std::chrono::microseconds selected_benchmark_latency_{};
  std::string_view last_failure_stage_{"none"};
};

}  // namespace pdb::video
