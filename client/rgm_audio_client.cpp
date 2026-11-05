// RdpGameMouse — звук з віддаленої машини (канал RGAUDIO).
//
// Агент шле звук, знятий з мікшера віддаленої машини ще до буферизації RDP, пакетами до 8 мс.
// Тут вони потрапляють в адаптивний буфер (від 30 мс; росте, лише коли пакети запізнюються)
// і відтворюються через WASAPI. Поки канал відкрито, стандартний звук RDP у цьому процесі
// mstsc заглушено, щоб не було луни.
//
// Налаштування (HKCU\Software\RdpGameMouse, DWORD):
//   Audio = 0        не приймати канал: агент не шле звук, лишається звичайний звук RDP;
//   AudioBufferMs    мінімальний буфер, мс (типово 30).

#include "rgm_audio_client.h"
#include "com_object.h"
#include "../common/rgm_audio_proto.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <avrt.h>
#include <wrl/client.h>
#include <atomic>
#include <cwchar>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;

static const wchar_t kSettingsKey[] = L"Software\\RdpGameMouse";
// Позначка «звук RDP заглушено нами», щоб повернути його, навіть якщо mstsc завершився аварійно:
// Windows пам'ятає гучність застосунку між запусками.
static const wchar_t kMutedFlag[] = L"RdpAudioMuted";
// Власний аудіосеанс для нашого потоку, щоб заглушення звуку RDP у цьому ж процесі його не зачепило.
static const GUID kSessionGuid = {0x6f1c0b52, 0x3a9e, 0x4c2b, {0x9d, 0x57, 0x1e, 0x8a, 0x4f, 0x2c, 0x7b, 0x31}};

static const DWORD kIdleCloseMs = 5000;    // без пакетів стільки — закриваємо потік відтворення
static const DWORD kMaxBufferMs = 150;
static const DWORD kBufferStepMs = 10;
static const DWORD kHardCapMs = 300;       // більше в черзі — скидаємо до цільового рівня
static const DWORD kLatePacketMs = 100;    // пакет після спорожнення черги в межах цього — затримка мережі
static const REFERENCE_TIME kDeviceBuffer = 200000;  // 20 мс

static DWORD ReadSetting(const wchar_t* name, DWORD def) {
    DWORD v = 0, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return v;
    return def;
}

static void WriteSetting(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kSettingsKey, name, REG_DWORD, &v, sizeof(v));
}

// Черга кадрів (стерео int16 як UINT32) між потоком каналу і потоком відтворення.
// Тримає затримку мінімальною: запас понад цільовий скидається, а ціль зростає лише тоді,
// коли черга спорожніла і пакет прийшов слідом (запізнення в мережі, а не кінець звуку).
class JitterBuffer {
    std::mutex lock_;
    std::vector<UINT32> ring_ = std::vector<UINT32>(1 << 17);
    size_t head_ = 0, size_ = 0;
    UINT32 rate_ = 0;
    DWORD lastPacket_ = 0, starvedAt_ = 0, lastUnderrun_ = 0, windowStart_ = 0;
    DWORD baseMs_ = 30, bufferMs_ = 30;
    size_t minCushion_ = SIZE_MAX;
    bool playing_ = false;

    size_t Frames(DWORD ms) const { return static_cast<size_t>(rate_) * ms / 1000; }
    void Drop(size_t n) {
        n = min(n, size_);
        head_ = (head_ + n) % ring_.size();
        size_ -= n;
    }

public:
    void SetBase(DWORD ms) {
        std::lock_guard<std::mutex> l(lock_);
        baseMs_ = bufferMs_ = min(max(ms, 10ul), kMaxBufferMs);
    }
    UINT32 Rate() {
        std::lock_guard<std::mutex> l(lock_);
        return rate_;
    }
    DWORD LastPacket() {
        std::lock_guard<std::mutex> l(lock_);
        return lastPacket_;
    }
    void Reset() {
        std::lock_guard<std::mutex> l(lock_);
        head_ = size_ = 0;
        playing_ = false;
        starvedAt_ = 0;
    }

    void Push(UINT32 rate, const BYTE* frames, size_t n) {
        std::lock_guard<std::mutex> l(lock_);
        DWORD now = GetTickCount();
        if (rate != rate_) {
            rate_ = rate;
            head_ = size_ = 0;
            playing_ = false;
        }
        if (starvedAt_ && now - starvedAt_ < kLatePacketMs) {
            bufferMs_ = min(bufferMs_ + kBufferStepMs, kMaxBufferMs);
            lastUnderrun_ = now;
        }
        starvedAt_ = 0;
        lastPacket_ = now;
        const size_t cap = ring_.size();
        if (size_ + n > cap) Drop(size_ + n - cap);
        size_t tail = (head_ + size_) % cap;
        for (size_t i = 0; i < n; i++) memcpy(&ring_[(tail + i) % cap], frames + i * 4, 4);
        size_ += n;
    }

    // Заповнює n кадрів для пристрою; чого бракує — тиша.
    void Fill(UINT32* out, size_t n) {
        std::lock_guard<std::mutex> l(lock_);
        DWORD now = GetTickCount();
        if (size_ > Frames(kHardCapMs)) Drop(size_ - Frames(bufferMs_));
        if (!playing_ && size_ > 0 && size_ >= Frames(bufferMs_)) {
            playing_ = true;
            minCushion_ = SIZE_MAX;
            windowStart_ = now;
        }
        size_t take = playing_ ? min(n, size_) : 0;
        for (size_t i = 0; i < take; i++) out[i] = ring_[(head_ + i) % ring_.size()];
        head_ = (head_ + take) % ring_.size();
        size_ -= take;
        memset(out + take, 0, (n - take) * 4);
        if (!playing_) return;
        if (take < n) {
            playing_ = false;
            starvedAt_ = now;
            return;
        }
        minCushion_ = min(minCushion_, size_);
        if (now - windowStart_ >= 1000) {
            // Увесь останній інтервал у черзі лежав зайвий запас (різниця годинників, пачка
            // пакетів після затримки) — скорочуємо до цільового рівня.
            size_t target = Frames(bufferMs_);
            if (minCushion_ > target + Frames(10)) Drop(minCushion_ - target);
            if (bufferMs_ > baseMs_ && now - lastUnderrun_ > 30000) {
                bufferMs_ = max(baseMs_, bufferMs_ - kBufferStepMs / 2);
                lastUnderrun_ = now;
            }
            minCushion_ = SIZE_MAX;
            windowStart_ = now;
        }
    }
};

static JitterBuffer g_jitter;
static std::atomic<int> g_openChannels{0};
static std::atomic<bool> g_audioStop{false};
static HANDLE g_wake;
static HANDLE g_audioThread;
static IWTSListener* g_audioListener;

static std::wstring DefaultRenderId() {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    LPWSTR id = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || FAILED(dev->GetId(&id)))
        return L"";
    std::wstring s = id;
    CoTaskMemFree(id);
    return s;
}

class Player {
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT32 bufferFrames_ = 0, rate_ = 0;
    std::wstring deviceId_, sessionId_;

public:
    ~Player() {
        Close();
        CloseHandle(event_);
    }
    bool IsOpen() const { return render_ != nullptr; }
    UINT32 Rate() const { return rate_; }
    const std::wstring& SessionId() const { return sessionId_; }
    bool DefaultDeviceChanged() const { return DefaultRenderId() != deviceId_; }

    bool Open(UINT32 rate) {
        Close();
        ComPtr<IMMDeviceEnumerator> en;
        ComPtr<IMMDevice> dev;
        LPWSTR id = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || FAILED(dev->GetId(&id)))
            return false;
        deviceId_ = id;
        CoTaskMemFree(id);

        // Формат агента як є; Windows сама перетворить його на формат пристрою.
        WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 2, rate, rate * 4, 4, 16, 0};
        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()))) ||
            FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBuffer, 0, &wf, &kSessionGuid)) ||
            FAILED(client_->GetBufferSize(&bufferFrames_)) || FAILED(client_->SetEventHandle(event_)) ||
            FAILED(client_->GetService(IID_PPV_ARGS(&render_)))) {
            Close();
            return false;
        }
        ComPtr<IAudioSessionControl> ctl;
        ComPtr<IAudioSessionControl2> ctl2;
        if (SUCCEEDED(client_->GetService(IID_PPV_ARGS(&ctl))) && SUCCEEDED(ctl.As(&ctl2)) &&
            SUCCEEDED(ctl2->GetSessionInstanceIdentifier(&id))) {
            sessionId_ = id;
            CoTaskMemFree(id);
        }
        ComPtr<ISimpleAudioVolume> vol;
        if (SUCCEEDED(client_->GetService(IID_PPV_ARGS(&vol)))) vol->SetMute(FALSE, nullptr);
        if (FAILED(client_->Start())) {
            Close();
            return false;
        }
        rate_ = rate;
        return true;
    }

    void Close() {
        if (client_) client_->Stop();
        render_.Reset();
        client_.Reset();
        rate_ = 0;
        sessionId_.clear();
    }

    // Чекає, поки пристрій попросить даних, і доливає буфер. false — пристрій зник.
    bool Pump() {
        WaitForSingleObject(event_, 50);
        UINT32 padding = 0;
        if (FAILED(client_->GetCurrentPadding(&padding))) return false;
        UINT32 free = bufferFrames_ - padding;
        if (!free) return true;
        BYTE* data = nullptr;
        if (FAILED(render_->GetBuffer(free, &data))) return false;
        g_jitter.Fill(reinterpret_cast<UINT32*>(data), free);
        return SUCCEEDED(render_->ReleaseBuffer(free, 0));
    }
};

// Глушить або повертає інші аудіосеанси цього процесу (тобто стандартний звук RDP у mstsc)
// на всіх пристроях. Повертає кількість знайдених сеансів.
static int MuteRdpAudio(BOOL mute, const std::wstring& ownSession) {
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDeviceCollection> devs;
    UINT count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devs)) || FAILED(devs->GetCount(&count)))
        return 0;
    wchar_t guid[40];
    StringFromGUID2(kSessionGuid, guid, 40);
    const DWORD pid = GetCurrentProcessId();
    int found = 0;
    for (UINT d = 0; d < count; d++) {
        ComPtr<IMMDevice> dev;
        ComPtr<IAudioSessionManager2> mgr;
        ComPtr<IAudioSessionEnumerator> sessions;
        int n = 0;
        if (FAILED(devs->Item(d, &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(mgr.GetAddressOf()))) ||
            FAILED(mgr->GetSessionEnumerator(&sessions)) || FAILED(sessions->GetCount(&n)))
            continue;
        for (int i = 0; i < n; i++) {
            ComPtr<IAudioSessionControl> ctl;
            ComPtr<IAudioSessionControl2> ctl2;
            ComPtr<ISimpleAudioVolume> vol;
            DWORD spid = 0;
            if (FAILED(sessions->GetSession(i, &ctl)) || FAILED(ctl.As(&ctl2)) || ctl2->GetProcessId(&spid) != S_OK ||
                spid != pid || FAILED(ctl.As(&vol)))
                continue;
            bool own = false;
            LPWSTR id = nullptr;
            if (SUCCEEDED(ctl2->GetSessionInstanceIdentifier(&id))) {
                own = ownSession == id;
                CoTaskMemFree(id);
            }
            if (SUCCEEDED(ctl2->GetSessionIdentifier(&id))) {
                _wcsupr_s(id, wcslen(id) + 1);
                own = own || wcsstr(id, guid) != nullptr;
                CoTaskMemFree(id);
            }
            if (own) continue;
            found++;
            BOOL cur = FALSE;
            if (SUCCEEDED(vol->GetMute(&cur)) && cur != mute) vol->SetMute(mute, nullptr);
        }
    }
    return found;
}

static DWORD WINAPI AudioWorker(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    g_jitter.SetBase(ReadSetting(L"AudioBufferMs", 30));

    Player player;
    bool muted = ReadSetting(kMutedFlag, 0) != 0;  // попередній mstsc міг не встигнути повернути звук
    DWORD lastCheck = 0;
    while (!g_audioStop) {
        DWORD now = GetTickCount();
        bool open = g_openChannels > 0;
        UINT32 rate = g_jitter.Rate();
        bool active = open && rate && now - g_jitter.LastPacket() < kIdleCloseMs;
        if (active) {
            if (player.IsOpen() && player.Rate() != rate) player.Close();
            if (!player.IsOpen()) {
                if (!player.Open(rate)) {
                    WaitForSingleObject(g_wake, 500);
                    continue;
                }
                lastCheck = 0;  // заглушити звук RDP одразу
            }
            if (!player.Pump()) player.Close();
        } else {
            if (player.IsOpen()) player.Close();
            WaitForSingleObject(g_wake, 200);
        }

        now = GetTickCount();
        if (now - lastCheck < 1000) continue;
        lastCheck = now;
        if (player.IsOpen() && player.DefaultDeviceChanged()) player.Close();
        // Перевіряємо щосекунди: mstsc створює свій аудіопотік, лише коли звук RDP починає грати.
        if (open && player.IsOpen()) {
            if (MuteRdpAudio(TRUE, player.SessionId()) > 0 && !muted) {
                muted = true;
                WriteSetting(kMutedFlag, 1);
            }
        } else if (!open && muted && MuteRdpAudio(FALSE, L"") > 0) {
            muted = false;
            WriteSetting(kMutedFlag, 0);
        }
    }
    player.Close();
    if (muted && MuteRdpAudio(FALSE, L"") > 0) WriteSetting(kMutedFlag, 0);
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
    return 0;
}

class AudioChannelCallback : public ComObject<IWTSVirtualChannelCallback> {
public:
    HRESULT STDMETHODCALLTYPE OnDataReceived(ULONG cbSize, BYTE* pBuffer) override {
        AudioHeader h;
        if (cbSize < sizeof(h)) return S_OK;
        memcpy(&h, pBuffer, sizeof(h));
        if (h.magic != kAudioMagic || h.channels != 2 || !h.sampleRate || h.frames > kAudioMaxFrames ||
            cbSize < sizeof(h) + h.frames * 4u)
            return S_OK;
        g_jitter.Push(h.sampleRate, pBuffer + sizeof(h), h.frames);
        SetEvent(g_wake);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnClose() override {
        g_openChannels--;
        g_jitter.Reset();
        SetEvent(g_wake);
        return S_OK;
    }
};

class AudioListenerCallback : public ComObject<IWTSListenerCallback> {
public:
    HRESULT STDMETHODCALLTYPE OnNewChannelConnection(IWTSVirtualChannel*, BSTR, BOOL* pbAccept,
                                                     IWTSVirtualChannelCallback** ppCallback) override {
        if (!ReadSetting(L"Audio", 1)) {
            *pbAccept = FALSE;
            *ppCallback = nullptr;
            return S_OK;
        }
        *pbAccept = TRUE;
        *ppCallback = new AudioChannelCallback();
        g_openChannels++;
        SetEvent(g_wake);
        return S_OK;
    }
};

HRESULT AudioInitialize(IWTSVirtualChannelManager* mgr) {
    AudioListenerCallback* cb = new AudioListenerCallback();
    HRESULT hr = mgr->CreateListener(kAudioChannelName, 0, cb, &g_audioListener);
    cb->Release();
    if (FAILED(hr)) return hr;
    g_audioStop = false;
    // Подія лишається відкритою до вивантаження DLL: колбеки каналу можуть сигналити її й після зупинки.
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_audioThread = CreateThread(nullptr, 0, AudioWorker, nullptr, 0, nullptr);
    return S_OK;
}

void AudioTerminate() {
    g_audioStop = true;
    if (g_audioThread) {
        SetEvent(g_wake);
        WaitForSingleObject(g_audioThread, 2000);
        CloseHandle(g_audioThread);
        g_audioThread = nullptr;
    }
    if (g_audioListener) {
        g_audioListener->Release();
        g_audioListener = nullptr;
    }
}
