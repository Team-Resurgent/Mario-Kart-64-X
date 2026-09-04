/* pathtest.cpp — which path form does RXDK's fopen accept?
 *
 * The game's data loader builds every path as "%s/dc_data/<file>" from a
 * prefix, so the prefix has to work with forward slashes. On Dreamcast that
 * prefix is "/cd"; on Xbox the disc is D:, but it is not obvious whether the
 * C runtime here accepts "D:/...", "D:\\...", or neither.
 *
 * Reading printf is not available in this setup, so the answer is drawn: one
 * bar per candidate, green if fopen succeeded, red if it did not. The bars are
 * in the order listed in kPaths.
 */

#include <xtl.h>
#include <stdio.h>
#include <string.h>

static const char *kPaths[] = {
    "D:/dc_data/common_data.bin",
    "D:\\dc_data\\common_data.bin",
    "d:/dc_data/common_data.bin",
    "/dc_data/common_data.bin",
    "dc_data/common_data.bin",
    "D:/default.xbe",
};
static const int kCount = sizeof(kPaths) / sizeof(kPaths[0]);

struct V { float x, y, z, rhw; D3DCOLOR c; };
#define FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// Must have C linkage: RXDK's startup looks up the unmangled _main.
extern "C" void __cdecl main(void)
{
    LPDIRECT3D8 d3d = Direct3DCreate8(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp;
    memset(&pp, 0, sizeof(pp));
    pp.BackBufferWidth  = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferCount  = 1;
    pp.SwapEffect       = D3DSWAPEFFECT_DISCARD;

    LPDIRECT3DDEVICE8 dev = NULL;
    d3d->CreateDevice(0, D3DDEVTYPE_HAL, NULL, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    dev->SetVertexShader(FVF);
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG2);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

    /* Probe once, before drawing, so the disc is not being touched mid-frame. */
    int ok[16];
    for (int i = 0; i < kCount; i++) {
        FILE *f = fopen(kPaths[i], "rb");
        ok[i] = (f != NULL);
        if (f) fclose(f);
    }

    for (;;) {
        dev->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(16, 16, 24), 1.0f, 0);
        dev->BeginScene();
        for (int i = 0; i < kCount; i++) {
            const float y0 = 40.0f + i * 64.0f, y1 = y0 + 44.0f;
            const D3DCOLOR c = ok[i] ? D3DCOLOR_XRGB(40, 200, 70)
                                     : D3DCOLOR_XRGB(200, 50, 40);
            V v[6] = {
                {  60.0f, y0, 0.0f, 1.0f, c }, { 580.0f, y0, 0.0f, 1.0f, c },
                { 580.0f, y1, 0.0f, 1.0f, c }, {  60.0f, y0, 0.0f, 1.0f, c },
                { 580.0f, y1, 0.0f, 1.0f, c }, {  60.0f, y1, 0.0f, 1.0f, c },
            };
            dev->DrawVerticesUP(D3DPT_TRIANGLELIST, 6, v, sizeof(V));
        }
        dev->EndScene();
        dev->Present(NULL, NULL, NULL, NULL);
    }
}
