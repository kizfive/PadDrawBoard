#include "pdb/video/async_mft_state.h"
#include "pdb/video/backpressure_policy.h"
#include "pdb/video/desktop_duplication_capture.h"
#include "pdb/video/h264_encoder.h"
#include "pdb/video/latest_frame_queue.h"
#include "pdb/video/monitor_enumerator.h"
#include "pdb/video/resolution_ladder.h"
#include "pdb/video/telemetry.h"

#include <cassert>
#include <chrono>
#include <initializer_list>
#include <string>
#include <vector>

namespace pdb::video::test {

void PrivateCopyTextureDescriptionSupportsVideoProcessorInputView() {
  D3D11_TEXTURE2D_DESC source{};
  source.Width = 3200;
  source.Height = 2136;
  source.MipLevels = 4;
  source.ArraySize = 2;
  source.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  source.SampleDesc.Count = 8;
  source.SampleDesc.Quality = 7;
  source.Usage = D3D11_USAGE_STAGING;
  source.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  source.CPUAccessFlags = D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE;
  source.MiscFlags = D3D11_RESOURCE_MISC_SHARED;

  const auto normalized = NormalizePrivateCopyTextureDesc(source);
  assert(normalized.Width == source.Width);
  assert(normalized.Height == source.Height);
  assert(normalized.MipLevels == source.MipLevels);
  assert(normalized.ArraySize == source.ArraySize);
  assert(normalized.Format == source.Format);
  assert(normalized.BindFlags ==
         (D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
  assert(normalized.Usage == D3D11_USAGE_DEFAULT);
  assert(normalized.CPUAccessFlags == 0);
  assert(normalized.MiscFlags == 0);
  assert(normalized.SampleDesc.Count == 1);
  assert(normalized.SampleDesc.Quality == 0);
}

void ResolutionLadderPrefersNativeFitAndMaintainsAspectRatio() {
  assert((SelectEncodeResolution({3200, 2136}, 3200) == Size{3200, 2136}));
  assert((SelectEncodeResolution({3840, 2160}, 3200) == Size{3200, 1800}));
  assert((SelectEncodeResolution({3840, 2160}, 2560) == Size{2560, 1440}));
  assert((SelectEncodeResolution({3201, 2137}, 3200) == Size{3200, 2136}));
  assert((SelectEncodeResolution({1920, 1080}, 1919) == Size{}));
}

void ResolutionLadderStepsDownWithoutUpscaling() {
  assert((NextLowerResolution({3200, 1800}, {3840, 2160}) == Size{2560, 1440}));
  assert((NextLowerResolution({2560, 1440}, {3840, 2160}) == Size{1920, 1080}));
  assert((NextLowerResolution({1920, 1080}, {3840, 2160}) == Size{}));
}

void AdaptiveResolutionControllerUsesPersistentBreachesAndStableUpgrade() {
  AdaptiveResolutionController controller;
  controller.Start(3200);
  controller.SetNativeSize({3840, 2160});
  const auto start = SteadyTime{} + std::chrono::seconds(1);

  ResolutionPolicyObservation breach{};
  breach.timestamp = start;
  breach.queue_age = std::chrono::milliseconds(51);
  assert(controller.Observe(breach).action == ResolutionPolicyAction::kNone);
  breach.timestamp += std::chrono::milliseconds(1);
  assert(controller.Observe(breach).action == ResolutionPolicyAction::kNone);
  breach.timestamp += std::chrono::milliseconds(1);
  const auto downgrade = controller.Observe(breach);
  assert(downgrade.action == ResolutionPolicyAction::kDowngrade);
  assert(downgrade.request_stream_reset);
  assert(downgrade.max_longest_edge == 2560);
  assert((downgrade.encode_size == Size{2560, 1440}));

  ResolutionPolicyObservation clean{};
  clean.timestamp = breach.timestamp + std::chrono::seconds(9);
  assert(controller.Observe(clean).action == ResolutionPolicyAction::kNone);
  clean.timestamp += std::chrono::seconds(1);
  const auto upgrade = controller.Observe(clean);
  assert(upgrade.action == ResolutionPolicyAction::kUpgrade);
  assert(upgrade.max_longest_edge == 3200);
  assert((upgrade.encode_size == Size{3200, 1800}));
}

void AdaptiveResolutionControllerNeverDropsBelowMinimumRung() {
  AdaptiveResolutionController controller;
  controller.Start(3200);
  controller.SetNativeSize({3840, 2160});
  auto timestamp = SteadyTime{} + std::chrono::seconds(1);
  ResolutionPolicyObservation breach{};
  breach.queue_age = std::chrono::milliseconds(51);
  for (int rung = 0; rung < 6; ++rung) {
    breach.timestamp = timestamp;
    timestamp += std::chrono::milliseconds(1);
    static_cast<void>(controller.Observe(breach));
  }
  assert(controller.current_max_longest_edge() == 1920);
  breach.timestamp = timestamp;
  static_cast<void>(controller.Observe(breach));
  assert(controller.current_max_longest_edge() == 1920);
}

void EncoderSelectionUsesDeterministicTieBreak() {
  H264EncoderCandidateDiagnostics current{};
  current.usable = true;
  current.name = L"AMD Hardware H.264";
  current.enumeration_index = 4;
  current.measured_latency = std::chrono::microseconds(100);

  H264EncoderCandidateDiagnostics faster = current;
  faster.name = L"NVIDIA Hardware H.264";
  faster.measured_latency = std::chrono::microseconds(99);
  assert(PreferH264EncoderCandidate(faster, current));

  H264EncoderCandidateDiagnostics same_latency = current;
  same_latency.name = L"A Hardware H.264";
  same_latency.enumeration_index = 1;
  assert(PreferH264EncoderCandidate(same_latency, current));
  assert(!PreferH264EncoderCandidate(current, same_latency));

  H264EncoderCandidateDiagnostics unusable = current;
  unusable.usable = false;
  assert(!PreferH264EncoderCandidate(unusable, current));
  assert(PreferH264EncoderCandidate(current, unusable));
}

void EncoderFormalStreamSetupRequiresTypesBeforeStart() {
  H264EncoderStreamSetupSequence sequence;
  assert(!sequence.complete());
  assert(!sequence.media_types_configured());
  assert(!sequence.Advance(H264EncoderStreamSetupStep::kStartOfStream));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kSetD3DManager));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kConfigureCodecApi));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kSetOutputType));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kSetInputType));
  assert(!sequence.Advance(H264EncoderStreamSetupStep::kStartOfStream));
  assert(sequence.media_types_configured());
  assert(sequence.Advance(H264EncoderStreamSetupStep::kBeginStreaming));
  assert(sequence.Advance(H264EncoderStreamSetupStep::kStartOfStream));
  assert(sequence.complete());
}

void OptionalCodecApiPropertyFailuresUseDocumentedDefaults() {
  assert(IsOptionalH264CodecApiPropertyFailure(E_INVALIDARG));
  assert(IsOptionalH264CodecApiPropertyFailure(E_NOTIMPL));
  assert(IsOptionalH264CodecApiPropertyFailure(E_NOINTERFACE));
  assert(IsOptionalH264CodecApiPropertyFailure(HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED)));
  assert(!IsOptionalH264CodecApiPropertyFailure(E_FAIL));
}

void LatestFrameQueueReplacesOnlyPendingRawFrame() {
  LatestFrameQueue<std::string> queue;
  queue.Push("old");
  queue.Push("new");
  const auto value = queue.TryPop();
  assert(value.has_value() && *value == "new");
  assert(queue.replaced_count() == 1);
  assert(!queue.TryPop().has_value());
}

void LatestFrameQueuePreservesNewerFrameWhenRetryingAsyncInput() {
  LatestFrameQueue<std::string> queue;
  queue.Push("newer");
  assert(!queue.PushIfEmpty("older"));
  const auto newer = queue.TryPop();
  assert(newer.has_value() && *newer == "newer");
  assert(queue.PushIfEmpty("retry"));
  const auto retry = queue.TryPop();
  assert(retry.has_value() && *retry == "retry");
}

void BackpressureRequiresResetAndFreshIdr() {
  EncodedBackpressurePolicy policy;
  assert(policy.MaySendAccessUnit());
  assert(policy.OnTransportWouldBlock() == TransportAction::kRequestStreamReset);
  assert(!policy.MaySendAccessUnit());
  assert(policy.OnTransportWouldBlock() == TransportAction::kHold);
  assert(policy.reset_requests() == 1);
  policy.OnFreshIdrProduced();
  assert(policy.MaySendAccessUnit());
}

void AsyncMftRequiresNeedInputAndKeepsOnlyOneFrameInFlight() {
  AsyncMftStateMachine state;
  state.StartStream();
  assert(!state.CanAcceptInput());

  state.OnEvent(AsyncMftEvent::kNeedInput);
  assert(state.CanAcceptInput());
  assert(state.OnInputAccepted(17));
  assert(!state.CanAcceptInput());
  assert(state.HasPendingInput());

  // Hardware is allowed to ask early, but accepting another frame would make
  // the encode queue deeper than the latency contract permits.
  state.OnEvent(AsyncMftEvent::kNeedInput);
  assert(!state.CanAcceptInput());
}

void AsyncMftAssociatesOutputAndHandlesStaleOutputEvents() {
  AsyncMftStateMachine state;
  state.StartStream();
  state.OnEvent(AsyncMftEvent::kNeedInput);
  assert(state.OnInputAccepted(23));
  state.OnEvent(AsyncMftEvent::kHaveOutput);
  assert(state.HasOutput());
  const auto sequence = state.OnOutputProduced();
  assert(sequence.has_value() && *sequence == 23);
  assert(!state.HasPendingInput());

  state.OnEvent(AsyncMftEvent::kHaveOutput);
  const auto stale_sequence = state.OnOutputProduced();
  assert(!stale_sequence.has_value());
}

void AsyncMftDrainAndFailureSuppressFurtherInput() {
  AsyncMftStateMachine state;
  state.StartStream();
  state.OnEvent(AsyncMftEvent::kNeedInput);
  state.BeginDrain();
  assert(!state.CanAcceptInput());
  state.OnEvent(AsyncMftEvent::kEndOfStream);
  state.OnEvent(AsyncMftEvent::kDrainComplete);
  assert(state.end_of_stream());
  assert(state.drain_complete());

  state.StartStream();
  state.OnEvent(AsyncMftEvent::kNeedInput);
  state.MarkFatal();
  assert(state.faulted());
  assert(!state.CanAcceptInput());
  assert(state.TakeRestartRequest());
  assert(!state.TakeRestartRequest());
}

void PendingEncodeTelemetryCompletesDelayedOutputExactlyOnce() {
  PendingEncodeTelemetryState state;
  const auto acquired = SteadyTime{} + std::chrono::milliseconds(10);
  const auto started = acquired + std::chrono::milliseconds(4);
  assert(state.Begin({42, acquired, std::chrono::microseconds(4'000), started}));

  EncodedAccessUnit output{};
  output.sequence = 42;
  output.encoded_at = started + std::chrono::milliseconds(20);
  output.is_idr = true;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, output.encoded_at, &telemetry) ==
         EncodeTelemetryCompletion::kCompleted);
  assert(telemetry.sequence == 42);
  assert(output.acquired_at == acquired);
  assert(telemetry.acquired_at == acquired);
  assert(telemetry.encoded_at == output.encoded_at);
  assert(telemetry.conversion_duration == std::chrono::microseconds(4'000));
  assert(telemetry.encode_duration == std::chrono::milliseconds(20));
  assert(telemetry.capture_to_encode_duration == std::chrono::milliseconds(24));
  assert(telemetry.idr);
  assert(!state.has_pending());
  assert(state.Complete(output, output.encoded_at, &telemetry) ==
         EncodeTelemetryCompletion::kNoPending);
}

void EncodedAccessUnitCarriesSynchronousCaptureAndEncodeTimes() {
  PendingEncodeTelemetryState state;
  const auto acquired = SteadyTime{} + std::chrono::milliseconds(100);
  const auto started = acquired + std::chrono::milliseconds(2);
  const auto encoded = acquired + std::chrono::milliseconds(8);
  assert(state.Begin({7, acquired, std::chrono::milliseconds(2), started}));

  EncodedAccessUnit output{};
  output.sequence = 7;
  output.encoded_at = encoded;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, encoded, &telemetry) == EncodeTelemetryCompletion::kCompleted);
  assert(output.acquired_at == acquired);
  assert(output.encoded_at == encoded);
  assert(telemetry.acquired_at == acquired);
  assert(telemetry.encoded_at == encoded);
  assert(telemetry.capture_to_encode_duration == std::chrono::milliseconds(8));
}

void EncodedAccessUnitCarriesDelayedAsyncCaptureTimeBySequence() {
  PendingEncodeTelemetryState state;
  const auto acquired = SteadyTime{} + std::chrono::seconds(3);
  const auto started = acquired + std::chrono::milliseconds(3);
  const auto encoded = acquired + std::chrono::milliseconds(27);
  assert(state.Begin({91, acquired, std::chrono::milliseconds(3), started}));

  // The output arrives later but retains the MFT-assigned sequence. The
  // pending input timestamp must be restored onto the output before the app
  // serializes its VideoFrame.
  EncodedAccessUnit output{};
  output.sequence = 91;
  output.encoded_at = encoded;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, encoded, &telemetry) == EncodeTelemetryCompletion::kCompleted);
  assert(output.acquired_at == acquired);
  assert(output.encoded_at == encoded);
  assert(telemetry.capture_to_encode_duration == std::chrono::milliseconds(27));
}

void PendingEncodeTelemetryResetClearsWithoutCallbacks() {
  PendingEncodeTelemetryState state;
  assert(state.Begin({7, SteadyTime{}, {}, SteadyTime{}}));
  state.Clear();
  EncodedAccessUnit output{};
  output.sequence = 7;
  EncodeTelemetry telemetry{};
  std::uint32_t on_encode = 0;
  std::uint32_t on_policy = 0;
  const auto result = state.Complete(output, SteadyTime{}, &telemetry);
  if (result == EncodeTelemetryCompletion::kCompleted) {
    ++on_encode;
    ++on_policy;
  }
  assert(result == EncodeTelemetryCompletion::kNoPending);
  assert(on_encode == 0 && on_policy == 0);
}

void PendingEncodeTelemetryRejectsMismatchedSequenceAndDoesNotAttribute() {
  PendingEncodeTelemetryState state;
  assert(state.Begin({11, SteadyTime{}, {}, SteadyTime{}}));
  EncodedAccessUnit output{};
  output.sequence = 12;
  EncodeTelemetry telemetry{};
  assert(state.Complete(output, SteadyTime{}, &telemetry) ==
         EncodeTelemetryCompletion::kSequenceMismatch);
  assert(!state.has_pending());
}

void PendingEncodeTelemetryCompletionDrivesExactlyOneEncodeAndPolicyCallback() {
  PendingEncodeTelemetryState state;
  assert(state.Begin({19, SteadyTime{}, {}, SteadyTime{}}));
  EncodedAccessUnit output{};
  output.sequence = 19;
  output.encoded_at = SteadyTime{};
  EncodeTelemetry telemetry{};
  std::uint32_t on_encode = 0;
  std::uint32_t on_policy = 0;
  for (int attempt = 0; attempt < 2; ++attempt) {
    if (state.Complete(output, SteadyTime{}, &telemetry) ==
        EncodeTelemetryCompletion::kCompleted) {
      ++on_encode;
      ++on_policy;
    }
  }
  assert(on_encode == 1 && on_policy == 1);
}

void AnnexBIdrDetectionHandlesStartCodesAndMalformedInput() {
  const auto contains = [](std::initializer_list<std::uint8_t> bytes) {
    const std::vector<std::uint8_t> access_unit(bytes);
    return AnnexBAccessUnitContainsIdr(access_unit);
  };
  assert(contains({0x00, 0x00, 0x01, 0x65, 0x88}));
  assert(contains({0x00, 0x00, 0x00, 0x01, 0x65, 0x88}));

  const std::vector<std::uint8_t> parameter_sets_and_idr{
      0x00, 0x00, 0x01, 0x67, 0x64, 0x00, 0x1f,
      0x00, 0x00, 0x01, 0x68, 0xee, 0x3c, 0x80,
      0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84};
  assert(AnnexBAccessUnitContainsIdr(parameter_sets_and_idr));

  assert(!contains({0x00, 0x00, 0x01, 0x41, 0x9a}));
  assert(!contains({}));
  assert(!contains({0x00, 0x00, 0x01}));
  assert(!contains({0x00, 0x00, 0x00, 0x01}));
  assert(!contains({0x00, 0x00, 0x02, 0x65, 0x88}));
  assert(!contains({0x00, 0x00, 0x00, 0x02, 0x65, 0x88}));
  assert(!contains({0x00, 0x00, 0x01, 0x85, 0x88}));
}

void ForceKeyFrameVariantUsesDocumentedUInt32Type() {
  const VARIANT value = MakeForceKeyFrameVariant();
  assert(value.vt == VT_UI4);
  assert(value.ulVal == 1u);
}

void MonitorIdParsingMatchesSerializedFormat() {
  MonitorId parsed{};
  assert(MonitorId::TryParse(L"-1:123:4", &parsed));
  assert(parsed.adapter_luid.HighPart == -1);
  assert(parsed.adapter_luid.LowPart == 123u);
  assert(parsed.target_id == 4u);

  assert(!MonitorId::TryParse(L"-1:123", &parsed));
  assert(!MonitorId::TryParse(L"-1:x:4", &parsed));
  assert(!MonitorId::TryParse(L"-1:123:x", &parsed));
}

}  // namespace pdb::video::test

#if defined(PDB_VIDEO_TEST_MAIN)
int main() {
  pdb::video::test::PrivateCopyTextureDescriptionSupportsVideoProcessorInputView();
  pdb::video::test::ResolutionLadderPrefersNativeFitAndMaintainsAspectRatio();
  pdb::video::test::ResolutionLadderStepsDownWithoutUpscaling();
  pdb::video::test::AdaptiveResolutionControllerUsesPersistentBreachesAndStableUpgrade();
  pdb::video::test::AdaptiveResolutionControllerNeverDropsBelowMinimumRung();
  pdb::video::test::EncoderSelectionUsesDeterministicTieBreak();
  pdb::video::test::EncoderFormalStreamSetupRequiresTypesBeforeStart();
  pdb::video::test::OptionalCodecApiPropertyFailuresUseDocumentedDefaults();
  pdb::video::test::LatestFrameQueueReplacesOnlyPendingRawFrame();
  pdb::video::test::LatestFrameQueuePreservesNewerFrameWhenRetryingAsyncInput();
  pdb::video::test::BackpressureRequiresResetAndFreshIdr();
  pdb::video::test::AsyncMftRequiresNeedInputAndKeepsOnlyOneFrameInFlight();
  pdb::video::test::AsyncMftAssociatesOutputAndHandlesStaleOutputEvents();
  pdb::video::test::AsyncMftDrainAndFailureSuppressFurtherInput();
  pdb::video::test::PendingEncodeTelemetryCompletesDelayedOutputExactlyOnce();
  pdb::video::test::EncodedAccessUnitCarriesSynchronousCaptureAndEncodeTimes();
  pdb::video::test::EncodedAccessUnitCarriesDelayedAsyncCaptureTimeBySequence();
  pdb::video::test::PendingEncodeTelemetryResetClearsWithoutCallbacks();
  pdb::video::test::PendingEncodeTelemetryRejectsMismatchedSequenceAndDoesNotAttribute();
  pdb::video::test::PendingEncodeTelemetryCompletionDrivesExactlyOneEncodeAndPolicyCallback();
  pdb::video::test::AnnexBIdrDetectionHandlesStartCodesAndMalformedInput();
  pdb::video::test::ForceKeyFrameVariantUsesDocumentedUInt32Type();
  pdb::video::test::MonitorIdParsingMatchesSerializedFormat();
}
#endif
