// RdpGameMouse — звук з віддаленої машини: слухач каналу RGAUDIO і потік відтворення.
#pragma once
#include <windows.h>
#include <tsvirtualchannels.h>

HRESULT AudioInitialize(IWTSVirtualChannelManager* mgr);
void AudioTerminate();
