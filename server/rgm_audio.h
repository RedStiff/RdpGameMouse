// RdpGameMouse — передача звуку віддаленої машини плагіну клієнта (канал RGAUDIO).
#pragma once
#include <windows.h>
#include <audioclient.h>
#include <wrl/client.h>
#include <string>
#include <vector>

// Захоплює звук, який система відтворює на пристрої за замовчуванням (у RDP-сеансі це
// «Віддалений звук»), раніше, ніж RDP його закодує й буферизує. COM має бути ініціалізовано.
class LoopbackCapture {
public:
    bool Open();
    void Close();
    bool IsOpen() const { return capture_ != nullptr; }
    UINT32 SampleRate() const { return rate_; }
    // Дописує захоплене в out як стерео int16. false — пристрій зник, треба відкрити знову.
    bool Read(std::vector<INT16>* out);
    // Пристрій за замовчуванням змінився (наприклад, після перепідключення RDP).
    bool DefaultDeviceChanged() const;

private:
    float Sample(const BYTE* p) const;

    Microsoft::WRL::ComPtr<IAudioClient> client_;
    Microsoft::WRL::ComPtr<IAudioCaptureClient> capture_;
    std::wstring deviceId_;
    UINT32 rate_ = 0;
    WORD channels_ = 0, bits_ = 0, blockAlign_ = 0;
    bool float_ = false;
};

// Фоновий потік: відкриває канал RGAUDIO, коли плагін клієнта його приймає, і шле в нього
// захоплений звук. log викликається з цього потоку.
void StartAudioForwarding(void (*log)(const wchar_t* msg));
