// RdpGameMouse — плагін клієнта RDP (mstsc), завантажується як DVC-плагін.
//
// Коли агент на віддаленій машині просить «гейм-режим», плагін фіксує локальний
// курсор у центрі вікна RDP (щоб він не впирався в край і mstsc не слав абсолютні
// координати), читає відносний рух миші через Raw Input і пересилає його агенту
// віртуальним каналом RGMOUSE. Кнопки й колесо в цьому режимі теж перехоплюються
// і йдуть каналом: mstsc додає до кожного кліку абсолютну позицію, від якої камера
// в грі стрибала б. Клавіатура йде через RDP як завжди.
//
// Протокол: агент → клієнт: 1 байт (1 = захопити, 0 = відпустити), шле кожні ~200 мс.
//           клієнт → агент: записи по 16 байт {int32 MOUSEEVENTF_*, int32 dx, int32 dy, int32 mouseData}.
// Якщо від агента немає повідомлень понад 1 с, курсор відпускається.

#include <windows.h>
#include <tsvirtualchannels.h>
#include <atomic>
#include <mutex>

static const char kChannelName[] = "RGMOUSE";
static const DWORD kServerTimeoutMs = 1000;
static const DWORD kReclipMs = 200;

static HINSTANCE g_inst;
static std::mutex g_channelLock;
static IWTSVirtualChannel* g_channel;
static IWTSListener* g_listener;
static std::atomic<bool> g_captureRequested{false};
static std::atomic<DWORD> g_lastServerMsg{0};
static std::atomic<bool> g_stop{false};
static HANDLE g_worker;

template <class I>
class ComObject : public I {
    LONG ref_ = 1;

public:
    virtual ~ComObject() {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(I)) {
            *ppv = static_cast<I*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
};

static void SetChannel(IWTSVirtualChannel* ch) {
    std::lock_guard<std::mutex> lock(g_channelLock);
    if (ch) ch->AddRef();
    if (g_channel) g_channel->Release();
    g_channel = ch;
}

static void SendEvent(DWORD flags, LONG dx, LONG dy, LONG data) {
    INT32 rec[4] = {static_cast<INT32>(flags), dx, dy, data};
    std::lock_guard<std::mutex> lock(g_channelLock);
    if (g_channel) g_channel->Write(sizeof(rec), reinterpret_cast<BYTE*>(rec), nullptr);
}

// Стан робочого потоку; хук миші викликається в цьому ж потоці.
static bool g_clipped;
static LONG g_accX, g_accY;

struct ButtonInfo { DWORD down, up; LONG data; UINT bit; };
static const ButtonInfo kButtons[] = {
    {MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0, 1},
    {MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0, 2},
    {MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0, 4},
    {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1, 8},
    {MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2, 16},
};
static UINT g_heldButtons;

static void TrackButton(DWORD flags, LONG data) {
    for (const auto& b : kButtons) {
        if (b.data != 0 && b.data != data) continue;
        if (flags == b.down) g_heldButtons |= b.bit;
        else if (flags == b.up) g_heldButtons &= ~b.bit;
    }
}

static void FlushMove() {
    if (g_accX || g_accY) {
        SendEvent(MOUSEEVENTF_MOVE, g_accX, g_accY, 0);
        g_accX = g_accY = 0;
    }
}

// У гейм-режимі забирає кнопки й колесо в mstsc і шле їх каналом без координат.
static LRESULT CALLBACK MouseHook(int code, WPARAM wParam, LPARAM lParam) {
    auto* info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if (code != HC_ACTION || !g_clipped || (info->flags & LLMHF_INJECTED))
        return CallNextHookEx(nullptr, code, wParam, lParam);

    DWORD flags = 0;
    LONG data = 0;
    switch (wParam) {
        case WM_LBUTTONDOWN: flags = MOUSEEVENTF_LEFTDOWN; break;
        case WM_LBUTTONUP: flags = MOUSEEVENTF_LEFTUP; break;
        case WM_RBUTTONDOWN: flags = MOUSEEVENTF_RIGHTDOWN; break;
        case WM_RBUTTONUP: flags = MOUSEEVENTF_RIGHTUP; break;
        case WM_MBUTTONDOWN: flags = MOUSEEVENTF_MIDDLEDOWN; break;
        case WM_MBUTTONUP: flags = MOUSEEVENTF_MIDDLEUP; break;
        case WM_XBUTTONDOWN: flags = MOUSEEVENTF_XDOWN; data = HIWORD(info->mouseData); break;
        case WM_XBUTTONUP: flags = MOUSEEVENTF_XUP; data = HIWORD(info->mouseData); break;
        case WM_MOUSEWHEEL: flags = MOUSEEVENTF_WHEEL; data = static_cast<SHORT>(HIWORD(info->mouseData)); break;
        case WM_MOUSEHWHEEL: flags = MOUSEEVENTF_HWHEEL; data = static_cast<SHORT>(HIWORD(info->mouseData)); break;
        default: return CallNextHookEx(nullptr, code, wParam, lParam);
    }
    FlushMove();  // рух до кліку має дійти раніше за сам клік
    SendEvent(flags, 0, 0, data);
    TrackButton(flags, data);
    return 1;
}

// Відпускає на віддаленій машині кнопки, натиснуті в гейм-режимі, щоб вони не «залипли»,
// якщо режим вимкнувся раніше, ніж кнопку відпустили.
static void ReleaseHeldButtons() {
    for (const auto& b : kButtons)
        if (g_heldButtons & b.bit) SendEvent(b.up, 0, 0, b.data);
    g_heldButtons = 0;
}

class ChannelCallback : public ComObject<IWTSVirtualChannelCallback> {
    IWTSVirtualChannel* channel_;  // без власного посилання, лише для порівняння

public:
    explicit ChannelCallback(IWTSVirtualChannel* ch) : channel_(ch) {}

    HRESULT STDMETHODCALLTYPE OnDataReceived(ULONG cbSize, BYTE* pBuffer) override {
        if (cbSize > 0) {
            g_captureRequested = pBuffer[cbSize - 1] != 0;
            g_lastServerMsg = GetTickCount();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnClose() override {
        g_captureRequested = false;
        std::lock_guard<std::mutex> lock(g_channelLock);
        if (g_channel == channel_) {
            g_channel->Release();
            g_channel = nullptr;
        }
        return S_OK;
    }
};

class ListenerCallback : public ComObject<IWTSListenerCallback> {
public:
    HRESULT STDMETHODCALLTYPE OnNewChannelConnection(IWTSVirtualChannel* pChannel, BSTR, BOOL* pbAccept,
                                                     IWTSVirtualChannelCallback** ppCallback) override {
        *pbAccept = TRUE;
        *ppCallback = new ChannelCallback(pChannel);
        SetChannel(pChannel);
        return S_OK;
    }
};

static bool ForegroundIsOurs(HWND* fg) {
    *fg = GetForegroundWindow();
    if (!*fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(*fg, &pid);
    return pid == GetCurrentProcessId();
}

static POINT g_clipPoint;

static void Reclip() {
    RECT clip = {g_clipPoint.x, g_clipPoint.y, g_clipPoint.x + 1, g_clipPoint.y + 1};
    ClipCursor(&clip);
}

// Фіксує курсор там, де він зараз (рух читається з Raw Input, тож позиція неважлива).
// Курсор не переноситься: інакше в меню він стрибав би при кожному короткому перемиканні.
static void StartClip(HWND wnd) {
    RECT rc;
    POINT tl = {0, 0}, br = {0, 0}, p;
    if (GetClientRect(wnd, &rc)) {
        br = {rc.right, rc.bottom};
        ClientToScreen(wnd, &tl);
        ClientToScreen(wnd, &br);
    }
    GetCursorPos(&p);
    if (p.x < tl.x || p.y < tl.y || p.x >= br.x || p.y >= br.y) {
        p = {(tl.x + br.x) / 2, (tl.y + br.y) / 2};
        SetCursorPos(p.x, p.y);
    }
    g_clipPoint = p;
    Reclip();
}

static DWORD WINAPI Worker(void*) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = g_inst;
    wc.lpszClassName = L"RdpGameMouseClient";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);

    // INPUTSINK: отримуємо рух миші, хоча наше вікно ніколи не активне.
    RAWINPUTDEVICE rid = {0x01, 0x02, RIDEV_INPUTSINK, wnd};
    RegisterRawInputDevices(&rid, 1, sizeof(rid));
    HHOOK hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, g_inst, 0);

    g_clipped = false;
    DWORD lastClip = 0;

    while (!g_stop) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);

        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_INPUT && g_clipped) {
                RAWINPUT ri;
                UINT size = sizeof(ri);
                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(msg.lParam), RID_INPUT, &ri, &size,
                                    sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
                    ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    g_accX += ri.data.mouse.lLastX;
                    g_accY += ri.data.mouse.lLastY;
                }
            }
            DispatchMessageW(&msg);
        }

        HWND fg = nullptr;
        DWORD now = GetTickCount();
        bool want = g_captureRequested && now - g_lastServerMsg < kServerTimeoutMs && ForegroundIsOurs(&fg);

        if (want && (!g_clipped || now - lastClip > kReclipMs)) {
            // Windows скидає ClipCursor при перемиканні вікон, тому оновлюємо періодично.
            if (!g_clipped) {
                g_accX = g_accY = 0;
                StartClip(fg);
            } else {
                Reclip();
            }
            g_clipped = true;
            lastClip = now;
        } else if (!want && g_clipped) {
            ClipCursor(nullptr);
            g_clipped = false;
            FlushMove();
            ReleaseHeldButtons();
        }

        if (g_clipped) FlushMove();
    }

    if (hook) UnhookWindowsHookEx(hook);
    if (g_clipped) {
        ClipCursor(nullptr);
        ReleaseHeldButtons();
    }
    g_clipped = false;
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, g_inst);
    return 0;
}

class Plugin : public ComObject<IWTSPlugin> {
public:
    HRESULT STDMETHODCALLTYPE Initialize(IWTSVirtualChannelManager* mgr) override {
        ListenerCallback* cb = new ListenerCallback();
        HRESULT hr = mgr->CreateListener(kChannelName, 0, cb, &g_listener);
        cb->Release();
        if (FAILED(hr)) return hr;
        g_stop = false;
        g_worker = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Connected() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Disconnected(DWORD) override {
        g_captureRequested = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Terminated() override {
        g_stop = true;
        if (g_worker) {
            WaitForSingleObject(g_worker, 2000);
            CloseHandle(g_worker);
            g_worker = nullptr;
        }
        SetChannel(nullptr);
        if (g_listener) {
            g_listener->Release();
            g_listener = nullptr;
        }
        return S_OK;
    }
};

// Точка входу DVC-плагіна, коли в реєстрі AddIns\<ім'я>\Name вказано шлях до DLL.
extern "C" HRESULT __stdcall VirtualChannelGetInstance(REFIID refiid, ULONG* pNumObjs, VOID** ppObjArray) {
    if (refiid != __uuidof(IWTSPlugin)) return E_NOINTERFACE;
    if (!pNumObjs) return E_INVALIDARG;
    if (!ppObjArray) {
        *pNumObjs = 1;
        return S_OK;
    }
    if (*pNumObjs < 1) return E_INVALIDARG;
    ppObjArray[0] = static_cast<IWTSPlugin*>(new Plugin());
    *pNumObjs = 1;
    return S_OK;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_inst = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
