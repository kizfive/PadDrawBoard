#include "pdb/video/h264_encoder.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <wmcodecdsp.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace pdb::video {
namespace {

HRESULT SetUInt32(IMFAttributes* attributes, REFGUID key, UINT32 value) {
  return attributes == nullptr ? E_POINTER : attributes->SetUINT32(key, value);
}

HRESULT CreateVideoType(REFGUID subtype, const H264EncoderConfig& config, IMFMediaType** type) {
  if (type == nullptr) return E_POINTER;
  ComPtr<IMFMediaType> value;
  HRESULT hr = MFCreateMediaType(&value);
  if (FAILED(hr)) return hr;
  if (FAILED(hr = value->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video)) ||
      FAILED(hr = value->SetGUID(MF_MT_SUBTYPE, subtype)) ||
      FAILED(hr = MFSetAttributeSize(value.Get(), MF_MT_FRAME_SIZE, config.size.width, config.size.height)) ||
      FAILED(hr = MFSetAttributeRatio(value.Get(), MF_MT_FRAME_RATE, config.frame_rate, 1)) ||
      FAILED(hr = SetUInt32(value.Get(), MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive))) {
    return hr;
  }
  *type = value.Detach();
  return S_OK;
}

HRESULT CreateInputSample(ID3D11Texture2D* frame, const H264EncoderConfig& config,
                          LONGLONG sample_time_100ns, IMFSample** sample) {
  if (frame == nullptr || sample == nullptr) return E_POINTER;
  ComPtr<IMFMediaBuffer> buffer;
  HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), frame, 0, FALSE, &buffer);
  if (FAILED(hr)) return hr;
  ComPtr<IMFSample> value;
  hr = MFCreateSample(&value);
  if (FAILED(hr)) return hr;
  if (FAILED(hr = value->AddBuffer(buffer.Get())) ||
      FAILED(hr = value->SetSampleTime(sample_time_100ns)) ||
      FAILED(hr = value->SetSampleDuration(10'000'000LL / config.frame_rate))) {
    return hr;
  }
  *sample = value.Detach();
  return S_OK;
}

HRESULT QueryCodecApi(IMFTransform* transform, ComPtr<ICodecAPI>* codec) {
  if (transform == nullptr || codec == nullptr) return E_POINTER;
  codec->Reset();
  void* queried = nullptr;
  const HRESULT hr = transform->QueryInterface(IID_ICodecAPI, &queried);
  if (FAILED(hr)) return hr;
  // QueryInterface transferred one reference. Attach adopts it without an
  // extra AddRef and avoids WRL's __uuidof-based As/IID_PPV_ARGS machinery,
  // which MinGW does not instantiate for ICodecAPI.
  codec->Attach(static_cast<ICodecAPI*>(queried));
  return S_OK;
}

void FlushCandidate(IMFTransform* transform) noexcept {
  if (transform == nullptr) return;
  static_cast<void>(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
  static_cast<void>(transform->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
  static_cast<void>(transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
  static_cast<void>(transform->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
}

}  // namespace

bool AnnexBAccessUnitContainsIdr(
    std::span<const std::uint8_t> access_unit) noexcept {
  std::size_t search_from = 0;
  while (search_from < access_unit.size()) {
    std::size_t nal_header = access_unit.size();
    for (std::size_t index = search_from; index + 2 < access_unit.size(); ++index) {
      if (access_unit[index] != 0 || access_unit[index + 1] != 0) continue;
      if (access_unit[index + 2] == 1) {
        nal_header = index + 3;
        break;
      }
      if (index + 3 < access_unit.size() && access_unit[index + 2] == 0 &&
          access_unit[index + 3] == 1) {
        nal_header = index + 4;
        break;
      }
    }
    if (nal_header == access_unit.size()) return false;
    if (nal_header < access_unit.size()) {
      const std::uint8_t nal_unit_header = access_unit[nal_header];
      if ((nal_unit_header & 0x80u) == 0 && (nal_unit_header & 0x1fu) == 5u) {
        return true;
      }
      search_from = nal_header + 1;
    }
  }
  return false;
}

VARIANT MakeForceKeyFrameVariant() noexcept {
  VARIANT value{};
  value.vt = VT_UI4;
  value.ulVal = 1;
  return value;
}

bool PreferH264EncoderCandidate(const H264EncoderCandidateDiagnostics& candidate,
                                const H264EncoderCandidateDiagnostics& current) noexcept {
  if (!candidate.usable) return false;
  if (!current.usable) return true;
  if (candidate.measured_latency != current.measured_latency) {
    return candidate.measured_latency < current.measured_latency;
  }
  if (candidate.name != current.name) return candidate.name < current.name;
  return candidate.enumeration_index < current.enumeration_index;
}

HardwareH264Encoder::~HardwareH264Encoder() { Shutdown(); }

HRESULT HardwareH264Encoder::Initialize(ID3D11Device* device, const H264EncoderConfig& config) {
  last_failure_stage_ = "initialize_validation";
  if (device == nullptr || !config.size.valid() || (config.size.width & 1) || (config.size.height & 1) ||
      config.frame_rate != 60 || config.bitrate_bits_per_second < 20'000'000 ||
      config.bitrate_bits_per_second > 120'000'000) {
    return E_INVALIDARG;
  }
  Shutdown();
  candidate_diagnostics_.clear();
  selected_benchmark_latency_ = {};
  last_failure_stage_ = "mf_startup";
  HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) return hr;
  mf_started_ = true;
  device_ = device;
  config_ = config;
  last_failure_stage_ = "create_dxgi_device_manager";
  hr = MFCreateDXGIDeviceManager(&device_manager_reset_token_, &device_manager_);
  if (FAILED(hr)) {
    Shutdown();
    return hr;
  }
  last_failure_stage_ = "reset_dxgi_device_manager";
  hr = device_manager_->ResetDevice(device_.Get(), device_manager_reset_token_);
  if (FAILED(hr)) {
    Shutdown();
    return hr;
  }

  MFT_REGISTER_TYPE_INFO input_type{MFMediaType_Video, MFVideoFormat_NV12};
  MFT_REGISTER_TYPE_INFO output_type{MFMediaType_Video, MFVideoFormat_H264};
  IMFActivate** activations = nullptr;
  UINT32 activation_count = 0;
  last_failure_stage_ = "enumerate_hardware_mft";
  hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER,
                 MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
                 &input_type, &output_type, &activations, &activation_count);
  if (FAILED(hr)) {
    Shutdown();
    return hr;
  }

  HRESULT last_error = MF_E_TOPO_CODEC_NOT_FOUND;
  ComPtr<IMFTransform> best_transform;
  ComPtr<IMFMediaEventGenerator> best_event_generator;
  std::wstring best_name;
  std::chrono::microseconds best_latency{};
  bool best_async = false;
  bool have_best = false;
  H264EncoderCandidateDiagnostics best_diagnostics{};
  for (UINT32 index = 0; index < activation_count; ++index) {
    H264EncoderCandidateDiagnostics diagnostics{};
    diagnostics.enumeration_index = index;
    WCHAR* allocated_name = nullptr;
    UINT32 name_length = 0;
    if (SUCCEEDED(activations[index]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute,
                                                          &allocated_name, &name_length))) {
      diagnostics.name = allocated_name;
      CoTaskMemFree(allocated_name);
    }
    ComPtr<IMFTransform> candidate;
    hr = activations[index]->ActivateObject(IID_PPV_ARGS(&candidate));
    if (FAILED(hr)) {
      diagnostics.setup_status = hr;
      diagnostics.benchmark_status = hr;
      candidate_diagnostics_.push_back(std::move(diagnostics));
      last_error = hr;
      continue;
    }

    ComPtr<IMFAttributes> attributes;
    UINT32 is_async = FALSE;
    bool candidate_async = false;
    ComPtr<IMFMediaEventGenerator> candidate_events;
    if (SUCCEEDED(candidate->GetAttributes(&attributes)) && attributes != nullptr &&
        SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &is_async)) && is_async != FALSE) {
      candidate_async = true;
      hr = candidate.As(&candidate_events);
      if (SUCCEEDED(hr)) hr = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
      if (FAILED(hr)) {
        diagnostics.setup_status = hr;
        diagnostics.benchmark_status = hr;
        candidate_diagnostics_.push_back(std::move(diagnostics));
        last_error = hr;
        continue;
      }
    }

    transform_ = std::move(candidate);
    event_generator_ = std::move(candidate_events);
    asynchronous_ = candidate_async;
    diagnostics.asynchronous = candidate_async;
    selected_name_ = diagnostics.name;
    last_failure_stage_ = "candidate_configure";
    hr = ConfigureTransform(config);
    diagnostics.setup_status = hr;
    if (SUCCEEDED(hr)) {
      std::chrono::microseconds latency{};
      std::uint32_t measured_frames = 0;
      last_failure_stage_ = "candidate_benchmark";
      const HRESULT benchmark = BenchmarkCurrentCandidate(device, config, &latency, &measured_frames);
      diagnostics.benchmark_status = benchmark;
      diagnostics.measured_latency = latency;
      diagnostics.measured_frames = measured_frames;
      diagnostics.usable = SUCCEEDED(benchmark);
      candidate_diagnostics_.push_back(diagnostics);
      if (SUCCEEDED(benchmark)) {
        diagnostics.measured_latency = latency;
        const bool better = !have_best || PreferH264EncoderCandidate(diagnostics, best_diagnostics);
        if (better) {
          if (best_transform) FlushCandidate(best_transform.Get());
          best_transform = std::move(transform_);
          best_event_generator = std::move(event_generator_);
          best_name = diagnostics.name;
          best_latency = latency;
          best_async = candidate_async;
          best_diagnostics = diagnostics;
          have_best = true;
        } else {
          ShutdownCurrentCandidate();
        }
      } else {
        last_error = benchmark;
        ShutdownCurrentCandidate();
      }
    } else {
      diagnostics.benchmark_status = hr;
      candidate_diagnostics_.push_back(std::move(diagnostics));
      last_error = hr;
      ShutdownCurrentCandidate();
    }
  }
  for (UINT32 index = 0; index < activation_count; ++index) activations[index]->Release();
  CoTaskMemFree(activations);
  if (!have_best) {
    Shutdown();
    return last_error;
  }
  transform_ = std::move(best_transform);
  event_generator_ = std::move(best_event_generator);
  asynchronous_ = best_async;
  selected_name_ = std::move(best_name);
  selected_benchmark_latency_ = best_latency;
  async_state_.StartStream();
  // The benchmark used real samples and may have left the selected transform
  // without negotiated media types after its reset. ResetAndRequestIdr()
  // rebuilds the complete formal stream state before requesting the first IDR.
  hr = ResetAndRequestIdr();
  if (FAILED(hr)) {
    Shutdown();
    return hr;
  }
  return S_OK;
}

void HardwareH264Encoder::BeginAsyncShutdown() noexcept {
  if (!asynchronous_ || !transform_) return;
  async_state_.BeginDrain();
  const HRESULT eos = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
  if (SUCCEEDED(eos)) {
    const HRESULT drain = transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    if (SUCCEEDED(drain)) static_cast<void>(PumpAsyncEvents());
  }
  // Shutdown is allowed to abandon a still-running hardware operation. A
  // completed drain gets the normal end-streaming message; otherwise flush
  // cancels the work before COM references are released.
  if (async_state_.drain_complete()) {
    static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
  }
  static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
}

void HardwareH264Encoder::ShutdownCurrentCandidate() noexcept {
  if (transform_) {
    if (asynchronous_) {
      BeginAsyncShutdown();
    } else {
      FlushCandidate(transform_.Get());
    }
  }
  event_generator_.Reset();
  transform_.Reset();
  selected_name_.clear();
  asynchronous_ = false;
  async_state_.StartStream();
  fatal_restart_requested_ = false;
  last_async_fatal_ = S_OK;
}

void HardwareH264Encoder::Shutdown() {
  if (transform_) {
    if (asynchronous_) {
      BeginAsyncShutdown();
    } else {
      static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0));
      static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0));
      static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0));
      static_cast<void>(transform_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0));
    }
  }
  event_generator_.Reset();
  transform_.Reset();
  device_manager_.Reset();
  device_manager_reset_token_ = 0;
  device_.Reset();
  selected_name_.clear();
  config_ = {};
  async_state_.StartStream();
  asynchronous_ = false;
  fatal_restart_requested_ = false;
  last_async_fatal_ = S_OK;
  if (mf_started_) {
    MFShutdown();
    mf_started_ = false;
  }
}

HRESULT HardwareH264Encoder::ConfigureTransform(const H264EncoderConfig& config) {
  ComPtr<IMFMediaType> input;
  ComPtr<IMFMediaType> output;
  HRESULT hr = CreateVideoType(MFVideoFormat_NV12, config, &input);
  if (FAILED(hr)) return hr;
  hr = CreateVideoType(MFVideoFormat_H264, config, &output);
  if (FAILED(hr)) return hr;
  if (FAILED(hr = output->SetUINT32(MF_MT_AVG_BITRATE, config.bitrate_bits_per_second)) ||
      FAILED(hr = output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main))) {
    return hr;
  }
  last_failure_stage_ = "configure_set_d3d_manager";
  hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
  if (FAILED(hr)) return hr;

  last_failure_stage_ = "configure_codec_api";
  hr = ConfigureCodecApi();
  if (FAILED(hr)) return hr;

  last_failure_stage_ = "configure_set_output";
  hr = transform_->SetOutputType(0, output.Get(), 0);
  if (FAILED(hr)) return hr;

  last_failure_stage_ = "configure_set_input";
  hr = transform_->SetInputType(0, input.Get(), 0);
  if (FAILED(hr)) return hr;

  if (asynchronous_) {
    async_state_.StartStream();
    last_async_fatal_ = S_OK;
  }
  last_failure_stage_ = "configure_begin_streaming";
  if (FAILED(hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0)) ||
      FAILED(hr = (last_failure_stage_ = "configure_start_stream",
                   transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)))) {
    return hr;
  }
  return asynchronous_ ? PumpAsyncEvents() : S_OK;
}

HRESULT HardwareH264Encoder::ReconfigureSelectedTransformForStream() {
  if (!transform_) return MF_E_NOT_INITIALIZED;

  // Benchmarking is intentionally allowed to exercise the transform. End
  // that stream and flush all in-flight state before negotiating the formal
  // stream again. Some hardware encoders clear media types as part of
  // COMMAND_FLUSH even though the benchmark's ProcessInput calls succeeded.
  FlushCandidate(transform_.Get());
  if (asynchronous_) {
    async_state_.StartStream();
    last_async_fatal_ = S_OK;
  }

  ComPtr<IMFMediaType> input;
  ComPtr<IMFMediaType> output;
  HRESULT hr = CreateVideoType(MFVideoFormat_NV12, config_, &input);
  if (FAILED(hr)) return hr;
  hr = CreateVideoType(MFVideoFormat_H264, config_, &output);
  if (FAILED(hr)) return hr;
  if (FAILED(hr = output->SetUINT32(MF_MT_AVG_BITRATE, config_.bitrate_bits_per_second)) ||
      FAILED(hr = output->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main))) {
    return hr;
  }

  H264EncoderStreamSetupSequence sequence;
  if (!sequence.Advance(H264EncoderStreamSetupStep::kSetD3DManager)) return E_UNEXPECTED;
  last_failure_stage_ = "formal_set_d3d_manager";
  hr = transform_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                  reinterpret_cast<ULONG_PTR>(device_manager_.Get()));
  if (FAILED(hr)) return hr;

  if (!sequence.Advance(H264EncoderStreamSetupStep::kConfigureCodecApi)) return E_UNEXPECTED;
  last_failure_stage_ = "formal_codec_api";
  hr = ConfigureCodecApi();
  if (FAILED(hr)) return hr;

  if (!sequence.Advance(H264EncoderStreamSetupStep::kSetOutputType)) return E_UNEXPECTED;
  last_failure_stage_ = "formal_set_output";
  hr = transform_->SetOutputType(0, output.Get(), 0);
  if (FAILED(hr)) return hr;

  if (!sequence.Advance(H264EncoderStreamSetupStep::kSetInputType)) return E_UNEXPECTED;
  last_failure_stage_ = "formal_set_input";
  hr = transform_->SetInputType(0, input.Get(), 0);
  if (FAILED(hr)) return hr;

  if (!sequence.Advance(H264EncoderStreamSetupStep::kBeginStreaming)) return E_UNEXPECTED;
  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
  if (FAILED(hr)) return hr;

  if (!sequence.Advance(H264EncoderStreamSetupStep::kStartOfStream)) return E_UNEXPECTED;
  hr = transform_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
  if (FAILED(hr)) return hr;
  if (!sequence.complete() || !sequence.media_types_configured()) return E_UNEXPECTED;
  return asynchronous_ ? PumpAsyncEvents() : S_OK;
}

HRESULT HardwareH264Encoder::ConfigureCodecApi() {
  ComPtr<ICodecAPI> codec;
  HRESULT hr = QueryCodecApi(transform_.Get(), &codec);
  if (FAILED(hr)) return hr;
  VARIANT value;
  VariantInit(&value);
  value.vt = VT_UI4;
  value.ulVal = eAVEncCommonRateControlMode_CBR;
  last_failure_stage_ = "codec_rate_control_cbr";
  hr = codec->SetValue(&CODECAPI_AVEncCommonRateControlMode, &value);
  if (FAILED(hr)) return hr;
  value.ulVal = config_.bitrate_bits_per_second;
  last_failure_stage_ = "codec_mean_bitrate";
  hr = codec->SetValue(&CODECAPI_AVEncCommonMeanBitRate, &value);
  if (FAILED(hr)) return hr;
  value.ulVal = 0;
  last_failure_stage_ = "codec_b_frame_count";
  hr = codec->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &value);
  if (FAILED(hr)) {
    if (!IsOptionalH264CodecApiPropertyFailure(hr)) return hr;
    // Hardware H.264 encoders default to zero B pictures. Keep that default
    // when this optional CodecAPI property is absent or rejected.
    last_failure_stage_ = "codec_b_frame_count_default";
  }
  value.vt = VT_BOOL;
  value.boolVal = VARIANT_TRUE;
  last_failure_stage_ = "codec_low_latency_mode";
  hr = codec->SetValue(&CODECAPI_AVLowLatencyMode, &value);
  if (SUCCEEDED(hr)) return S_OK;

  // Older hardware MFTs expose the legacy low-latency property instead. Try
  // it after the modern property; failure of both remains fatal because the
  // product's latency contract requires low-latency mode.
  last_failure_stage_ = "codec_low_latency_legacy";
  return codec->SetValue(&CODECAPI_AVEncCommonLowLatency, &value);
}

HRESULT HardwareH264Encoder::BenchmarkCurrentCandidate(
    ID3D11Device* device, const H264EncoderConfig& config,
    std::chrono::microseconds* latency, std::uint32_t* measured_frames) {
  if (device == nullptr || !transform_ || latency == nullptr || measured_frames == nullptr) {
    return E_POINTER;
  }

  D3D11_TEXTURE2D_DESC description{};
  description.Width = config.size.width;
  description.Height = config.size.height;
  description.MipLevels = 1;
  description.ArraySize = 1;
  description.Format = DXGI_FORMAT_NV12;
  description.SampleDesc.Count = 1;
  description.Usage = D3D11_USAGE_DEFAULT;
  description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  ComPtr<ID3D11Texture2D> input_texture;
  last_failure_stage_ = "benchmark_create_nv12_texture";
  HRESULT hr = device->CreateTexture2D(&description, nullptr, &input_texture);
  if (FAILED(hr)) return hr;

  constexpr std::uint32_t kWarmupFrames = 1;
  constexpr std::uint32_t kMeasuredFrames = 3;
  constexpr auto kFrameDeadline = std::chrono::milliseconds(250);
  std::vector<std::chrono::microseconds> samples;
  samples.reserve(kMeasuredFrames);

  for (std::uint32_t frame = 0; frame < kWarmupFrames + kMeasuredFrames; ++frame) {
    const auto acceptance_deadline = std::chrono::steady_clock::now() + kFrameDeadline;
    for (;;) {
      if (asynchronous_) {
        hr = PumpAsyncEvents();
        if (FAILED(hr)) return hr;
      }
      if (CanAcceptFrame()) break;
      if (std::chrono::steady_clock::now() >= acceptance_deadline) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    EncodedAccessUnit output{};
    const auto started = std::chrono::steady_clock::now();
    last_failure_stage_ = "benchmark_encode";
    hr = Encode(input_texture.Get(), frame + 1,
                static_cast<LONGLONG>(frame) * (10'000'000LL / config.frame_rate), &output);
    if (FAILED(hr) && hr != MF_E_NOTACCEPTING) return hr;
    while (hr != S_OK) {
      if (hr == MF_E_NOTACCEPTING) return hr;
      if (std::chrono::steady_clock::now() >= started + kFrameDeadline) {
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
      last_failure_stage_ = asynchronous_ ? "benchmark_poll" : "benchmark_process_output";
      hr = asynchronous_ ? Poll(&output) : ProcessOutput(frame + 1, &output);
      if (FAILED(hr) && hr != S_FALSE) return hr;
      if (hr == S_FALSE) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (output.bytes.empty()) return E_FAIL;
    if (frame >= kWarmupFrames) {
      samples.push_back(std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - started));
    }
  }
  if (samples.empty()) return E_FAIL;
  std::sort(samples.begin(), samples.end());
  *latency = samples[samples.size() / 2];
  *measured_frames = static_cast<std::uint32_t>(samples.size());
  return S_OK;
}

HRESULT HardwareH264Encoder::MarkAsyncFatal(HRESULT failure) noexcept {
  async_state_.MarkFatal();
  fatal_restart_requested_ = true;
  last_async_fatal_ = FAILED(failure) ? failure : E_FAIL;
  return failure;
}

HRESULT HardwareH264Encoder::PumpAsyncEvents() {
  if (!asynchronous_) return S_OK;
  if (!event_generator_) return MarkAsyncFatal(E_NOINTERFACE);
  for (;;) {
    ComPtr<IMFMediaEvent> event;
    HRESULT hr = event_generator_->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event);
    if (hr == MF_E_NO_EVENTS_AVAILABLE) return S_OK;
    if (FAILED(hr)) return MarkAsyncFatal(hr);
    if (!event) return MarkAsyncFatal(E_UNEXPECTED);

    MediaEventType type{};
    hr = event->GetType(&type);
    if (FAILED(hr)) return MarkAsyncFatal(hr);
    HRESULT event_status = S_OK;
    hr = event->GetStatus(&event_status);
    if (FAILED(hr)) return MarkAsyncFatal(hr);
    if (type == MEError) return MarkAsyncFatal(FAILED(event_status) ? event_status : E_FAIL);
    if (FAILED(event_status)) return MarkAsyncFatal(event_status);

    switch (type) {
      case METransformNeedInput:
        async_state_.OnEvent(AsyncMftEvent::kNeedInput);
        break;
      case METransformHaveOutput:
        async_state_.OnEvent(AsyncMftEvent::kHaveOutput);
        break;
      case METransformDrainComplete:
        async_state_.OnEvent(AsyncMftEvent::kDrainComplete);
        break;
      case MEEndOfStream:
        async_state_.OnEvent(AsyncMftEvent::kEndOfStream);
        break;
      default:
        break;
    }
  }
}

HRESULT HardwareH264Encoder::ProcessAsyncOutput(EncodedAccessUnit* output) {
  if (!async_state_.HasOutput()) return S_FALSE;
  const auto sequence = async_state_.pending_sequence();
  if (!sequence.has_value()) return MarkAsyncFatal(E_UNEXPECTED);

  const HRESULT hr = ProcessOutput(*sequence, output);
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT || hr == S_FALSE) {
    async_state_.OnOutputUnavailable();
    return S_FALSE;
  }
  if (FAILED(hr)) return MarkAsyncFatal(hr);
  if (!async_state_.OnOutputProduced().has_value()) return MarkAsyncFatal(E_UNEXPECTED);
  return S_OK;
}

HRESULT HardwareH264Encoder::Poll(EncodedAccessUnit* output) {
  if (output == nullptr) return E_POINTER;
  *output = {};
  if (!transform_) return MF_E_NOT_INITIALIZED;
  if (!asynchronous_) return S_FALSE;
  if (async_state_.faulted()) return last_async_fatal_;
  HRESULT hr = PumpAsyncEvents();
  if (FAILED(hr)) return hr;
  return ProcessAsyncOutput(output);
}

bool HardwareH264Encoder::CanAcceptFrame() const noexcept {
  return transform_ != nullptr && (!asynchronous_ || async_state_.CanAcceptInput());
}

HRESULT HardwareH264Encoder::Encode(ID3D11Texture2D* nv12_frame, std::uint64_t sequence,
                                    LONGLONG sample_time_100ns, EncodedAccessUnit* output) {
  if (!transform_ || !device_ || nv12_frame == nullptr || output == nullptr) return E_POINTER;
  *output = {};
  D3D11_TEXTURE2D_DESC desc{};
  nv12_frame->GetDesc(&desc);
  if (desc.Format != DXGI_FORMAT_NV12 || desc.Width != config_.size.width || desc.Height != config_.size.height) {
    return kErrFrameFormatUnsupported;
  }
  if (asynchronous_) {
    HRESULT hr = PumpAsyncEvents();
    if (FAILED(hr)) return hr;
    if (!async_state_.CanAcceptInput()) return MF_E_NOTACCEPTING;
    ComPtr<IMFSample> sample;
    hr = CreateInputSample(nv12_frame, config_, sample_time_100ns, &sample);
    if (FAILED(hr)) return hr;
    last_failure_stage_ = "async_process_input";
    hr = transform_->ProcessInput(0, sample.Get(), 0);
    if (FAILED(hr)) {
      if (hr != MF_E_NOTACCEPTING) return MarkAsyncFatal(hr);
      return hr;
    }
    if (!async_state_.OnInputAccepted(sequence)) return MarkAsyncFatal(E_UNEXPECTED);
    return Poll(output);
  }

  ComPtr<IMFSample> sample;
  HRESULT hr = CreateInputSample(nv12_frame, config_, sample_time_100ns, &sample);
  if (FAILED(hr)) return hr;
  last_failure_stage_ = "sync_process_input";
  hr = transform_->ProcessInput(0, sample.Get(), 0);
  if (FAILED(hr)) {
    fatal_restart_requested_ = true;
    return hr;
  }
  last_failure_stage_ = "sync_process_output";
  hr = ProcessOutput(sequence, output);
  if (FAILED(hr) && hr != MF_E_TRANSFORM_NEED_MORE_INPUT) fatal_restart_requested_ = true;
  return hr;
}

HRESULT HardwareH264Encoder::ProcessOutput(std::uint64_t sequence, EncodedAccessUnit* output) {
  MFT_OUTPUT_STREAM_INFO stream_info{};
  last_failure_stage_ = "get_output_stream_info";
  HRESULT hr = transform_->GetOutputStreamInfo(0, &stream_info);
  if (FAILED(hr)) return hr;
  MFT_OUTPUT_DATA_BUFFER output_data{};
  ComPtr<IMFSample> sample;
  if ((stream_info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) == 0) {
    ComPtr<IMFMediaBuffer> buffer;
    hr = MFCreateMemoryBuffer(stream_info.cbSize, &buffer);
    if (FAILED(hr)) return hr;
    hr = MFCreateSample(&sample);
    if (FAILED(hr)) return hr;
    hr = sample->AddBuffer(buffer.Get());
    if (FAILED(hr)) return hr;
    output_data.pSample = sample.Get();
  }
  DWORD status{};
  last_failure_stage_ = "process_output";
  hr = transform_->ProcessOutput(0, 1, &output_data, &status);
  ComPtr<IMFCollection> events;
  if (output_data.pEvents != nullptr) events.Attach(output_data.pEvents);
  if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return S_FALSE;
  if (FAILED(hr)) return hr;
  ComPtr<IMFSample> produced;
  if (sample) {
    produced = sample;
  } else if (output_data.pSample != nullptr) {
    // An MFT-provided sample has a transferred reference that we must release.
    produced.Attach(output_data.pSample);
  }
  if (!produced) return E_UNEXPECTED;
  ComPtr<IMFMediaBuffer> contiguous;
  hr = produced->ConvertToContiguousBuffer(&contiguous);
  if (FAILED(hr)) return hr;
  BYTE* bytes = nullptr;
  DWORD max_length{};
  DWORD length{};
  hr = contiguous->Lock(&bytes, &max_length, &length);
  if (FAILED(hr)) return hr;
  output->bytes.assign(bytes, bytes + length);
  contiguous->Unlock();
  UINT32 clean_point{};
  const bool clean_point_idr =
      SUCCEEDED(produced->GetUINT32(MFSampleExtension_CleanPoint, &clean_point)) &&
      clean_point != 0;
  output->is_idr = clean_point_idr || AnnexBAccessUnitContainsIdr(output->bytes);
  output->sequence = sequence;
  output->encoded_at = std::chrono::steady_clock::now();
  return S_OK;
}

bool HardwareH264Encoder::TakeFatalRestartRequest() noexcept {
  if (asynchronous_) static_cast<void>(async_state_.TakeRestartRequest());
  const bool requested = fatal_restart_requested_;
  fatal_restart_requested_ = false;
  return requested;
}

HRESULT HardwareH264Encoder::ResetAndRequestIdr() {
  if (!transform_) return MF_E_NOT_INITIALIZED;
  // A flush can clear media types on hardware encoders. Always rebuild the
  // manager, input/output types, codec settings, and stream messages before
  // sending the key-frame request; this also makes recovery resets safe.
  last_failure_stage_ = "formal_stream_reconfigure";
  HRESULT hr = ReconfigureSelectedTransformForStream();
  if (FAILED(hr)) return asynchronous_ ? MarkAsyncFatal(hr) : hr;
  ComPtr<ICodecAPI> codec;
  hr = QueryCodecApi(transform_.Get(), &codec);
  if (FAILED(hr)) return asynchronous_ ? MarkAsyncFatal(hr) : hr;
  VARIANT value = MakeForceKeyFrameVariant();
  hr = codec->SetValue(&CODECAPI_AVEncVideoForceKeyFrame, &value);
  if (FAILED(hr)) return asynchronous_ ? MarkAsyncFatal(hr) : hr;
  fatal_restart_requested_ = false;
  return asynchronous_ ? PumpAsyncEvents() : S_OK;
}

}  // namespace pdb::video
