// RdpGameMouse — звук віддаленої машини для плагіна клієнта.
//
// Стандартний звук RDP накопичується у великому буфері, тому відстає від зображення.
// Тут звук знімається з мікшера Windows (WASAPI loopback) і йде каналом RGAUDIO
// пакетами до 8 мс, а плагін відтворює його з невеликим буфером.

#include "rgm_audio.h"
#include "../common/rgm_audio_proto.h"
#include <mmdeviceapi.h>
#include <wtsapi32.h>
#include <avrt.h>
#include <cstdarg>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

static const DWORD kChannelRetryMs = 3000;
static const DWORD kDeviceCheckMs = 2000;
static const DWORD kPollMs = 4;

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

bool LoopbackCapture::Open() {
    Close();
    ComPtr<IMMDeviceEnumerator> en;
    ComPtr<IMMDevice> dev;
    LPWSTR id = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
        FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) || FAILED(dev->GetId(&id)))
        return false;
    deviceId_ = id;
    CoTaskMemFree(id);

    WAVEFORMATEX* fmt = nullptr;
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client_.GetAddressOf()))) ||
        FAILED(client_->GetMixFormat(&fmt))) {
        Close();
        return false;
    }
    WORD tag = fmt->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE) tag = static_cast<WORD>(reinterpret_cast<WAVEFORMATEXTENSIBLE*>(fmt)->SubFormat.Data1);
    float_ = tag == WAVE_FORMAT_IEEE_FLOAT;
    bits_ = fmt->wBitsPerSample;
    channels_ = fmt->nChannels;
    blockAlign_ = fmt->nBlockAlign;
    rate_ = fmt->nSamplesPerSec;
    bool supported = (tag == WAVE_FORMAT_IEEE_FLOAT && (bits_ == 32 || bits_ == 64)) ||
                     (tag == WAVE_FORMAT_PCM && (bits_ == 8 || bits_ == 16 || bits_ == 24 || bits_ == 32));
    // Буфер захоплення лише страхує від пауз у відправленні; затримку визначає частота опитування.
    HRESULT hr = supported && channels_ > 0
                     ? client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 2000000, 0, fmt, nullptr)
                     : E_FAIL;
    CoTaskMemFree(fmt);
    if (FAILED(hr) || FAILED(client_->GetService(IID_PPV_ARGS(&capture_))) || FAILED(client_->Start())) {
        Close();
        return false;
    }
    return true;
}

void LoopbackCapture::Close() {
    if (client_) client_->Stop();
    capture_.Reset();
    client_.Reset();
}

bool LoopbackCapture::DefaultDeviceChanged() const { return DefaultRenderId() != deviceId_; }

float LoopbackCapture::Sample(const BYTE* p) const {
    if (float_) return bits_ == 64 ? static_cast<float>(*reinterpret_cast<const double*>(p)) : *reinterpret_cast<const float*>(p);
    switch (bits_) {
        case 8: return (p[0] - 128) / 128.0f;
        case 16: return *reinterpret_cast<const INT16*>(p) / 32768.0f;
        case 24: return static_cast<INT32>(static_cast<UINT32>(p[0]) << 8 | static_cast<UINT32>(p[1]) << 16 |
                                           static_cast<UINT32>(p[2]) << 24) / 2147483648.0f;
        default: return *reinterpret_cast<const INT32*>(p) / 2147483648.0f;
    }
}

static INT16 ToInt16(float v) {
    v *= 32767.0f;
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return static_cast<INT16>(v);
}

bool LoopbackCapture::Read(std::vector<INT16>* out) {
    const UINT step = bits_ / 8;
    for (;;) {
        UINT32 next = 0;
        if (FAILED(capture_->GetNextPacketSize(&next))) return false;
        if (!next) return true;
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        if (FAILED(capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) return false;
        size_t base = out->size();
        out->resize(base + frames * 2, 0);
        if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
            // Більше двох каналів: беремо передні лівий і правий.
            for (UINT32 f = 0; f < frames; f++) {
                const BYTE* p = data + static_cast<size_t>(f) * blockAlign_;
                float l = Sample(p);
                float r = channels_ > 1 ? Sample(p + step) : l;
                (*out)[base + f * 2] = ToInt16(l);
                (*out)[base + f * 2 + 1] = ToInt16(r);
            }
        }
        capture_->ReleaseBuffer(frames);
    }
}

static void (*g_log)(const wchar_t*);

static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 256, fmt, args);
    va_end(args);
    g_log(buf);
}

static bool SendChunks(HANDLE ch, UINT32 rate, const std::vector<INT16>& samples, UINT32* seq) {
    BYTE pkt[sizeof(AudioHeader) + kAudioMaxFrames * 4];
    size_t frames = samples.size() / 2;
    for (size_t off = 0; off < frames; off += kAudioMaxFrames) {
        UINT16 n = static_cast<UINT16>(min(static_cast<size_t>(kAudioMaxFrames), frames - off));
        AudioHeader h = {kAudioMagic, rate, 2, n, (*seq)++};
        memcpy(pkt, &h, sizeof(h));
        memcpy(pkt + sizeof(h), &samples[off * 2], n * 4u);
        ULONG written = 0;
        if (!WTSVirtualChannelWrite(ch, reinterpret_cast<PCHAR>(pkt), static_cast<ULONG>(sizeof(h) + n * 4u), &written))
            return false;
    }
    return true;
}

static DWORD WINAPI ForwardThread(void*) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);

    LoopbackCapture cap;
    HANDLE ch = nullptr;
    std::vector<INT16> buf;
    UINT32 seq = 0;
    DWORD lastDeviceCheck = 0;
    for (;;) {
        if (!ch) {
            ch = WTSVirtualChannelOpenEx(WTS_CURRENT_SESSION, const_cast<LPSTR>(kAudioChannelName),
                                         WTS_CHANNEL_OPTION_DYNAMIC | WTS_CHANNEL_OPTION_DYNAMIC_PRI_REAL);
            if (!ch) {
                Sleep(kChannelRetryMs);
                continue;
            }
            Log(L"Звуковий канал підключено.");
        }
        DWORD now = GetTickCount();
        if (cap.IsOpen() && now - lastDeviceCheck > kDeviceCheckMs) {
            lastDeviceCheck = now;
            if (cap.DefaultDeviceChanged()) cap.Close();
        }
        if (!cap.IsOpen()) {
            if (!cap.Open()) {
                Sleep(1000);
                continue;
            }
            Log(L"Захоплення звуку: %u Гц.", cap.SampleRate());
        }
        buf.clear();
        if (!cap.Read(&buf)) {
            cap.Close();
            continue;
        }
        if (!buf.empty() && !SendChunks(ch, cap.SampleRate(), buf, &seq)) {
            WTSVirtualChannelClose(ch);
            ch = nullptr;
            cap.Close();
            Log(L"Звуковий канал закрито, повторне підключення...");
            continue;
        }
        Sleep(kPollMs);
    }
}

void StartAudioForwarding(void (*log)(const wchar_t* msg)) {
    g_log = log;
    if (HANDLE t = CreateThread(nullptr, 0, ForwardThread, nullptr, 0, nullptr)) CloseHandle(t);
}
