#pragma once
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <string>

namespace D3D11Identity {
// Identity transport carries G in Y, B in U, R in V. Y410 is sampled UYV;
// AYUV's RGBA view is VUY. No YUV matrix, offsets or limited-range expansion.
inline std::string pixelShaderSource(bool y410) {
    return std::string(
        "Texture2D<float4> videoTex : register(t0);"
        "SamplerState theSampler : register(s0);"
        "struct Input { float4 pos : SV_POSITION; float2 tex : TEXCOORD0; };"
        "float4 main(Input input) : SV_TARGET {"
        "float4 value = videoTex.Sample(theSampler, input.tex); return float4(value.") +
        (y410 ? "bgr" : "rbg") + ", 1); }";
}
inline bool createShader(ID3D11Device* device, bool y410, ID3D11PixelShader** output) {
    const auto source = pixelShaderSource(y410);
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(source.data(), source.size(), "fernweh-identity", nullptr, nullptr,
                         "main", "ps_4_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &code, &errors))) return false;
    return SUCCEEDED(device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, output));
}
}
