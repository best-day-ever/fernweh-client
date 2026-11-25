#include <QtTest>
#include "streaming/video/ffmpeg-renderers/d3d11identity.h"
#include <vector>
using Microsoft::WRL::ComPtr;
class TestD3D11Identity : public QObject {
    Q_OBJECT
private slots:
    void tenBit444IdentityOnTwoTargets() { checkRamp(true); }
    void eightBit444IdentityOnTwoTargets() { checkRamp(false); }
private:
    void checkRamp(bool tenBit) {
        ComPtr<ID3D11Device> device;
        ComPtr<ID3D11DeviceContext> context;
        D3D_FEATURE_LEVEL level;
        QVERIFY(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,
            D3D11_SDK_VERSION,&device,&level,&context)));
        const int levels=tenBit?1024:256;
        std::vector<uint32_t> pixels(levels);
        for(int i=0;i<levels;++i) {
            const auto g=(i*17)%levels, b=(i*41)%levels;
            // Y410 SRV UYV = BGR; AYUV RGBA view VUY = RBG.
            pixels[i]=tenBit ? uint32_t(b|(g<<10)|(i<<20)|0xc0000000u) :
                               uint32_t(i|(b<<8)|(g<<16)|0xff000000u);
        }
        D3D11_TEXTURE2D_DESC source{};
        source.Width=levels; source.Height=1; source.MipLevels=1; source.ArraySize=1;
        source.SampleDesc.Count=1;
        source.Format=tenBit?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        source.Usage=D3D11_USAGE_IMMUTABLE; source.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA data{}; data.pSysMem=pixels.data(); data.SysMemPitch=levels*4;
        ComPtr<ID3D11Texture2D> input;
        QVERIFY(SUCCEEDED(device->CreateTexture2D(&source,&data,&input)));
        ComPtr<ID3D11ShaderResourceView> view;
        QVERIFY(SUCCEEDED(device->CreateShaderResourceView(input.Get(),nullptr,&view)));
        ComPtr<ID3D11PixelShader> shader;
        QVERIFY(D3D11Identity::createShader(device.Get(),tenBit,&shader));
        D3D11_SAMPLER_DESC samplerDesc{};
        samplerDesc.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU=samplerDesc.AddressV=samplerDesc.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.MaxLOD=D3D11_FLOAT32_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        QVERIFY(SUCCEEDED(device->CreateSamplerState(&samplerDesc,&sampler)));
        context->PSSetShader(shader.Get(),nullptr,0);
        context->PSSetSamplers(0,1,sampler.GetAddressOf());
        context->PSSetShaderResources(0,1,view.GetAddressOf());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        for(int target=0;target<2;++target) {
            const auto vs=std::string("struct Out{float4 pos:SV_POSITION;float2 tex:TEXCOORD0;};")+
                "Out main(uint id:SV_VertexID){Out o;float2 uv=float2((id<<1)&2,id&2);"
                "o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);o.tex=float2(uv.x*.5+"+
                (target?".5":"0")+",uv.y);return o;}";
            ComPtr<ID3DBlob> code,errors;
            QVERIFY(SUCCEEDED(D3DCompile(vs.data(),vs.size(),"identity-test",nullptr,nullptr,
                "main","vs_4_0",0,0,&code,&errors)));
            ComPtr<ID3D11VertexShader> vertex;
            QVERIFY(SUCCEEDED(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),nullptr,&vertex)));
            context->VSSetShader(vertex.Get(),nullptr,0);
            D3D11_TEXTURE2D_DESC output=source;
            output.Width=levels/2; output.Usage=D3D11_USAGE_DEFAULT;
            output.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> color;
            QVERIFY(SUCCEEDED(device->CreateTexture2D(&output,nullptr,&color)));
            ComPtr<ID3D11RenderTargetView> targetView;
            QVERIFY(SUCCEEDED(device->CreateRenderTargetView(color.Get(),nullptr,&targetView)));
            context->OMSetRenderTargets(1,targetView.GetAddressOf(),nullptr);
            D3D11_VIEWPORT viewport{}; viewport.Width=float(levels/2);viewport.Height=1;viewport.MaxDepth=1;
            context->RSSetViewports(1,&viewport);context->Draw(3,0);
            output.Usage=D3D11_USAGE_STAGING;output.BindFlags=0;output.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            QVERIFY(SUCCEEDED(device->CreateTexture2D(&output,nullptr,&readback)));
            context->CopyResource(readback.Get(),color.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            QVERIFY(SUCCEEDED(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped)));
            const auto values=static_cast<const uint32_t*>(mapped.pData);
            for(int x=0;x<levels/2;++x) {
                const int i=x+target*levels/2,g=(i*17)%levels,b=(i*41)%levels;
                const auto expected=tenBit?uint32_t(i|(g<<10)|(b<<20)|0xc0000000u):
                                           uint32_t(i|(g<<8)|(b<<16)|0xff000000u);
                QCOMPARE(values[x],expected);
            }
            context->Unmap(readback.Get(),0);
        }
    }
};
QTEST_GUILESS_MAIN(TestD3D11Identity)
#include "test_d3d11identity.moc"
