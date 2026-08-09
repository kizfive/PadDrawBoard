#pragma once

#include "pdb/video/types.h"

#include <string_view>

namespace pdb::video {

// D3D11 video-processor based scale/BGRA-to-NV12 conversion. This is kept as
// an abstraction because some adapters do not expose NV12 processor output.
class D3D11Nv12Converter final {
 public:
  D3D11Nv12Converter() = default;
  ~D3D11Nv12Converter();
  D3D11Nv12Converter(const D3D11Nv12Converter&) = delete;
  D3D11Nv12Converter& operator=(const D3D11Nv12Converter&) = delete;

  [[nodiscard]] HRESULT Initialize(ID3D11Device* device);
  void Reset();
  [[nodiscard]] HRESULT Convert(ID3D11Texture2D* bgra_source, Size output_size,
                                ComPtr<ID3D11Texture2D>* nv12_output);
  [[nodiscard]] std::string_view last_failure_stage() const noexcept {
    return last_failure_stage_;
  }

 private:
  [[nodiscard]] HRESULT Configure(Size input_size, DXGI_FORMAT input_format, Size output_size);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<ID3D11VideoDevice> video_device_;
  ComPtr<ID3D11VideoContext> video_context_;
  ComPtr<ID3D11VideoProcessorEnumerator> enumerator_;
  ComPtr<ID3D11VideoProcessor> processor_;
  Size input_size_{};
  DXGI_FORMAT input_format_{DXGI_FORMAT_UNKNOWN};
  Size output_size_{};
  std::string_view last_failure_stage_{"none"};
};

}  // namespace pdb::video
