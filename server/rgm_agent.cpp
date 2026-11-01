// RdpGameMouse — агент на віддаленій машині (запускається всередині RDP-сеансу).
//
// Відкриває віртуальний канал RGMOUSE до плагіна в mstsc, вирішує, коли потрібен
// «гейм-режим», і вводить отримані від клієнта відносний рух, кнопки й колесо через
// SendInput, тож гра бачить звичайну мишу, а не абсолютні координати RDP.
//
// Використання: RdpGameMouseAgent.exe [--process ІмяПроцесу] [--key Ctrl+Alt+F12] [--mode auto|always|off]
//                                     [--exit-with-game] [--log файл] [--fullscreen [--exclude a,b,c]]
//   --process         процес гри без .exe; без нього працює з будь-яким активним вікном.
//   --fullscreen      загальносистемний режим: ціль — будь-яке вікно на весь монітор, крім
//                     процесів з --exclude (за замовчуванням браузери й відеоплеєри). Один агент на сеанс.
//                     Фонова збірка RdpGameMouseSystem.exe — та сама програма без вікна консолі.
//   --key             комбінація перемикання режимів Авто → Завжди → Вимкнено
//                     (модифікатори Ctrl/Alt/Shift + F1–F24, літера/цифра, ScrollLock, Pause, Insert, Home, End, PageUp, PageDown).
//   --exit-with-game  чекати запуску гри (до 2 хв) і завершитися, коли її закрито.
//   --log             журнал рішень авто-режиму (для діагностики).

#include <windows.h>
#include <wtsapi32.h>
#include <pchannel.h>
#include <tlhelp32.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")

static const char kChannelName[] = "RGMOUSE";
static const DWORD kStateIntervalMs = 200;
static const DWORD kReconnectMs = 3000;
static const DWORD kGameStartTimeoutMs = 120000;
// Ігри на мить міняють курсор (стрілка посеред геймплею, приховування посеред меню),
// тому стан має протриматися, перш ніж перемикати режим. Заміряно на UE5-грі:
// такі спалахи тривають до ~45 мс (приховано) і ~33 мс (видно).
static const DWORD kEngageDelayMs = 70;
static const DWORD kReleaseDelayMs = 80;

// Кнопки, натиснуті через агент; відпускаються, коли гейм-режим вимикається, щоб не «залипали».
static UINT g_injectedButtons;

static void TrackInjected(DWORD flags, DWORD data) {
    if (flags & MOUSEEVENTF_LEFTDOWN) g_injectedButtons |= 1;
    if (flags & MOUSEEVENTF_LEFTUP) g_injectedButtons &= ~1u;
    if (flags & MOUSEEVENTF_RIGHTDOWN) g_injectedButtons |= 2;
    if (flags & MOUSEEVENTF_RIGHTUP) g_injectedButtons &= ~2u;
    if (flags & MOUSEEVENTF_MIDDLEDOWN) g_injectedButtons |= 4;
    if (flags & MOUSEEVENTF_MIDDLEUP) g_injectedButtons &= ~4u;
    UINT x = data == XBUTTON1 ? 8 : data == XBUTTON2 ? 16 : 0;
    if (flags & MOUSEEVENTF_XDOWN) g_injectedButtons |= x;
    if (flags & MOUSEEVENTF_XUP) g_injectedButtons &= ~x;
}

static void ReleaseInjectedButtons() {
    static const struct { UINT bit; DWORD up; DWORD data; } kUps[] = {
        {1, MOUSEEVENTF_LEFTUP, 0}, {2, MOUSEEVENTF_RIGHTUP, 0}, {4, MOUSEEVENTF_MIDDLEUP, 0},
        {8, MOUSEEVENTF_XUP, XBUTTON1}, {16, MOUSEEVENTF_XUP, XBUTTON2}};
    for (const auto& u : kUps) {
        if (!(g_injectedButtons & u.bit)) continue;
        INPUT in = {};
        in.type = INPUT_MOUSE;
        in.mi.dwFlags = u.up;
        in.mi.mouseData = u.data;
        SendInput(1, &in, sizeof(in));
    }
    g_injectedButtons = 0;
}

enum Mode { ModeAuto, ModeAlways, ModeOff };
static const wchar_t* kModeNames[] = {L"АВТО (лише коли гра ховає курсор)", L"ЗАВЖДИ", L"ВИМКНЕНО"};

class Channel {
    HANDLE wts_ = nullptr;
    HANDLE file_ = nullptr;
    OVERLAPPED rov_ = {}, wov_ = {};
    BYTE rbuf_[sizeof(CHANNEL_PDU_HEADER) + CHANNEL_CHUNK_LENGTH];
    BYTE wbyte_ = 0;
    bool writePending_ = false;

    bool StartRead() {
        if (ReadFile(file_, rbuf_, sizeof(rbuf_), nullptr, &rov_)) return true;
        return GetLastError() == ERROR_IO_PENDING;
    }

public:
    ~Channel() { Close(); }
    bool IsOpen() const { return file_ != nullptr; }
    HANDLE ReadEvent() const { return rov_.hEvent; }

    bool Open() {
        wts_ = WTSVirtualChannelOpenEx(WTS_CURRENT_SESSION, const_cast<LPSTR>(kChannelName), WTS_CHANNEL_OPTION_DYNAMIC);
        if (!wts_) return false;
        void* buf = nullptr;
        DWORD len = 0;
        if (!WTSVirtualChannelQuery(wts_, WTSVirtualFileHandle, &buf, &len) || len != sizeof(HANDLE)) {
            if (buf) WTSFreeMemory(buf);
            Close();
            return false;
        }
        DuplicateHandle(GetCurrentProcess(), *static_cast<HANDLE*>(buf), GetCurrentProcess(), &file_, 0, FALSE,
                        DUPLICATE_SAME_ACCESS);
        WTSFreeMemory(buf);
        rov_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        wov_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!file_ || !StartRead()) {
            Close();
            return false;
        }
        return true;
    }

    void Close() {
        if (file_) {
            CancelIo(file_);
            CloseHandle(file_);
            file_ = nullptr;
        }
        if (wts_) {
            WTSVirtualChannelClose(wts_);
            wts_ = nullptr;
        }
        if (rov_.hEvent) CloseHandle(rov_.hEvent);
        if (wov_.hEvent) CloseHandle(wov_.hEvent);
        rov_ = {};
        wov_ = {};
        writePending_ = false;
    }

    // Обробляє завершене читання; false — канал розірвано.
    bool PumpRead() {
        if (WaitForSingleObject(rov_.hEvent, 0) != WAIT_OBJECT_0) return true;
        DWORD n = 0;
        if (!GetOverlappedResult(file_, &rov_, &n, FALSE)) return false;
        if (n > sizeof(CHANNEL_PDU_HEADER)) {
            const BYTE* data = rbuf_ + sizeof(CHANNEL_PDU_HEADER);
            DWORD len = n - sizeof(CHANNEL_PDU_HEADER);
            // Запис: {MOUSEEVENTF_*, dx, dy, mouseData}. Абсолютні координати не пропускаємо.
            const DWORD allowed = MOUSEEVENTF_MOVE | MOUSEEVENTF_LEFTDOWN | MOUSEEVENTF_LEFTUP |
                                  MOUSEEVENTF_RIGHTDOWN | MOUSEEVENTF_RIGHTUP | MOUSEEVENTF_MIDDLEDOWN |
                                  MOUSEEVENTF_MIDDLEUP | MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP |
                                  MOUSEEVENTF_WHEEL | MOUSEEVENTF_HWHEEL;
            for (DWORD off = 0; off + 16 <= len; off += 16) {
                INT32 rec[4];
                memcpy(rec, data + off, sizeof(rec));
                INPUT in = {};
                in.type = INPUT_MOUSE;
                in.mi.dwFlags = static_cast<DWORD>(rec[0]) & allowed;
                in.mi.dx = rec[1];
                in.mi.dy = rec[2];
                in.mi.mouseData = static_cast<DWORD>(rec[3]);
                if (in.mi.dwFlags) {
                    SendInput(1, &in, sizeof(in));
                    TrackInjected(in.mi.dwFlags, in.mi.mouseData);
                }
            }
        }
        return StartRead();
    }

    bool SendState(bool capture) {
        if (writePending_) {
            if (WaitForSingleObject(wov_.hEvent, 0) != WAIT_OBJECT_0) return true;  // попередній ще йде
            DWORD n;
            if (!GetOverlappedResult(file_, &wov_, &n, FALSE)) return false;
            writePending_ = false;
        }
        wbyte_ = capture ? 1 : 0;
        if (WriteFile(file_, &wbyte_, 1, nullptr, &wov_) || GetLastError() == ERROR_IO_PENDING) {
            writePending_ = true;
            return true;
        }
        return false;
    }
};

static std::wstring g_process;  // без .exe, нижній регістр

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

static bool g_fullscreen;           // загальносистемний режим: ціль — будь-яке повноекранне вікно
static std::wstring g_exclude = L",chrome,msedge,firefox,opera,brave,vivaldi,vlc,mpc-hc64,mpc-be64,potplayermini64,explorer,mstsc,liteshell,";

// Ім'я процесу вікна без .exe, у нижньому регістрі.
static const std::wstring& WindowProcessName(HWND wnd) {
    static DWORD cachedPid = 0;
    static std::wstring cachedName;
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid != cachedPid) {
        cachedPid = pid;
        cachedName.clear();
        if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t path[MAX_PATH];
            DWORD size = MAX_PATH;
            if (QueryFullProcessImageNameW(h, 0, path, &size)) {
                std::wstring name = path;
                name = name.substr(name.find_last_of(L'\\') + 1);
                if (name.size() > 4) name.resize(name.size() - 4);
                cachedName = Lower(name);
            }
            CloseHandle(h);
        }
    }
    return cachedName;
}

// Вікно на весь монітор (ексклюзивний або безрамковий повноекранний режим).
// Розгорнуті звичайні вікна теж накривають монітор, але це не ігри.
static bool IsFullscreenWindow(HWND wnd) {
    if (!IsWindowVisible(wnd) || IsIconic(wnd) || IsZoomed(wnd)) return false;
    if ((GetWindowLongW(wnd, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) return false;
    wchar_t cls[32] = L"";
    GetClassNameW(wnd, cls, 32);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    RECT wr;
    MONITORINFO mi = {sizeof(mi)};
    if (!GetWindowRect(wnd, &wr) || !GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi))
        return false;
    return wr.left <= mi.rcMonitor.left && wr.top <= mi.rcMonitor.top && wr.right >= mi.rcMonitor.right &&
           wr.bottom >= mi.rcMonitor.bottom;
}

static bool IsTargetWindow(HWND wnd) {
    if (!wnd) return false;
    if (g_fullscreen) {
        if (!IsFullscreenWindow(wnd)) return false;
        const std::wstring& name = WindowProcessName(wnd);
        return !name.empty() && g_exclude.find(L"," + name + L",") == std::wstring::npos;
    }
    if (g_process.empty()) return true;
    return WindowProcessName(wnd) == g_process;
}

static bool GameRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W pe = {sizeof(pe)};
    bool found = false;
    std::wstring exe = g_process + L".exe";
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe))
        found = Lower(pe.szExeFile) == exe;
    CloseHandle(snap);
    return found;
}

static bool BitmapPixels(HBITMAP bmp, std::vector<DWORD>* px) {
    BITMAP bm;
    if (!GetObjectW(bmp, sizeof(bm), &bm)) return false;
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    px->resize(static_cast<size_t>(bm.bmWidth) * bm.bmHeight);
    HDC dc = GetDC(nullptr);
    int lines = GetDIBits(dc, bmp, 0, bm.bmHeight, px->data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == bm.bmHeight;
}

// Деякі ігри «ховають» курсор, підставляючи повністю прозоре зображення замість SetCursor(NULL).
static bool IsTransparentCursor(HCURSOR cursor) {
    static std::unordered_map<HCURSOR, bool> cache;
    auto it = cache.find(cursor);
    if (it != cache.end()) return it->second;

    bool transparent = false;
    ICONINFO ii;
    if (GetIconInfo(cursor, &ii)) {
        std::vector<DWORD> mask, color;
        bool maskOk = ii.hbmMask && BitmapPixels(ii.hbmMask, &mask);
        if (ii.hbmColor) {
            if (maskOk && BitmapPixels(ii.hbmColor, &color) && color.size() == mask.size()) {
                bool hasAlpha = false;
                for (DWORD p : color) hasAlpha |= (p >> 24) != 0;
                transparent = true;
                for (size_t i = 0; i < color.size() && transparent; i++)
                    transparent = hasAlpha ? (color[i] >> 24) == 0
                                           : (color[i] & 0xFFFFFF) == 0 && (mask[i] & 0xFFFFFF) != 0;
            }
        } else if (maskOk) {
            // Монохромний: верхня половина — AND-маска, нижня — XOR. Прозорий піксель: AND=1, XOR=0.
            size_t half = mask.size() / 2;
            transparent = half > 0;
            for (size_t i = 0; i < half && transparent; i++)
                transparent = (mask[i] & 0xFFFFFF) != 0 && (mask[half + i] & 0xFFFFFF) == 0;
        }
        if (ii.hbmMask) DeleteObject(ii.hbmMask);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
    }
    if (cache.size() > 64) cache.clear();  // дескриптори знищених курсорів можуть перевикористовуватися
    cache[cursor] = transparent;
    return transparent;
}

// У гейм-режимі тримає курсор у центральній половині вікна. Інакше введений рух доводить
// його до краю екрана, де спрацьовують панелі оболонки й гарячі кути. Гра читає рух
// через Raw Input, тож обмеження позиції на огляд не впливає. Windows і сама гра можуть
// скидати обмеження, тому воно відновлюється на кожному кроці.
static void KeepCursorInside(HWND wnd) {
    RECT wr;
    if (!GetWindowRect(wnd, &wr)) return;
    LONG mx = (wr.right - wr.left) / 4, my = (wr.bottom - wr.top) / 4;
    RECT want = {wr.left + mx, wr.top + my, wr.right - mx, wr.bottom - my};
    RECT cur;
    if (!GetClipCursor(&cur) || !EqualRect(&cur, &want)) ClipCursor(&want);
}

static bool CursorHidden(CURSORINFO* ci) {
    *ci = {sizeof(*ci)};
    if (!GetCursorInfo(ci)) return false;
    return !(ci->flags & CURSOR_SHOWING) || !ci->hCursor || IsTransparentCursor(ci->hCursor);
}

static FILE* g_log;

static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t;
    GetLocalTime(&t);
    fwprintf(g_log, L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfwprintf(g_log, fmt, args);
    va_end(args);
    fputwc(L'\n', g_log);
    fflush(g_log);
}

// Пише в журнал усе, від чого залежить рішення авто-режиму, коли щось із цього змінюється.
static void LogDecision(HWND fg, bool target, bool hung, bool hidden, const CURSORINFO& ci, bool capture) {
    static std::wstring last;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    wchar_t cls[64] = L"";
    if (fg) GetClassNameW(fg, cls, 64);
    RECT clip, wr = {};
    GetClipCursor(&clip);
    if (fg) GetWindowRect(fg, &wr);
    wchar_t line[400];
    swprintf(line, 400,
             L"fg=%p pid=%lu class=%ls target=%d hung=%d hidden=%d cursorFlags=%lu hCursor=%p clip=[%ld,%ld,%ld,%ld] wnd=[%ld,%ld,%ld,%ld] capture=%d",
             fg, pid, cls, target, hung, hidden, ci.flags, ci.hCursor, clip.left, clip.top, clip.right, clip.bottom, wr.left, wr.top,
             wr.right, wr.bottom, capture);
    if (last != line) {
        last = line;
        Log(L"%ls", line);
    }
}

struct Hotkey {
    bool ctrl = false, alt = false, shift = false;
    int vk = 0;
};

static int ParseKeyName(std::wstring s) {
    s = Lower(s);
    if (s.size() >= 2 && s[0] == L'f') {
        int n = _wtoi(s.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    if (s == L"scrolllock") return VK_SCROLL;
    if (s == L"pause") return VK_PAUSE;
    if (s == L"insert") return VK_INSERT;
    if (s == L"home") return VK_HOME;
    if (s == L"end") return VK_END;
    if (s == L"pageup") return VK_PRIOR;
    if (s == L"pagedown") return VK_NEXT;
    if (s.size() == 1 && iswalnum(s[0])) return towupper(s[0]);
    return 0;
}

// Формат: "Ctrl+Alt+F12", "Shift+Pause", "ScrollLock".
static bool ParseHotkey(const std::wstring& text, Hotkey* hk) {
    *hk = Hotkey();
    size_t start = 0;
    for (;;) {
        size_t plus = text.find(L'+', start);
        std::wstring part = Lower(text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
        if (plus == std::wstring::npos) {
            hk->vk = ParseKeyName(part);
            return hk->vk != 0;
        }
        if (part == L"ctrl") hk->ctrl = true;
        else if (part == L"alt") hk->alt = true;
        else if (part == L"shift") hk->shift = true;
        else return false;
        start = plus + 1;
    }
}

static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static bool HotkeyDown(const Hotkey& hk) {
    return Down(hk.vk) && Down(VK_CONTROL) == hk.ctrl && Down(VK_MENU) == hk.alt && Down(VK_SHIFT) == hk.shift;
}

// Повідомлення в консоль (якщо вона є) і в журнал.
static bool g_console;

static void Out(const wchar_t* fmt, ...) {
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    vswprintf(buf, 512, fmt, args);
    va_end(args);
    if (g_console) wprintf(L"%ls\n", buf);
    Log(L"%ls", buf);
}

int wmain(int argc, wchar_t** argv) {
    // Фонова збірка (без консолі) має недійсний stdout.
    g_console = _fileno(stdout) >= 0;
    if (g_console) _setmode(_fileno(stdout), _O_U16TEXT);

    std::wstring keyName = L"Ctrl+Alt+F12";
    std::wstring logPath;
    Mode mode = ModeAuto;
    bool exitWithGame = false;
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"--fullscreen") {
            g_fullscreen = true;
        } else if (a == L"--exclude" && i + 1 < argc) {
            g_exclude = L"," + Lower(argv[++i]) + L",";
        } else if (a == L"--process" && i + 1 < argc) {
            g_process = Lower(argv[++i]);
            if (g_process.size() > 4 && g_process.compare(g_process.size() - 4, 4, L".exe") == 0)
                g_process.resize(g_process.size() - 4);
        } else if (a == L"--key" && i + 1 < argc) {
            keyName = argv[++i];
        } else if (a == L"--log" && i + 1 < argc) {
            logPath = argv[++i];
        } else if (a == L"--mode" && i + 1 < argc) {
            std::wstring m = Lower(argv[++i]);
            mode = m == L"always" ? ModeAlways : m == L"off" ? ModeOff : ModeAuto;
        } else if (a == L"--exit-with-game") {
            exitWithGame = true;
        } else {
            Out(L"Невідомий параметр: %ls", a.c_str());
            return 2;
        }
    }
    // Один фоновий агент на сеанс користувача.
    if (g_fullscreen) {
        CreateMutexW(nullptr, FALSE, L"Local\\RdpGameMouseSystemAgent");
        if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    }
    if (!logPath.empty()) g_log = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
    Hotkey hotkey;
    if (!ParseHotkey(keyName, &hotkey)) {
        Out(L"Невідома клавіша: %ls", keyName.c_str());
        return 2;
    }
    if (exitWithGame && g_process.empty()) {
        Out(L"--exit-with-game потребує --process");
        return 2;
    }

    timeBeginPeriod(1);
    Out(L"Ціль: %ls. Перемикання режимів: %ls.",
        g_fullscreen ? L"будь-яке повноекранне вікно" : g_process.empty() ? L"будь-яке активне вікно" : g_process.c_str(),
        keyName.c_str());
    Out(L"Режим: %ls", kModeNames[mode]);

    Channel ch;
    DWORD started = GetTickCount(), lastGameCheck = 0, lastOpenTry = 0, lastStateSent = 0;
    DWORD visibleSince = 0, hiddenSince = 0;
    bool gameSeen = false, keyWasDown = false, capture = false, lastSent = false;

    for (;;) {
        DWORD now = GetTickCount();

        if (exitWithGame && now - lastGameCheck > 1000) {
            lastGameCheck = now;
            if (GameRunning()) {
                gameSeen = true;
            } else if (gameSeen || now - started > kGameStartTimeoutMs) {
                Out(gameSeen ? L"Гру закрито, вихід." : L"Гра не запустилася за 2 хв, вихід.");
                if (ch.IsOpen()) ch.SendState(false);
                return 0;
            }
        }

        if (!ch.IsOpen() && now - lastOpenTry > kReconnectMs) {
            lastOpenTry = now;
            if (ch.Open()) {
                Out(L"Плагін клієнта підключено.");
                lastStateSent = 0;
            }
        }

        bool keyDown = HotkeyDown(hotkey);
        if (keyDown && !keyWasDown) {
            mode = static_cast<Mode>((mode + 1) % 3);
            Out(L"Режим: %ls", kModeNames[mode]);
            Beep(mode == ModeOff ? 400 : 900, 80);
        }
        keyWasDown = keyDown;

        HWND fg = GetForegroundWindow();
        CURSORINFO ci;
        bool hidden = CursorHidden(&ci);
        bool target = IsTargetWindow(fg);
        // Під час завантаження гра «не відповідає», і Windows може підмінити її вікно
        // вікном-привидом (клас Ghost). У цей час зберігаємо попередній стан.
        wchar_t cls[16] = L"";
        bool hung = fg && (IsHungAppWindow(fg) || (GetClassNameW(fg, cls, 16) && wcscmp(cls, L"Ghost") == 0));
        bool wasCapture = capture;
        if (mode == ModeOff || !target) {
            if (!hung) capture = false;
            visibleSince = hiddenSince = 0;
        } else if (mode == ModeAlways) {
            capture = true;
            visibleSince = hiddenSince = 0;
        } else if (hidden) {
            visibleSince = 0;
            if (!hiddenSince) hiddenSince = now;
            if (now - hiddenSince >= kEngageDelayMs) capture = true;
        } else {
            hiddenSince = 0;
            if (capture && !hung) {
                if (!visibleSince) visibleSince = now;
                if (now - visibleSince >= kReleaseDelayMs) capture = false;
            }
        }
        if (wasCapture && !capture) {
            ReleaseInjectedButtons();
            ClipCursor(nullptr);
        }
        if (capture && !hung) KeepCursorInside(fg);
        LogDecision(fg, target, hung, hidden, ci, capture);

        if (ch.IsOpen()) {
            bool ok = ch.PumpRead();
            if (ok && (capture != lastSent || now - lastStateSent > kStateIntervalMs)) {
                ok = ch.SendState(capture);
                lastSent = capture;
                lastStateSent = now;
            }
            if (!ok) {
                ch.Close();
                ReleaseInjectedButtons();
                Out(L"Зв'язок з плагіном клієнта втрачено, повторне підключення...");
            }
        }

        if (ch.IsOpen())
            WaitForSingleObject(ch.ReadEvent(), 2);
        else
            Sleep(20);
    }
}
