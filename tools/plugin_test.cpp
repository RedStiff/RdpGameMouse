// Loads the client DLL like mstsc would and runs its plugin lifecycle against fake channel objects.
#include <windows.h>
#include <tsvirtualchannels.h>
#include <cstdio>

static int g_writes = 0;
static IWTSListenerCallback* g_cb = nullptr;

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
        g_cb = cb; cb->AddRef();
        if (l) *l = nullptr;
        return S_OK;
    }
};

typedef HRESULT(__stdcall* GetInstance)(REFIID, ULONG*, VOID**);

int main(int, char** argv) {
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
    g_cb->OnNewChannelConnection(&ch, nullptr, &accept, &ccb);
    printf("accepted=%d channel refs=%ld\n", accept, ch.ref);
    BYTE on = 1;
    for (int i = 0; i < 5; i++) { ccb->OnDataReceived(1, &on); Sleep(100); }
    BYTE off = 0; ccb->OnDataReceived(1, &off);
    ccb->OnClose(); ccb->Release();
    printf("after close channel refs=%ld writes=%d\n", ch.ref, g_writes);
    p->Disconnected(0);
    printf("Terminated hr=%08lx\n", p->Terminated());
    p->Release(); g_cb->Release();
    FreeLibrary(m);
    printf("OK\n");
    return 0;
}
