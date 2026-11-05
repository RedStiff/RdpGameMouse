// RdpGameMouse — протокол звукового каналу RGAUDIO (спільний для агента і плагіна).
//
// Агент → клієнт: пакети {AudioHeader, frames × стерео int16}. Пакет не більший за один
// фрагмент віртуального каналу (1600 байт), тож доходить одним повідомленням.
// Клієнт → агент: нічого.
#pragma once
#include <windows.h>

static const char kAudioChannelName[] = "RGAUDIO";
static const UINT32 kAudioMagic = 0x31414752;  // "RGA1"
static const UINT kAudioMaxFrames = 384;

#pragma pack(push, 1)
struct AudioHeader {
    UINT32 magic;
    UINT32 sampleRate;
    UINT16 channels;  // завжди 2
    UINT16 frames;
    UINT32 seq;
};
#pragma pack(pop)
