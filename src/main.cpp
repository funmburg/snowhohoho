// Снегопад (Snegopad) — снег поверх рабочего стола Windows.
// Win32 + Direct2D + DirectComposition: рисует видеокарта, окно прозрачно для кликов.

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <wtsapi32.h>
#include <winhttp.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dcomp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

// ---------------------------------------------------------------------------
// Константы и настройки
// ---------------------------------------------------------------------------

static const wchar_t* kAppName  = L"Snegopad";
static const wchar_t* kTitle    = L"Снегопад";
static const wchar_t* kRegKey   = L"Software\\Snegopad";
#ifndef SNEG_VERSION
#define SNEG_VERSION "0.0.0"
#endif
#define SNEG_WIDE2(x) L##x
#define SNEG_WIDE(x) SNEG_WIDE2(x)
static const wchar_t* kVersion  = SNEG_WIDE(SNEG_VERSION);       // подставляется при сборке релиза
static const wchar_t* kRepoPath = L"/funmburg/snowhohoho";
static const wchar_t* kRunKey   = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

enum : UINT {
    WM_APP_TRAY = WM_APP + 1,
    WM_APP_GRAB,
    WM_APP_RELEASE,
    WM_APP_HOOK,
    WM_APP_UPDATE,      // wParam: 0 обновлений нет, 1 скачано, 2 ошибка; lParam: проверку запустили вручную
};

enum : UINT {
    ID_PAUSE = 1, ID_D0, ID_D1, ID_D2, ID_GARL, ID_BALLS, ID_PLOW, ID_PLOWNOW,
    ID_SHAKE, ID_CLEARBALLS, ID_FPS30, ID_PFS, ID_AUTO, ID_EXIT, ID_M0, ID_M1, ID_M2, ID_WIDGETS, ID_AUTOUPD, ID_CHECKUPD,
};

struct Settings {
    int  density = 1;            // 0 мало, 1 средне, 2 много
    bool garlands = true;
    bool balls = true;
    bool plow = true;
    bool fps30 = false;
    bool pauseFullscreen = true;
    bool paused = false;
    bool autoUpdate = true;      // обновляться автоматически
    bool widgets = true;         // сугробы на виджетах Rainmeter
    int  maxMode = 1;            // развёрнутое окно: 0 обычный снег, 1 меньше, 2 без снега
};

// ---------------------------------------------------------------------------
// Глобальное состояние
// ---------------------------------------------------------------------------

struct Gfx {
    ComPtr<ID3D11Device>          d3d;
    ComPtr<IDXGISwapChain1>       swap;
    ComPtr<ID2D1Factory1>         factory;
    ComPtr<ID2D1Device>           d2dDev;
    ComPtr<ID2D1DeviceContext>    dc;
    ComPtr<ID2D1Bitmap1>          target;
    ComPtr<IDCompositionDevice>   dcomp;
    ComPtr<IDCompositionTarget>   dtarget;
    ComPtr<IDCompositionVisual>   visual;
    ComPtr<ID2D1SolidColorBrush>  brush;
};

struct Surface {
    RECT rc{};
    bool taskbar = false;
    bool widget = false;                        // виджет Rainmeter: без гирлянд
    bool init = false;
    bool alive = false;
    bool dirty = true;
    float seed = 0;
    double lastBuild = 0;
    std::vector<float> h;                       // высота сугроба по столбцам
    std::vector<std::pair<int, int>> segs;      // видимые участки верхнего края
    ComPtr<ID2D1PathGeometry1> geo;
};

struct Seg { float x0, x1, y; Surface* s; };

struct Flake {
    float x = 0, y = 0, vy = 0, amp = 0, phase = 0, phaseSpeed = 0, r = 1, alpha = 1;
    float vis = 1;               // плавное скрытие над развёрнутым окном
    bool close = false;
    bool keep = false;           // остаётся в режиме «меньше снега»
};

struct Particle { float x, y, vx, vy, life, maxLife, r; };

struct Body {                    // снежок или стопка снежков (3 = снеговик)
    float x = 0, base = 0;       // base — нижняя точка
    float vx = 0, vy = 0;
    float appear = 0;            // 0..1 анимация появления
    std::vector<float> radii;
};

struct Plow {
    bool active = false;
    float x = 0;
    int dir = 1;
    HWND surf = nullptr;
    float cooldown = 40.0f;
};

struct WinInfo { HWND h; RECT rc; bool surface; bool taskbar; bool widget; };

static HINSTANCE g_inst;
static HANDLE g_mutex;
static std::atomic<bool> g_updating{false};
static std::wstring g_newTag;
static HWND g_overlay, g_msgWnd;
static UINT WM_TASKBARCREATED;
static Settings g_set;
static Gfx G;
static bool g_gfxOk = false;
static bool g_needReinit = true;
static bool g_running = true;
static bool g_active = false;
static bool g_locked = false;
static int g_vx, g_vy, g_vw, g_vh;          // виртуальный экран
static float g_scale = 1.0f;
static float g_cw = 3.0f;                   // ширина столбца сугроба
static double g_time = 0, g_last = 0;
static float g_scanTimer = 0, g_topTimer = 0;
static HWND g_lastFg; static RECT g_lastFgRc;

static std::unordered_map<HWND, Surface> g_surf;
static std::vector<Seg> g_segs;
static std::vector<WinInfo> g_wins;
static std::vector<Flake> g_flakes;
static std::vector<Particle> g_parts;
static std::vector<Body> g_bodies;
static Plow g_plow;
static std::vector<RECT> g_covered;         // мониторы, занятые развёрнутым окном
static std::vector<HMONITOR> g_coveredMon;
static bool g_allCovered = false;
static bool g_taskView = false;             // открыто переключение окон: только падающий снег
static int g_dragIdx = -1;
static float g_grabDx = 0, g_grabDy = 0;
static int g_hoverIdx = -1;

static DWORD g_hookThreadId = 0;
static HANDLE g_hookThread = nullptr;
static HHOOK g_hook = nullptr;
static bool g_hookOn = false;
static std::atomic<bool> g_hot{false};
static std::atomic<bool> g_dragging{false};

// ---------------------------------------------------------------------------
// Мелочи
// ---------------------------------------------------------------------------

static uint32_t g_rng = 0x9E3779B9u;
static inline uint32_t Rnd() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static inline float Frand() { return (float)(Rnd() >> 8) * (1.0f / 16777216.0f); }
static inline float Frand(float a, float b) { return a + (b - a) * Frand(); }
static inline float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }

static double Now() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

// ---------------------------------------------------------------------------
// Реестр: настройки и автозапуск
// ---------------------------------------------------------------------------

static DWORD RegReadDword(const wchar_t* name, DWORD def) {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, name, RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS) return v;
    return def;
}
static void RegWriteDword(const wchar_t* name, DWORD v) {
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, name, REG_DWORD, &v, sizeof(v));
}
static void LoadSettings() {
    g_set.density = (int)std::min<DWORD>(2, RegReadDword(L"Density", 1));
    g_set.garlands = RegReadDword(L"Garlands", 1) != 0;
    g_set.balls = RegReadDword(L"Balls", 1) != 0;
    g_set.plow = RegReadDword(L"Plow", 1) != 0;
    g_set.fps30 = RegReadDword(L"Fps30", 0) != 0;
    g_set.pauseFullscreen = RegReadDword(L"PauseFullscreen", 1) != 0;
    g_set.maxMode = (int)std::min<DWORD>(2, RegReadDword(L"MaxMode", 1));
    g_set.widgets = RegReadDword(L"Widgets", 1) != 0;
    g_set.autoUpdate = RegReadDword(L"AutoUpdate", 1) != 0;
}
static void SaveSettings() {
    RegWriteDword(L"Density", (DWORD)g_set.density);
    RegWriteDword(L"Garlands", g_set.garlands);
    RegWriteDword(L"Balls", g_set.balls);
    RegWriteDword(L"Plow", g_set.plow);
    RegWriteDword(L"Fps30", g_set.fps30);
    RegWriteDword(L"PauseFullscreen", g_set.pauseFullscreen);
    RegWriteDword(L"MaxMode", (DWORD)g_set.maxMode);
    RegWriteDword(L"Widgets", g_set.widgets);
    RegWriteDword(L"AutoUpdate", g_set.autoUpdate);
}
static bool AutostartEnabled() {
    DWORD sz = 0;
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, &sz) == ERROR_SUCCESS;
}
static void SetAutostart(bool on) {
    if (on) {
        wchar_t path[MAX_PATH + 2] = L"\"";
        DWORD n = GetModuleFileNameW(nullptr, path + 1, MAX_PATH - 1);
        if (!n) return;
        path[n + 1] = L'"'; path[n + 2] = 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName, REG_SZ, path, (DWORD)((n + 3) * sizeof(wchar_t)));
    } else {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kAppName);
    }
}

// ---------------------------------------------------------------------------
// Графика
// ---------------------------------------------------------------------------

static bool CreateGfx(HWND hwnd, int w, int h) {
    G = Gfx();
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   nullptr, 0, D3D11_SDK_VERSION, &G.d3d, nullptr, nullptr);
    if (FAILED(hr))
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                               nullptr, 0, D3D11_SDK_VERSION, &G.d3d, nullptr, nullptr);
    if (FAILED(hr)) return false;

    ComPtr<IDXGIDevice> dxgiDev;
    if (FAILED(G.d3d.As(&dxgiDev))) return false;

    ComPtr<IDXGIFactory2> dxgiFactory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&dxgiFactory)))) return false;

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = (UINT)std::max(1, w);
    desc.Height = (UINT)std::max(1, h);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(dxgiFactory->CreateSwapChainForComposition(dxgiDev.Get(), &desc, nullptr, &G.swap))) return false;

    D2D1_FACTORY_OPTIONS opts = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &opts,
                                 reinterpret_cast<void**>(G.factory.GetAddressOf())))) return false;
    if (FAILED(G.factory->CreateDevice(dxgiDev.Get(), &G.d2dDev))) return false;
    if (FAILED(G.d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &G.dc))) return false;

    ComPtr<IDXGISurface> surface;
    if (FAILED(G.swap->GetBuffer(0, IID_PPV_ARGS(&surface)))) return false;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(G.dc->CreateBitmapFromDxgiSurface(surface.Get(), &props, &G.target))) return false;
    G.dc->SetTarget(G.target.Get());
    G.dc->SetDpi(96.0f, 96.0f);
    if (FAILED(G.dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &G.brush))) return false;

    if (FAILED(DCompositionCreateDevice(dxgiDev.Get(), IID_PPV_ARGS(&G.dcomp)))) return false;
    if (FAILED(G.dcomp->CreateTargetForHwnd(hwnd, TRUE, &G.dtarget))) return false;
    if (FAILED(G.dcomp->CreateVisual(&G.visual))) return false;
    G.visual->SetContent(G.swap.Get());
    G.dtarget->SetRoot(G.visual.Get());
    if (FAILED(G.dcomp->Commit())) return false;
    return true;
}

static inline void Col(float r, float g, float b, float a = 1.0f) { G.brush->SetColor(D2D1::ColorF(r, g, b, a)); }
static inline void FillCircle(float x, float y, float r) {
    G.dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), G.brush.Get());
}
static inline void StrokeCircle(float x, float y, float r, float w) {
    G.dc->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), r, r), G.brush.Get(), w);
}
static inline void FillBox(float l, float t, float r, float b, float rad = 0.0f) {
    if (rad > 0) G.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l, t, r, b), rad, rad), G.brush.Get());
    else G.dc->FillRectangle(D2D1::RectF(l, t, r, b), G.brush.Get());
}
static inline void DrawSeg(float x0, float y0, float x1, float y1, float w) {
    G.dc->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), G.brush.Get(), w);
}

// ---------------------------------------------------------------------------
// Снежинки и частицы
// ---------------------------------------------------------------------------

static void SpawnFlake(Flake& f, bool anywhere) {
    f.x = Frand((float)g_vx, (float)(g_vx + g_vw));
    f.y = anywhere ? Frand((float)g_vy, (float)(g_vy + g_vh)) : (float)g_vy - Frand(4.0f, 60.0f) * g_scale;
    float depth = Frand();                              // 0 далеко, 1 близко
    f.close = depth > 0.5f;
    f.r = (0.9f + 2.1f * depth) * g_scale;
    f.vy = (34.0f + 80.0f * depth) * g_scale * Frand(0.85f, 1.15f);
    f.amp = Frand(8.0f, 26.0f) * g_scale;
    f.phase = Frand(0.0f, 6.283f);
    f.phaseSpeed = Frand(0.6f, 1.8f);
    f.alpha = 0.45f + 0.5f * depth;
}

static void InitFlakes() {
    static const float base[3] = {110.0f, 260.0f, 520.0f};
    float area = (float)g_vw * (float)g_vh / (g_scale * g_scale);
    int n = (int)(base[g_set.density] * area / (1920.0f * 1080.0f));
    n = std::max(40, std::min(2000, n));
    g_flakes.resize((size_t)n);
    for (size_t i = 0; i < g_flakes.size(); ++i) {
        SpawnFlake(g_flakes[i], true);
        g_flakes[i].keep = (i % 4) == 0;
        g_flakes[i].vis = 1.0f;
    }
}

static void AddParticle(float x, float y, float vx, float vy, float life, float r) {
    if (g_parts.size() > 600) return;
    g_parts.push_back({x, y, vx, vy, life, life, r});
}

static void Poof(float x, float y, int n, float power) {
    for (int i = 0; i < n; ++i) {
        float a = Frand(0.0f, 6.283f), sp = Frand(30.0f, 150.0f) * g_scale * power;
        AddParticle(x, y, cosf(a) * sp, sinf(a) * sp - 60.0f * g_scale, Frand(0.4f, 0.9f), Frand(1.2f, 2.6f) * g_scale);
    }
}

// ---------------------------------------------------------------------------
// Сугробы
// ---------------------------------------------------------------------------

static float MaxHeight(const Surface& s) { return (s.taskbar ? 16.0f : 10.0f) * g_scale; }

static float CapAt(const Surface& s, int j) {
    int n = (int)s.h.size();
    float cap = MaxHeight(s);
    cap *= 0.78f + 0.22f * sinf(j * 0.071f + s.seed) * sinf(j * 0.023f + s.seed * 2.0f);
    if (!s.taskbar) {
        int edge = std::min(j, n - 1 - j);
        cap *= std::min(1.0f, (edge + 0.5f) / 5.0f);     // закругление у углов окна
    }
    return cap;
}

static float HeightAt(const Surface& s, float x) {
    int c = (int)((x - (float)s.rc.left) / g_cw);
    if (c < 0 || c >= (int)s.h.size()) return 0.0f;
    return s.h[(size_t)c];
}

static void Deposit(Surface& s, float x, float amt) {
    static const float k[5] = {0.25f, 0.6f, 1.0f, 0.6f, 0.25f};
    int n = (int)s.h.size();
    int c = (int)((x - (float)s.rc.left) / g_cw);
    for (int i = -2; i <= 2; ++i) {
        int j = c + i;
        if (j < 0 || j >= n) continue;
        float cap = CapAt(s, j);
        float& hv = s.h[(size_t)j];
        if (hv < cap) hv = std::min(cap, hv + amt * k[i + 2]);
    }
    s.dirty = true;
}

static void Shed(Surface& s) {                 // снег осыпается, когда окно сдвинули
    int n = (int)s.h.size();
    int step = std::max(2, n / 70);
    for (int j = 0; j < n; j += step) {
        float hv = s.h[(size_t)j];
        if (hv < 1.5f) continue;
        float x = (float)s.rc.left + (j + 0.5f) * g_cw;
        AddParticle(x, (float)s.rc.top - hv * 0.5f, Frand(-25.0f, 25.0f) * g_scale, Frand(10.0f, 70.0f) * g_scale,
                    Frand(0.7f, 1.3f), Frand(1.3f, 2.6f) * g_scale);
    }
    std::fill(s.h.begin(), s.h.end(), 0.0f);
    s.dirty = true;
}

static void BuildGeo(Surface& s) {
    s.geo.Reset();
    s.dirty = false;
    s.lastBuild = g_time;
    if (s.segs.empty() || s.h.empty()) return;
    ComPtr<ID2D1PathGeometry1> geo;
    if (FAILED(G.factory->CreatePathGeometry(&geo))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geo->Open(&sink))) return;
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    static std::vector<D2D1_POINT_2F> pts;
    const float base = (float)s.rc.top + 1.0f;
    const int n = (int)s.h.size();
    bool any = false;
    for (auto& sg : s.segs) {
        float x0 = (float)sg.first, x1 = (float)sg.second;
        int c0 = std::max(0, std::min(n - 1, (int)((x0 - (float)s.rc.left) / g_cw)));
        int c1 = std::max(0, std::min(n - 1, (int)((x1 - (float)s.rc.left) / g_cw)));
        pts.clear();
        for (int c = c0; c <= c1; ++c) {
            float x = Clampf((float)s.rc.left + (c + 0.5f) * g_cw, x0, x1);
            pts.push_back(D2D1::Point2F(x, base - s.h[(size_t)c]));
        }
        pts.push_back(D2D1::Point2F(x1, base));
        sink->BeginFigure(D2D1::Point2F(x0, base), D2D1_FIGURE_BEGIN_FILLED);
        sink->AddLines(pts.data(), (UINT32)pts.size());
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
        any = true;
    }
    if (FAILED(sink->Close()) || !any) return;
    s.geo = geo;
}

// ---------------------------------------------------------------------------
// Поиск окон и панели задач
// ---------------------------------------------------------------------------

static BOOL CALLBACK EnumProc(HWND h, LPARAM) {
    if (h == g_overlay || h == g_msgWnd) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    // Виджеты Rainmeter — это окна-инструменты без заголовка, часто «прозрачные» для кликов
    const bool widget = g_set.widgets && !wcscmp(cls, L"RainmeterMeterWindow");
    if (!widget) {
        if (ex & WS_EX_TRANSPARENT) return TRUE;
        if (ex & WS_EX_LAYERED) {
            BYTE a = 255; DWORD fl = 0; COLORREF key = 0;
            if (GetLayeredWindowAttributes(h, &key, &a, &fl) && (fl & LWA_ALPHA) && a == 0) return TRUE;
        }
    }
    RECT rc;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &rc, sizeof(rc))))
        if (!GetWindowRect(h, &rc)) return TRUE;
    int w = rc.right - rc.left, ht = rc.bottom - rc.top;
    if (w < 8 || ht < 8) return TRUE;
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW")) return TRUE;

    WinInfo wi{h, rc, false, false, false};
    if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) {
        wi.surface = wi.taskbar = true;
    } else if (widget) {
        MONITORINFO mi{sizeof(mi)};
        if (w >= (int)(50 * g_scale) && ht >= (int)(20 * g_scale) &&
            GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi) &&
            rc.top > mi.rcMonitor.top + (int)(6 * g_scale))
            wi.surface = wi.widget = true;
    } else {
        wchar_t title[4];
        bool app = !(ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_NOACTIVATE) &&
                   InternalGetWindowText(h, title, 4) > 0 &&
                   w >= (int)(160 * g_scale) && ht >= (int)(80 * g_scale);
        if (app) {
            MONITORINFO mi{sizeof(mi)};
            if (GetMonitorInfoW(MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST), &mi) &&
                rc.top > mi.rcMonitor.top + (int)(6 * g_scale))        // не развёрнутое на весь экран
                wi.surface = true;
        }
    }
    g_wins.push_back(wi);
    return TRUE;
}

static void SubtractRange(std::vector<std::pair<int, int>>& segs, int a, int b) {
    static std::vector<std::pair<int, int>> out;
    out.clear();
    for (auto& s : segs) {
        if (b <= s.first || a >= s.second) { out.push_back(s); continue; }
        if (a > s.first) out.push_back({s.first, a});
        if (b < s.second) out.push_back({b, s.second});
    }
    segs = out;
}

static void Rescan() {
    g_wins.clear();
    EnumWindows(EnumProc, 0);
    for (auto& kv : g_surf) kv.second.alive = false;

    const int vL = g_vx, vR = g_vx + g_vw;
    const int minLen = (int)(28 * g_scale);
    std::vector<std::pair<int, int>> segs;
    for (size_t i = 0; i < g_wins.size(); ++i) {
        WinInfo& w = g_wins[i];
        if (!w.surface) continue;
        Surface& s = g_surf[w.h];
        if (!s.init || !EqualRect(&s.rc, &w.rc)) {
            if (s.init) Shed(s);
            s.rc = w.rc;
            s.taskbar = w.taskbar;
            s.widget = w.widget;
            int cols = (int)((float)(w.rc.right - w.rc.left) / g_cw) + 1;
            s.h.assign((size_t)std::max(1, cols), 0.0f);
            if (!s.init) s.seed = Frand(0.0f, 100.0f);
            s.init = true;
            s.dirty = true;
        }
        s.alive = true;

        segs.clear();
        segs.push_back({std::max<int>(w.rc.left, vL), std::min<int>(w.rc.right, vR)});
        const int probe = w.rc.top - 2;                 // точка чуть выше верхнего края
        for (size_t j = 0; j < i && !segs.empty(); ++j) {
            const RECT& o = g_wins[j].rc;               // окна выше по Z-порядку закрывают край
            if (o.top <= probe && o.bottom > probe) SubtractRange(segs, o.left, o.right);
        }
        segs.erase(std::remove_if(segs.begin(), segs.end(),
                                  [&](const std::pair<int, int>& p) { return p.second - p.first < minLen; }),
                   segs.end());
        if (g_taskView && !w.taskbar) segs.clear();     // окна сейчас показаны миниатюрами, сугробам лежать не на чем
        if (segs != s.segs) { s.segs = segs; s.dirty = true; }
    }

    for (auto it = g_surf.begin(); it != g_surf.end();) {
        if (!it->second.alive) it = g_surf.erase(it); else ++it;
    }
    g_segs.clear();
    for (auto& kv : g_surf)
        for (auto& sg : kv.second.segs)
            g_segs.push_back({(float)sg.first, (float)sg.second, (float)kv.second.rc.top, &kv.second});
}

static bool IsTaskViewClass(const wchar_t* cls) {
    return !wcscmp(cls, L"XamlExplorerHostIslandWindow") ||      // Windows 11: «Представление задач», Alt+Tab
           !wcscmp(cls, L"MultitaskingViewFrame") ||             // Windows 10
           !wcscmp(cls, L"TaskSwitcherWnd") || !wcscmp(cls, L"ForegroundStaging");
}

static bool IsExplorerProcess(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!pid) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    bool res = false;
    if (QueryFullProcessImageNameW(proc, 0, path, &n)) {
        const wchar_t* name = wcsrchr(path, L'\\');
        res = _wcsicmp(name ? name + 1 : path, L"explorer.exe") == 0;
    }
    CloseHandle(proc);
    return res;
}

// Окна самой оболочки Windows: рабочий стол, панель задач, переключение окон
static bool IsShellClass(HWND h) {
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") ||
        !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd") || IsTaskViewClass(cls))
        return true;
    if (!wcscmp(cls, L"CabinetWClass") || !wcscmp(cls, L"ExploreWClass")) return false;   // обычные окна Проводника
    return IsExplorerProcess(h);
}

static bool IsTaskView(HWND h) {
    if (!h) return false;
    wchar_t cls[64] = L"";
    GetClassNameW(h, cls, 64);
    return IsTaskViewClass(cls);
}

static bool ForegroundIsFullscreen() {
    HWND fg = GetForegroundWindow();
    if (!fg || fg == g_overlay || fg == g_msgWnd || IsShellClass(fg)) return false;
    RECT rc;
    if (!GetWindowRect(fg, &rc)) return false;
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return EqualRect(&rc, &mi.rcMonitor) != FALSE;       // игра или видео точно во весь экран
}

static BOOL CALLBACK CoveredProc(HWND h, LPARAM) {
    if (h == g_overlay || h == g_msgWnd) return TRUE;
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT)) return TRUE;
    wchar_t title[4];
    if (InternalGetWindowText(h, title, 4) <= 0 || IsShellClass(h)) return TRUE;
    RECT rc;
    if (!GetWindowRect(h, &rc)) return TRUE;
    HMONITOR mon = MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;
    const RECT& w = mi.rcWork;
    bool covers = IsZoomed(h) || (rc.left <= w.left && rc.top <= w.top && rc.right >= w.right && rc.bottom >= w.bottom);
    if (!covers) return TRUE;
    if (std::find(g_coveredMon.begin(), g_coveredMon.end(), mon) == g_coveredMon.end()) {
        g_coveredMon.push_back(mon);
        g_covered.push_back(mi.rcMonitor);
    }
    return TRUE;
}

// На каких мониторах окно развёрнуто на весь экран (рабочего стола там не видно)
static void ScanCovered() {
    g_covered.clear();
    g_coveredMon.clear();
    g_allCovered = false;
    if (g_set.maxMode == 0 || IsTaskView(GetForegroundWindow())) return;
    EnumWindows(CoveredProc, 0);
    g_allCovered = !g_covered.empty() && (int)g_covered.size() >= GetSystemMetrics(SM_CMONITORS);
}

// ---------------------------------------------------------------------------
// Снежки, снеговики
// ---------------------------------------------------------------------------

static const float kStack = 0.82f;                       // насколько шары «вдавлены» друг в друга

static float BodyHeight(const Body& b) {
    float h = 0;
    for (size_t i = 0; i < b.radii.size(); ++i)
        h += 2.0f * b.radii[i] * (i + 1 < b.radii.size() ? kStack : 1.0f);
    return h * b.appear;
}
static float BallCenterY(const Body& b, size_t i) {
    float off = 0;
    for (size_t k = 0; k < i; ++k) off += 2.0f * b.radii[k] * kStack;
    return b.base - (off + b.radii[i]) * b.appear;
}

static int HitBody(float px, float py, float pad) {
    for (int i = (int)g_bodies.size() - 1; i >= 0; --i) {
        const Body& b = g_bodies[(size_t)i];
        for (size_t k = 0; k < b.radii.size(); ++k) {
            float r = b.radii[k] * b.appear + pad;
            float dx = px - b.x, dy = py - BallCenterY(b, k);
            if (dx * dx + dy * dy <= r * r) return i;
        }
    }
    return -1;
}

static float SupportY(float x, float bottom) {
    float best = (float)(g_vy + g_vh);
    const float tol = 8.0f * g_scale;
    for (auto& sg : g_segs)
        if (x >= sg.x0 && x <= sg.x1 && sg.y >= bottom - tol && sg.y < best) best = sg.y;
    return best;
}

static void SetHook(bool on) {
    if (on == g_hookOn || !g_hookThreadId) return;
    g_hookOn = on;
    PostThreadMessageW(g_hookThreadId, WM_APP_HOOK, on ? 1 : 0, 0);
}

static void DropDrag() {
    g_dragIdx = -1;
    g_dragging = false;
}

// Курсор над сугробом? pad > 0 — «рядом с сугробом» (чтобы заранее включить перехват мыши)
static bool SnowHit(float px, float py, float pad) {
    for (auto& sg : g_segs) {
        if (px < sg.x0 - pad || px > sg.x1 + pad) continue;
        float h = HeightAt(*sg.s, Clampf(px, sg.x0, sg.x1));
        if (pad <= 0 && h < 4.0f * g_scale) continue;
        float top = sg.y - (pad > 0 ? MaxHeight(*sg.s) : h);
        if (py >= top - 2.0f * g_scale - pad && py <= sg.y + 1.0f + pad) return true;
    }
    return false;
}

static const size_t kMaxBodies = 12;

static void OnReleaseDrag();

static void UpdateBodies(float dt) {
    POINT cp{};
    GetCursorPos(&cp);

    if (!g_set.balls) {
        if (!g_bodies.empty()) { g_bodies.clear(); DropDrag(); }
        g_hot = false; g_hoverIdx = -1;
        SetHook(false);
        return;
    }
    if (g_taskView) {                               // снежки замирают и прячутся
        OnReleaseDrag();
        g_hot = false; g_hoverIdx = -1;
        SetHook(false);
        return;
    }

    const float grav = 1900.0f * g_scale;
    const float vL = (float)g_vx, vR = (float)(g_vx + g_vw);
    int mergeA = -1, mergeB = -1;

    for (int i = 0; i < (int)g_bodies.size(); ++i) {
        Body& b = g_bodies[(size_t)i];
        if (b.appear < 1.0f) b.appear = std::min(1.0f, b.appear + dt * 2.2f);

        if (i == g_dragIdx) {
            float nx = (float)cp.x - g_grabDx, ny = (float)cp.y - g_grabDy;
            float ivx = (nx - b.x) / dt, ivy = (ny - b.base) / dt;
            b.vx += (ivx - b.vx) * 0.35f;
            b.vy += (ivy - b.vy) * 0.35f;
            b.x = nx; b.base = ny;
            // снежок катят по сугробу — он собирает снег и растёт
            if (b.radii.size() == 1) {
                float& r = b.radii[0];
                const float rmax = 26.0f * g_scale;
                for (auto& sg : g_segs) {
                    if (r >= rmax) break;
                    if (fabsf(b.base - sg.y) > 14.0f * g_scale) continue;
                    float xa = std::max(b.x - r, sg.x0), xb = std::min(b.x + r, sg.x1);
                    if (xa >= xb) continue;
                    Surface& sf = *sg.s;
                    int n = (int)sf.h.size();
                    int c0 = std::max(0, (int)((xa - (float)sf.rc.left) / g_cw));
                    int c1 = std::min(n - 1, (int)((xb - (float)sf.rc.left) / g_cw));
                    float sum = 0;
                    for (int c = c0; c <= c1; ++c) { sum += sf.h[(size_t)c]; sf.h[(size_t)c] = 0; }
                    if (sum > 0) {
                        sf.dirty = true;
                        r = std::min(rmax, sqrtf(r * r + sum * g_cw * 1.1f / 3.14159f));
                    }
                }
            }
            continue;
        }

        float sup = SupportY(b.x, b.base);
        if (b.base < sup - 0.5f || b.vy < 0) {
            float prev = b.base;
            b.vy += grav * dt;
            b.base += b.vy * dt;
            b.x += b.vx * dt;
            // приземление на другой снежок — слепляем
            if (b.radii.size() == 1 && b.vy > 0 && mergeA < 0) {
                for (int j = 0; j < (int)g_bodies.size(); ++j) {
                    if (j == i || j == g_dragIdx) continue;
                    Body& o = g_bodies[(size_t)j];
                    if (o.radii.size() >= 3 || o.appear < 1.0f) continue;
                    float topR = o.radii.back();
                    float contact = o.base - BodyHeight(o) + 2.0f * topR * (1.0f - kStack);
                    if (fabsf(b.x - o.x) < topR * 0.8f + b.radii[0] * 0.3f &&
                        prev <= contact + 4.0f * g_scale && b.base >= contact) {
                        mergeA = i; mergeB = j;
                        break;
                    }
                }
            }
            if (b.base >= sup) {
                b.base = sup;
                b.vy = (b.vy > 420.0f * g_scale) ? -b.vy * 0.22f : 0.0f;
            }
        } else {
            b.base = sup;
            b.vy = 0;
            b.x += b.vx * dt;
            b.vx *= expf(-5.0f * dt);
            if (fabsf(b.vx) < 3.0f) b.vx = 0;
        }
        float r0 = b.radii[0];
        if (b.x < vL + r0) { b.x = vL + r0; b.vx = fabsf(b.vx) * 0.4f; }
        if (b.x > vR - r0) { b.x = vR - r0; b.vx = -fabsf(b.vx) * 0.4f; }
    }

    if (mergeA >= 0) {
        Body& a = g_bodies[(size_t)mergeA];
        Body& o = g_bodies[(size_t)mergeB];
        o.radii.push_back(a.radii[0]);
        float topY = o.base - BodyHeight(o);
        bool snowman = o.radii.size() == 3;
        Poof(o.x, topY + a.radii[0], snowman ? 34 : 12, snowman ? 1.5f : 0.8f);
        g_bodies.erase(g_bodies.begin() + mergeA);
        if (g_dragIdx > mergeA) --g_dragIdx;
    }

    // наведение мыши: перехват кликов включаем только рядом со снежком
    g_hoverIdx = g_dragIdx >= 0 ? g_dragIdx : HitBody((float)cp.x, (float)cp.y, 2.0f * g_scale);
    bool canRoll = g_bodies.size() < kMaxBodies;
    g_hot = g_dragIdx < 0 && (g_hoverIdx >= 0 || (canRoll && SnowHit((float)cp.x, (float)cp.y, 0.0f)));
    bool nearBall = g_dragIdx >= 0 || g_dragging.load() || HitBody((float)cp.x, (float)cp.y, 40.0f * g_scale) >= 0 ||
                    (canRoll && SnowHit((float)cp.x, (float)cp.y, 24.0f * g_scale));
    SetHook(nearBall);
}

static void OnGrab(int px, int py) {
    if (!g_active || !g_set.balls) { g_dragging = false; return; }
    int i = HitBody((float)px, (float)py, 3.0f * g_scale);
    if (i < 0) {
        // клик по сугробу — начинаем катать новый снежок
        if (g_bodies.size() >= kMaxBodies || !SnowHit((float)px, (float)py, 0.0f)) { g_dragging = false; return; }
        Body nb;
        float r0 = 7.0f * g_scale;
        nb.x = (float)px;
        nb.base = (float)py + r0;
        nb.appear = 1.0f;
        nb.radii.push_back(r0);
        g_bodies.push_back(nb);
        i = (int)g_bodies.size() - 1;
    }
    g_dragIdx = i;
    Body& b = g_bodies[(size_t)i];
    g_grabDx = (float)px - b.x;
    g_grabDy = (float)py - b.base;
    b.vx = b.vy = 0;
}

static void OnRelease();
static void OnReleaseDrag() { OnRelease(); g_dragIdx = -1; g_dragging = false; }

static void OnRelease() {
    if (g_dragIdx >= 0 && g_dragIdx < (int)g_bodies.size()) {
        Body& b = g_bodies[(size_t)g_dragIdx];
        const float lim = 2600.0f * g_scale;
        b.vx = Clampf(b.vx, -lim, lim);
        b.vy = Clampf(b.vy, -lim, lim);
        if (b.radii.size() == 1 && b.radii[0] < 10.0f * g_scale) {      // слишком маленький комок рассыпается
            Poof(b.x, b.base - b.radii[0], 8, 0.6f);
            g_bodies.erase(g_bodies.begin() + g_dragIdx);
        }
    }
    g_dragIdx = -1;
}

// ---------------------------------------------------------------------------
// Снегоуборщик
// ---------------------------------------------------------------------------

static Surface* FindSurface(HWND h) {
    auto it = g_surf.find(h);
    return it == g_surf.end() ? nullptr : &it->second;
}

static void StartPlow() {
    if (g_plow.active) return;
    for (auto& kv : g_surf) {
        Surface& s = kv.second;
        if (!s.taskbar || s.segs.empty()) continue;
        g_plow.active = true;
        g_plow.surf = kv.first;
        g_plow.dir = (Rnd() & 1) ? 1 : -1;
        g_plow.x = g_plow.dir > 0 ? (float)s.rc.left - 50.0f * g_scale : (float)s.rc.right + 50.0f * g_scale;
        return;
    }
}

static void UpdatePlow(float dt) {
    if (!g_plow.active) {
        if (!g_set.plow) return;
        g_plow.cooldown -= dt;
        if (g_plow.cooldown > 0) return;
        g_plow.cooldown = 8.0f;
        for (auto& kv : g_surf) {                         // выезжает, когда панель задач завалило
            Surface& s = kv.second;
            if (!s.taskbar || s.h.empty() || s.segs.empty()) continue;
            float sum = 0;
            for (float v : s.h) sum += v;
            if (sum / (float)s.h.size() > MaxHeight(s) * 0.62f) { StartPlow(); break; }
        }
        return;
    }
    Surface* s = FindSurface(g_plow.surf);
    if (!s) { g_plow.active = false; return; }
    float prevBlade = g_plow.x + g_plow.dir * 22.0f * g_scale;
    g_plow.x += g_plow.dir * 125.0f * g_scale * dt;
    float blade = g_plow.x + g_plow.dir * 22.0f * g_scale;
    int c0 = (int)((std::min(prevBlade, blade) - (float)s->rc.left) / g_cw);
    int c1 = (int)((std::max(prevBlade, blade) - (float)s->rc.left) / g_cw);
    int n = (int)s->h.size();
    for (int c = std::max(0, c0); c <= c1 && c < n; ++c) {
        float& hv = s->h[(size_t)c];
        if (hv > 0.3f) {
            if (Frand() < 0.5f + hv / (20.0f * g_scale))
                AddParticle(blade, (float)s->rc.top - hv * 0.5f,
                            g_plow.dir * Frand(50.0f, 190.0f) * g_scale, -Frand(90.0f, 260.0f) * g_scale,
                            Frand(0.5f, 1.0f), Frand(1.2f, 2.8f) * g_scale);
            hv = 0;
            s->dirty = true;
        }
    }
    if ((g_plow.dir > 0 && g_plow.x > (float)s->rc.right + 60.0f * g_scale) ||
        (g_plow.dir < 0 && g_plow.x < (float)s->rc.left - 60.0f * g_scale)) {
        g_plow.active = false;
        g_plow.cooldown = 60.0f;
    }
}

// ---------------------------------------------------------------------------
// Обновление мира
// ---------------------------------------------------------------------------

static void Update(float dt) {
    const float wind = (24.0f * sinf((float)g_time * 0.13f) + 12.0f * sinf((float)g_time * 0.41f + 1.3f)) * g_scale;
    const float vL = (float)g_vx, vR = (float)(g_vx + g_vw), vB = (float)(g_vy + g_vh);

    for (auto& f : g_flakes) {
        float prevY = f.y;
        f.phase += f.phaseSpeed * dt;
        f.x += (wind * (0.4f + 0.6f * f.alpha) + sinf(f.phase) * f.amp) * dt;
        f.y += f.vy * dt;
        if (f.x < vL) f.x += (float)g_vw; else if (f.x >= vR) f.x -= (float)g_vw;
        if (f.y > vB + 6.0f) { SpawnFlake(f, false); continue; }
        float target = 1.0f;
        if (g_set.maxMode != 0 && !g_covered.empty()) {
            for (auto& rc : g_covered)
                if (f.x >= (float)rc.left && f.x < (float)rc.right && f.y >= (float)rc.top && f.y < (float)rc.bottom) {
                    target = (g_set.maxMode == 1 && f.keep) ? 1.0f : 0.0f;
                    break;
                }
        }
        if (f.vis != target) f.vis = Clampf(f.vis + (target > f.vis ? 2.5f : -2.5f) * dt, 0.0f, 1.0f);
        if (!f.close || f.vis < 0.5f) continue;
        for (auto& sg : g_segs) {
            if (f.x < sg.x0 || f.x > sg.x1) continue;
            float top = sg.y - HeightAt(*sg.s, f.x);
            if (prevY < top && f.y >= top) {
                Deposit(*sg.s, f.x, 0.55f * f.r);
                SpawnFlake(f, false);
                break;
            }
        }
    }

    const float pg = 520.0f * g_scale;
    for (size_t i = 0; i < g_parts.size();) {
        Particle& p = g_parts[i];
        p.life -= dt;
        if (p.life <= 0) { g_parts[i] = g_parts.back(); g_parts.pop_back(); continue; }
        p.vy += pg * dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        ++i;
    }

    UpdatePlow(dt);
    UpdateBodies(dt);
}

// ---------------------------------------------------------------------------
// Отрисовка
// ---------------------------------------------------------------------------

static void DrawGarlands() {
    static const float pal[5][3] = {
        {1.00f, 0.25f, 0.22f}, {1.00f, 0.80f, 0.20f}, {0.30f, 0.90f, 0.40f}, {0.30f, 0.60f, 1.00f}, {0.95f, 0.40f, 0.95f}};
    const float span = 66.0f * g_scale, sag = 7.0f * g_scale;
    const float t = (float)g_time;
    for (auto& sg : g_segs) {
        if (sg.s->taskbar || sg.s->widget) continue;
        float len = sg.x1 - sg.x0;
        if (len < 110.0f * g_scale) continue;
        int ns = std::max(1, (int)(len / span + 0.5f));
        float sw = len / (float)ns;
        float y0 = sg.y + 2.0f * g_scale;
        Col(0.08f, 0.22f, 0.12f, 0.9f);
        for (int k = 0; k < ns; ++k) {
            float xs = sg.x0 + k * sw;
            float px = xs, py = y0;
            for (int q = 1; q <= 6; ++q) {
                float u = q / 6.0f;
                float x = xs + sw * u, y = y0 + sag * 4.0f * u * (1.0f - u);
                DrawSeg(px, py, x, y, 1.2f * g_scale);
                px = x; py = y;
            }
        }
        int idx = (int)(sg.x0 * 0.01f);
        for (int k = 0; k < ns; ++k) {
            float xs = sg.x0 + k * sw;
            for (int q = 0; q < 3; ++q, ++idx) {
                float u = (1.0f + 2.0f * q) / 6.0f;
                float x = xs + sw * u, y = y0 + sag * 4.0f * u * (1.0f - u) + 2.5f * g_scale;
                const float* c = pal[((idx % 5) + 5) % 5];
                float br = 0.5f + 0.5f * sinf(t * 2.3f + idx * 1.9f);
                Col(c[0], c[1], c[2], 0.10f + 0.20f * br);
                FillCircle(x, y, 5.5f * g_scale);
                Col(c[0], c[1], c[2], 0.55f + 0.45f * br);
                FillCircle(x, y, 2.6f * g_scale);
            }
        }
    }
}

static void DrawBody(const Body& b, bool hover) {
    const float a = b.appear;
    for (size_t i = 0; i < b.radii.size(); ++i) {
        float r = b.radii[i] * a, cy = BallCenterY(b, i);
        if (hover) { Col(1, 1, 1, 0.45f); StrokeCircle(b.x, cy, r + 2.0f * g_scale, 2.0f * g_scale); }
        Col(0.80f, 0.86f, 0.95f);
        FillCircle(b.x, cy, r);
        Col(1, 1, 1);
        FillCircle(b.x - 0.10f * r, cy - 0.12f * r, r * 0.86f);
        Col(0.60f, 0.70f, 0.86f, 0.55f);
        StrokeCircle(b.x, cy, r, 1.0f * g_scale);
    }
    if (b.radii.size() < 3) return;

    // снеговик: руки, пуговицы, шарф, лицо, шляпа
    float rm = b.radii[1], cm = BallCenterY(b, 1);
    float rh = b.radii[2], ch = BallCenterY(b, 2);
    Col(0.42f, 0.28f, 0.16f);
    DrawSeg(b.x - 0.85f * rm, cm - 0.1f * rm, b.x - 1.75f * rm, cm - 0.75f * rm, 1.7f * g_scale);
    DrawSeg(b.x + 0.85f * rm, cm - 0.1f * rm, b.x + 1.75f * rm, cm - 0.75f * rm, 1.7f * g_scale);
    DrawSeg(b.x - 1.45f * rm, cm - 0.53f * rm, b.x - 1.6f * rm, cm - 0.25f * rm, 1.3f * g_scale);
    DrawSeg(b.x + 1.45f * rm, cm - 0.53f * rm, b.x + 1.6f * rm, cm - 0.25f * rm, 1.3f * g_scale);
    Col(0.13f, 0.14f, 0.18f);
    FillCircle(b.x, cm - 0.05f * rm, std::max(1.2f, 0.11f * rm));
    FillCircle(b.x, cm + 0.4f * rm, std::max(1.2f, 0.11f * rm));
    // шарф
    float sy = ch + 0.74f * rh;
    Col(0.86f, 0.16f, 0.20f);
    FillBox(b.x - 0.82f * rh, sy - 0.16f * rh, b.x + 0.82f * rh, sy + 0.18f * rh, 0.15f * rh);
    FillBox(b.x + 0.28f * rh, sy, b.x + 0.58f * rh, sy + 0.85f * rh, 0.08f * rh);
    // глаза и нос-морковка
    Col(0.10f, 0.11f, 0.14f);
    FillCircle(b.x - 0.32f * rh, ch - 0.15f * rh, std::max(1.2f, 0.12f * rh));
    FillCircle(b.x + 0.32f * rh, ch - 0.15f * rh, std::max(1.2f, 0.12f * rh));
    Col(1.0f, 0.52f, 0.10f);
    DrawSeg(b.x, ch + 0.10f * rh, b.x + 0.50f * rh, ch + 0.17f * rh, 0.24f * rh);
    DrawSeg(b.x + 0.45f * rh, ch + 0.165f * rh, b.x + 0.85f * rh, ch + 0.21f * rh, 0.12f * rh);
    // шляпа
    float by = ch - 0.72f * rh;
    Col(0.12f, 0.12f, 0.16f);
    FillBox(b.x - 0.92f * rh, by - 0.07f * rh, b.x + 0.92f * rh, by + 0.09f * rh, 0.05f * rh);
    FillBox(b.x - 0.58f * rh, by - 0.85f * rh, b.x + 0.58f * rh, by, 0.08f * rh);
    Col(0.86f, 0.16f, 0.20f);
    FillBox(b.x - 0.58f * rh, by - 0.26f * rh, b.x + 0.58f * rh, by - 0.07f * rh);
}

static void DrawPlow(const D2D1::Matrix3x2F& world) {
    Surface* s = FindSurface(g_plow.surf);
    if (!s) return;
    const float S = g_scale;
    G.dc->SetTransform(D2D1::Matrix3x2F::Scale((float)g_plow.dir * S, S) *
                       D2D1::Matrix3x2F::Translation(g_plow.x, (float)s->rc.top) * world);
    // кузов и кабина
    Col(1.0f, 0.74f, 0.08f);
    FillBox(-20, -17, 16, -6, 2.5f);
    FillBox(-9, -27, 6, -15, 2.5f);
    Col(0.82f, 0.55f, 0.05f);
    FillBox(-20, -9, 16, -6, 1.5f);
    Col(0.74f, 0.90f, 1.0f);
    FillBox(-5.5f, -24.5f, 3.5f, -17.5f, 1.2f);
    // маячок
    float blink = 0.35f + 0.65f * (0.5f + 0.5f * sinf((float)g_time * 11.0f));
    Col(1.0f, 0.45f, 0.05f, blink);
    FillBox(-3.5f, -30.5f, 0.5f, -27, 1.0f);
    Col(1.0f, 0.55f, 0.10f, 0.22f * blink);
    FillCircle(-1.5f, -29, 7);
    // отвал
    Col(0.22f, 0.24f, 0.28f);
    DrawSeg(16, -9, 21, -9, 2.2f);
    Col(0.62f, 0.67f, 0.74f);
    DrawSeg(19.5f, -17, 25, -1.5f, 3.6f);
    // колёса
    Col(0.13f, 0.14f, 0.17f);
    FillCircle(-12, -4.6f, 4.8f);
    FillCircle(9, -4.6f, 4.8f);
    Col(0.62f, 0.66f, 0.72f);
    FillCircle(-12, -4.6f, 1.9f);
    FillCircle(9, -4.6f, 1.9f);
    G.dc->SetTransform(world);
}

static void Render() {
    const D2D1::Matrix3x2F world = D2D1::Matrix3x2F::Translation((float)-g_vx, (float)-g_vy);
    G.dc->BeginDraw();
    G.dc->SetTransform(world);
    G.dc->Clear(D2D1::ColorF(0, 0, 0, 0));

    if (g_set.garlands) DrawGarlands();

    // сугробы
    Col(0.96f, 0.98f, 1.0f, 0.97f);
    for (auto& kv : g_surf) {
        Surface& s = kv.second;
        if (s.dirty && (g_time - s.lastBuild > 0.08 || !s.geo)) BuildGeo(s);
        if (s.geo) G.dc->FillGeometry(s.geo.Get(), G.brush.Get());
    }

    if (g_plow.active) DrawPlow(world);

    if (!g_taskView)
        for (int i = 0; i < (int)g_bodies.size(); ++i)
            DrawBody(g_bodies[(size_t)i], i == g_hoverIdx);

    for (auto& p : g_parts) {
        Col(1, 1, 1, 0.9f * Clampf(p.life / p.maxLife * 1.6f, 0.0f, 1.0f));
        FillCircle(p.x, p.y, p.r);
    }
    for (auto& f : g_flakes) {
        if (f.vis < 0.02f) continue;
        Col(1, 1, 1, f.alpha * f.vis);
        FillCircle(f.x, f.y, f.r);
    }

    HRESULT hr = G.dc->EndDraw();
    HRESULT hp = G.swap->Present(g_set.fps30 ? 2 : 1, 0);
    if (hr == D2DERR_RECREATE_TARGET || hp == DXGI_ERROR_DEVICE_REMOVED || hp == DXGI_ERROR_DEVICE_RESET || FAILED(hp))
        g_needReinit = true;
}

// ---------------------------------------------------------------------------
// Кадр
// ---------------------------------------------------------------------------

static void Reinit() {
    g_needReinit = false;
    g_vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    g_vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    g_vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    g_vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_scale = std::max(1.0f, (float)GetDpiForSystem() / 96.0f);
    g_cw = std::max(2.0f, 3.0f * g_scale);
    SetWindowPos(g_overlay, HWND_TOPMOST, g_vx, g_vy, g_vw, g_vh, SWP_NOACTIVATE);
    g_surf.clear();
    g_segs.clear();
    g_plow.active = false;
    g_gfxOk = CreateGfx(g_overlay, g_vw, g_vh);
    InitFlakes();
    g_scanTimer = 0;
    g_last = Now();
}

static void Frame() {
    if (g_needReinit) { Reinit(); if (!g_gfxOk) return; }
    double now = Now();
    float dt = (float)(now - g_last);
    g_last = now;
    dt = Clampf(dt, 0.001f, 0.05f);
    g_time += dt;

    // если активное окно двигают — пересканируем сразу, иначе 4 раза в секунду
    HWND fg = GetForegroundWindow();
    RECT fr{};
    if (fg) GetWindowRect(fg, &fr);
    if (fg != g_lastFg || !EqualRect(&fr, &g_lastFgRc)) {
        g_lastFg = fg; g_lastFgRc = fr; g_scanTimer = 0;
        g_taskView = IsTaskView(fg);
    }
    g_scanTimer -= dt;
    if (g_scanTimer <= 0) { g_scanTimer = 0.25f; Rescan(); }

    g_topTimer -= dt;
    if (g_topTimer <= 0) {
        g_topTimer = 2.0f;
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    Update(dt);
    Render();
}

static void UpdateActive() {
    ScanCovered();
    bool want = !g_set.paused && !g_locked && !(g_set.pauseFullscreen && ForegroundIsFullscreen()) &&
                !(g_set.maxMode == 2 && g_allCovered);      // «без снега»: ничего не рисуем вовсе
    if (want == g_active) return;
    g_active = want;
    if (want) {
        g_last = Now();
        g_scanTimer = 0;
        ShowWindow(g_overlay, SW_SHOWNOACTIVATE);
        SetWindowPos(g_overlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(g_overlay, SW_HIDE);
        OnRelease();
        DropDrag();
        g_hot = false;
        SetHook(false);
    }
}

// ---------------------------------------------------------------------------
// Перехват мыши (отдельный поток, включается только рядом со снежком)
// ---------------------------------------------------------------------------

static LRESULT CALLBACK MouseHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const MSLLHOOKSTRUCT* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if (wp == WM_LBUTTONDOWN && g_hot.load() && !g_dragging.load()) {
            g_dragging = true;
            PostMessageW(g_msgWnd, WM_APP_GRAB, (WPARAM)(INT_PTR)m->pt.x, (LPARAM)(INT_PTR)m->pt.y);
            return 1;
        }
        if (wp == WM_LBUTTONUP && g_dragging.load()) {
            g_dragging = false;
            PostMessageW(g_msgWnd, WM_APP_RELEASE, 0, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

static DWORD WINAPI HookThread(LPVOID param) {
    MSG msg;
    PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);     // создаём очередь сообщений
    SetEvent(reinterpret_cast<HANDLE>(param));
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd == nullptr && msg.message == WM_APP_HOOK) {
            if (msg.wParam && !g_hook)
                g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
            else if (!msg.wParam && g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
        } else {
            DispatchMessageW(&msg);
        }
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    return 0;
}

// ---------------------------------------------------------------------------
// Обновление: новая версия берётся из релизов на GitHub
// ---------------------------------------------------------------------------

static bool ParseVersion(const wchar_t* str, int v[3]) {
    v[0] = v[1] = v[2] = 0;
    while (*str && (*str < L'0' || *str > L'9')) ++str;
    return swscanf_s(str, L"%d.%d.%d", &v[0], &v[1], &v[2]) >= 1;
}

static bool IsNewer(const wchar_t* candidate, const wchar_t* current) {
    int a[3], b[3];
    if (!ParseVersion(candidate, a)) return false;
    ParseVersion(current, b);
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i]) return a[i] > b[i];
    return false;
}

static HINTERNET HttpSession() {
    HINTERNET s = WinHttpOpen(L"Snegopad", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (s) WinHttpSetTimeouts(s, 8000, 8000, 15000, 30000);
    return s;
}

// Адрес /releases/latest перенаправляет на /releases/tag/<версия> — оттуда и берём номер
static bool FetchLatestTag(std::wstring& tag) {
    bool ok = false;
    HINTERNET s = HttpSession(), c = nullptr, r = nullptr;
    std::wstring path = std::wstring(kRepoPath) + L"/releases/latest";
    if (s) c = WinHttpConnect(s, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) r = WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (r) {
        DWORD opt = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &opt, sizeof(opt));
        if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            wchar_t loc[1024] = L"";
            DWORD len = sizeof(loc);
            if (WinHttpQueryHeaders(r, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX, loc, &len,
                                    WINHTTP_NO_HEADER_INDEX)) {
                const wchar_t* t = wcsstr(loc, L"/releases/tag/");
                if (t) {
                    tag = t + 14;
                    size_t cut = tag.find_first_of(L"?#/");
                    if (cut != std::wstring::npos) tag.resize(cut);
                    ok = !tag.empty() && tag.size() < 40;
                }
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static bool HttpDownload(const wchar_t* path, const wchar_t* file) {
    bool ok = false;
    HINTERNET s = HttpSession(), c = nullptr, r = nullptr;
    if (s) c = WinHttpConnect(s, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (c) r = WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        DWORD status = 0, sz = sizeof(status);
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &status, &sz, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                std::vector<char> buf(65536);
                DWORD total = 0;
                bool err = false, mz = false;
                for (;;) {
                    DWORD n = 0, w = 0;
                    if (!WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n)) { err = true; break; }
                    if (!n) break;
                    if (total == 0) mz = n >= 2 && buf[0] == 'M' && buf[1] == 'Z';
                    if (!WriteFile(f, buf.data(), n, &w, nullptr) || w != n) { err = true; break; }
                    total += n;
                }
                CloseHandle(f);
                ok = !err && mz && total > 50000;           // похоже на настоящий .exe
                if (!ok) DeleteFileW(file);
            }
        }
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    if (s) WinHttpCloseHandle(s);
    return ok;
}

static std::wstring ExePath() {
    wchar_t exe[MAX_PATH] = L"";
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return exe;
}

static DWORD WINAPI UpdateThread(LPVOID param) {
    const LPARAM manual = (LPARAM)(INT_PTR)param;
    std::wstring exe = ExePath();
    DeleteFileW((exe + L".old").c_str());               // остаток от прошлого обновления
    WPARAM result = 2;
    std::wstring tag;
    if (FetchLatestTag(tag)) {
        if (!IsNewer(tag.c_str(), kVersion)) {
            result = 0;
        } else {
            std::wstring url = std::wstring(kRepoPath) + L"/releases/download/" + tag + L"/Snegopad.exe";
            if (HttpDownload(url.c_str(), (exe + L".new").c_str())) {
                g_newTag = tag;
                result = 1;
            }
        }
    }
    if (result != 1) g_updating = false;
    PostMessageW(g_msgWnd, WM_APP_UPDATE, result, manual);
    return 0;
}

static void StartUpdateCheck(bool manual) {
    if (g_updating.exchange(true)) return;
    HANDLE t = CreateThread(nullptr, 0, UpdateThread, (LPVOID)(INT_PTR)(manual ? 1 : 0), 0, nullptr);
    if (t) CloseHandle(t); else g_updating = false;
}

// Подменяем свой .exe скачанным и перезапускаемся. Работающий файл нельзя удалить, но можно переименовать.
static bool ApplyUpdate() {
    std::wstring exe = ExePath(), oldf = exe + L".old", newf = exe + L".new";
    DeleteFileW(oldf.c_str());
    if (!MoveFileExW(exe.c_str(), oldf.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(newf.c_str()); return false; }
    if (!MoveFileExW(newf.c_str(), exe.c_str(), 0)) {
        MoveFileExW(oldf.c_str(), exe.c_str(), 0);
        DeleteFileW(newf.c_str());
        return false;
    }
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); g_mutex = nullptr; }
    std::wstring cmd = L"\"" + exe + L"\" --updated";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    g_running = false;
    PostQuitMessage(0);
    return true;
}

static void OnUpdateResult(WPARAM result, bool manual) {
    const UINT fl = MB_OK | MB_TOPMOST | MB_SETFOREGROUND;
    if (result == 1) {
        if (!ApplyUpdate()) {
            g_updating = false;
            if (manual) MessageBoxW(nullptr, L"Новая версия скачана, но заменить файл программы не удалось.\n"
                                             L"Переложи Snegopad.exe в папку, куда можно записывать, например в «Документы».",
                                    kTitle, fl | MB_ICONWARNING);
        }
    } else if (manual) {
        if (result == 0) MessageBoxW(nullptr, L"У тебя уже последняя версия.", kTitle, fl | MB_ICONINFORMATION);
        else MessageBoxW(nullptr, L"Не получилось проверить обновления. Проверь интернет и попробуй позже.",
                         kTitle, fl | MB_ICONWARNING);
    }
}

// ---------------------------------------------------------------------------
// Значок в трее и меню
// ---------------------------------------------------------------------------

static void AddTray() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(101), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    if (!nid.hIcon) nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(nid.szTip, kTitle);
    Shell_NotifyIconW(NIM_ADD, &nid);
}
static void RemoveTray() {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

static void ShowBalloon(const wchar_t* text) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_msgWnd;
    nid.uID = 1;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcscpy_s(nid.szInfoTitle, kTitle);
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void Item(HMENU m, UINT id, const wchar_t* text, bool checked = false) {
    AppendMenuW(m, MF_STRING | (checked ? MF_CHECKED : 0), id, text);
}

static void ShowMenu() {
    HMENU m = CreatePopupMenu();
    HMENU d = CreatePopupMenu();
    Item(d, ID_D0, L"Мало", g_set.density == 0);
    Item(d, ID_D1, L"Средне", g_set.density == 1);
    Item(d, ID_D2, L"Много", g_set.density == 2);
    CheckMenuRadioItem(d, ID_D0, ID_D2, ID_D0 + (UINT)g_set.density, MF_BYCOMMAND);

    HMENU mm = CreatePopupMenu();
    Item(mm, ID_M0, L"Обычный снег", g_set.maxMode == 0);
    Item(mm, ID_M1, L"Меньше снега", g_set.maxMode == 1);
    Item(mm, ID_M2, L"Без снега (только на рабочем столе и с небольшими окнами)", g_set.maxMode == 2);
    CheckMenuRadioItem(mm, ID_M0, ID_M2, ID_M0 + (UINT)g_set.maxMode, MF_BYCOMMAND);

    Item(m, ID_PAUSE, L"Пауза", g_set.paused);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)d, L"Количество снега");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)mm, L"Когда окно развёрнуто на весь экран");
    Item(m, ID_GARL, L"Гирлянды на окнах", g_set.garlands);
    Item(m, ID_WIDGETS, L"Снег на виджетах Rainmeter", g_set.widgets);
    Item(m, ID_BALLS, L"Снежки: катать мышкой из сугроба", g_set.balls);
    Item(m, ID_PLOW, L"Снегоуборщик", g_set.plow);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    Item(m, ID_PLOWNOW, L"Вызвать снегоуборщик");
    Item(m, ID_SHAKE, L"Стряхнуть весь снег");
    Item(m, ID_CLEARBALLS, L"Убрать снежки и снеговиков");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    Item(m, ID_FPS30, L"Экономный режим (30 кадров/с)", g_set.fps30);
    Item(m, ID_PFS, L"Пауза в играх и полноэкранном видео", g_set.pauseFullscreen);
    Item(m, ID_AUTO, L"Запускать вместе с Windows", AutostartEnabled());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring ver = std::wstring(L"Версия ") + (kVersion[0] == L'v' ? kVersion + 1 : kVersion);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, ver.c_str());
    Item(m, ID_AUTOUPD, L"Обновляться автоматически", g_set.autoUpdate);
    Item(m, ID_CHECKUPD, L"Проверить обновления");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    Item(m, ID_EXIT, L"Выход");

    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_msgWnd);
    SetTimer(g_msgWnd, 1, 16, nullptr);                 // снег идёт и пока открыто меню
    UINT cmd = (UINT)TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, g_msgWnd, nullptr);
    KillTimer(g_msgWnd, 1);
    PostMessageW(g_msgWnd, WM_NULL, 0, 0);
    DestroyMenu(m);

    switch (cmd) {
    case ID_PAUSE: g_set.paused = !g_set.paused; UpdateActive(); break;
    case ID_D0: case ID_D1: case ID_D2: g_set.density = (int)(cmd - ID_D0); InitFlakes(); break;
    case ID_GARL: g_set.garlands = !g_set.garlands; break;
    case ID_BALLS: g_set.balls = !g_set.balls; break;
    case ID_WIDGETS: g_set.widgets = !g_set.widgets; g_scanTimer = 0; break;
    case ID_M0: case ID_M1: case ID_M2: g_set.maxMode = (int)(cmd - ID_M0); UpdateActive(); break;
    case ID_PLOW: g_set.plow = !g_set.plow; break;
    case ID_PLOWNOW: StartPlow(); break;
    case ID_SHAKE: for (auto& kv : g_surf) Shed(kv.second); break;
    case ID_CLEARBALLS:
        for (auto& b : g_bodies) Poof(b.x, b.base - BodyHeight(b) * 0.5f, 10, 1.0f);
        g_bodies.clear(); DropDrag();
        break;
    case ID_FPS30: g_set.fps30 = !g_set.fps30; break;
    case ID_PFS: g_set.pauseFullscreen = !g_set.pauseFullscreen; UpdateActive(); break;
    case ID_AUTO: SetAutostart(!AutostartEnabled()); break;
    case ID_AUTOUPD: g_set.autoUpdate = !g_set.autoUpdate; break;
    case ID_CHECKUPD: StartUpdateCheck(true); break;
    case ID_EXIT: g_running = false; PostQuitMessage(0); break;
    default: break;
    }
    if (cmd) SaveSettings();
}

// ---------------------------------------------------------------------------
// Оконные процедуры
// ---------------------------------------------------------------------------

static LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: ValidateRect(h, nullptr); return 0;
    case WM_DISPLAYCHANGE:
    case WM_DPICHANGED: g_needReinit = true; return 0;
    case WM_CLOSE: return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK MsgProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED && WM_TASKBARCREATED) { AddTray(); return 0; }
    switch (msg) {
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowMenu();
        return 0;
    case WM_APP_GRAB: OnGrab((int)(INT_PTR)wp, (int)(INT_PTR)lp); return 0;
    case WM_APP_RELEASE: OnRelease(); return 0;
    case WM_APP_UPDATE: OnUpdateResult(wp, lp != 0); return 0;
    case WM_TIMER:
        if (wp == 1 && g_active && g_gfxOk) Frame();
        return 0;
    case WM_HOTKEY:
        if (wp == 1) { g_running = false; PostQuitMessage(0); }
        return 0;
    case WM_WTSSESSION_CHANGE:
        if (wp == WTS_SESSION_LOCK) g_locked = true;
        else if (wp == WTS_SESSION_UNLOCK) g_locked = false;
        UpdateActive();
        return 0;
    case WM_DISPLAYCHANGE: g_needReinit = true; return 0;
    case WM_ENDSESSION:
    case WM_CLOSE: g_running = false; PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Точка входа
// ---------------------------------------------------------------------------

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    const bool updated = cmdLine && wcsstr(cmdLine, L"--updated");
    for (int attempt = 0;; ++attempt) {                 // после обновления ждём, пока закроется старая копия
        g_mutex = CreateMutexW(nullptr, TRUE, L"Snegopad.SingleInstance");
        if (GetLastError() != ERROR_ALREADY_EXISTS) break;
        if (g_mutex) { CloseHandle(g_mutex); g_mutex = nullptr; }
        if (!updated || attempt >= 50) return 0;
        Sleep(100);
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_inst = hInst;
    g_rng ^= GetTickCount();
    if (!g_rng) g_rng = 1;
    LoadSettings();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInst;
    wc.lpfnWndProc = MsgProc;
    wc.lpszClassName = L"SnegopadMsg";
    RegisterClassExW(&wc);
    wc.lpfnWndProc = OverlayProc;
    wc.lpszClassName = L"SnegopadOverlay";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    g_msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, L"SnegopadMsg", kTitle, WS_POPUP, 0, 0, 0, 0,
                               nullptr, nullptr, hInst, nullptr);
    g_overlay = CreateWindowExW(
        WS_EX_NOREDIRECTIONBITMAP | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        L"SnegopadOverlay", kTitle, WS_POPUP, 0, 0, 16, 16, nullptr, nullptr, hInst, nullptr);
    if (!g_msgWnd || !g_overlay) return 1;
    SetLayeredWindowAttributes(g_overlay, 0, 255, LWA_ALPHA);

    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");
    AddTray();
    RegisterHotKey(g_msgWnd, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, 'Q');   // аварийный выход
    WTSRegisterSessionNotification(g_msgWnd, NOTIFY_FOR_THIS_SESSION);

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hookThread = CreateThread(nullptr, 0, HookThread, ready, 0, &g_hookThreadId);
    if (g_hookThread) WaitForSingleObject(ready, 2000);
    CloseHandle(ready);

    Reinit();

    if (updated) {
        std::wstring msg = std::wstring(L"Программа обновлена до версии ") + (kVersion[0] == L'v' ? kVersion + 1 : kVersion);
        ShowBalloon(msg.c_str());
    }
    const bool releaseBuild = wcscmp(kVersion, L"0.0.0") != 0;
    double nextState = 0, nextRetry = 0, nextUpdate = Now() + 8.0;
    while (g_running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) g_running = false;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!g_running) break;

        double t = Now();
        if (t >= nextState) { nextState = t + 0.5; UpdateActive(); }
        if (t >= nextUpdate) {                           // при запуске и потом раз в 6 часов
            nextUpdate = t + 6.0 * 3600.0;
            if (g_set.autoUpdate && releaseBuild) StartUpdateCheck(false);
        }

        if (g_active && g_gfxOk) {
            double t0 = Now();
            Frame();
            if (Now() - t0 < 0.004) Sleep(g_set.fps30 ? 30 : 12);       // страховка, если Present не ждёт vsync
        } else {
            if (g_active && !g_gfxOk && t >= nextRetry) { nextRetry = t + 1.0; Reinit(); }
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 300, QS_ALLINPUT);
        }
    }

    if (g_hookThreadId) PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    if (g_hookThread) { WaitForSingleObject(g_hookThread, 1000); CloseHandle(g_hookThread); }
    WTSUnRegisterSessionNotification(g_msgWnd);
    UnregisterHotKey(g_msgWnd, 1);
    RemoveTray();
    SaveSettings();
    g_surf.clear();
    G = Gfx();
    DestroyWindow(g_overlay);
    DestroyWindow(g_msgWnd);
    if (g_mutex) { ReleaseMutex(g_mutex); CloseHandle(g_mutex); }
    return 0;
}
