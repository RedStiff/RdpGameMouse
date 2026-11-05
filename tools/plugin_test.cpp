// Loads the client DLL like mstsc would and runs its plugin lifecycle against fake channel objects.
// With "tone" as the second argument it also opens RGAUDIO and plays a quiet 440 Hz tone for 2 s
// through the plugin's audio path (packets paced like the agent sends them).
#include <windows.h>
#include <tsvirtualchannels.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "../common/rgm_audio_proto.h"

static int g_writes = 0;
static IWTSListenerCallback* g_mouseCb = nullptr;
static IWTSListenerCallback* g_audioCb = nullptr;

struct FakeChannel : IWTSVirtualChannel {
    LONG ref = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { *p = this; AddRef(); return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref; }
    ULONG STDMETHODCALLTYPE Release() override { return --ref; }
    HRESULT STDMETHODCALLTYPE Write(ULONG, BYTE*, IUnknown*) override { g_writes++; return S_OK; }
    HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }
};

struct FakeMgr : IWTSVirtualChannelManager {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** p) override { *p = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE CreateListener(const char* name, ULONG, IWTSListenerCallback* cb, IWTSListener** l) override {
        printf("CreateListener(%s)\n", name);
        (strcmp(name, kAudioChannelName) == 0 ? g_audioCb : g_mouseCb) = cb;
        cb->AddRef();
        if (l) *l = nullptr;
        return S_OK;
    }
};

typedef HRESULT(__stdcall* GetInstance)(REFIID, ULONG*, VOID**);

static void PlayTone() {
    FakeChannel ch;
    BOOL accept = FALSE; IWTSVirtualChannelCallback* ccb = nullptr;
    g_audioCb->OnNewChannelConnection(&ch, nullptr, &accept, &ccb);
    printf("audio accepted=%d\n", accept);
    if (!accept) return;
    const UINT32 rate = 48000, perPacket = 384;
    BYTE pkt[sizeof(AudioHeader) + perPacket * 4];
    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    UINT64 sent = 0;
    for (UINT32 seq = 0; sent < rate * 2; seq++) {
        AudioHeader h = {kAudioMagic, rate, 2, (UINT16)perPacket, seq};
        memcpy(pkt, &h, sizeof h);
        INT16* s = (INT16*)(pkt + sizeof h);
        for (UINT32 i = 0; i < perPacket; i++) s[i * 2] = s[i * 2 + 1] = (INT16)(3000 * sin(2 * 3.14159265 * 440 * (sent + i) / rate));
        ccb->OnDataReceived(sizeof pkt, pkt);
        sent += perPacket;
        for (;;) {  // pace in real time
            QueryPerformanceCounter(&t);
            if ((t.QuadPart - t0.QuadPart) * rate / f.QuadPart >= sent) break;
            Sleep(1);
        }
    }
    Sleep(300);
    ccb->OnClose(); ccb->Release();
    printf("tone sent: %llu frames\n", sent);
}

int main(int argc, char** argv) {
    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("LoadLibrary failed %lu\n", GetLastError()); return 1; }
    auto gi = (GetInstance)GetProcAddress(m, "VirtualChannelGetInstance");
    if (!gi) { printf("export missing\n"); return 1; }
    ULONG n = 0;
    gi(__uuidof(IWTSPlugin), &n, nullptr);
    IWTSPlugin* p = nullptr;
    HRESULT hr = gi(__uuidof(IWTSPlugin), &n, (void**)&p);
    printf("GetInstance hr=%08lx n=%lu\n", hr, n);
    FakeMgr mgr;
    printf("Initialize hr=%08lx\n", p->Initialize(&mgr));
    p->Connected();
    FakeChannel ch;
    BOOL accept = FALSE; IWTSVirtualChannelCallback* ccb = nullptr;
    g_mouseCb->OnNewChannelConnection(&ch, nullptr, &accept, &ccb);
    printf("accepted=%d channel refs=%ld\n", accept, ch.ref);
    BYTE on = 1;
    for (int i = 0; i < 5; i++) { ccb->OnDataReceived(1, &on); Sleep(100); }
    BYTE off = 0; ccb->OnDataReceived(1, &off);
    ccb->OnClose(); ccb->Release();
    printf("after close channel refs=%ld writes=%d\n", ch.ref, g_writes);
    if (argc > 2 && strcmp(argv[2], "tone") == 0 && g_audioCb) PlayTone();
    p->Disconnected(0);
    printf("Terminated hr=%08lx\n", p->Terminated());
    p->Release(); g_mouseCb->Release();
    if (g_audioCb) g_audioCb->Release();
    FreeLibrary(m);
    printf("OK\n");
    return 0;
}
