/* SPDX-License-Identifier: GPL-2.0-or-later */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <directx-native/dxn_shared.h>
#include <cassert>
#include <cstdio>
#include <unistd.h>
#include <sys/mman.h>
int main() {
    ID3D11Device *producer=nullptr,*consumer=nullptr;
    ID3D11DeviceContext *draw=nullptr,*read=nullptr;
    D3D_FEATURE_LEVEL level;
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&producer,&level,&draw)));
    assert(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&consumer,&level,&read)));
    D3D11_TEXTURE2D_DESC desc={};desc.Width=97;desc.Height=49;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
    desc.Format=DXGI_FORMAT_B8G8R8A8_UNORM;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D *source=nullptr,*imported=nullptr,*staging=nullptr;
    assert(SUCCEEDED(producer->CreateTexture2D(&desc,nullptr,&source)));
    IDXGIResource *resource=nullptr;assert(SUCCEEDED(source->QueryInterface(__uuidof(IDXGIResource),(void **)&resource)));
    HANDLE handle=nullptr;assert(SUCCEEDED(resource->GetSharedHandle(&handle)) && handle);
    auto *shared=reinterpret_cast<dxn_shared_texture *>(handle);
    assert(shared->fd>=0 && shared->size && shared->planes[0].pitch>=97*4);
    assert(SUCCEEDED(consumer->OpenSharedResource(handle,__uuidof(ID3D11Texture2D),(void **)&imported)));
    ID3D11RenderTargetView *rtv=nullptr;assert(SUCCEEDED(producer->CreateRenderTargetView(source,nullptr,&rtv)));
    float color[4]={1,0.25,0,1};draw->ClearRenderTargetView(rtv,color);
    D3D11_QUERY_DESC queryDesc={D3D11_QUERY_EVENT,0};ID3D11Query *query=nullptr;
    assert(SUCCEEDED(producer->CreateQuery(&queryDesc,&query)));draw->End(query);draw->Flush();
    HRESULT hr=S_FALSE;
    for(unsigned i=0;i<10000 && hr==S_FALSE;i++) {hr=draw->GetData(query,nullptr,0,0);if(hr==S_FALSE)usleep(1000);}
    assert(hr==S_OK);
    desc.BindFlags=desc.MiscFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    assert(SUCCEEDED(consumer->CreateTexture2D(&desc,nullptr,&staging)));read->CopyResource(staging,imported);
    D3D11_MAPPED_SUBRESOURCE mapped={};assert(SUCCEEDED(read->Map(staging,0,D3D11_MAP_READ,0,&mapped)));
    for(unsigned y=0;y<49;y++)for(unsigned x=0;x<97;x++) {
        auto *p=(const unsigned char *)mapped.pData+y*mapped.RowPitch+x*4;
        assert(p[0]==0 && p[1]==64 && p[2]==255 && p[3]==255);
    }
    read->Unmap(staging,0);
    void *alias=mmap(nullptr,shared->size,PROT_READ,MAP_SHARED,shared->fd,0);assert(alias!=MAP_FAILED);
    auto *p=(const unsigned char *)alias+shared->planes[0].offset;assert(p[0]==0 && p[1]==64 && p[2]==255 && p[3]==255);
    munmap(alias,shared->size);
    query->Release();rtv->Release();staging->Release();imported->Release();resource->Release();source->Release();
    draw->Release();read->Release();producer->Release();consumer->Release();
    puts("PASS: two native D3D11 devices share a rendered BGRA texture; every imported pixel and the exported FD alias agree");
}
