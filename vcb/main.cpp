#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <gdiplus.h>
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>

#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "winmm.lib")

using namespace Gdiplus;

// ------------------------------------------------------------------ config
struct Config {
    int   fov           = 150;
    bool  drawOverlay   = true;
    bool  drawFov       = true;
    int   aimKey        = VK_XBUTTON2;
    int   triggerKey    = VK_MENU;
    int   hueMin        = 45, hueMax = 70, satMin = 110, valMin = 140;
    int   minArea       = 14, maxArea = 12000;
    float yBias         = 0.30f;
    float smooth        = 14.0f;
    float maxSpeed      = 55.0f;
    float minSpeed      = 0.8f;
    float sens          = 1.0f;
    float predictMs     = 0.0f;
    int   triggerRadius = 3;
    int   triggerDelayMs= 45;
    int   fireHoldMs    = 45;
    int   cooldownMs    = 90;
    int   lockRadius    = 28;
} cfg;

static float iniF(const wchar_t* k, float d) {
    wchar_t b[64]; GetPrivateProfileStringW(L"main", k, L"", b, 64, L"config.ini");
    return b[0] ? (float)_wtof(b) : d;
}
static int iniI(const wchar_t* k, int d) {
    return GetPrivateProfileIntW(L"main", k, d, L"config.ini");
}
static void loadConfig() {
    cfg.fov            = iniI(L"fov", cfg.fov);
    cfg.drawOverlay    = iniI(L"overlay", 1) != 0;
    cfg.drawFov        = iniI(L"draw_fov", 1) != 0;
    cfg.aimKey         = iniI(L"aim_key", cfg.aimKey);
    cfg.triggerKey     = iniI(L"trigger_key", cfg.triggerKey);
    cfg.hueMin         = iniI(L"hue_min", cfg.hueMin);
    cfg.hueMax         = iniI(L"hue_max", cfg.hueMax);
    cfg.satMin         = iniI(L"sat_min", cfg.satMin);
    cfg.valMin         = iniI(L"val_min", cfg.valMin);
    cfg.minArea        = iniI(L"min_area", cfg.minArea);
    cfg.maxArea        = iniI(L"max_area", cfg.maxArea);
    cfg.triggerRadius  = iniI(L"trigger_radius", cfg.triggerRadius);
    cfg.triggerDelayMs = iniI(L"trigger_delay_ms", cfg.triggerDelayMs);
    cfg.fireHoldMs     = iniI(L"fire_hold_ms", cfg.fireHoldMs);
    cfg.cooldownMs     = iniI(L"cooldown_ms", cfg.cooldownMs);
    cfg.lockRadius     = iniI(L"lock_radius", cfg.lockRadius);
    cfg.yBias          = iniF(L"y_bias", cfg.yBias);
    cfg.smooth         = iniF(L"smooth", cfg.smooth);
    cfg.maxSpeed       = iniF(L"max_speed", cfg.maxSpeed);
    cfg.minSpeed       = iniF(L"min_speed", cfg.minSpeed);
    cfg.sens           = iniF(L"sens", cfg.sens);
    cfg.predictMs      = iniF(L"predict_ms", cfg.predictMs);
}

// ------------------------------------------------------------------ globals
static HWND   g_game = nullptr;
static HWND   g_ovl  = nullptr;
static HDC    g_ovlDC = nullptr;
static HBITMAP g_ovlBmp = nullptr;
static void*  g_ovlBits = nullptr;
static Bitmap* g_ovlGp = nullptr;
static int    g_ow = 0, g_oh = 0;

static HDC    g_roiDC = nullptr;
static HBITMAP g_roiBmp = nullptr;
static uint32_t* g_roiBits = nullptr;
static int    g_rw = 0, g_rh = 0;

static RECT   g_client{};
static int    g_roiX = 0, g_roiY = 0;
static std::vector<uint8_t> g_mask;
static std::vector<int>     g_label;
static bool   g_run = true;

struct Blob { int x0, y0, x1, y1, area; float cx, cy; };
static bool   g_haveTarget = false;
static float  g_lastCx = 0, g_lastCy = 0;
static float  g_velX = 0, g_velY = 0;
static float  g_accX = 0, g_accY = 0;
static DWORD  g_onSince = 0;
static DWORD  g_nextFire = 0, g_fireUntil = 0;

// ------------------------------------------------------------------ capture
static bool findGame() {
    g_game = nullptr;
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        wchar_t cls[64]{};
        GetClassNameW(h, cls, 64);
        if (!lstrcmpW(cls, L"UnrealWindow") && IsWindowVisible(h)) { g_game = h; return FALSE; }
        return TRUE;
    }, 0);
    return g_game != nullptr;
}

static bool refreshGeometry() {
    if (!g_game || !IsWindow(g_game)) return false;
    if (GetForegroundWindow() != g_game) return false;
    RECT c{}; POINT o{ 0,0 };
    GetClientRect(g_game, &c);
    ClientToScreen(g_game, &o);
    RECT n{ o.x, o.y, o.x + c.right, o.y + c.bottom };
    if (!EqualRect(&n, &g_client)) g_client = n;

    int cx = g_client.left + (g_client.right - g_client.left) / 2;
    int cy = g_client.top + (g_client.bottom - g_client.top) / 2;
    int x = cx - cfg.fov, y = cy - cfg.fov, s = cfg.fov * 2;

    if (s != g_rw) {
        if (g_roiBmp) { DeleteObject(g_roiBmp); g_roiBmp = nullptr; }
        if (g_roiDC) { DeleteDC(g_roiDC); g_roiDC = nullptr; }
        BITMAPV5HEADER bi{};
        bi.bV5Size = sizeof(bi); bi.bV5Width = s; bi.bV5Height = -s; bi.bV5Planes = 1;
        bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
        bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00;
        bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
        HDC dc = GetDC(nullptr);
        g_roiBmp = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, (void**)&g_roiBits, nullptr, 0);
        ReleaseDC(nullptr, dc);
        if (!g_roiBmp) return false;
        g_roiDC = CreateCompatibleDC(nullptr);
        SelectObject(g_roiDC, g_roiBmp);
        g_rw = g_rh = s;
        g_mask.assign((size_t)s * s, 0);
        g_label.assign((size_t)s * s, -1);
    }
    g_roiX = x; g_roiY = y;
    return true;
}

static bool grab() {
    HDC dc = GetDC(nullptr);
    BOOL ok = BitBlt(g_roiDC, 0, 0, g_rw, g_rh, dc, g_roiX, g_roiY, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, dc);
    return ok != 0;
}

// ------------------------------------------------------------------ detect
static inline void rgb2hsv(int r, int g, int b, float& h, float& s, float& v) {
    int mx = max(r, max(g, b)), mn = min(r, min(g, b));
    v = mx / 255.0f;
    s = mx ? (mx - mn) / (float)mx : 0.0f;
    if (!mx) { h = 0; return; }
    float d = (float)(mx - mn);
    if (mx == r)      h = fmodf((g - b) / d, 6.0f) * 60.0f;
    else if (mx == g) h = ((b - r) / d + 2.0f) * 60.0f;
    else              h = ((r - g) / d + 4.0f) * 60.0f;
    if (h < 0) h += 360.0f;
}

static int detect(std::vector<Blob>& out) {
    out.clear();
    const int n = g_rw * g_rh;
    const float sMin = cfg.satMin / 255.0f, vMin = cfg.valMin / 255.0f;
    const bool wrap = cfg.hueMin > cfg.hueMax;

    for (int i = 0; i < n; ++i) {
        uint32_t px = g_roiBits[i];
        int b = px & 0xFF, g = (px >> 8) & 0xFF, r = (px >> 16) & 0xFF;
        float h, s, v; rgb2hsv(r, g, b, h, s, v);
        bool ok = s >= sMin && v >= vMin &&
                  (wrap ? (h >= cfg.hueMin || h <= cfg.hueMax)
                        : (h >= cfg.hueMin && h <= cfg.hueMax));
        g_mask[i] = ok ? 1 : 0;
        g_label[i] = -1;
    }

    std::vector<int> stack;
    int id = 0;
    for (int i = 0; i < n; ++i) {
        if (!g_mask[i] || g_label[i] >= 0) continue;
        Blob bl{ i % g_rw, i / g_rw, i % g_rw, i / g_rw, 0, 0.0f, 0.0f };
        double sx = 0, sy = 0;
        stack.clear(); stack.push_back(i); g_label[i] = id;
        while (!stack.empty()) {
            int p = stack.back(); stack.pop_back();
            int px = p % g_rw, py = p / g_rw;
            bl.x0 = min(bl.x0, px); bl.x1 = max(bl.x1, px);
            bl.y0 = min(bl.y0, py); bl.y1 = max(bl.y1, py);
            bl.area++; sx += px; sy += py;
            const int dx[4] = { 1,-1,0,0 }, dy[4] = { 0,0,1,-1 };
            for (int k = 0; k < 4; ++k) {
                int nx = px + dx[k], ny = py + dy[k];
                if (nx < 0 || ny < 0 || nx >= g_rw || ny >= g_rh) continue;
                int q = ny * g_rw + nx;
                if (g_mask[q] && g_label[q] < 0) { g_label[q] = id; stack.push_back(q); }
            }
        }
        ++id;
        int w = bl.x1 - bl.x0 + 1, h = bl.y1 - bl.y0 + 1;
        if (bl.area < cfg.minArea || bl.area > cfg.maxArea) continue;
        float ar = w / (float)h;
        if (ar < 0.15f || ar > 8.0f) continue;
        if (bl.area / (float)(w * h) < 0.15f) continue;
        bl.cx = (float)(sx / bl.area); bl.cy = (float)(sy / bl.area);
        out.push_back(bl);
    }
    return (int)out.size();
}

// ------------------------------------------------------------------ input
static void moveMouse(float dx, float dy) {
    g_accX += dx; g_accY += dy;
    int ix = (int)g_accX, iy = (int)g_accY;
    if (!ix && !iy) return;
    g_accX -= ix; g_accY -= iy;
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dx = ix; in.mi.dy = iy; in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}
static void click(bool down) {
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(in));
}

// ------------------------------------------------------------------ overlay
static void destroyOverlay() {
    delete g_ovlGp; g_ovlGp = nullptr;
    if (g_ovlBmp) { DeleteObject(g_ovlBmp); g_ovlBmp = nullptr; }
    if (g_ovlDC) { DeleteDC(g_ovlDC); g_ovlDC = nullptr; }
    if (g_ovl) { DestroyWindow(g_ovl); g_ovl = nullptr; }
    g_ow = g_oh = 0;
}

static bool ensureOverlay(int w, int h) {
    if (g_ovl && w == g_ow && h == g_oh) return true;
    destroyOverlay();
    if (w <= 0 || h <= 0) return false;

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"vcbo";
    RegisterClassExW(&wc);

    g_ovl = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"vcbo", L"", WS_POPUP, 0, 0, w, h, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_ovl) return false;

    BITMAPV5HEADER bi{};
    bi.bV5Size = sizeof(bi); bi.bV5Width = w; bi.bV5Height = -h; bi.bV5Planes = 1;
    bi.bV5BitCount = 32; bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000; bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF; bi.bV5AlphaMask = 0xFF000000;
    HDC dc = GetDC(nullptr);
    g_ovlBmp = CreateDIBSection(dc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &g_ovlBits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    g_ovlDC = CreateCompatibleDC(nullptr);
    SelectObject(g_ovlDC, g_ovlBmp);
    g_ovlGp = new Bitmap(w, h, w * 4, PixelFormat32bppPARGB, (BYTE*)g_ovlBits);
    g_ow = w; g_oh = h;

    ShowWindow(g_ovl, SW_SHOWNOACTIVATE);
    SetWindowPos(g_ovl, HWND_TOPMOST, g_client.left, g_client.top, w, h,
                 SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    return true;
}

static void paintOverlay(const std::vector<Blob>& blobs, int best, bool aimOn, bool trigOn, float ms) {
    if (!cfg.drawOverlay || !g_ovlGp) return;
    Graphics g(g_ovlGp);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    SolidBrush clear(Color(0, 0, 0, 0));
    g.FillRectangle(&clear, 0, 0, g_ow, g_oh);

    int ox = g_roiX - g_client.left, oy = g_roiY - g_client.top;
    Pen box(Color(200, 60, 220, 255), 1.4f), tgt(Color(255, 0, 255, 90), 1.6f);
    SolidBrush fill(Color(40, 0, 200, 255));

    for (size_t i = 0; i < blobs.size(); ++i) {
        const Blob& b = blobs[i];
        if ((int)i == best) {
            g.DrawRectangle(&tgt, ox + b.x0 - 2, oy + b.y0 - 2,
                            b.x1 - b.x0 + 4, b.y1 - b.y0 + 4);
            g.FillEllipse(&fill, ox + b.cx - 2, oy + b.cy - 2, 4, 4);
        }
        else g.DrawRectangle(&box, ox + b.x0, oy + b.y0, b.x1 - b.x0, b.y1 - b.y0);
    }

    if (cfg.drawFov) {
        Pen fov(Color(90, 255, 255, 255), 1.0f);
        g.DrawEllipse(&fov, g_ow / 2 - cfg.fov, g_oh / 2 - cfg.fov, cfg.fov * 2, cfg.fov * 2);
    }

    wchar_t txt[160];
    swprintf_s(txt, L"%.2fms  blobs:%zu  aim:%s  trig:%s  [END=quit]",
               ms, blobs.size(), aimOn ? L"ON" : L"-", trigOn ? L"ON" : L"-");
    Font f(L"Consolas", 12.0f, FontStyleRegular, UnitPixel);
    SolidBrush tx(Color(230, 255, 255, 255));
    g.DrawString(txt, -1, &f, PointF(12, 12), &tx);

    POINT dst{ g_client.left, g_client.top }, src{ 0,0 };
    SIZE sz{ g_ow, g_oh };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_ovl, nullptr, &dst, &sz, g_ovlDC, &src, 0, &bf, ULW_ALPHA);
}

// ------------------------------------------------------------------ frame
static void frame(float dt) {
    if (!refreshGeometry()) return;
    if (!ensureOverlay(g_client.right - g_client.left, g_client.bottom - g_client.top)) return;
    if (!grab()) return;

    LARGE_INTEGER a, b, fq; QueryPerformanceCounter(&a);
    std::vector<Blob> blobs;
    detect(blobs);
    QueryPerformanceCounter(&b);
    QueryPerformanceFrequency(&fq);
    float ms = float((b.QuadPart - a.QuadPart) * 1000.0 / fq.QuadPart);

    const float ccx = g_rw * 0.5f, ccy = g_rh * 0.5f;
    const bool aimOn = (GetAsyncKeyState(cfg.aimKey) & 0x8000) != 0;
    const bool trigOn = (GetAsyncKeyState(cfg.triggerKey) & 0x8000) != 0;

    int best = -1; float bestScore = 1e9f;
    for (size_t i = 0; i < blobs.size(); ++i) {
        float dx = blobs[i].cx - ccx, dy = blobs[i].cy - ccy;
        if (dx * dx + dy * dy > (float)cfg.fov * cfg.fov) continue;
        float s = sqrtf(dx * dx + dy * dy) - min(blobs[i].area, 400) * 0.05f;
        if (g_haveTarget) {
            float lx = blobs[i].cx - g_lastCx, ly = blobs[i].cy - g_lastCy;
            if (lx * lx + ly * ly < (float)cfg.lockRadius * cfg.lockRadius) s -= 40.0f;
        }
        if (s < bestScore) { bestScore = s; best = (int)i; }
    }

    if (best >= 0) {
        const Blob& t = blobs[best];
        float ax = t.cx, ay = t.y0 + (t.y1 - t.y0) * cfg.yBias;
        if (g_haveTarget) {
            float vx = (t.cx - g_lastCx) / max(dt, 0.001f);
            float vy = (t.cy - g_lastCy) / max(dt, 0.001f);
            g_velX = g_velX * 0.7f + vx * 0.3f;
            g_velY = g_velY * 0.7f + vy * 0.3f;
            float ext = cfg.predictMs / 1000.0f;
            ax += std::clamp(g_velX * ext, -60.0f, 60.0f);
            ay += std::clamp(g_velY * ext, -60.0f, 60.0f);
        }
        g_lastCx = t.cx; g_lastCy = t.cy; g_haveTarget = true;

        if (aimOn) {
            float dx = (ax - ccx) * cfg.sens, dy = (ay - ccy) * cfg.sens;
            float k = 1.0f - expf(-cfg.smooth * dt);
            float mx = dx * k, my = dy * k;
            float len = sqrtf(mx * mx + my * my);
            float cap = cfg.maxSpeed * dt * 60.0f;
            if (len > cap) { mx *= cap / len; my *= cap / len; }
            else if (len < cfg.minSpeed && len > 0.01f) { mx *= cfg.minSpeed / len; my *= cfg.minSpeed / len; }
            moveMouse(mx, my);
        }
    } else { g_haveTarget = false; g_velX = g_velY = 0; }

    bool onTarget = false;
    for (int y = -cfg.triggerRadius; y <= cfg.triggerRadius && !onTarget; ++y)
        for (int x = -cfg.triggerRadius; x <= cfg.triggerRadius; ++x) {
            int px = (int)ccx + x, py = (int)ccy + y;
            if (px < 0 || py < 0 || px >= g_rw || py >= g_rh) continue;
            if (g_mask[py * g_rw + px]) { onTarget = true; break; }
        }

    DWORD now = GetTickCount();
    if (onTarget) { if (!g_onSince) g_onSince = now; } else g_onSince = 0;

    if (trigOn && g_onSince && now - g_onSince >= (DWORD)cfg.triggerDelayMs && now >= g_nextFire) {
        click(true);
        g_fireUntil = now + cfg.fireHoldMs;
        g_nextFire  = now + cfg.fireHoldMs + cfg.cooldownMs;
    }
    if (g_fireUntil && (now >= g_fireUntil || !trigOn)) { click(false); g_fireUntil = 0; }

    paintOverlay(blobs, best, aimOn, trigOn, ms);
}

// ------------------------------------------------------------------ entry
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS);
    timeBeginPeriod(1);
    GdiplusStartupInput gi; ULONG_PTR tok;
    GdiplusStartup(&tok, &gi, nullptr);
    loadConfig();

    LARGE_INTEGER freq, prev; QueryPerformanceFrequency(&freq); QueryPerformanceCounter(&prev);

    while (g_run) {
        if (GetAsyncKeyState(VK_END) & 0x8000) break;

        if (!g_game || !IsWindow(g_game)) {
            if (!findGame()) { Sleep(250); continue; }
        }

        LARGE_INTEGER now; QueryPerformanceCounter(&now);
        float dt = float(now.QuadPart - prev.QuadPart) / freq.QuadPart;
        prev = now;
        dt = std::clamp(dt, 0.0005f, 0.05f);

        if (IsWindow(g_game) && GetForegroundWindow() == g_game) frame(dt);
        else if (g_ovl) ShowWindow(g_ovl, SW_HIDE);

        if (g_ovl && GetForegroundWindow() == g_game) ShowWindow(g_ovl, SW_SHOWNOACTIVATE);
        Sleep(2);
    }

    click(false);
    destroyOverlay();
    if (g_roiBmp) DeleteObject(g_roiBmp);
    if (g_roiDC) DeleteDC(g_roiDC);
    timeEndPeriod(1);
    GdiplusShutdown(tok);
    return 0;
}