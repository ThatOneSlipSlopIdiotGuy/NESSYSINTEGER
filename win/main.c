/* NESSYSINTEGER - Windows front end.
 * Plain Win32 + GDI + waveOut: no SDL, no DirectX SDK, no extra DLLs.
 * Builds with MSVC (cl) or MinGW-w64 (gcc) - see build.bat / README.md. */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define _CRT_SECURE_NO_WARNINGS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef WINVER
#define WINVER 0x0601
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <mmsystem.h>
#include <commdlg.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include "../src/nes.h"

#ifdef _MSC_VER
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

#define APP L"NESSYSINTEGER"
#define FPS 60.0988

enum {
    ID_OPEN = 100, ID_HOTSWAP, ID_PULL, ID_SAVE, ID_LOAD, ID_SHOT, ID_EXIT,
    ID_SLOT0 = 120,
    ID_RESET = 140, ID_POWER, ID_PAUSE, ID_ADVANCE, ID_OPPOSITE,
    ID_RAM0 = 150, ID_ALIGN0 = 160, ID_EXPAUDIO = 170, ID_CONSOLE0 = 250,
    ID_CLEANCART = 260, ID_CLEANCONN, ID_WEAR, ID_TRAY, ID_KEY0 = 270, ID_PPUVIEW = 280,
    ID_TILT = 180, ID_RESEAT, ID_BLOW, ID_CIC,
    ID_FLOAT0 = 190,
    ID_DEBUG = 200, ID_CHEATS,
    ID_SCALE1 = 210, ID_ASPECT = 220, ID_CROP, ID_FULL,
    ID_AUDIO = 230, ID_HELP = 240, ID_ABOUT
};

static Nes *nes;
static HINSTANCE g_inst;
static HWND g_wnd, g_tilt, g_dbg, g_cheat;
static HMENU g_menu;
static HFONT g_ui, g_small, g_mono, g_osd_font;
static int g_osd_font_h;
static uint32_t g_pal[512], g_pix[256 * 240];
static BITMAPINFO g_bmi;
static int g_have_rom, g_paused, g_advance, g_slot, g_full;
static int g_scale = 3, g_aspect = 1, g_crop = 1, g_audio_on = 1;
static WINDOWPLACEMENT g_wp;
static wchar_t g_rom[MAX_PATH], g_ini[MAX_PATH], g_osd[200];
static DWORD g_osd_until;
static char g_cheat_text[8192];
static int g_keys[2][8];
static const int KEY_DEF[2][8] = {
    { 'X', 'Z', VK_RSHIFT, VK_RETURN, VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT },
    { 'H', 'G', 'T', 'Y', 'W', 'S', 'A', 'D' } };
static const wchar_t *BTN_NAME[8] = { L"A", L"B", L"Select", L"Start", L"Up", L"Down", L"Left", L"Right" };

static void wfmt(wchar_t *b, size_t n, const wchar_t *f, ...) {
    va_list ap; va_start(ap, f);
    _vsnwprintf(b, n - 1, f, ap);
    va_end(ap);
    b[n - 1] = 0;
}
static void widen(wchar_t *d, int n, const char *s) { d[0] = 0; MultiByteToWideChar(CP_UTF8, 0, s, -1, d, n); d[n - 1] = 0; }
static void osd(const wchar_t *f, ...) {
    va_list ap; va_start(ap, f);
    _vsnwprintf(g_osd, 199, f, ap);
    va_end(ap);
    g_osd[199] = 0;
    g_osd_until = GetTickCount() + 2500;
}
static void path_ext(wchar_t *out, const wchar_t *in, const wchar_t *ext) {
    wcscpy(out, in);
    wchar_t *dot = wcsrchr(out, L'.'), *sl = wcsrchr(out, L'\\'), *sl2 = wcsrchr(out, L'/');
    if (sl2 > sl) sl = sl2;
    if (dot && (!sl || dot > sl)) *dot = 0;
    if (wcslen(out) + wcslen(ext) < MAX_PATH) wcscat(out, ext);
}
static const wchar_t *file_name(const wchar_t *p) {
    const wchar_t *a = wcsrchr(p, L'\\'), *b = wcsrchr(p, L'/');
    if (b > a) a = b;
    return a ? a + 1 : p;
}
static int write_file(const wchar_t *path, const void *d, size_t n) {
    FILE *f = _wfopen(path, L"wb");
    if (!f) return 0;
    size_t w = fwrite(d, 1, n, f);
    fclose(f);
    return w == n;
}
static uint8_t *read_file(const wchar_t *path, size_t *n) {
    FILE *f = _wfopen(path, L"rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return NULL; }
    uint8_t *d = (uint8_t *)malloc((size_t)sz + 1);
    if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
    fclose(f);
    if (d) { d[sz] = 0; *n = (size_t)sz; }
    return d;
}

/* ===================================================================== settings */
static int ini_get(const wchar_t *sec, const wchar_t *key, int def) { return (int)GetPrivateProfileIntW(sec, key, def, g_ini); }
static void ini_put(const wchar_t *sec, const wchar_t *key, int v) {
    wchar_t b[32]; wfmt(b, 32, L"%d", v); WritePrivateProfileStringW(sec, key, b, g_ini);
}
/* cartridge-handling state (the panel itself is further down) */
enum { SEQ_NONE, SEQ_EJECT, SEQ_INSERT, SEQ_REINSERT, SEQ_SWAP };
static int g_power_on = 1, g_wear = 1, g_cond_slot = -1, g_seq, g_seq_stage, g_was_dead;
static float g_t_seat = 1, g_t_tray = 1;
static wchar_t g_swap_path[MAX_PATH];
static void cond_store(void);
static void cond_sync(void);
static void cond_save_all(void);
static void cart_grime_load(uint32_t crc);
static void seq_start(int seq);
static void on_command(int id);
static void load_settings(void) {
    g_scale = ini_get(L"Video", L"Scale", 3); if (g_scale < 1 || g_scale > 6) g_scale = 3;
    g_aspect = ini_get(L"Video", L"Aspect8to7", 1);
    g_crop = ini_get(L"Video", L"CropOverscan", 1);
    g_audio_on = ini_get(L"Audio", L"Enabled", 1);
    { int m = ini_get(L"System", L"PowerOnRAM", 4); nes->ram_init = (m >= 0 && m <= 4) ? m : 4; }
    nes->console_id = (uint32_t)ini_get(L"System", L"ConsoleId", 0) & 0x7FFFFFFFu;
    if (!nes->console_id) nes->console_id = (((uint32_t)GetTickCount() * 2654435761u) & 0x7FFFFFFFu) | 1u;   /* first run: a new console */
    nes->cpu_align = ini_get(L"System", L"CpuPpuAlignment", 0) & 3;
    nes->exp_audio = ini_get(L"System", L"ExpansionAudio", 1) != 0;
    { int c = ini_get(L"System", L"ConsoleModel", 0); nes->console_setting = (c >= 0 && c <= 3) ? c : 0; }
    nes->allow_opposite = ini_get(L"System", L"AllowOppositeDirections", 0);
    nes->cic_enabled = ini_get(L"Cartridge", L"CICLockout", 1);
    { int fm = ini_get(L"Cartridge", L"FloatingPins", FLOAT_REAL); nes->conn.float_mode = (uint8_t)(fm >= 0 && fm <= 4 ? fm : FLOAT_REAL); }
    { int k = ini_get(L"Cartridge", L"CICKey", 0); nes->cic_key = k >= 0 && k <= 3 ? k : 0; }
    g_wear = ini_get(L"Cartridge", L"WearAndDirt", 1) != 0;
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 8; i++) {
            wchar_t k[32]; wfmt(k, 32, L"P%d_%ls", p + 1, BTN_NAME[i]);
            g_keys[p][i] = ini_get(L"Keys", k, KEY_DEF[p][i]);
            ini_put(L"Keys", k, g_keys[p][i]);   /* write back so the file documents itself */
        }
}
static void save_settings(void) {
    ini_put(L"Video", L"Scale", g_scale); ini_put(L"Video", L"Aspect8to7", g_aspect); ini_put(L"Video", L"CropOverscan", g_crop);
    ini_put(L"Audio", L"Enabled", g_audio_on);
    ini_put(L"System", L"PowerOnRAM", nes->ram_init); ini_put(L"System", L"ConsoleId", (int)(nes->console_id & 0x7FFFFFFFu)); ini_put(L"System", L"CpuPpuAlignment", nes->cpu_align); ini_put(L"System", L"ExpansionAudio", nes->exp_audio); ini_put(L"System", L"ConsoleModel", nes->console_setting);
    ini_put(L"System", L"AllowOppositeDirections", nes->allow_opposite);
    ini_put(L"Cartridge", L"CICLockout", nes->cic_enabled); ini_put(L"Cartridge", L"FloatingPins", nes->conn.float_mode);
    ini_put(L"Cartridge", L"CICKey", nes->cic_key); ini_put(L"Cartridge", L"WearAndDirt", g_wear); cond_save_all();
}

/* ===================================================================== audio (waveOut) */
#define NBUF 8
static HWAVEOUT g_wo;
static WAVEHDR g_hdr[NBUF];
static int16_t g_abuf[NBUF][AUDIO_MAX];
static int g_anext;
static void audio_open(void) {
    WAVEFORMATEX wf;
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = AUDIO_RATE;
    wf.wBitsPerSample = 16; wf.nBlockAlign = 2; wf.nAvgBytesPerSec = AUDIO_RATE * 2;
    if (waveOutOpen(&g_wo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) g_wo = NULL;
}
static int hdr_busy(WAVEHDR *h) { return (h->dwFlags & WHDR_PREPARED) && !(h->dwFlags & WHDR_DONE); }
static int audio_queued(void) { int q = 0; for (int i = 0; i < NBUF; i++) q += hdr_busy(&g_hdr[i]); return q; }
static void audio_push(const int16_t *s, int n) {
    WAVEHDR *h = &g_hdr[g_anext];
    if (!g_wo || n <= 0 || hdr_busy(h)) return;
    if (h->dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, h, (UINT)sizeof *h);
    memcpy(g_abuf[g_anext], s, (size_t)n * 2);
    memset(h, 0, sizeof *h);
    h->lpData = (LPSTR)g_abuf[g_anext]; h->dwBufferLength = (DWORD)n * 2;
    if (waveOutPrepareHeader(g_wo, h, (UINT)sizeof *h) == MMSYSERR_NOERROR) waveOutWrite(g_wo, h, (UINT)sizeof *h);
    g_anext = (g_anext + 1) % NBUF;
}
static void audio_close(void) {
    if (!g_wo) return;
    waveOutReset(g_wo);
    for (int i = 0; i < NBUF; i++) if (g_hdr[i].dwFlags & WHDR_PREPARED) waveOutUnprepareHeader(g_wo, &g_hdr[i], (UINT)sizeof g_hdr[i]);
    waveOutClose(g_wo);
    g_wo = NULL;
}

/* ===================================================================== input (keyboard + XInput) */
typedef struct { WORD wButtons; BYTE bLeftTrigger, bRightTrigger; SHORT sThumbLX, sThumbLY, sThumbRX, sThumbRY; } XPad;
typedef struct { DWORD dwPacketNumber; XPad Gamepad; } XState;
typedef DWORD (WINAPI *XGetState_t)(DWORD, XState *);
static XGetState_t g_xget;
static DWORD g_xretry[2];
static void xinput_init(void) {
    static const wchar_t *dll[3] = { L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !g_xget; i++) {
        HMODULE h = LoadLibraryW(dll[i]);
        if (h) g_xget = (XGetState_t)(void *)GetProcAddress(h, "XInputGetState");
    }
}
static uint8_t xinput_pad(int p) {
    if (!g_xget || GetTickCount() < g_xretry[p]) return 0;
    XState st;
    memset(&st, 0, sizeof st);
    if (g_xget((DWORD)p, &st) != ERROR_SUCCESS) { g_xretry[p] = GetTickCount() + 2000; return 0; }
    WORD b = st.Gamepad.wButtons;
    uint8_t r = 0;
    if (b & 0xA000) r |= 0x01;                       /* pad B / Y  -> NES A */
    if (b & 0x5000) r |= 0x02;                       /* pad A / X  -> NES B */
    if (b & 0x0020) r |= 0x04;                       /* Back       -> Select */
    if (b & 0x0010) r |= 0x08;                       /* Start */
    if ((b & 0x0001) || st.Gamepad.sThumbLY > 16000) r |= 0x10;
    if ((b & 0x0002) || st.Gamepad.sThumbLY < -16000) r |= 0x20;
    if ((b & 0x0004) || st.Gamepad.sThumbLX < -16000) r |= 0x40;
    if ((b & 0x0008) || st.Gamepad.sThumbLX > 16000) r |= 0x80;
    return r;
}
static uint8_t read_pad(int p) {
    uint8_t r = 0;
    if (GetForegroundWindow() == g_wnd)
        for (int i = 0; i < 8; i++)
            if (g_keys[p][i] && (GetAsyncKeyState(g_keys[p][i]) & 0x8000)) r |= (uint8_t)(1 << i);
    return (uint8_t)(r | xinput_pad(p));
}

/* ===================================================================== video */
static int vis_top(void) { return g_crop ? 8 : 0; }
static int vis_h(void) { return g_crop ? 224 : 240; }
static void draw_text_shadow(HDC dc, int x, int y, const wchar_t *s, COLORREF c) {
    int n = (int)wcslen(s);
    SetTextColor(dc, RGB(0, 0, 0)); TextOutW(dc, x + 2, y + 2, s, n);
    SetTextColor(dc, c); TextOutW(dc, x, y, s, n);
}
static void blit(HDC dc) {
    RECT rc; GetClientRect(g_wnd, &rc);
    int cw = rc.right, ch = rc.bottom, h = vis_h();
    if (cw <= 0 || ch <= 0) return;
    double sw = 256.0 * (g_aspect ? 8.0 / 7.0 : 1.0), sh = h;
    double k = (cw / sw < ch / sh) ? cw / sw : ch / sh;
    int dw = (int)(sw * k + 0.5), dh = (int)(sh * k + 0.5), dx = (cw - dw) / 2, dy = (ch - dh) / 2;
    g_bmi.bmiHeader.biHeight = -h;
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, 256, h, g_pix + vis_top() * 256, &g_bmi, DIB_RGB_COLORS, SRCCOPY);
    HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RECT r;
    if (dy > 0) { SetRect(&r, 0, 0, cw, dy); FillRect(dc, &r, black); SetRect(&r, 0, dy + dh, cw, ch); FillRect(dc, &r, black); }
    if (dx > 0) { SetRect(&r, 0, dy, dx, dy + dh); FillRect(dc, &r, black); SetRect(&r, dx + dw, dy, cw, dy + dh); FillRect(dc, &r, black); }
    /* overlays */
    int fh = ch / 26; if (fh < 14) fh = 14;
    if (fh != g_osd_font_h) {
        if (g_osd_font) DeleteObject(g_osd_font);
        g_osd_font = CreateFontW(-fh, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        g_osd_font_h = fh;
    }
    HGDIOBJ old = SelectObject(dc, g_osd_font);
    SetBkMode(dc, TRANSPARENT);
    int y = dy + fh / 2;
    if (!g_have_rom) { draw_text_shadow(dc, dx + fh, y, L"Open a .nes ROM (Ctrl+O) or drag one onto this window.", RGB(255, 255, 255)); y += fh * 3 / 2; }
    if (g_have_rom && !nes->conn.powered) { draw_text_shadow(dc, dx + fh, y, L"NO POWER TO CARTRIDGE (+5V/GND pin lifted)", RGB(255, 90, 90)); y += fh * 3 / 2; }
    else if (g_have_rom && nes->cic_enabled && !nes->conn.cic_ok) {
        draw_text_shadow(dc, dx + fh, y, L"CIC LOCKOUT - the console resets about once a second", RGB(255, 90, 90)); y += fh * 3 / 2;
    }
    if (g_paused) { draw_text_shadow(dc, dx + fh, y, L"PAUSED  (F3 resume, F4 step a frame)", RGB(255, 230, 120)); y += fh * 3 / 2; }
    if (GetTickCount() < g_osd_until) draw_text_shadow(dc, dx + fh, y, g_osd, RGB(140, 230, 255));
    if (g_have_rom && nes->cic_enabled && !nes->conn.cic_ok && (GetTickCount() / 500) % 2 == 0) {   /* blinking power LED */
        HBRUSH red = CreateSolidBrush(RGB(255, 30, 30));
        HGDIOBJ ob = SelectObject(dc, red), op = SelectObject(dc, GetStockObject(NULL_PEN));
        Ellipse(dc, dx + dw - fh * 2, dy + dh - fh * 2, dx + dw - fh, dy + dh - fh);
        SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(red);
    }
    SelectObject(dc, old);
}
static void present(void) {
    for (int i = 0; i < 256 * 240; i++) g_pix[i] = g_pal[nes->fb[i] & 511];
    HDC dc = GetDC(g_wnd); blit(dc); ReleaseDC(g_wnd, dc);
}
static void resize_window(void) {
    if (g_full) return;
    RECT r = { 0, 0, (int)(256 * g_scale * (g_aspect ? 8.0 / 7.0 : 1.0) + 0.5), vis_h() * g_scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    SetWindowPos(g_wnd, NULL, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}
static void toggle_fullscreen(void) {
    DWORD style = (DWORD)GetWindowLongW(g_wnd, GWL_STYLE);
    if (!g_full) {
        MONITORINFO mi;
        mi.cbSize = sizeof mi;
        g_wp.length = sizeof g_wp;
        if (!GetWindowPlacement(g_wnd, &g_wp) || !GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi)) return;
        SetWindowLongW(g_wnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
        SetMenu(g_wnd, NULL);
        SetWindowPos(g_wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full = 1;
    } else {
        SetWindowLongW(g_wnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetMenu(g_wnd, g_menu);
        SetWindowPlacement(g_wnd, &g_wp);
        SetWindowPos(g_wnd, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_full = 0;
    }
}
static void screenshot(void) {
    if (!g_have_rom) return;
    wchar_t base[MAX_PATH], path[MAX_PATH];
    path_ext(base, g_rom, L"");
    for (int i = 1; i < 1000; i++) {
        wfmt(path, MAX_PATH, L"%ls-%03d.bmp", base, i);
        if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) break;
    }
    size_t sz = 54 + 256 * 240 * 3;
    uint8_t *b = (uint8_t *)calloc(1, sz);
    if (!b) return;
    BITMAPFILEHEADER bf; BITMAPINFOHEADER bi;
    memset(&bf, 0, sizeof bf); memset(&bi, 0, sizeof bi);
    bf.bfType = 0x4D42; bf.bfSize = (DWORD)sz; bf.bfOffBits = 54;
    bi.biSize = sizeof bi; bi.biWidth = 256; bi.biHeight = 240; bi.biPlanes = 1; bi.biBitCount = 24; bi.biCompression = BI_RGB;
    memcpy(b, &bf, 14); memcpy(b + 14, &bi, 40);
    for (int y = 0; y < 240; y++)
        for (int x = 0; x < 256; x++) {
            uint32_t c = g_pal[nes->fb[(239 - y) * 256 + x] & 511];
            uint8_t *q = b + 54 + (y * 256 + x) * 3;
            q[0] = (uint8_t)c; q[1] = (uint8_t)(c >> 8); q[2] = (uint8_t)(c >> 16);
        }
    if (write_file(path, b, sz)) osd(L"Screenshot saved: %ls", file_name(path)); else osd(L"Could not save screenshot");
    free(b);
}

/* ===================================================================== ROM / saves / states */
static void tilt_refresh(void);
static void update_menu(void);
static void update_title(void) {
    wchar_t t[400];
    if (!g_have_rom) { SetWindowTextW(g_wnd, APP); return; }
    wchar_t mname[64]; widen(mname, 64, nes_mapper_name(nes->mapper));
    wchar_t cname[32]; widen(cname, 32, nes_console_name(nes->console));
    wfmt(t, 400, L"%ls - %ls  [mapper %d %ls, %ls]%ls", APP, file_name(g_rom), nes->mapper, mname, cname, !g_power_on ? L"  (power off)" : g_paused ? L"  (paused)" : L"");
    SetWindowTextW(g_wnd, t);
}
static void save_battery(void) {
    if (!g_have_rom || !nes->battery || !nes->wram_size) return;
    wchar_t p[MAX_PATH]; path_ext(p, g_rom, L".sav");
    write_file(p, nes->s.cart.wram, nes->wram_size);
}
static void load_battery(void) {
    if (!nes->battery || !nes->wram_size) return;
    wchar_t p[MAX_PATH]; path_ext(p, g_rom, L".sav");
    size_t n; uint8_t *d = read_file(p, &n);
    if (d) { memcpy(nes->s.cart.wram, d, n < nes->wram_size ? n : nes->wram_size); free(d); }
}
static int apply_cheats(const char *text) {
    nes->ngenie = 0; nes->nfreeze = 0;
    int bad = 0;
    const char *p = text;
    while (*p) {
        char line[128]; int k = 0;
        while (*p && *p != '\n' && *p != '\r' && *p != ',' && *p != ';') { if (k < 127) line[k++] = *p; p++; }
        while (*p == '\n' || *p == '\r' || *p == ',' || *p == ';') p++;
        line[k] = 0;
        char *hash = strchr(line, '#'); if (hash) *hash = 0;
        char *s = line; while (*s == ' ' || *s == '\t') s++;
        k = (int)strlen(s); while (k > 0 && (s[k - 1] == ' ' || s[k - 1] == '\t')) s[--k] = 0;
        if (!*s) continue;
        unsigned a, v; uint16_t ad; uint8_t va, cm; int hc;
        if ((sscanf(s, "%x:%x", &a, &v) == 2 || sscanf(s, "%x=%x", &a, &v) == 2) && a < 0x10000 && v < 0x100) {
            if (nes->nfreeze < 32) { nes->freeze[nes->nfreeze].addr = (uint16_t)a; nes->freeze[nes->nfreeze].val = (uint8_t)v; nes->nfreeze++; }
        } else if (nes_genie_decode(s, &ad, &va, &cm, &hc)) {
            if (nes->ngenie < 16) {
                nes->genie[nes->ngenie].addr = ad; nes->genie[nes->ngenie].val = va;
                nes->genie[nes->ngenie].cmp = cm; nes->genie[nes->ngenie].has_cmp = (uint8_t)hc; nes->ngenie++;
            }
        } else bad++;
    }
    return bad;
}
static void load_cheats(void) {
    wchar_t p[MAX_PATH]; path_ext(p, g_rom, L".cht");
    size_t n; uint8_t *d = read_file(p, &n);
    g_cheat_text[0] = 0;
    if (d) { size_t m = n < sizeof g_cheat_text - 1 ? n : sizeof g_cheat_text - 1; memcpy(g_cheat_text, d, m); g_cheat_text[m] = 0; free(d); }
    apply_cheats(g_cheat_text);
}
static int load_rom_file(const wchar_t *path, int hotswap) {
    size_t n; uint8_t *d = read_file(path, &n);
    if (!d) { MessageBoxW(g_wnd, L"Could not read that file.", APP, MB_ICONERROR); return 0; }
    char err[256]; err[0] = 0;
    int hs = hotswap && g_have_rom;
    if (g_have_rom) { save_battery(); cond_save_all(); }
    nes->seed ^= GetTickCount();
    conn_reseat(&nes->conn);           /* a freshly inserted cartridge sits straight */
    g_t_seat = g_t_tray = 1; if (!hotswap) g_seq = SEQ_NONE;
    conn_update(nes);
    if (!nes_load_rom(nes, d, n, hs, err, sizeof err)) {
        wchar_t w[300]; widen(w, 300, err);
        MessageBoxW(g_wnd, w, APP, MB_ICONERROR);
        free(d); return 0;
    }
    free(d);
    wcscpy(g_rom, path);
    g_have_rom = 1; g_paused = 0; g_power_on = 1;
    cart_grime_load(nes->rom_crc); cond_sync();
    load_battery();
    load_cheats();
    wchar_t mname[64]; widen(mname, 64, nes_mapper_name(nes->mapper));
    osd(hs ? L"Hot-swapped in %ls (power stayed on!)" : L"Loaded %ls  - mapper %d (%ls)", file_name(path), nes->mapper, mname);
    update_title(); update_menu(); tilt_refresh();
    return 1;
}
static void open_dialog(int hotswap) {
    wchar_t path[MAX_PATH] = L"";
    OPENFILENAMEW ofn;
    memset(&ofn, 0, sizeof ofn);
    ofn.lStructSize = sizeof ofn; ofn.hwndOwner = g_wnd;
    ofn.lpstrFilter = L"NES ROMs (*.nes)\0*.nes\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = path; ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = hotswap ? L"Hot-swap: insert another cartridge while the console stays on" : L"Open NES ROM";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    if (GetOpenFileNameW(&ofn)) {
        if (hotswap && g_have_rom && g_power_on) { wcscpy(g_swap_path, path); seq_start(SEQ_SWAP); osd(L"Pulling the cartridge out with the power on..."); }
        else load_rom_file(path, hotswap);
    }
}
static void state_path(wchar_t *p) { wchar_t e[8]; wfmt(e, 8, L".ss%d", g_slot); path_ext(p, g_rom, e); }
static void save_state(void) {
    if (!g_have_rom) return;
    size_t n = nes_state_size();
    uint8_t *b = (uint8_t *)malloc(n);
    if (!b) return;
    nes_save_state(nes, b);
    wchar_t p[MAX_PATH]; state_path(p);
    if (write_file(p, b, n)) osd(L"State saved to slot %d", g_slot); else osd(L"Could not write state file");
    free(b);
}
static void load_state(void) {
    if (!g_have_rom) return;
    wchar_t p[MAX_PATH]; state_path(p);
    size_t n; uint8_t *b = read_file(p, &n);
    if (!b) { osd(L"Slot %d is empty", g_slot); return; }
    if (nes_load_state(nes, b, n)) osd(L"State loaded from slot %d", g_slot);
    else osd(L"Slot %d belongs to a different ROM or emulator version", g_slot);
    free(b);
}

/* ===================================================================== cartridge & connector panel */
/* The physical side of the console: insert and pull the cartridge, the NES-001's push-down
 * tray, tilting, dust, breath moisture, cleaning and connector repairs. The connector's
 * condition is saved per console model and each cartridge's contacts per ROM, so wear and
 * grime persist the way they do on real hardware. */
enum { CARE_NONE, CARE_BLOW, CARE_CLEAN_CART, CARE_CLEAN_CONN };
enum { B_PULL, B_TRAY, B_PRESS, B_WIGGLE, B_RESEAT, B_AUTO, B_BLOW, B_CLEANCART, B_CLEANCONN, B_BEND, B_BOIL, B_NEWCONN, B_COUNT };
static const wchar_t *B_NAME[B_COUNT] = {
    L"Pull out / insert", L"Tray down / release", L"Press down (hold)", L"Wiggle it", L"Reseat straight", L"All pins: auto",
    L"Blow on it", L"Clean cart (alcohol)", L"Clean connector (kit)", L"Bend pins back", L"Boil the connector", L"Replace connector..." };
static const wchar_t *SLOT_SEC[3] = { L"Connector NES-001", L"Connector NES-101", L"Connector Famicom" };
static const wchar_t *SLOT_NAME[3] = { L"NES-001 front-loader, 72-pin tray connector", L"NES-101 top-loader, 72-pin slot", L"Famicom HVC-001, 60-pin slot" };
static const wchar_t *FLOAT_NAME[5] = { L"High", L"Low", L"Noise", L"Last value", L"Realistic" };
static const wchar_t *KEY_NAME[4] = { L"Licensed key", L"Other region", L"No key", L"Unlicensed stun" };
static const wchar_t *COND_NAME[5] = { L"Brand new (original spec)", L"Typical used", L"Worn out (years of use)", L"Aftermarket (very tight)", L"Refurbished original" };
static int g_care, g_press_btn, g_tick, g_th = -1, g_tdrag = -1, g_cdrag, g_dx0, g_dy0;
static float g_d_roll, g_d_seat, g_d_pitch;
static float g_springs[3][PINS + 1], g_oxid[3][PINS + 1], g_fing[PINS + 1];
static int g_have_cond[3];
static uint32_t g_cart_crc;

#define TILT_W 900
#define TILT_H 752
#define SCN_Y 34
#define SCN_H 176
#define PIN_X 30
#define PIN_Y 248
#define PIN_H 32
#define PIN_GAP 8
#define HOV_Y 352
#define SX0 250
#define SX1 640
#define SY0 382
#define SDY 30
#define BTN_Y 508
#define OPT_Y 598

static uint32_t ux(uint32_t *z) { uint32_t v = *z; v ^= v << 13; v ^= v >> 17; v ^= v << 5; return *z = v; }
static float uf(uint32_t *z) { return (float)(ux(z) % 1000u) / 1000.0f; }
static int hexv(wchar_t ch) { return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : -1; }
static void hex_put(const wchar_t *sec, const wchar_t *key, const float *v, float scale) {
    static const wchar_t HX[] = L"0123456789ABCDEF";
    wchar_t b[PINS * 2 + 2];
    for (int i = 0; i < PINS; i++) {
        int x = (int)(v[i + 1] * scale + 0.5f); x = x < 0 ? 0 : x > 255 ? 255 : x;
        b[2 * i] = HX[x >> 4]; b[2 * i + 1] = HX[x & 15];
    }
    b[PINS * 2] = 0;
    WritePrivateProfileStringW(sec, key, b, g_ini);
}
static int hex_get(const wchar_t *sec, const wchar_t *key, float *v, float scale) {
    wchar_t b[PINS * 2 + 8];
    if (GetPrivateProfileStringW(sec, key, L"", b, PINS * 2 + 8, g_ini) < PINS * 2) return 0;
    for (int i = 0; i < PINS; i++) {
        int hi = hexv(b[2 * i]), lo = hexv(b[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        v[i + 1] = (float)(hi * 16 + lo) / scale;
    }
    return 1;
}

/* ---- condition bookkeeping: the core holds the live values, these hold what gets saved ---- */
static void cond_store(void) {
    Connector *c = &nes->conn;
    if (!g_wear || g_cond_slot < 0) return;
    memcpy(g_springs[g_cond_slot], c->tension, sizeof c->tension);
    memcpy(g_oxid[g_cond_slot], c->cgrime, sizeof c->cgrime);
    if (g_cart_crc) memcpy(g_fing, c->fgrime, sizeof c->fgrime);
}
static void cond_apply(void) {
    Connector *c = &nes->conn;
    if (g_wear && g_cond_slot >= 0) {
        memcpy(c->tension, g_springs[g_cond_slot], sizeof c->tension);
        memcpy(c->cgrime, g_oxid[g_cond_slot], sizeof c->cgrime);
        memcpy(c->fgrime, g_fing, sizeof c->fgrime);
    } else for (int p = 0; p <= PINS; p++) { c->tension[p] = 1.0f; c->cgrime[p] = c->fgrime[p] = 0.0f; }
    conn_update(nes);
}
static void cond_sync(void) {                 /* after power-on or loading: the console model may have changed */
    Connector *c = &nes->conn;
    int sl = c->slot;
    if (sl != g_cond_slot) {
        cond_store();                         /* the live arrays still belong to the previous slot */
        g_cond_slot = sl;
        if (!g_have_cond[sl]) {
            if (!hex_get(SLOT_SEC[sl], L"Springs", g_springs[sl], 100.0f) || !hex_get(SLOT_SEC[sl], L"Oxidation", g_oxid[sl], 255.0f)) {
                conn_set_condition(c, COND_USED, nes->console_id * 2654435761u + (uint32_t)sl * 7919u);   /* a used console */
                memcpy(g_springs[sl], c->tension, sizeof c->tension); memcpy(g_oxid[sl], c->cgrime, sizeof c->cgrime);
            }
            g_have_cond[sl] = 1;
        }
    }
    cond_apply();
}
static void cart_grime_load(uint32_t crc) {
    wchar_t key[16]; wfmt(key, 16, L"%08X", crc);
    g_cart_crc = crc;
    if (hex_get(L"Cartridge contacts", key, g_fing, 255.0f)) return;
    uint32_t r = crc | 1u;                    /* a used cartridge: some grime, now and then an oxidised patch */
    float base = 0.02f + 0.14f * uf(&r);
    for (int p = 1; p <= PINS; p++) g_fing[p] = base * (0.5f + uf(&r));
    if (ux(&r) % 5 == 0) {
        int st = 1 + (int)(ux(&r) % 60), len = 2 + (int)(ux(&r) % 5);
        float g = 0.05f + 0.15f * uf(&r);
        for (int k = 0; k < len && st + k <= PINS; k++) g_fing[st + k] += g;
    }
}
static void cond_save_all(void) {
    cond_store();
    for (int sl = 0; sl < 3; sl++)
        if (g_have_cond[sl]) { hex_put(SLOT_SEC[sl], L"Springs", g_springs[sl], 100.0f); hex_put(SLOT_SEC[sl], L"Oxidation", g_oxid[sl], 255.0f); }
    if (g_cart_crc) { wchar_t key[16]; wfmt(key, 16, L"%08X", g_cart_crc); hex_put(L"Cartridge contacts", key, g_fing, 255.0f); }
}

/* ---- handling ---- */
static void conn_changed(void) {
    Connector *c = &nes->conn;
    if (c->seat < 0) c->seat = 0;
    if (c->seat > 1) c->seat = 1;
    if (c->roll < -1) c->roll = -1;
    if (c->roll > 1) c->roll = 1;
    if (c->pitch < -1) c->pitch = -1;
    if (c->pitch > 1) c->pitch = 1;
    if (c->dirt < 0) c->dirt = 0;
    if (c->dirt > 1) c->dirt = 1;
    conn_update(nes);
    tilt_refresh();
}
static void seq_start(int seq) { g_seq = seq; g_seq_stage = 0; }
static int cart_in(void) { return nes->conn.seat > 0.5f; }
static void do_care(void) {
    Connector *c = &nes->conn;
    uint32_t r = (uint32_t)GetTickCount() * 2654435761u | 1u;
    switch (g_care) {
    case CARE_BLOW:
        c->moist = c->moist + 0.7f > 1.0f ? 1.0f : c->moist + 0.7f;
        c->dirt *= 0.3f;                                       /* loose dust goes flying */
        if (g_wear) for (int p = 1; p <= PINS; p++) { c->fgrime[p] += 0.003f + 0.004f * uf(&r); if (c->fgrime[p] > 1) c->fgrime[p] = 1; }
        osd(L"*fffffff*  Breath moisture conducts for a while - and slowly corrodes the contacts"); break;
    case CARE_CLEAN_CART:
        for (int p = 1; p <= PINS; p++) c->fgrime[p] *= 0.12f;  /* stubborn oxidation stays */
        c->dirt = 0; c->moist = 0;
        osd(L"Cleaned the cartridge contacts with isopropyl alcohol and a cotton swab"); break;
    case CARE_CLEAN_CONN:
        for (int p = 1; p <= PINS; p++) c->cgrime[p] *= 0.25f;
        c->dirt = 0;
        osd(L"Ran a cleaning kit through the console's connector"); break;
    }
    g_care = CARE_NONE;
    cond_store();
}
static int need_power_off(void) {
    if (!g_wear) { osd(L"Wear and dirt are switched off (Cartridge menu)"); return 1; }
    if (g_power_on && g_have_rom) { osd(L"Switch the console off first (F2) - this means opening it up"); return 1; }
    return 0;
}
static void replace_connector(int cond) {
    if (need_power_off()) return;
    conn_set_condition(&nes->conn, cond, (uint32_t)GetTickCount());
    cond_store(); conn_changed();
    osd(L"Fitted a connector: %ls", COND_NAME[cond]);
}
static void care(int what) {                  /* the cartridge has to come out for this */
    g_care = what;
    if (cart_in()) seq_start(SEQ_REINSERT); else do_care();
}
static void tilt_button(int b) {
    Connector *c = &nes->conn;
    uint32_t r = (uint32_t)GetTickCount() * 2654435761u | 1u;
    switch (b) {
    case B_PULL:
        if (!g_have_rom) return;
        osd(cart_in() ? L"Pulling the cartridge out%ls" : L"Pushing the cartridge in%ls", g_power_on ? L" - with the power on!" : L"");
        seq_start(cart_in() ? SEQ_EJECT : SEQ_INSERT); break;
    case B_TRAY:
        if (c->slot != SLOT_NES) { osd(L"Only the NES-001 front-loader has a push-down tray"); return; }
        g_t_tray = c->tray > 0.5f ? 0.0f : 1.0f;
        osd(g_t_tray > 0.5f ? L"Tray pushed down - click" : L"Tray released - it springs up"); break;
    case B_WIGGLE:
        c->roll += (uf(&r) - 0.5f) * 0.45f; c->pitch += (uf(&r) - 0.5f) * 0.45f; c->seat -= uf(&r) * 0.10f;
        g_t_seat = c->seat < 0 ? 0 : c->seat; g_seq = SEQ_NONE;
        osd(L"Wiggle wiggle"); break;
    case B_RESEAT: { uint8_t fm = c->float_mode; conn_reseat(c); c->float_mode = fm; g_t_seat = g_t_tray = 1; g_seq = SEQ_NONE; osd(L"Cartridge reseated straight"); } break;
    case B_AUTO: memset(c->mode, PIN_AUTO, sizeof c->mode); osd(L"All pins back to automatic"); break;
    case B_BLOW: care(CARE_BLOW); break;
    case B_CLEANCART: care(CARE_CLEAN_CART); break;
    case B_CLEANCONN: care(CARE_CLEAN_CONN); break;
    case B_BEND:
        if (need_power_off()) return;
        for (int p = 1; p <= PINS; p++)
            if (c->tension[p] < 1.0f) { c->tension[p] += 0.12f + 0.30f * uf(&r); if (c->tension[p] > 1.12f) c->tension[p] = 1.12f - 0.05f * uf(&r); }
        cond_store(); osd(L"Bent every pin back up with a safety pin - uneven, but much better"); break;
    case B_BOIL:
        if (need_power_off()) return;
        for (int p = 1; p <= PINS; p++) { if (c->tension[p] < 0.92f) c->tension[p] = 0.92f + 0.10f * uf(&r); c->cgrime[p] *= 0.4f; }
        cond_store();
        osd(c->slot == SLOT_NES ? L"Boiled the 72-pin connector - the springs are back" : L"Cleaned and re-tensioned the slot's contacts"); break;
    }
    conn_changed();
}
/* runs once per frame: animates handling, ages breath moisture */
static void anim_step(void) {
    Connector *c = &nes->conn;
    int moved = 0;
    if (c->slot != SLOT_NES) { g_t_tray = 1.0f; if (c->tray != 1.0f) { c->tray = 1.0f; moved = 1; } }
    switch (g_seq) {
    case SEQ_EJECT: case SEQ_REINSERT: case SEQ_SWAP:
        if (g_seq_stage == 0) { g_t_tray = 0.0f; if (c->tray <= 0.001f || c->slot != SLOT_NES) { g_t_seat = 0.0f; g_seq_stage = 1; } }
        else if (c->seat <= 0.001f) {
            if (g_seq == SEQ_EJECT) { g_seq = SEQ_NONE; osd(L"The cartridge is out"); break; }
            if (g_seq == SEQ_SWAP) {
                load_rom_file(g_swap_path, 1);
                c->seat = 0.0f; c->tray = c->slot == SLOT_NES ? 0.0f : 1.0f; g_t_seat = 0.0f; g_t_tray = c->tray; moved = 1;
            }
            if (g_care) do_care();
            seq_start(SEQ_INSERT);
        }
        break;
    case SEQ_INSERT:
        if (g_seq_stage == 0) { g_t_seat = 1.0f; if (c->seat >= 0.999f) { g_seq_stage = 1; if (c->slot == SLOT_NES) g_t_tray = 1.0f; } }
        else if (c->tray >= 0.999f) g_seq = SEQ_NONE;
        break;
    }
    if (fabsf(c->seat - g_t_seat) > 0.0005f) {
        float d = g_t_seat - c->seat;
        c->seat += d > 0.09f ? 0.09f : d < -0.09f ? -0.09f : d;
        c->roll += (float)(rand() % 100 - 50) / 3000.0f;     /* hands are never quite straight */
        c->pitch += (float)(rand() % 100 - 50) / 3000.0f;
        if (fabsf(c->seat - g_t_seat) <= 0.0005f) {
            c->seat = g_t_seat;
            if (g_t_seat >= 1.0f) {
                c->roll *= 0.25f; c->pitch *= 0.25f;
                if (c->slot != SLOT_NES && g_wear) {          /* tight top-loading slots wipe the contacts */
                    for (int p = 1; p <= PINS; p++) { c->fgrime[p] *= 0.93f; c->cgrime[p] *= 0.98f; }
                    cond_store();
                }
            }
        }
        moved = 1;
    }
    if (fabsf(c->tray - g_t_tray) > 0.0005f) {
        float d = g_t_tray - c->tray;
        c->tray += d > 0.34f ? 0.34f : d < -0.34f ? -0.34f : d;
        if (fabsf(c->tray - g_t_tray) <= 0.0005f) {
            c->tray = g_t_tray;
            if (g_t_tray >= 1.0f && g_wear) { conn_wear_pushdown(c); cond_store(); }   /* every push-down bends the springs a bit */
        }
        moved = 1;
    }
    if (c->moist > 0.002f) { c->moist *= 0.99962f; if (++g_tick % 20 == 0) moved = 1; }
    else if (c->moist > 0.0f) { c->moist = 0.0f; moved = 1; }
    if (moved) conn_changed();
}
static void power_switch(void) {
    if (!g_have_rom) return;
    if (g_power_on) {
        int held = nes->reset_button;
        nes_power_off(nes);                   /* the brownout: unless RESET is held the CPU may scribble on the save */
        save_battery(); cond_save_all();
        g_power_on = 0;
        for (int i = 0; i < NES_W * NES_H; i++) nes->fb[i] = 0x0F;
        nes->audio_len = 0;
        osd(held ? L"Power OFF  (RESET held - the save RAM was safe)" : L"Power OFF");
    } else {
        nes->seed ^= GetTickCount();
        nes_power(nes); cond_sync();
        g_power_on = 1; g_was_dead = 0;
        osd(L"Power ON");
    }
    update_title(); tilt_refresh();
}

/* ---- drawing ---- */
static void fill(HDC dc, int l, int t, int r, int b, COLORREF col) {
    RECT rc; HBRUSH br = CreateSolidBrush(col);
    SetRect(&rc, l, t, r, b); FillRect(dc, &rc, br); DeleteObject(br);
}
static void text(HDC dc, int x, int y, COLORREF col, const wchar_t *str) { SetTextColor(dc, col); TextOutW(dc, x, y, str, (int)wcslen(str)); }
static int per_row(void) { return nes->conn.slot == SLOT_FC ? 30 : 36; }
static int pin_pitch(void) { return nes->conn.slot == SLOT_FC ? 28 : 23; }
static void pin_rect(int p, RECT *r) {
    int row; float x; conn_pin_geom(&nes->conn, p, &row, &x);
    int i = (int)(x * (float)(per_row() - 1) + 0.5f), w = pin_pitch(), y = PIN_Y + row * (PIN_H + PIN_GAP);
    SetRect(r, PIN_X + i * w + 1, y, PIN_X + i * w + w - 1, y + PIN_H);
}
static int pin_at(int x, int y) {
    for (int p = 1; p <= nes->conn.npins; p++) { RECT r; POINT pt; pt.x = x; pt.y = y; pin_rect(p, &r); if (PtInRect(&r, pt)) return p; }
    return 0;
}
static int sig_class(int sg) {
    return sg == S_GND || sg == S_VCC ? 1 : sg >= S_CA0 && sg < S_CA0 + 15 ? 2 : sg >= S_CD0 && sg < S_CD0 + 8 ? 3 :
           sg >= S_PA0 && sg < S_PA0 + 14 ? 4 : sg >= S_PD0 && sg < S_PD0 + 8 ? 5 : sg >= S_CIC0 && sg < S_CIC0 + 4 ? 6 :
           sg >= S_EXP0 && sg < S_EXP0 + 10 ? 7 : (sg == S_AIN || sg == S_AOUT) ? 8 : sg == S_NONE ? 0 : 9;
}
static void scene_geom(float *cx, float *bottom, float *cw, float *ch, float *ang) {
    Connector *c = &nes->conn;
    int fc = c->slot == SLOT_FC;
    float slot_y = (float)(SCN_Y + SCN_H - 40);
    *cw = fc ? 280.0f : 320.0f; *ch = fc ? 104.0f : 128.0f;
    *cx = TILT_W / 2.0f;
    *bottom = slot_y + 22.0f - (1.0f - c->seat) * 96.0f - (c->slot == SLOT_NES ? (1.0f - c->tray) * 12.0f : 0.0f);
    *ang = c->roll * 0.14f;
}
static void rot(float px, float py, float ang, float x, float y, POINT *o) {
    float sn = sinf(ang), cs = cosf(ang);
    o->x = (LONG)(px + (x - px) * cs + (y - py) * sn);
    o->y = (LONG)(py - (x - px) * sn + (y - py) * cs);
}
static void quad(HDC dc, float px, float py, float ang, float x0, float y0, float x1, float y1, COLORREF col) {
    POINT q[4];
    rot(px, py, ang, x0, y0, &q[0]); rot(px, py, ang, x1, y0, &q[1]); rot(px, py, ang, x1, y1, &q[2]); rot(px, py, ang, x0, y1, &q[3]);
    HBRUSH br = CreateSolidBrush(col); HPEN pn = CreatePen(PS_SOLID, 1, RGB(20, 20, 24));
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pn);
    Polygon(dc, q, 4);
    SelectObject(dc, ob); SelectObject(dc, op); DeleteObject(br); DeleteObject(pn);
}
static void draw_scene(HDC dc) {
    Connector *c = &nes->conn;
    float cx, bottom, cw, ch, ang; scene_geom(&cx, &bottom, &cw, &ch, &ang);
    int slot_y = SCN_Y + SCN_H - 40;
    fill(dc, 20, SCN_Y, TILT_W - 20, SCN_Y + SCN_H, RGB(46, 48, 56));
    fill(dc, (int)(cx - cw / 2 - 26), slot_y, (int)(cx + cw / 2 + 26), slot_y + 34, RGB(14, 14, 17));
    if (g_have_rom) {
        static const COLORREF FC_COL[8] = { RGB(34, 34, 38), RGB(222, 220, 212), RGB(186, 42, 42), RGB(44, 82, 168),
                                            RGB(226, 196, 44), RGB(58, 136, 70), RGB(116, 70, 148), RGB(206, 112, 42) };
        float px = ang > 0 ? cx - cw / 2 : cx + cw / 2;           /* it pivots on the end that stays down */
        quad(dc, px, bottom, ang, cx - cw / 2, bottom - ch, cx + cw / 2, bottom, c->slot == SLOT_FC ? FC_COL[g_cart_crc % 8] : RGB(150, 150, 156));
        for (int k = 0; k < 5; k++)                                /* grip ridges */
            quad(dc, px, bottom, ang, cx - cw / 2 + 14, bottom - ch + 6 + k * 4, cx + cw / 2 - 14, bottom - ch + 7 + k * 4, RGB(110, 110, 116));
        float ly0 = bottom - ch + 30, ly1 = bottom - ch * 0.30f;
        quad(dc, px, bottom, ang, cx - cw / 2 + 24, ly0, cx + cw / 2 - 24, ly1, RGB(236, 228, 205));
        POINT mid; rot(px, bottom, ang, cx, (ly0 + ly1) / 2, &mid);
        wchar_t nm[MAX_PATH]; wcscpy(nm, file_name(g_rom));
        wchar_t *dot = wcsrchr(nm, L'.'); if (dot) *dot = 0;
        RECT tr; SetRect(&tr, mid.x - (int)(cw / 2) + 30, mid.y - 9, mid.x + (int)(cw / 2) - 30, mid.y + 9);
        SetTextColor(dc, RGB(40, 40, 52));
        DrawTextW(dc, nm, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    fill(dc, (int)(cx - cw / 2 - 30), slot_y + 18, (int)(cx + cw / 2 + 30), slot_y + 34, RGB(64, 66, 74));   /* the slot's lip */
    wchar_t s[200];
    SelectObject(dc, g_small);
    wfmt(s, 200, L"Inserted %d%%", (int)(c->seat * 100.0f + 0.5f)); text(dc, 32, SCN_Y + 8, RGB(200, 200, 210), s);
    if (c->slot == SLOT_NES) { wfmt(s, 200, L"Tray: %ls", c->tray > 0.99f ? L"down (latched)" : c->tray < 0.01f ? L"up" : L"moving"); text(dc, 32, SCN_Y + 24, RGB(200, 200, 210), s); }
    if (c->press > 0) text(dc, 32, SCN_Y + 40, RGB(255, 210, 120), L"Pressing down hard");
    if (c->moist > 0.01f) { wfmt(s, 200, L"Breath moisture %d%%", (int)(c->moist * 100.0f)); text(dc, 32, SCN_Y + 56, RGB(140, 200, 255), s); }
    if (c->pitch > 0.05f || c->pitch < -0.05f) { wfmt(s, 200, L"Leaning toward the %ls side", c->pitch > 0 ? L"back" : L"label"); text(dc, TILT_W - 250, SCN_Y + 8, RGB(200, 200, 210), s); }
    if (!g_power_on) text(dc, TILT_W - 250, SCN_Y + 24, RGB(255, 140, 120), L"Console switched off");
    text(dc, 32, SCN_Y + SCN_H - 18, RGB(140, 140, 155), L"Drag the cartridge: sideways = tilt, up/down = pull/insert. Right-drag up/down = lean.");
    SelectObject(dc, g_ui);
}
static void tilt_paint(HDC dc, int w, int h) {
    Connector *c = &nes->conn;
    wchar_t s[400];
    fill(dc, 0, 0, w, h, RGB(28, 30, 34));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_ui);
    int open = 0;
    for (int p = 1; p <= c->npins; p++) open += conn_pin_signal(c, p) != S_NONE && !c->conn[p];
    const wchar_t *lock = nes->console != CON_NES ? L"no lock chip" : !nes->cic_enabled ? L"chip disabled" :
                          nes->lock_dead ? L"LOCKOUT - blinking" : nes->cic_key == KEY_STUN ? L"stunned" : L"OK";
    wfmt(s, 400, L"%ls   |   cart power: %ls   |   10NES: %ls   |   open: %d   intermittent: %d", SLOT_NAME[c->slot],
         !c->powered ? L"LOST" : c->vcc_flaky ? L"flickering" : L"OK", lock, open, c->nflaky);
    text(dc, 20, 10, c->perfect && !nes->lock_dead ? RGB(130, 230, 140) : RGB(255, 200, 90), s);
    int led = g_power_on && g_have_rom && nes->led;            /* the NES-001's LED hangs off the lock's reset output */
    HBRUSH lb = CreateSolidBrush(led ? RGB(255, 40, 30) : RGB(70, 16, 14)); HGDIOBJ ob = SelectObject(dc, lb);
    Ellipse(dc, TILT_W - 34, 9, TILT_W - 20, 23); SelectObject(dc, ob); DeleteObject(lb);
    SelectObject(dc, g_small); text(dc, TILT_W - 82, 10, RGB(170, 170, 180), L"POWER"); SelectObject(dc, g_ui);
    draw_scene(dc);
    /* the contacts, as you face the label */
    SelectObject(dc, g_small);
    for (int row = 0; row < 2; row++) {
        int y = PIN_Y + row * (PIN_H + PIN_GAP), first = -1, cls = -1;
        text(dc, PIN_X + per_row() * pin_pitch() + 8, y + 9, RGB(130, 130, 145), row ? L"back" : L"label side");
        for (int i = 0; i <= per_row(); i++) {
            int p = 0, k = 0;
            if (i < per_row()) for (int q = 1; q <= c->npins; q++) { RECT r; pin_rect(q, &r); if (r.left == PIN_X + i * pin_pitch() + 1 && r.top == y) { p = q; break; } }
            k = p ? sig_class(conn_pin_signal(c, p)) : -2;
            if (k != cls) {
                static const wchar_t *CN[10] = { L"", L"", L"CPU address", L"CPU data", L"PPU address", L"PPU data", L"CIC", L"EXP", L"audio", L"" };
                if (cls >= 2 && cls <= 8 && i - first >= 2) {
                    int x0 = PIN_X + first * pin_pitch() + 2, x1 = PIN_X + i * pin_pitch() - 2, ly = row ? y + PIN_H + 3 : y - 5;
                    fill(dc, x0, ly, x1, ly + 2, RGB(120, 120, 130));
                    RECT tr; SetRect(&tr, x0 - 30, row ? ly + 3 : ly - 16, x1 + 30, row ? ly + 18 : ly - 1);
                    SetTextColor(dc, RGB(190, 190, 200)); DrawTextW(dc, CN[cls], -1, &tr, DT_CENTER | DT_SINGLELINE | DT_NOCLIP);
                }
                cls = k; first = i;
            }
        }
    }
    for (int p = 1; p <= c->npins; p++) {
        RECT r; pin_rect(p, &r);
        int sg = conn_pin_signal(c, p);
        COLORREF col = !c->conn[p] ? RGB(205, 55, 55) : c->flaky[p] ? RGB(235, 190, 40) : RGB(60, 180, 85);
        if (sg == S_GND || sg == S_VCC) col = !c->conn[p] ? RGB(150, 20, 20) : c->flaky[p] ? RGB(240, 150, 40) : RGB(200, 170, 60);
        if (sg == S_NONE) col = RGB(70, 70, 76);
        fill(dc, r.left, r.top, r.right, r.bottom, col);
        int gr = (int)((c->cgrime[p] + c->fgrime[p]) * 10.0f); if (gr > 10) gr = 10;   /* oxidation: a brown stain from the bottom */
        if (gr > 0) fill(dc, r.left, r.bottom - gr, r.right, r.bottom, RGB(110, 70, 30));
        if (p == g_th) FrameRect(dc, &r, (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (c->mode[p] != PIN_AUTO) fill(dc, r.left + 5, r.top + 3, r.right - 5, r.top + 8, RGB(255, 255, 255));
        wchar_t num[4]; wfmt(num, 4, L"%d", p);
        RECT nr = r; nr.bottom -= 11;
        SetTextColor(dc, RGB(15, 15, 15)); DrawTextW(dc, num, -1, &nr, DT_CENTER | DT_BOTTOM | DT_SINGLELINE);
    }
    SelectObject(dc, g_ui);
    if (g_th > 0 && g_th <= c->npins) {
        static const wchar_t *MODE[4] = { L"auto", L"forced OPEN", L"forced ON", L"forced FLAKY" };
        wchar_t nm[48]; widen(nm, 48, conn_pin_name(c, g_th));
        int row; float x; conn_pin_geom(c, g_th, &row, &x);
        wfmt(s, 400, L"Pin %d (%ls): %ls  -  %ls %d%%  |  spring %d%%  |  oxidation: pin %d%%, finger %d%%  [%ls]",
             g_th, row ? L"back" : L"label side", nm, !c->conn[g_th] ? L"open, contact" : c->flaky[g_th] ? L"intermittent, gets through" : L"solid, gets through",
             (int)(c->flaky_p[g_th] * 100.0f + 0.5f), (int)(c->tension[g_th] * 100.0f + 0.5f), (int)(c->cgrime[g_th] * 100.0f + 0.5f),
             (int)(c->fgrime[g_th] * 100.0f + 0.5f), MODE[c->mode[g_th] & 3]);
        text(dc, 20, HOV_Y, RGB(255, 255, 255), s);
    } else text(dc, 20, HOV_Y, RGB(150, 150, 160), L"Hover a contact for details. Click: auto > forced open > forced flaky > forced on. Right-click: auto.");
    static const wchar_t *SL_NAME[4] = { L"Inserted (how far in)", L"Tilt (lift left / right end)", L"Lean (label / back side)", L"Loose dust" };
    for (int i = 0; i < 4; i++) {
        int y = SY0 + i * SDY;
        float v = i == 0 ? c->seat : i == 1 ? c->roll : i == 2 ? c->pitch : c->dirt;
        float lo = (i == 1 || i == 2) ? -1.0f : 0.0f, t = (v - lo) / (1.0f - lo);
        int kx = SX0 + (int)(t * (SX1 - SX0));
        text(dc, 20, y, RGB(220, 220, 225), SL_NAME[i]);
        fill(dc, SX0, y + 7, SX1, y + 11, RGB(90, 90, 100));
        if (lo < 0) fill(dc, (SX0 + SX1) / 2 - 1, y + 2, (SX0 + SX1) / 2 + 1, y + 16, RGB(140, 140, 150));
        fill(dc, kx - 6, y, kx + 6, y + 18, g_tdrag == i ? RGB(255, 255, 255) : RGB(190, 200, 230));
        if (lo < 0) wfmt(s, 400, L"%+.2f", v); else wfmt(s, 400, L"%3.0f%%", v * 100.0f);
        text(dc, SX1 + 18, y, RGB(220, 220, 225), s);
    }
    for (int k = 0; k < B_COUNT; k++) {
        int x = 20 + (k % 6) * 146, y = BTN_Y + (k / 6) * 38;
        int dim = (k == B_TRAY && c->slot != SLOT_NES) || ((k == B_BEND || k == B_BOIL || k == B_NEWCONN) && g_power_on && g_have_rom);
        fill(dc, x, y, x + 140, y + 30, k == B_PRESS && g_press_btn ? RGB(110, 120, 160) : k >= B_BLOW ? RGB(58, 74, 70) : RGB(62, 66, 78));
        RECT r; SetRect(&r, x, y, x + 140, y + 30);
        SetTextColor(dc, dim ? RGB(130, 130, 135) : RGB(240, 240, 240));
        DrawTextW(dc, B_NAME[k], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    text(dc, 20, OPT_Y + 5, RGB(220, 220, 225), L"Open inputs read as:");
    for (int k = 0; k < 5; k++) {
        int x = 200 + k * 112;
        fill(dc, x, OPT_Y, x + 106, OPT_Y + 26, c->float_mode == k ? RGB(70, 110, 190) : RGB(55, 58, 66));
        RECT r; SetRect(&r, x, OPT_Y, x + 106, OPT_Y + 26); SetTextColor(dc, RGB(240, 240, 240));
        DrawTextW(dc, FLOAT_NAME[k], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    text(dc, 20, OPT_Y + 39, RGB(220, 220, 225), L"Cartridge's CIC:");
    for (int k = 0; k < 4; k++) {
        int x = 200 + k * 140;
        fill(dc, x, OPT_Y + 34, x + 134, OPT_Y + 60, nes->cic_key == k ? RGB(70, 110, 190) : RGB(55, 58, 66));
        RECT r; SetRect(&r, x, OPT_Y + 34, x + 134, OPT_Y + 60); SetTextColor(dc, RGB(240, 240, 240));
        DrawTextW(dc, KEY_NAME[k], -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    fill(dc, 20, OPT_Y + 72, 38, OPT_Y + 90, RGB(200, 200, 210));
    if (nes->cic_enabled) fill(dc, 24, OPT_Y + 76, 34, OPT_Y + 86, RGB(40, 140, 60));
    text(dc, 46, OPT_Y + 73, RGB(220, 220, 225), L"10NES lock chip active (NES-001)");
    fill(dc, 420, OPT_Y + 72, 438, OPT_Y + 90, RGB(200, 200, 210));
    if (g_wear) fill(dc, 424, OPT_Y + 76, 434, OPT_Y + 86, RGB(40, 140, 60));
    text(dc, 446, OPT_Y + 73, RGB(220, 220, 225), L"Wear and dirt (off = ideal contacts)");
    float sp = 0, ox = 0, fg = 0; int np = c->npins;
    for (int p = 1; p <= np; p++) { sp += c->tension[p]; ox += c->cgrime[p]; fg += c->fgrime[p]; }
    wfmt(s, 400, L"Connector springs %d%%, oxidation %d%%   |   this cartridge's contacts: grime %d%%",
         (int)(sp * 100.0f / (float)np + 0.5f), (int)(ox * 100.0f / (float)np + 0.5f), (int)(fg * 100.0f / (float)np + 0.5f));
    text(dc, 20, OPT_Y + 104, RGB(170, 200, 170), s);
    SelectObject(dc, g_small);
    text(dc, 20, OPT_Y + 128, RGB(130, 130, 145), L"Game window keys:  F1 hold = RESET   F2 = power switch   . = pull out / insert   / = tray   [ ] tilt   ; ' lean   - = insert depth   Backspace = reseat");
    SelectObject(dc, of);
}
static void tilt_slider_set(int i, int x) {
    Connector *c = &nes->conn;
    float t = (float)(x - SX0) / (float)(SX1 - SX0);
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    if (i == 1 || i == 2) { float v = -1.0f + 2.0f * t; if (v > -0.03f && v < 0.03f) v = 0; if (i == 1) c->roll = v; else c->pitch = v; }
    else if (i == 0) { c->seat = t; g_t_seat = t; g_seq = SEQ_NONE; }
    else c->dirt = t;
    conn_changed();
}
static int on_cart(int x, int y) {
    float cx, bottom, cw, ch, ang; scene_geom(&cx, &bottom, &cw, &ch, &ang);
    return g_have_rom && x > cx - cw / 2 - 12 && x < cx + cw / 2 + 12 && y > bottom - ch - 24 && y < bottom + 12;
}
static LRESULT CALLBACK tilt_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int x = (short)LOWORD(l), y = (short)HIWORD(l);
    Connector *c = &nes->conn;
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;     /* keep the game window focused while you handle the cart */
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bm);
        tilt_paint(mem, rc.right, rc.bottom);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE:
        if (g_tdrag >= 0) { tilt_slider_set(g_tdrag, x); return 0; }
        if (g_cdrag == 1) {
            c->roll = g_d_roll + (float)(x - g_dx0) / 220.0f;
            c->seat = g_d_seat - (float)(y - g_dy0) / 96.0f; if (c->seat < 0) c->seat = 0; if (c->seat > 1) c->seat = 1;
            g_t_seat = c->seat; conn_changed(); return 0;
        }
        if (g_cdrag == 2) { c->pitch = g_d_pitch - (float)(y - g_dy0) / 120.0f; conn_changed(); return 0; }
        { int p = pin_at(x, y); if (p != g_th) { g_th = p; InvalidateRect(h, NULL, FALSE); } }
        return 0;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        int right = m == WM_RBUTTONDOWN;
        int p = pin_at(x, y);
        if (p) {
            static const uint8_t NEXT[4] = { PIN_OFF, PIN_FLAKY, PIN_AUTO, PIN_ON };   /* auto>off>flaky>on>auto */
            c->mode[p] = right ? (uint8_t)PIN_AUTO : NEXT[c->mode[p] & 3];
            conn_changed(); return 0;
        }
        if (on_cart(x, y)) {
            g_cdrag = right ? 2 : 1; g_dx0 = x; g_dy0 = y; g_d_roll = c->roll; g_d_seat = c->seat; g_d_pitch = c->pitch;
            g_seq = SEQ_NONE; SetCapture(h); return 0;
        }
        if (right) return 0;
        for (int i = 0; i < 4; i++) {
            int sy = SY0 + i * SDY;
            if (y >= sy - 4 && y <= sy + 22 && x >= SX0 - 10 && x <= SX1 + 10) { g_tdrag = i; SetCapture(h); tilt_slider_set(i, x); return 0; }
        }
        for (int k = 0; k < B_COUNT; k++) {
            int bx = 20 + (k % 6) * 146, by = BTN_Y + (k / 6) * 38;
            if (x < bx || x > bx + 140 || y < by || y > by + 30) continue;
            if (k == B_PRESS) { g_press_btn = 1; c->press = 1.0f; SetCapture(h); conn_changed(); return 0; }
            if (k == B_NEWCONN) {
                HMENU pm = CreatePopupMenu();
                for (int q = 0; q < 5; q++) AppendMenuW(pm, MF_STRING, (UINT_PTR)(q + 1), COND_NAME[q]);
                POINT pt; pt.x = bx; pt.y = by + 30; ClientToScreen(h, &pt);
                int cmd = (int)TrackPopupMenu(pm, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN, pt.x, pt.y, 0, h, NULL);
                DestroyMenu(pm);
                if (cmd > 0) replace_connector(cmd - 1);
                return 0;
            }
            tilt_button(k); return 0;
        }
        if (y >= OPT_Y && y <= OPT_Y + 26)
            for (int k = 0; k < 5; k++) if (x >= 200 + k * 112 && x <= 306 + k * 112) { c->float_mode = (uint8_t)k; save_settings(); update_menu(); conn_changed(); return 0; }
        if (y >= OPT_Y + 34 && y <= OPT_Y + 60)
            for (int k = 0; k < 4; k++) if (x >= 200 + k * 140 && x <= 334 + k * 140) { nes->cic_key = k; save_settings(); update_menu(); tilt_refresh(); return 0; }
        if (y >= OPT_Y + 70 && y <= OPT_Y + 92) {
            if (x >= 20 && x < 400) { nes->cic_enabled ^= 1; save_settings(); update_menu(); conn_changed(); }
            else if (x >= 420 && x < 760) { cond_store(); g_wear ^= 1; cond_apply(); save_settings(); update_menu(); tilt_refresh(); }
        }
        return 0;
    }
    case WM_LBUTTONUP: case WM_RBUTTONUP:
        if (g_press_btn) { g_press_btn = 0; c->press = 0.0f; conn_changed(); }
        if (g_tdrag >= 0 || g_cdrag) { g_tdrag = -1; g_cdrag = 0; ReleaseCapture(); InvalidateRect(h, NULL, FALSE); }
        return 0;
    case WM_CAPTURECHANGED:
        g_tdrag = -1; g_cdrag = 0;
        if (g_press_btn) { g_press_btn = 0; c->press = 0.0f; conn_changed(); }
        return 0;
    case WM_MOUSELEAVE: g_th = -1; InvalidateRect(h, NULL, FALSE); return 0;
    case WM_DESTROY: g_tilt = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void tilt_refresh(void) { if (g_tilt) InvalidateRect(g_tilt, NULL, FALSE); }
static void show_tilt(void) {
    if (g_tilt) { ShowWindow(g_tilt, SW_SHOWNOACTIVATE); return; }
    RECT r = { 0, 0, TILT_W, TILT_H }, wr;
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&r, st, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    GetWindowRect(g_wnd, &wr);
    g_tilt = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"NessysTilt", L"Cartridge & connector - handle it like the real thing",
                             st, wr.right + 8, wr.top, r.right - r.left, r.bottom - r.top, g_wnd, NULL, g_inst, NULL);
    ShowWindow(g_tilt, SW_SHOWNOACTIVATE);
}

/* ===================================================================== PPU viewer */
/* Live view of the PPU's memory: nametables (yellow = what each scanline showed), both pattern
 * tables, palette RAM and OAM. Reading has no side effects, so it never disturbs the game. */
#define PV_INFO_H 48
static HWND g_ppuw;
static int g_vpal, g_vgrid, g_vend;
static uint32_t *g_vbuf;
static char g_vinfo[220];
static BITMAPINFO g_vbmi;
static LRESULT CALLBACK ppuv_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    int x = (short)LOWORD(l), y = (short)HIWORD(l);
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ ob = SelectObject(mem, bm);
        fill(mem, 0, 0, rc.right, rc.bottom, RGB(32, 34, 40));
        SetBkMode(mem, TRANSPARENT);
        HGDIOBJ of = SelectObject(mem, g_small);
        if (g_have_rom && g_vbuf) {
            Ppu *p = &nes->s.ppu;
            wchar_t t[160];
            nes_view_render(nes, g_vbuf, g_pal, g_vpal, g_vgrid, g_vend);
            StretchDIBits(mem, 0, 0, VIEW_W, VIEW_H, 0, 0, VIEW_W, VIEW_H, g_vbuf, &g_vbmi, DIB_RGB_COLORS, SRCCOPY);
            text(mem, VIEW_CHR_X, 127, RGB(170, 170, 185), L"Pattern tables  $0000  |  $1000");
            text(mem, VIEW_PAL_X, 171, RGB(170, 170, 185), L"Sprites (OAM 0-63)");
            int ty = VIEW_OAM_Y + 2, tx = VIEW_OAM_X + 136;
            wfmt(t, 160, L"PPUCTRL  $%02X", p->ctrl); text(mem, tx, ty, RGB(220, 220, 230), t);
            wfmt(t, 160, L"PPUMASK  $%02X", p->mask); text(mem, tx, ty + 15, RGB(220, 220, 230), t);
            wfmt(t, 160, L"BG tiles  $%04X", (p->ctrl & 0x10) ? 0x1000 : 0); text(mem, tx, ty + 30, RGB(220, 220, 230), t);
            wfmt(t, 160, L"Sprites  %ls", (p->ctrl & 0x20) ? L"8x16" : (p->ctrl & 8) ? L"8x8 $1000" : L"8x8 $0000"); text(mem, tx, ty + 45, RGB(220, 220, 230), t);
            wfmt(t, 160, L"Rendering  %ls", (p->mask & 0x18) ? L"on" : L"off"); text(mem, tx, ty + 60, RGB(220, 220, 230), t);
            wfmt(t, 160, L"Frame  %lu", (unsigned long)p->frame); text(mem, tx, ty + 75, RGB(220, 220, 230), t);
            int ly = VIEW_OAM_Y + 140;
            text(mem, VIEW_PAL_X, ly, RGB(255, 224, 64), L"Yellow: what each scanline showed");
            text(mem, VIEW_PAL_X, ly + 15, RGB(170, 170, 185), L"(a status bar split shows up as a step)");
            text(mem, VIEW_PAL_X, ly + 36, RGB(170, 170, 185), L"Each tile: 8x8 pixels, 16 bytes, 2 bits/pixel.");
            text(mem, VIEW_PAL_X, ly + 51, RGB(170, 170, 185), L"Each 16x16 area picks 1 of 4 palettes");
            text(mem, VIEW_PAL_X, ly + 66, RGB(170, 170, 185), L"via the attribute table.");
            text(mem, VIEW_PAL_X, ly + 87, RGB(170, 170, 185), L"Click a palette to recolour the tiles.");
        } else text(mem, 20, 20, RGB(170, 170, 185), L"Load a ROM to see its tiles.");
        SelectObject(mem, g_ui);
        wchar_t wi[220]; widen(wi, 220, g_vinfo);
        text(mem, 10, VIEW_H + 4, RGB(255, 255, 255), wi[0] ? wi : L"Hover the nametables, tiles, palettes or sprites for details.");
        text(mem, 10, VIEW_H + 25, g_vgrid ? RGB(130, 230, 140) : RGB(150, 150, 160), g_vgrid ? L"[x] attribute grid (click here)" : L"[ ] attribute grid (click here)");
        text(mem, 270, VIEW_H + 25, RGB(150, 150, 160), g_vend ? L"Tiles drawn with CHR banks: at the END of the frame (click)" : L"Tiles drawn with CHR banks: at the TOP of the frame (click)");
        SelectObject(mem, of);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        char b[220];
        if (!g_have_rom || !nes_view_describe(nes, x, y, g_vend, b, (int)sizeof b)) b[0] = 0;
        if (strcmp(b, g_vinfo)) { strcpy(g_vinfo, b); InvalidateRect(h, NULL, FALSE); }
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (x >= VIEW_PAL_X && x < VIEW_PAL_X + 256 && y >= VIEW_PAL_Y && y < VIEW_PAL_Y + 32) g_vpal = ((y - VIEW_PAL_Y) / 16) * 4 + (x - VIEW_PAL_X) / 64;
        else if (y >= VIEW_H + 22 && x < 260) g_vgrid ^= 1;
        else if (y >= VIEW_H + 22 && x >= 270) g_vend ^= 1;
        InvalidateRect(h, NULL, FALSE);
        return 0;
    case WM_DESTROY: g_ppuw = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void show_ppuview(void) {
    if (g_ppuw) { ShowWindow(g_ppuw, SW_SHOWNOACTIVATE); return; }
    if (!g_vbuf) g_vbuf = (uint32_t *)malloc(sizeof(uint32_t) * VIEW_W * VIEW_H);
    memset(&g_vbmi, 0, sizeof g_vbmi);
    g_vbmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); g_vbmi.bmiHeader.biWidth = VIEW_W; g_vbmi.bmiHeader.biHeight = -VIEW_H;
    g_vbmi.bmiHeader.biPlanes = 1; g_vbmi.bmiHeader.biBitCount = 32; g_vbmi.bmiHeader.biCompression = BI_RGB;
    RECT r = { 0, 0, VIEW_W, VIEW_H + PV_INFO_H }, wr;
    DWORD st = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
    AdjustWindowRectEx(&r, st, FALSE, WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE);
    GetWindowRect(g_wnd, &wr);
    g_ppuw = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"NessysPPU", L"PPU viewer - nametables, tiles, palettes, sprites",
                             st, wr.left, wr.bottom + 8, r.right - r.left, r.bottom - r.top, g_wnd, NULL, g_inst, NULL);
    ShowWindow(g_ppuw, SW_SHOWNOACTIVATE);
}

/* ===================================================================== debugger / memory editor */
static const char DIS_MN[] =
    "BRKORAJAMSLONOPORAASLSLOPHPORAASLANCNOPORAASLSLOBPLORAJAMSLONOPORAASLSLOCLCORANOPSLONOPORAASLSLO"
    "JSRANDJAMRLABITANDROLRLAPLPANDROLANCBITANDROLRLABMIANDJAMRLANOPANDROLRLASECANDNOPRLANOPANDROLRLA"
    "RTIEORJAMSRENOPEORLSRSREPHAEORLSRALRJMPEORLSRSREBVCEORJAMSRENOPEORLSRSRECLIEORNOPSRENOPEORLSRSRE"
    "RTSADCJAMRRANOPADCRORRRAPLAADCRORARRJMPADCRORRRABVSADCJAMRRANOPADCRORRRASEIADCNOPRRANOPADCRORRRA"
    "NOPSTANOPSAXSTYSTASTXSAXDEYNOPTXAANESTYSTASTXSAXBCCSTAJAMSHASTYSTASTXSAXTYASTATXSTASSHYSTASHXSHA"
    "LDYLDALDXLAXLDYLDALDXLAXTAYLDATAXLXALDYLDALDXLAXBCSLDAJAMLAXLDYLDALDXLAXCLVLDATSXLASLDYLDALDXLAX"
    "CPYCMPNOPDCPCPYCMPDECDCPINYCMPDEXSBXCPYCMPDECDCPBNECMPJAMDCPNOPCMPDECDCPCLDCMPNOPDCPNOPCMPDECDCP"
    "CPXSBCNOPISCCPXSBCINCISCINXSBCNOPSBCCPXSBCINCISCBEQSBCJAMISCNOPSBCINCISCSEDSBCNOPISCNOPSBCINCISC";
static const char DIS_MODE[] = "i(i(zzzzi#A#aaaar)i)xxxxiYiYXXXXa(i(zzzzi#A#aaaar)i)xxxxiYiYXXXXi(i(zzzzi#A#aaaar)i)xxxxiYiYXXXXi(i(zzzzi#A#naaar)i)xxxxiYiYXXXX#(#(zzzzi#i#aaaar)i)xxyyiYiYXXYY#(#(zzzzi#i#aaaar)i)xxyyiYiYXXYY#(#(zzzzi#i#aaaar)i)xxxxiYiYXXXX#(#(zzzzi#i#aaaar)i)xxxxiYiYXXXX";
static int g_dspace, g_dbase, g_dsel, g_dnib = -1, g_cw = 8, g_chh = 16;
#define DBG_TOP 40
#define DBG_HEXCOL 56
static const wchar_t *SPACE_NAME[5] = { L"CPU bus  $0000-$FFFF", L"PPU bus  $0000-$3FFF", L"OAM (sprites)", L"Cart WRAM / battery", L"CPU RAM  $0000-$07FF" };
static int space_size(int s) { return s == 0 ? 0x10000 : s == 1 ? 0x4000 : s == 2 ? 0x100 : s == 3 ? (nes->wram_size ? (int)nes->wram_size : 0x10) : 0x800; }
static uint8_t space_peek(int s, int a) {
    switch (s) {
    case 0: return nes_peek(nes, (uint16_t)a);
    case 1: return nes_ppu_peek(nes, (uint16_t)a);
    case 2: return nes->s.ppu.oam[a & 0xFF];
    case 3: return nes->wram_size ? nes->s.cart.wram[a % nes->wram_size] : 0;
    default: return nes->s.ram[a & 0x7FF];
    }
}
static void space_poke(int s, int a, uint8_t v) {
    switch (s) {
    case 0: nes_poke(nes, (uint16_t)a, v); break;
    case 1: nes_ppu_poke(nes, (uint16_t)a, v); break;
    case 2: nes->s.ppu.oam[a & 0xFF] = v; break;
    case 3: if (nes->wram_size) nes->s.cart.wram[a % nes->wram_size] = v; break;
    default: nes->s.ram[a & 0x7FF] = v; break;
    }
}
static int disasm(uint16_t pc, wchar_t *out, int n) {
    uint8_t op = nes_peek(nes, pc), b1 = nes_peek(nes, (uint16_t)(pc + 1)), b2 = nes_peek(nes, (uint16_t)(pc + 2));
    char m = DIS_MODE[op];
    int len = (m == 'i' || m == 'A') ? 1 : (m == 'a' || m == 'X' || m == 'Y' || m == 'n') ? 3 : 2;
    wchar_t mn[4] = { (wchar_t)DIS_MN[op * 3], (wchar_t)DIS_MN[op * 3 + 1], (wchar_t)DIS_MN[op * 3 + 2], 0 }, arg[24], by[16];
    unsigned w = (unsigned)(b1 | b2 << 8);
    switch (m) {
    case 'A': wcscpy(arg, L"A"); break;
    case '#': wfmt(arg, 24, L"#$%02X", b1); break;
    case 'z': wfmt(arg, 24, L"$%02X", b1); break;
    case 'x': wfmt(arg, 24, L"$%02X,X", b1); break;
    case 'y': wfmt(arg, 24, L"$%02X,Y", b1); break;
    case 'a': wfmt(arg, 24, L"$%04X", w); break;
    case 'X': wfmt(arg, 24, L"$%04X,X", w); break;
    case 'Y': wfmt(arg, 24, L"$%04X,Y", w); break;
    case 'n': wfmt(arg, 24, L"($%04X)", w); break;
    case '(': wfmt(arg, 24, L"($%02X,X)", b1); break;
    case ')': wfmt(arg, 24, L"($%02X),Y", b1); break;
    case 'r': wfmt(arg, 24, L"$%04X", (unsigned)(uint16_t)(pc + 2 + (int8_t)b1)); break;
    default: arg[0] = 0; break;
    }
    if (len == 1) wfmt(by, 16, L"%02X      ", op);
    else if (len == 2) wfmt(by, 16, L"%02X %02X   ", op, b1);
    else wfmt(by, 16, L"%02X %02X %02X", op, b1, b2);
    wfmt(out, (size_t)n, L"%04X  %ls  %ls %ls", pc, by, mn, arg);
    return len;
}
static void dbg_line(HDC dc, int col, int row, COLORREF c, const wchar_t *s) { text(dc, 8 + col * g_cw, DBG_TOP + 6 + row * g_chh, c, s); }
static void dbg_paint(HDC dc, int w, int h) {
    NesState *s = &nes->s;
    wchar_t b[200];
    const COLORREF T = RGB(225, 225, 230), D = RGB(140, 145, 160), H = RGB(255, 210, 110);
    fill(dc, 0, 0, w, h, RGB(24, 26, 30));
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ of = SelectObject(dc, g_mono);
    Cpu *c = &s->cpu; Ppu *p = &s->ppu;
    int r = 0;
    wfmt(b, 200, L"CPU  PC=$%04X A=$%02X X=$%02X Y=$%02X S=$%02X", c->pc, c->a, c->x, c->y, c->s); dbg_line(dc, 0, r++, T, b);
    wfmt(b, 200, L"     P=%lc%lc-%lc%lc%lc%lc%lc  cycles %.0f%ls", (c->p & FN) ? L'N' : L'n', (c->p & FV) ? L'V' : L'v', (c->p & FB) ? L'B' : L'b',
         (c->p & FD) ? L'D' : L'd', (c->p & FI) ? L'I' : L'i', (c->p & FZ) ? L'Z' : L'z', (c->p & FC) ? L'C' : L'c', (double)c->cycles,
         c->jammed ? L"  JAMMED" : L"");
    dbg_line(dc, 0, r++, c->jammed ? RGB(255, 90, 90) : T, b);
    wfmt(b, 200, L"     IRQ:%ls%ls%ls  NMI line %d", (c->irq_src & IRQ_FRAME) ? L" frame" : L"", (c->irq_src & IRQ_DMC) ? L" dmc" : L"",
         s->cart.irq_line ? L" mapper" : L"", c->nmi_line);
    dbg_line(dc, 0, r++, D, b);
    wfmt(b, 200, L"PPU  scanline %3d  dot %3d  frame %.0f", p->scanline, p->dot, (double)p->frame); dbg_line(dc, 0, r++, T, b);
    wfmt(b, 200, L"     v=$%04X t=$%04X x=%d w=%d", p->v, p->t, p->fx, p->w); dbg_line(dc, 0, r++, D, b);
    wfmt(b, 200, L"     CTRL=$%02X MASK=$%02X STAT=$%02X OAMADDR=$%02X", p->ctrl, p->mask, p->status, p->oamaddr); dbg_line(dc, 0, r++, D, b);
    Apu *a = &s->apu;
    wfmt(b, 200, L"APU  len %3d %3d %3d %3d  dmc %4d left  %ls", a->sq[0].lc.len, a->sq[1].lc.len, a->tri.lc.len, a->noise.lc.len,
         a->dmc.remain, a->fc.mode ? L"5-step" : L"4-step");
    dbg_line(dc, 0, r++, D, b);
    if (g_have_rom) {
        wchar_t mn[64]; widen(mn, 64, nes_mapper_name(nes->mapper));
        wfmt(b, 200, L"Cart mapper %d %ls  PRG %uK CHR %uK%ls", nes->mapper, mn, nes->prg_size / 1024,
             (nes->chr_is_ram ? nes->chr_ram_size : nes->chr_size) / 1024, nes->chr_is_ram ? L" RAM" : L"");
        dbg_line(dc, 0, r++, T, b);
        wfmt(b, 200, L"     $8000:%02X $A000:%02X $C000:%02X $E000:%02X (8K banks)", s->cart.prg_off[0] / 0x2000,
             s->cart.prg_off[1] / 0x2000, s->cart.prg_off[2] / 0x2000, s->cart.prg_off[3] / 0x2000);
        dbg_line(dc, 0, r++, D, b);
    } else r += 2;
    {
        int open = 0; for (int q = 1; q <= PINS; q++) open += !nes->conn.conn[q];
        wfmt(b, 200, L"Slot %ls  %d open  %d flaky  power %ls", nes->conn.perfect ? L"perfect" : L"FAULTY", open, nes->conn.nflaky,
             nes->conn.powered ? L"ok" : L"LOST");
        dbg_line(dc, 0, r++, nes->conn.perfect ? D : RGB(255, 190, 90), b);
    }
    r++;
    uint16_t pc = c->pc;
    for (int i = 0; i < 16 && r < 40; i++, r++) {
        wchar_t d[64]; pc = (uint16_t)(pc + disasm(pc, d, 64));
        wfmt(b, 200, L"%lc %ls", i ? L' ' : L'>', d);
        dbg_line(dc, 0, r, i ? T : H, b);
    }
    /* hex view */
    int size = space_size(g_dspace);
    dbg_line(dc, DBG_HEXCOL, 0, H, SPACE_NAME[g_dspace]);
    dbg_line(dc, DBG_HEXCOL, 1, D, L"       00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F");
    for (int row = 0; row < 24; row++) {
        int base = g_dbase + row * 16;
        if (base >= size) break;
        wfmt(b, 200, L"%04X:", base); dbg_line(dc, DBG_HEXCOL, row + 2, D, b);
        for (int k = 0; k < 16 && base + k < size; k++) {
            int ad = base + k;
            wfmt(b, 200, L"%02X", space_peek(g_dspace, ad));
            int x = 8 + (DBG_HEXCOL + 7 + k * 3) * g_cw, y = DBG_TOP + 6 + (row + 2) * g_chh;
            if (ad == g_dsel) { fill(dc, x - 2, y, x + 2 * g_cw + 2, y + g_chh, RGB(70, 110, 190)); if (g_dnib >= 0) wfmt(b, 200, L"%X_", g_dnib); }
            text(dc, x, y, T, b);
        }
    }
    dbg_line(dc, DBG_HEXCOL, 27, D, L"Click a byte, type hex to change it. Wheel/PgUp/PgDn scroll.");
    dbg_line(dc, DBG_HEXCOL, 28, D, L"CPU-bus writes to $8000+ patch the loaded ROM image.");
    SelectObject(dc, of);
}
static void dbg_refresh(void) { if (g_dbg) InvalidateRect(g_dbg, NULL, FALSE); }
static void dbg_ensure_visible(void) {
    int size = space_size(g_dspace);
    if (g_dsel < 0) g_dsel = 0;
    if (g_dsel >= size) g_dsel = size - 1;
    if (g_dsel < g_dbase) g_dbase = g_dsel & ~15;
    if (g_dsel >= g_dbase + 24 * 16) g_dbase = (g_dsel & ~15) - 23 * 16;
    if (g_dbase < 0) g_dbase = 0;
}
static int on_key(int vk, int repeat);
static void dbg_step(int what) {
    if (!g_have_rom) return;
    g_paused = 1;
    if (what == 0) cpu_step(nes);
    else if (what == 1) { int sl = nes->s.ppu.scanline; uint64_t lim = nes->s.cpu.cycles + 200; while (nes->s.ppu.scanline == sl && nes->s.cpu.cycles < lim) cpu_step(nes); }
    else nes_run_frame(nes);
    nes->audio_len = 0;
    present(); dbg_refresh(); update_title();
}
static LRESULT CALLBACK dbg_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HWND cb = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 8, 8, 200, 200, h, (HMENU)(INT_PTR)1, g_inst, NULL);
        for (int i = 0; i < 5; i++) SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)SPACE_NAME[i]);
        SendMessageW(cb, CB_SETCURSEL, (WPARAM)g_dspace, 0);
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"0000", WS_CHILD | WS_VISIBLE | ES_UPPERCASE, 216, 9, 60, 22, h, (HMENU)(INT_PTR)2, g_inst, NULL);
        SendMessageW(e, EM_SETLIMITTEXT, 4, 0);
        static const wchar_t *BT[5] = { L"Go", L"Run / Pause", L"Step instr", L"Step line", L"Step frame" };
        static const int BX[5] = { 280, 340, 440, 530, 620 }, BWD[5] = { 50, 90, 80, 80, 80 };
        HWND ctl[7]; ctl[0] = cb; ctl[1] = e;
        for (int i = 0; i < 5; i++)
            ctl[2 + i] = CreateWindowExW(0, L"BUTTON", BT[i], WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, BX[i], 8, BWD[i], 24, h, (HMENU)(INT_PTR)(3 + i), g_inst, NULL);
        for (int i = 0; i < 7; i++) SendMessageW(ctl[i], WM_SETFONT, (WPARAM)g_ui, TRUE);
        SetTimer(h, 1, 100, NULL);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case 1: if (HIWORD(w) == CBN_SELCHANGE) { g_dspace = (int)SendMessageW((HWND)l, CB_GETCURSEL, 0, 0); g_dbase = g_dsel = 0; g_dnib = -1; dbg_refresh(); SetFocus(h); } break;
        case 3: { wchar_t t[8]; GetDlgItemTextW(h, 2, t, 8); g_dsel = (int)wcstol(t, NULL, 16); g_dbase = g_dsel & ~15; g_dnib = -1; dbg_ensure_visible(); dbg_refresh(); SetFocus(h); } break;
        case 4: g_paused ^= 1; update_title(); SetFocus(h); break;
        case 5: dbg_step(0); SetFocus(h); break;
        case 6: dbg_step(1); SetFocus(h); break;
        case 7: dbg_step(2); SetFocus(h); break;
        }
        return 0;
    case WM_TIMER: dbg_refresh(); return 0;
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        int x = (short)LOWORD(l), y = (short)HIWORD(l);
        int row = (y - DBG_TOP - 6) / g_chh - 2, col = (x - 8 - (DBG_HEXCOL + 7) * g_cw + g_cw / 2) / (3 * g_cw);
        if (row >= 0 && row < 24 && col >= 0 && col < 16 && x >= 8 + (DBG_HEXCOL + 6) * g_cw) {
            int a = g_dbase + row * 16 + col;
            if (a < space_size(g_dspace)) { g_dsel = a; g_dnib = -1; dbg_refresh(); }
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d = (short)HIWORD(w) / WHEEL_DELTA;
        g_dbase -= d * 48;
        int mx = space_size(g_dspace) - 24 * 16;
        if (g_dbase > mx) g_dbase = mx;
        if (g_dbase < 0) g_dbase = 0;
        g_dbase &= ~15;
        dbg_refresh();
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_LEFT: g_dsel--; break;
        case VK_RIGHT: g_dsel++; break;
        case VK_UP: g_dsel -= 16; break;
        case VK_DOWN: g_dsel += 16; break;
        case VK_PRIOR: g_dsel -= 256; g_dbase -= 256; break;
        case VK_NEXT: g_dsel += 256; g_dbase += 256; break;
        default: if (w == VK_F1) on_command(ID_RESET); else if (w >= VK_F2 && w <= VK_F11) on_key((int)w, 0); return 0;
        }
        g_dnib = -1; dbg_ensure_visible(); dbg_refresh();
        return 0;
    case WM_CHAR: {
        int ch = (int)w, d = (ch >= '0' && ch <= '9') ? ch - '0' : (ch >= 'a' && ch <= 'f') ? ch - 'a' + 10 : (ch >= 'A' && ch <= 'F') ? ch - 'A' + 10 : -1;
        if (d < 0) return 0;
        if (g_dnib < 0) g_dnib = d;
        else { space_poke(g_dspace, g_dsel, (uint8_t)(g_dnib << 4 | d)); g_dnib = -1; g_dsel++; dbg_ensure_visible(); if (g_paused) present(); }
        dbg_refresh();
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        if (rc.right < 1 || rc.bottom <= DBG_TOP + 1) { EndPaint(h, &ps); return 0; }
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bm = CreateCompatibleBitmap(dc, rc.right, rc.bottom - DBG_TOP);
        HGDIOBJ ob = SelectObject(mem, bm);
        SetViewportOrgEx(mem, 0, -DBG_TOP, NULL);
        dbg_paint(mem, rc.right, rc.bottom);
        SetViewportOrgEx(mem, 0, 0, NULL);
        BitBlt(dc, 0, DBG_TOP, rc.right, rc.bottom - DBG_TOP, mem, 0, 0, SRCCOPY);
        SelectObject(mem, ob); DeleteObject(bm); DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: { RECT rc; GetClientRect(h, &rc); rc.bottom = DBG_TOP; FillRect((HDC)w, &rc, GetSysColorBrush(COLOR_BTNFACE)); return 1; }
    case WM_DESTROY: KillTimer(h, 1); g_dbg = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void show_debugger(void) {
    if (g_dbg) { ShowWindow(g_dbg, SW_SHOW); SetForegroundWindow(g_dbg); return; }
    HDC dc = GetDC(g_wnd);
    HGDIOBJ o = SelectObject(dc, g_mono);
    TEXTMETRICW tm; GetTextMetricsW(dc, &tm);
    g_cw = tm.tmAveCharWidth; g_chh = tm.tmHeight + 1;
    SelectObject(dc, o); ReleaseDC(g_wnd, dc);
    RECT r = { 0, 0, (DBG_HEXCOL + 7 + 48 + 2) * g_cw + 16, DBG_TOP + 12 + 30 * g_chh };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_dbg = CreateWindowExW(0, L"NessysDebug", L"NESSYSINTEGER debugger / memory editor", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                            r.right - r.left, r.bottom - r.top, NULL, NULL, g_inst, NULL);
    ShowWindow(g_dbg, SW_SHOW);
}

/* ===================================================================== cheats */
static LRESULT CALLBACK cheat_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CREATE: {
        HWND t = CreateWindowExW(0, L"STATIC", L"One code per line.  Game Genie: 6 or 8 letters (e.g. SXIOPO).  RAM freeze: ADDR:VALUE in hex (e.g. 0075:09).  # starts a comment.",
                                 WS_CHILD | WS_VISIBLE, 10, 8, 460, 36, h, NULL, g_inst, NULL);
        wchar_t wt[8192]; widen(wt, 8192, g_cheat_text);
        HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wt, WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN | ES_UPPERCASE,
                                 10, 48, 460, 230, h, (HMENU)(INT_PTR)1, g_inst, NULL);
        HWND b1 = CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 290, 288, 85, 28, h, (HMENU)(INT_PTR)2, g_inst, NULL);
        HWND b2 = CreateWindowExW(0, L"BUTTON", L"Clear all", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 385, 288, 85, 28, h, (HMENU)(INT_PTR)3, g_inst, NULL);
        SendMessageW(t, WM_SETFONT, (WPARAM)g_ui, TRUE); SendMessageW(e, WM_SETFONT, (WPARAM)g_mono, TRUE);
        SendMessageW(b1, WM_SETFONT, (WPARAM)g_ui, TRUE); SendMessageW(b2, WM_SETFONT, (WPARAM)g_ui, TRUE);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) == 3) SetDlgItemTextW(h, 1, L"");
        if (LOWORD(w) == 2 || LOWORD(w) == 3) {
            wchar_t wt[8192]; GetDlgItemTextW(h, 1, wt, 8192);
            WideCharToMultiByte(CP_UTF8, 0, wt, -1, g_cheat_text, (int)sizeof g_cheat_text, NULL, NULL);
            g_cheat_text[sizeof g_cheat_text - 1] = 0;
            int bad = apply_cheats(g_cheat_text);
            if (g_have_rom) { wchar_t p[MAX_PATH]; path_ext(p, g_rom, L".cht"); write_file(p, g_cheat_text, strlen(g_cheat_text)); }
            osd(L"%d Game Genie code(s), %d RAM freeze(s) active%ls", nes->ngenie, nes->nfreeze, bad ? L" - some lines not understood" : L"");
        }
        return 0;
    case WM_DESTROY: g_cheat = NULL; return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
static void show_cheats(void) {
    if (g_cheat) { SetForegroundWindow(g_cheat); return; }
    RECT r = { 0, 0, 480, 326 };
    AdjustWindowRect(&r, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE);
    g_cheat = CreateWindowExW(WS_EX_TOOLWINDOW, L"NessysCheats", L"Cheats: Game Genie + RAM freeze", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                              CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, g_wnd, NULL, g_inst, NULL);
    ShowWindow(g_cheat, SW_SHOW);
}

/* ===================================================================== menus and commands */
static HMENU build_menu(void) {
    HMENU m = CreateMenu(), f = CreatePopupMenu(), slot = CreatePopupMenu(), con = CreatePopupMenu(), ram = CreatePopupMenu(),
          al = CreatePopupMenu(), cart = CreatePopupMenu(), fl = CreatePopupMenu(), tools = CreatePopupMenu(), vid = CreatePopupMenu(),
          sc = CreatePopupMenu(), au = CreatePopupMenu(), help = CreatePopupMenu();
    AppendMenuW(f, MF_STRING, ID_OPEN, L"&Open ROM...\tCtrl+O");
    AppendMenuW(f, MF_STRING, ID_HOTSWAP, L"&Hot-swap cartridge (power stays on)...\tCtrl+H");
    AppendMenuW(f, MF_SEPARATOR, 0, NULL);
    AppendMenuW(f, MF_STRING, ID_SAVE, L"&Save state\tF5");
    AppendMenuW(f, MF_STRING, ID_LOAD, L"&Load state\tF7");
    for (int i = 0; i < 10; i++) { wchar_t s[32]; wfmt(s, 32, L"Slot %d\t%d", i, i); AppendMenuW(slot, MF_STRING, ID_SLOT0 + i, s); }
    AppendMenuW(f, MF_POPUP, (UINT_PTR)slot, L"State s&lot");
    AppendMenuW(f, MF_SEPARATOR, 0, NULL);
    AppendMenuW(f, MF_STRING, ID_SHOT, L"Screens&hot\tF8");
    AppendMenuW(f, MF_SEPARATOR, 0, NULL);
    AppendMenuW(f, MF_STRING, ID_EXIT, L"E&xit");
    AppendMenuW(con, MF_STRING, ID_RESET, L"&Reset button (hold F1)\tF1");
    AppendMenuW(con, MF_STRING, ID_POWER, L"&Power switch\tF2");
    AppendMenuW(con, MF_STRING, ID_PAUSE, L"P&ause\tF3");
    AppendMenuW(con, MF_STRING, ID_ADVANCE, L"&Frame advance\tF4");
    AppendMenuW(con, MF_SEPARATOR, 0, NULL);
    AppendMenuW(ram, MF_STRING, ID_RAM0 + 0, L"All $00");
    AppendMenuW(ram, MF_STRING, ID_RAM0 + 1, L"All $FF");
    AppendMenuW(ram, MF_STRING, ID_RAM0 + 2, L"Random");
    AppendMenuW(ram, MF_STRING, ID_RAM0 + 3, L"$00/$FF stripes");
    AppendMenuW(ram, MF_STRING, ID_RAM0 + 4, L"Like a real console (default)");
    AppendMenuW(ram, MF_SEPARATOR, 0, NULL);
    AppendMenuW(ram, MF_STRING, ID_RAM0 + 5, L"Swap in a different console");
    AppendMenuW(con, MF_POPUP, (UINT_PTR)ram, L"Power-on &RAM contents");
    for (int i = 0; i < 4; i++) { wchar_t s[48]; wfmt(s, 48, L"Phase %d", i); AppendMenuW(al, MF_STRING, ID_ALIGN0 + i, s); }
    AppendMenuW(con, MF_POPUP, (UINT_PTR)al, L"CPU/PPU clock &alignment");
    AppendMenuW(con, MF_STRING, ID_EXPAUDIO, L"Cartridge &expansion audio (Famicom / EXP6 mod)");
    HMENU cm = CreatePopupMenu();
    AppendMenuW(cm, MF_STRING, ID_CONSOLE0 + 0, L"Auto (Famicom for Famicom-only boards)");
    AppendMenuW(cm, MF_STRING, ID_CONSOLE0 + 1, L"NES front-loader (NES-001)");
    AppendMenuW(cm, MF_STRING, ID_CONSOLE0 + 2, L"NES top-loader (NES-101)");
    AppendMenuW(cm, MF_STRING, ID_CONSOLE0 + 3, L"Famicom (HVC-001)");
    AppendMenuW(con, MF_POPUP, (UINT_PTR)cm, L"Console &model");
    AppendMenuW(con, MF_STRING, ID_OPPOSITE, L"Allow Left+Right / Up+Down");
    AppendMenuW(cart, MF_STRING, ID_TILT, L"&Tilt / connector panel...\tF9");
    AppendMenuW(cart, MF_STRING, ID_RESEAT, L"&Reseat straight\tBackspace");
    AppendMenuW(cart, MF_STRING, ID_PULL, L"&Pull out / push in");
    AppendMenuW(cart, MF_STRING, ID_TRAY, L"Push the &tray down / release it\t/");
    AppendMenuW(cart, MF_STRING, ID_BLOW, L"&Blow on the contacts");
    AppendMenuW(cart, MF_STRING, ID_CLEANCART, L"Clean the cartridge contacts (alcohol)");
    AppendMenuW(cart, MF_STRING, ID_CLEANCONN, L"Clean the console connector (kit)");
    AppendMenuW(cart, MF_STRING, ID_WEAR, L"&Wear and dirt (realistic contacts)");
    AppendMenuW(cart, MF_SEPARATOR, 0, NULL);
    AppendMenuW(cart, MF_STRING, ID_CIC, L"&CIC lockout chip active");
    for (int i = 0; i < 5; i++) AppendMenuW(fl, MF_STRING, ID_FLOAT0 + i, FLOAT_NAME[i]);
    HMENU km = CreatePopupMenu();
    for (int i = 0; i < 4; i++) AppendMenuW(km, MF_STRING, ID_KEY0 + i, KEY_NAME[i]);
    AppendMenuW(cart, MF_POPUP, (UINT_PTR)km, L"Cartridge's CIC &key");
    AppendMenuW(cart, MF_POPUP, (UINT_PTR)fl, L"&Open pins read as");
    AppendMenuW(tools, MF_STRING, ID_DEBUG, L"&Debugger / memory editor...\tF6");
    AppendMenuW(tools, MF_STRING, ID_PPUVIEW, L"&PPU viewer (tiles, nametables, palettes, sprites)...\tF12");
    AppendMenuW(tools, MF_STRING, ID_CHEATS, L"&Cheats (Game Genie, RAM freeze)...");
    for (int i = 0; i < 6; i++) { wchar_t s[16]; wfmt(s, 16, L"%dx", i + 1); AppendMenuW(sc, MF_STRING, ID_SCALE1 + i, s); }
    AppendMenuW(vid, MF_POPUP, (UINT_PTR)sc, L"&Window size");
    AppendMenuW(vid, MF_STRING, ID_ASPECT, L"8:7 pixel &aspect (like a TV)");
    AppendMenuW(vid, MF_STRING, ID_CROP, L"&Crop overscan (top/bottom 8 lines)");
    AppendMenuW(vid, MF_STRING, ID_FULL, L"&Fullscreen\tF11 / Alt+Enter");
    AppendMenuW(au, MF_STRING, ID_AUDIO, L"&Sound enabled");
    AppendMenuW(help, MF_STRING, ID_HELP, L"&Controls...");
    AppendMenuW(help, MF_STRING, ID_ABOUT, L"&About...");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)f, L"&File");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)con, L"&Console");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)cart, L"C&artridge");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)tools, L"&Tools");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)vid, L"&Video");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)au, L"A&udio");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)help, L"&Help");
    return m;
}
static void update_menu(void) {
    HMENU m = g_menu;
    CheckMenuRadioItem(m, ID_SLOT0, ID_SLOT0 + 9, ID_SLOT0 + (UINT)g_slot, MF_BYCOMMAND);
    CheckMenuRadioItem(m, ID_RAM0, ID_RAM0 + 4, ID_RAM0 + (UINT)nes->ram_init, MF_BYCOMMAND);
    CheckMenuRadioItem(m, ID_ALIGN0, ID_ALIGN0 + 3, ID_ALIGN0 + (UINT)nes->cpu_align, MF_BYCOMMAND);
    CheckMenuItem(m, ID_EXPAUDIO, MF_BYCOMMAND | (nes->exp_audio ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, ID_CONSOLE0, ID_CONSOLE0 + 3, ID_CONSOLE0 + (UINT)nes->console_setting, MF_BYCOMMAND);
    CheckMenuRadioItem(m, ID_FLOAT0, ID_FLOAT0 + 4, ID_FLOAT0 + (UINT)nes->conn.float_mode, MF_BYCOMMAND);
    CheckMenuRadioItem(m, ID_KEY0, ID_KEY0 + 3, ID_KEY0 + (UINT)nes->cic_key, MF_BYCOMMAND);
    CheckMenuItem(m, ID_WEAR, MF_BYCOMMAND | (g_wear ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuRadioItem(m, ID_SCALE1, ID_SCALE1 + 5, ID_SCALE1 + (UINT)g_scale - 1, MF_BYCOMMAND);
    CheckMenuItem(m, ID_PAUSE, MF_BYCOMMAND | (g_paused ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_OPPOSITE, MF_BYCOMMAND | (nes->allow_opposite ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_CIC, MF_BYCOMMAND | (nes->cic_enabled ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_ASPECT, MF_BYCOMMAND | (g_aspect ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_CROP, MF_BYCOMMAND | (g_crop ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_FULL, MF_BYCOMMAND | (g_full ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(m, ID_AUDIO, MF_BYCOMMAND | (g_audio_on ? MF_CHECKED : MF_UNCHECKED));
    UINT en = MF_BYCOMMAND | (g_have_rom ? MF_ENABLED : MF_GRAYED);
    EnableMenuItem(m, ID_SAVE, en); EnableMenuItem(m, ID_LOAD, en); EnableMenuItem(m, ID_SHOT, en);
    EnableMenuItem(m, ID_RESET, en); EnableMenuItem(m, ID_POWER, en);
}
static const wchar_t HELP_TEXT[] =
    L"PLAYER 1:  Arrows = D-pad    X = A    Z = B    Enter = Start    Right Shift = Select\n"
    L"PLAYER 2:  W A S D = D-pad    H = A    G = B    Y = Start    T = Select\n"
    L"Xbox-style gamepads work too (pad 1 = player 1, pad 2 = player 2).\n"
    L"Every key can be remapped in nessysinteger.ini, section [Keys] (Windows virtual-key codes).\n\n"
    L"F1 RESET button (hold it to keep the console in reset)     F2 power switch\n"
    L"F3 pause     F4 frame advance     hold Tab = fast forward\n"
    L"F5 save state     F7 load state     0-9 pick slot     F8 screenshot\n"
    L"F6 debugger / memory editor     F9 cartridge & connector panel     F12 PPU viewer\n"
    L"F11 or Alt+Enter fullscreen     Ctrl+O open ROM     Ctrl+H hot-swap cartridge (console stays on)\n\n"
    L"HANDLING THE CARTRIDGE (while you play):\n"
    L".  pull the cartridge out / push it in      /  push the NES-001 tray down / release it\n"
    L"[  ]  tilt it left / right       ;  '  lean it front / back\n"
    L"-  =  pull out / push in a little      Backspace  reseat it straight\n\n"
    L"Drag-and-drop a .nes file to load it; hold Ctrl while dropping to hot-swap.";
static void on_command(int id) {
    Connector *c = &nes->conn;
    if (id >= ID_SLOT0 && id < ID_SLOT0 + 10) { g_slot = id - ID_SLOT0; osd(L"State slot %d", g_slot); }
    else if (id >= ID_RAM0 && id < ID_RAM0 + 5) { nes->ram_init = id - ID_RAM0; osd(L"Power-on RAM pattern changed (takes effect at next power cycle)"); }
    else if (id == ID_RAM0 + 5) { nes->console_id = ((nes->console_id * 2654435761u + GetTickCount()) & 0x7FFFFFFFu) | 1u; nes->ram_init = 4; osd(L"Swapped in a different console (takes effect at next power cycle)"); }
    else if (id >= ID_ALIGN0 && id < ID_ALIGN0 + 4) { nes->cpu_align = id - ID_ALIGN0; osd(L"CPU/PPU phase %d (takes effect at next power cycle)", nes->cpu_align); }
    else if (id >= ID_FLOAT0 && id < ID_FLOAT0 + 5) { c->float_mode = (uint8_t)(id - ID_FLOAT0); conn_update(nes); tilt_refresh(); }
    else if (id >= ID_KEY0 && id < ID_KEY0 + 4) { nes->cic_key = id - ID_KEY0; tilt_refresh(); }
    else if (id >= ID_SCALE1 && id < ID_SCALE1 + 6) { g_scale = id - ID_SCALE1 + 1; resize_window(); }
    else switch (id) {
    case ID_OPEN: open_dialog(0); break;
    case ID_HOTSWAP: open_dialog(1); break;
    case ID_PULL: tilt_button(B_PULL); break;
    case ID_TRAY: tilt_button(B_TRAY); break;
    case ID_CLEANCART: tilt_button(B_CLEANCART); break;
    case ID_CLEANCONN: tilt_button(B_CLEANCONN); break;
    case ID_WEAR: cond_store(); g_wear ^= 1; cond_apply(); tilt_refresh(); break;
    case ID_SAVE: save_state(); break;
    case ID_LOAD: load_state(); break;
    case ID_SHOT: screenshot(); break;
    case ID_EXIT: PostMessageW(g_wnd, WM_CLOSE, 0, 0); break;
    case ID_RESET: if (g_have_rom && g_power_on) { nes->reset_pulse = 4; osd(L"Reset"); } break;
    case ID_CONSOLE0: case ID_CONSOLE0 + 1: case ID_CONSOLE0 + 2: case ID_CONSOLE0 + 3:
        nes->console_setting = id - ID_CONSOLE0; osd(L"Console model changed (takes effect at next power cycle)"); break;
    case ID_EXPAUDIO: nes->exp_audio = !nes->exp_audio; osd(nes->exp_audio ? L"Cartridge expansion audio on (Famicom / EXP6-modded NES)" : L"Cartridge expansion audio off (stock NES)"); break;
    case ID_POWER: power_switch(); break;
    case ID_PAUSE: g_paused ^= 1; update_title(); break;
    case ID_ADVANCE: if (g_have_rom) { g_paused = 1; g_advance = 1; update_title(); } break;
    case ID_OPPOSITE: nes->allow_opposite ^= 1; break;
    case ID_TILT: show_tilt(); break;
    case ID_PPUVIEW: show_ppuview(); break;
    case ID_RESEAT: tilt_button(B_RESEAT); break;
    case ID_BLOW: tilt_button(B_BLOW); break;
    case ID_CIC: nes->cic_enabled ^= 1; tilt_refresh(); break;
    case ID_DEBUG: show_debugger(); break;
    case ID_CHEATS: show_cheats(); break;
    case ID_ASPECT: g_aspect ^= 1; resize_window(); break;
    case ID_CROP: g_crop ^= 1; resize_window(); break;
    case ID_FULL: toggle_fullscreen(); break;
    case ID_AUDIO: g_audio_on ^= 1; break;
    case ID_HELP: MessageBoxW(g_wnd, HELP_TEXT, L"NESSYSINTEGER controls", MB_ICONINFORMATION); break;
    case ID_ABOUT:
        MessageBoxW(g_wnd, L"NESSYSINTEGER\nCycle-stepped NES (NTSC 2A03 + 2C02) emulator with a physical cartridge-connector model.\n\n"
                           L"CPU: every cycle is a real bus access (dummy reads/writes, DMA halts, open bus).\n"
                           L"PPU: dot-by-dot with 2-dot fetches visible to the cartridge.\n"
                           L"Cartridge: all 72 edge-connector pins are modelled.", L"About", MB_ICONINFORMATION);
        break;
    }
    update_menu(); save_settings(); InvalidateRect(g_wnd, NULL, FALSE);
}
static void nudge(float *v, float d, const wchar_t *what) { *v += d; conn_changed(); osd(L"%ls %+.2f", what, *v); }
static int on_key(int vk, int repeat) {
    int ctrl = GetKeyState(VK_CONTROL) < 0;
    Connector *c = &nes->conn;
    if (ctrl && vk == 'O') { on_command(ID_OPEN); return 1; }
    if (ctrl && vk == 'H') { on_command(ID_HOTSWAP); return 1; }
    if (ctrl) return 0;
    switch (vk) {
    case VK_F1: if (!repeat && g_have_rom && g_power_on) { nes->reset_button = 1; osd(L"RESET held"); } return 1;
    case VK_F2: if (!repeat) power_switch(); return 1;
    case VK_OEM_2: if (!repeat) tilt_button(B_TRAY); return 1;
    case VK_OEM_PERIOD: if (!repeat) tilt_button(B_PULL); return 1;
    case VK_F3: if (!repeat) on_command(ID_PAUSE); return 1;
    case VK_F4: on_command(ID_ADVANCE); return 1;
    case VK_F5: on_command(ID_SAVE); return 1;
    case VK_F6: on_command(ID_DEBUG); return 1;
    case VK_F7: on_command(ID_LOAD); return 1;
    case VK_F8: on_command(ID_SHOT); return 1;
    case VK_F9: on_command(ID_TILT); return 1;
    case VK_F11: if (!repeat) on_command(ID_FULL); return 1;
    case VK_F12: if (!repeat) on_command(ID_PPUVIEW); return 1;
    case VK_ESCAPE: if (g_full) on_command(ID_FULL); return 1;
    case VK_OEM_4: nudge(&c->roll, -0.04f, L"Roll"); return 1;          /* [ */
    case VK_OEM_6: nudge(&c->roll, 0.04f, L"Roll"); return 1;           /* ] */
    case VK_OEM_1: nudge(&c->pitch, -0.04f, L"Pitch"); return 1;        /* ; */
    case VK_OEM_7: nudge(&c->pitch, 0.04f, L"Pitch"); return 1;         /* ' */
    case VK_OEM_MINUS: nudge(&c->seat, -0.04f, L"Inserted"); return 1;
    case VK_OEM_PLUS: nudge(&c->seat, 0.04f, L"Inserted"); return 1;
    case VK_BACK: tilt_button(B_RESEAT); return 1;
    }
    if (vk >= '0' && vk <= '9') { on_command(ID_SLOT0 + vk - '0'); return 1; }
    return 0;
}
static LRESULT CALLBACK main_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_COMMAND: on_command(LOWORD(w)); return 0;
    case WM_KEYDOWN: if (on_key((int)w, (int)((l >> 30) & 1))) return 0; break;
    case WM_KEYUP: if (w == VK_F1 && nes->reset_button) { nes->reset_button = 0; osd(L"RESET released"); return 0; } break;
    case WM_KILLFOCUS: nes->reset_button = 0; break;
    case WM_SYSKEYDOWN: if (w == VK_RETURN && !((l >> 30) & 1)) { on_command(ID_FULL); return 0; } break;
    case WM_SYSCHAR: if (w == VK_RETURN) return 0; break;
    case WM_DROPFILES: {
        wchar_t p[MAX_PATH];
        if (DragQueryFileW((HDROP)w, 0, p, MAX_PATH)) {
            if (GetKeyState(VK_CONTROL) < 0 && g_have_rom && g_power_on) { wcscpy(g_swap_path, p); seq_start(SEQ_SWAP); osd(L"Pulling the cartridge out with the power on..."); }
            else load_rom_file(p, GetKeyState(VK_CONTROL) < 0);
        }
        DragFinish((HDROP)w);
        return 0;
    }
    case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); blit(dc); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, NULL, FALSE); return 0;
    case WM_CLOSE: save_battery(); DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

/* ===================================================================== entry point */
static double qpc_now(void) { LARGE_INTEGER t; QueryPerformanceCounter(&t); return (double)t.QuadPart; }
static void reg_class(const wchar_t *name, WNDPROC proc, HBRUSH bg) {
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize = sizeof wc; wc.lpfnWndProc = proc; wc.hInstance = g_inst; wc.lpszClassName = name;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW); wc.hIcon = LoadIconW(NULL, (LPCWSTR)IDI_APPLICATION); wc.hbrBackground = bg;
    RegisterClassExW(&wc);
}
int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmd, int show) {
    (void)prev; (void)cmd;
    g_inst = inst;
    typedef BOOL (WINAPI *DpiAware_t)(void);
    DpiAware_t dpi = (DpiAware_t)(void *)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDPIAware");
    if (dpi) dpi();
    nes = nes_create();
    nes->seed = GetTickCount();
    srand(GetTickCount());
    GetModuleFileNameW(NULL, g_ini, MAX_PATH);
    path_ext(g_ini, g_ini, L".ini");
    {   /* the ini is named nessysinteger.ini next to the exe */
        wchar_t *sl = wcsrchr(g_ini, L'\\');
        if (sl && (size_t)(sl - g_ini) + 20 < MAX_PATH) wcscpy(sl + 1, L"nessysinteger.ini");
    }
    load_settings();
    conn_update(nes);
    nes_palette(g_pal);
    memset(&g_bmi, 0, sizeof g_bmi);
    g_bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); g_bmi.bmiHeader.biWidth = 256; g_bmi.bmiHeader.biHeight = -240;
    g_bmi.bmiHeader.biPlanes = 1; g_bmi.bmiHeader.biBitCount = 32; g_bmi.bmiHeader.biCompression = BI_RGB;
    g_ui = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    g_small = CreateFontW(-10, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    g_mono = CreateFontW(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    reg_class(L"NessysMain", main_proc, NULL);
    reg_class(L"NessysTilt", tilt_proc, NULL);
    reg_class(L"NessysPPU", ppuv_proc, NULL);
    reg_class(L"NessysDebug", dbg_proc, NULL);
    reg_class(L"NessysCheats", cheat_proc, GetSysColorBrush(COLOR_BTNFACE));
    g_menu = build_menu();
    g_wnd = CreateWindowExW(0, L"NessysMain", APP, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 800, 700, NULL, g_menu, inst, NULL);
    if (!g_wnd) return 1;
    DragAcceptFiles(g_wnd, TRUE);
    resize_window();
    ShowWindow(g_wnd, show);
    update_menu();
    timeBeginPeriod(1);
    xinput_init();
    audio_open();
    {
        int argc = 0;
        wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        if (argv && argc > 1) load_rom_file(argv[1], 0);
        if (argv) LocalFree(argv);
    }
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    double period = (double)fq.QuadPart / FPS, next = qpc_now();
    int dbg_tick = 0;
    DWORD wait_start = 0;
    MSG msg;
    for (;;) {
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) goto done;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_have_rom || (g_paused && !g_advance)) {
            MsgWaitForMultipleObjects(0, NULL, FALSE, 100, QS_ALLINPUT);
            InvalidateRect(g_wnd, NULL, FALSE);
            next = qpc_now();
            continue;
        }
        int turbo = GetForegroundWindow() == g_wnd && (GetAsyncKeyState(VK_TAB) & 0x8000);
        anim_step();
        int use_audio = g_audio_on && g_wo && !turbo && !g_paused && g_power_on;
        if (use_audio) {
            if (audio_queued() >= 3) {                            /* the sound card is the clock */
                if (!wait_start) wait_start = GetTickCount();
                if (GetTickCount() - wait_start < 500) { Sleep(1); continue; }
                audio_close();                                    /* driver stalled: fall back to timer pacing */
                osd(L"Sound device stopped responding - continuing without sound");
            }
            wait_start = 0;
            next = qpc_now() + period;
        } else if (!turbo && !g_advance) {
            double now = qpc_now();
            if (now < next) { double ms = (next - now) * 1000.0 / (double)fq.QuadPart; Sleep(ms > 2.0 ? (DWORD)(ms - 1.0) : 0); continue; }
            next = (now - next > period * 4) ? now + period : next + period;
        }
        for (int i = 0; i < (turbo ? 5 : 1); i++) {
            if (!g_power_on) break;
            nes->pad[0] = read_pad(0); nes->pad[1] = read_pad(1);
            nes_run_frame(nes);
        }
        if (g_have_rom && g_power_on && nes->lock_dead && !g_was_dead)
            osd(L"Blinking! The lockout chip can't talk to the cartridge - reseat it (Backspace), push the tray down (/) or clean it (F9)");
        g_was_dead = nes->lock_dead;
        if ((g_tick & 63) == 0) tilt_refresh();
        { static unsigned vt; if (g_ppuw && (++vt & 3) == 0) InvalidateRect(g_ppuw, NULL, FALSE); }   /* PPU viewer: 15 updates a second */
        if (use_audio) audio_push(nes->audio, nes->audio_len);
        if (g_advance) { g_advance = 0; dbg_refresh(); }
        present();
        if (g_dbg && ++dbg_tick >= 6) { dbg_tick = 0; dbg_refresh(); }
    }
done:
    audio_close();
    timeEndPeriod(1);
    save_settings();
    nes_destroy(nes);
    return 0;
}
