// Checks the agent's loopback capture: records what this machine is playing for N seconds and
// prints level and an estimate of the dominant frequency.
// Build: cl /EHsc /O2 /utf-8 /DUNICODE tools\audio_test.cpp server\rgm_audio.cpp wtsapi32.lib
#include "../server/rgm_audio.h"
#include <cmath>
#include <cstdio>

int wmain(int argc, wchar_t** argv) {
    int seconds = argc > 1 ? _wtoi(argv[1]) : 3;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    LoopbackCapture cap;
    if (!cap.Open()) {
        printf("capture open failed\n");
        return 1;
    }
    printf("rate=%u\n", cap.SampleRate());
    std::vector<INT16> all;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < static_cast<DWORD>(seconds) * 1000) {
        if (!cap.Read(&all)) {
            printf("read failed\n");
            return 1;
        }
        Sleep(5);
    }
    size_t frames = all.size() / 2, crossings = 0, loud = 0;
    double sum = 0;
    int peak = 0;
    for (size_t i = 0; i < frames; i++) {
        int v = all[i * 2];
        sum += double(v) * v;
        peak = max(peak, abs(v));
        if (abs(v) > 300) loud++;
        if (i && (all[(i - 1) * 2] < 0) != (v < 0)) crossings++;
    }
    double rms = frames ? sqrt(sum / frames) : 0;
    double secs = double(frames) / cap.SampleRate();
    printf("frames=%zu (%.2f s) peak=%d rms=%.0f loud=%.0f%% freq~%.0f Hz\n", frames, secs, peak, rms,
           frames ? 100.0 * loud / frames : 0, secs > 0 ? crossings / 2.0 / secs : 0);
    return 0;
}
