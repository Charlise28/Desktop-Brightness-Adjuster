/*
 * DesktopBrightness.c
 * Desktop Brightness Adjuster - native Win32, any number of monitors.
 *
 * Links only against DLLs Windows has already loaded
 * (user32/gdi32/shell32/dxva2/advapi32), so process start costs nothing beyond
 * the exe itself - the point of dropping the earlier .NET/WinForms build, which
 * pulled in the CLR at launch and was rated "High" startup impact no matter how
 * much work was deferred inside the app.
 *
 * Scope: desktop monitors. Brightness is driven over DDC/CI where a monitor
 * supports it and by a software dimming overlay everywhere else, so every
 * monitor responds whatever it is. Laptop internal panels use a different
 * mechanism (WMI) and already have working brightness keys; they still get the
 * overlay, and a laptop's external monitors work as on a desktop.
 *
 * ------------------------------------------------------------------
 * Two strategies
 * ------------------------------------------------------------------
 * Monitors vary wildly. Two panels driven to the same DDC percentage do not
 * look the same - backlight PWM curves differ per panel, so they agree at 0%
 * and 100% and drift everywhere in between - and a surprising number of panels
 * accept a DDC/CI brightness write, report success, and then ignore it.
 *
 *   MODE_MATCHED - every backlight is held at one common level and the dial is
 *     realized by a dimming overlay with identical alpha on every screen. That
 *     identical alpha is what makes different panels track each other at every
 *     step. The common level is the highest every panel can actually reach: a
 *     panel that ignores writes is frozen at its own OSD setting, which caps
 *     the rest. A per-monitor "balance" trim dims a panel that is natively
 *     brighter than its neighbours, so they can be made to look alike.
 *
 *   MODE_HARDWARE - each backlight is driven across its own reported range.
 *     Real dimming: less backlight power, full contrast. Below
 *     OVERLAY_FLOOR_PCT the panel sits at its own minimum and the overlay
 *     carries on, because plenty of panels are still bright at that minimum.
 *     A monitor with no working DDC/CI gets the overlay across the whole dial.
 *
 * Automatic picks MODE_MATCHED whenever more than one screen is present (the
 * point of the app is that they look alike) and MODE_HARDWARE for a single
 * screen (nothing to match, and real dimming is better). Either can be forced
 * from the tray; the choice is remembered.
 *
 * ------------------------------------------------------------------
 * Threads
 * ------------------------------------------------------------------
 * The UI thread owns the screens and their overlay windows and never touches
 * DDC/CI. The worker thread owns the physical monitor handles and does every
 * I2C round-trip (~60 ms each). They share only a small summary under
 * g_shareLock, held for microseconds, so a keypress can never wait on I2C.
 *
 * Build: build_brightness_app.ps1
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <lowlevelmonitorconfigurationapi.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define APP_VERSION    L"2.1.0"

#define HOTKEY_UP_ID   9001
#define HOTKEY_DN_ID   9002

/* Defaults only. Alt+PgUp/PgDn is free on a stock Windows install but plenty of
   apps claim it, and key preference is personal, so all four are overridable
   from the registry - see LoadSettings and the README. */
#define DEF_HOTKEY_MOD MOD_ALT
#define DEF_HOTKEY_UP  VK_PRIOR   /* Page Up   */
#define DEF_HOTKEY_DN  VK_NEXT    /* Page Down */
#define DEF_STEP       5          /* percent per keypress */

#define MAX_SCREENS    16
#define MAX_PANELS     32
#define MAX_PHYS_PER   8

#define WM_TRAYICON     (WM_APP + 1)
#define WM_DDC_FALLBACK (WM_APP + 2)  /* worker found a panel that ignores writes   */
#define WM_PANELS_READY (WM_APP + 3)  /* worker (re)enumerated panels; wp = probed level or -1 */

/* VCP code for luminance in the MCCS standard every DDC/CI monitor implements. */
#define VCP_BRIGHTNESS 0x10

/* One-off settle time before reading a write back. Paid once per verification
   round, not once per monitor. */
#define DDC_VERIFY_DELAY_MS 120

/* A single dropped I2C round-trip is not proof of a broken panel. Give the
   monitor this many write+read attempts before writing it off as unwritable,
   otherwise one flaky reply permanently disables real backlight control. */
#define DDC_VERIFY_ATTEMPTS 3

/* Plenty of panels quantize a written value (write 37, read back 36 or 35).
   Treat a read-back this close to the target as the write having landed. */
#define DDC_VERIFY_TOLERANCE_PCT 3

#define IDM_AUTOSTART  100
#define IDM_RESET      101
#define IDM_EXIT       102
#define IDM_MODE_AUTO  103
#define IDM_MODE_HW    104
#define IDM_MODE_MATCH 105
#define IDM_REDETECT   106
#define IDM_TRIM_BASE  1000          /* + screen * TRIM_CHOICES + choice */
#define TRIM_CHOICES   9             /* 0, 5, 10 ... 40 percent dimmer */
#define TRIM_STEP      5

/* A keypress must only ever touch overlay alpha. Everything slower - the tray
   IPC round-trip, the registry write, and above all DDC/CI traffic - is
   coalesced onto this timer, which restarts on every adjustment. Holding a key
   therefore pays that cost once, after release, instead of once per repeat. */
#define IDT_SETTLE       1
#define SETTLE_DELAY_MS  250

/* Overlay z-order guard. Another window can end up above a topmost overlay -
   an app that raises itself as it opens (the Photos viewer does exactly this),
   the taskbar after a click, any other "always on top" window. The monitor
   underneath then shows at full brightness and the hotkeys appear dead on it
   while every other monitor still responds. Window events schedule a check on
   this short timer, so a burst of them costs one pass. */
#define IDT_ZORDER       2
#define ZORDER_DELAY_MS  15
/* Two apps that both insist on being on top would otherwise trade places
   forever. Past this many raises inside one window of time, back off. */
#define ZORDER_BURST_MAX     30
#define ZORDER_BURST_WINDOW  2000

/* Overlay can never go fully black, or a monitor would become unusable. */
#define OVERLAY_MAX_ALPHA 0.85
/* With a balance trim added on top, a little further is allowed. */
#define OVERLAY_HARD_CAP  0.92

/* Bottom slice of the dial reserved for the overlay in MODE_HARDWARE. */
#define OVERLAY_FLOOR_PCT 20

#define REG_APP_KEY    L"Software\\DesktopBrightness"
#define REG_TRIM_KEY   L"Software\\DesktopBrightness\\Balance"
#define REG_FROZEN_KEY L"Software\\DesktopBrightness\\IgnoresWrites"
#define REG_RUN_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_RUN_VALUE  L"DesktopBrightness"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

typedef enum { MODE_AUTO = 0, MODE_HARDWARE, MODE_MATCHED } BrightnessMode;

/* A screen is what Windows draws on: one HMONITOR, one overlay. Several
   physical panels can sit behind one screen (clone/duplicate mode); they share
   its overlay, so the image is never dimmed twice. Owned by the UI thread. */
typedef struct {
    RECT  bounds;          /* virtual-desktop coords, physical pixels */
    WCHAR device[32];      /* GDI name, \\.\DISPLAYn - joins screens to panels */
    WCHAR name[64];        /* monitor's own name from EDID, for the tray menu  */
    WCHAR key[160];        /* stable id for per-monitor settings               */
    int   trim;            /* balance: percent dimmer than the dial, 0..40     */
    HWND  overlay;
} Screen;

/* Some panels answer a read happily and then ignore every write, returning
   success with no error. Reads alone therefore prove nothing - writes are read
   back to find out what each monitor really does. */
typedef enum { DDC_UNVERIFIED = 0, DDC_WRITABLE, DDC_IGNORES_WRITES, DDC_NONE } DdcState;

/* A physical panel behind a screen. Owned by the worker thread. */
typedef struct {
    HANDLE   h;
    WCHAR    device[32];   /* which screen it belongs to                     */
    WCHAR    key[176];     /* stable id, for remembering a write-ignorer     */
    BOOL     lowLevel;     /* raw VCP 0x10 works; else the high-level API    */
    DdcState state;
    DWORD    minB, maxB;   /* panel's own brightness range                   */
    DWORD    lastWritten;  /* raw value last sent, or MAXDWORD if none yet   */
    DWORD    stuckVal;     /* raw value it shows, only if DDC_IGNORES_WRITES */
    BOOL     stuckKnown;   /* stuckVal came from an actual read              */
} Panel;

/* What the worker tells the UI about each screen. */
typedef struct {
    WCHAR device[32];
    BOOL  allControllable; /* every panel behind it obeys brightness writes */
} ScreenDdc;

static Screen g_screens[MAX_SCREENS];
static int    g_screenCount = 0;
static UINT   g_layoutGen   = 0;   /* bumped on every rebuild of g_screens */

static Panel  g_panels[MAX_PANELS];       /* worker thread only */
static int    g_panelCount = 0;

static CRITICAL_SECTION g_shareLock;      /* guards the three below */
static ScreenDdc g_shareDdc[MAX_SCREENS];
static int       g_shareDdcCount = 0;
static BOOL      g_shareDdcValid = FALSE;

static HINSTANCE g_hInst      = NULL;
static HWND      g_hwndMain   = NULL;
static BOOL      g_trayAdded  = FALSE;
static UINT      g_taskbarMsg = 0;   /* "TaskbarCreated" - re-add icon if Explorer restarts */
static HICON     g_trayIcon   = NULL;

static int  g_level       = 100;
static BOOL g_levelSaved  = FALSE;   /* level came from the registry */
static BOOL g_levelTouched = FALSE;  /* user adjusted since start - a probe must not override */
static BOOL g_savePending = FALSE;   /* an adjustment is waiting to be written out */

static BrightnessMode g_modePref = MODE_AUTO;

static UINT g_hotkeyMod = DEF_HOTKEY_MOD;
static UINT g_hotkeyUp  = DEF_HOTKEY_UP;
static UINT g_hotkeyDn  = DEF_HOTKEY_DN;
static int  g_step      = DEF_STEP;
static BOOL g_toldAboutFallback = FALSE; /* balloon shown once per monitor layout */

/* Z-order guard */
static HWINEVENTHOOK g_hooks[4];
static int           g_hookCount = 0;
static BOOL          g_zorderPending = FALSE;
static DWORD         g_raiseTimes[ZORDER_BURST_MAX];
static int           g_raiseNext = 0;
typedef BOOL (WINAPI *PFN_GetWindowBand)(HWND, DWORD *);
static PFN_GetWindowBand g_getWindowBand = NULL;

/* Worker thread. Requests are "latest wins": rapid keypresses coalesce, but the
   final value is always applied. */
typedef struct { LONG level; LONG matched; } DdcRequest;
static volatile LONG g_reqLevel     = -1;
static volatile LONG g_reqMatched   = 0;
static volatile LONG g_panelsStale  = 1;  /* re-enumerate panels on next run */
static volatile LONG g_forgetFrozen = 0;  /* "re-detect": retest write-ignorers */
/* No saved level yet: the worker's first enumeration reports where the hardware
   sits and waits for the UI to decide the level before driving anything, or it
   would yank the backlight to the default 100% first. */
static volatile LONG g_adoptPending = 0;
static HANDLE        g_wakeEvent    = NULL;
static HANDLE        g_workerThread = NULL;
static volatile LONG g_quitWorker   = 0;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */

static int ClampLevel(int v)
{
    if (v < 0)   return 0;
    if (v > 100) return 100;
    return v;
}

static void RegSetDword(const WCHAR *subkey, const WCHAR *name, DWORD val)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
        RegCloseKey(key);
    }
}

static void RegDeleteName(const WCHAR *subkey, const WCHAR *name)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS) {
        RegDeleteValueW(key, name);
        RegCloseKey(key);
    }
}

/* Reads a DWORD, leaving *out alone if absent or of the wrong type, so every
   setting keeps its compiled-in default until deliberately overridden. */
static BOOL RegGetDword(const WCHAR *subkey, const WCHAR *name, DWORD *out)
{
    HKEY  key;
    DWORD val = 0, size = sizeof(val), type = 0;
    BOOL  ok = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS
            && type == REG_DWORD) {
            *out = val;
            ok = TRUE;
        }
        RegCloseKey(key);
    }
    return ok;
}

static void LoadSettings(void)
{
    DWORD mod = g_hotkeyMod, up = g_hotkeyUp, dn = g_hotkeyDn;
    DWORD step = (DWORD)g_step, mode = (DWORD)g_modePref, level = 100;

    RegGetDword(REG_APP_KEY, L"HotkeyMod",    &mod);
    RegGetDword(REG_APP_KEY, L"HotkeyVkUp",   &up);
    RegGetDword(REG_APP_KEY, L"HotkeyVkDown", &dn);
    RegGetDword(REG_APP_KEY, L"Step",         &step);
    RegGetDword(REG_APP_KEY, L"Mode",         &mode);

    /* A zero modifier is legal to RegisterHotKey but would swallow a bare key
       system-wide, and a step outside 1..50 makes the dial useless - ignore
       either rather than honouring an obviously broken value. MOD_NOREPEAT is
       stripped: the app relies on autorepeat for a held key. */
    mod &= (MOD_ALT | MOD_CONTROL | MOD_SHIFT | MOD_WIN);
    if (mod != 0)                         g_hotkeyMod = (UINT)mod;
    if (up  != 0 && up  <= 0xFE)          g_hotkeyUp  = (UINT)up;
    if (dn  != 0 && dn  <= 0xFE)          g_hotkeyDn  = (UINT)dn;
    if (step >= 1 && step <= 50)          g_step      = (int)step;
    if (mode <= (DWORD)MODE_MATCHED)      g_modePref  = (BrightnessMode)mode;

    if (RegGetDword(REG_APP_KEY, L"Level", &level)) {
        g_level      = ClampLevel((int)level);
        g_levelSaved = TRUE;
    }
}

static BOOL IsAutoStartEnabled(void)
{
    HKEY  key;
    BOOL  found = FALSE;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        found = (RegQueryValueExW(key, REG_RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS);
        RegCloseKey(key);
    }
    return found;
}

/* True if the Run entry points at a file that no longer exists - the exe was
   moved or deleted. A valid entry is left alone: overwriting it on every start
   would repoint logon at wherever the exe happened to be launched from, such as
   a copy opened straight out of a zip into %TEMP%. */
static BOOL AutoStartTargetMissing(void)
{
    HKEY  key;
    WCHAR val[MAX_PATH + 4];
    DWORD size = sizeof(val) - sizeof(WCHAR), type = 0;
    BOOL  missing = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return FALSE;
    ZeroMemory(val, sizeof(val));
    if (RegQueryValueExW(key, REG_RUN_VALUE, NULL, &type, (BYTE *)val, &size) == ERROR_SUCCESS
        && type == REG_SZ) {
        WCHAR *p = val, *q;
        if (*p == L'"') {
            p++;
            q = p;
            while (*q && *q != L'"') q++;
            *q = 0;
        }
        missing = (*p != 0 && GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES);
    }
    RegCloseKey(key);
    return missing;
}

/* The Run entry stores the exe's full path. */
static void SetAutoStart(BOOL enable)
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS)
        return;

    if (enable) {
        WCHAR path[MAX_PATH + 2];
        WCHAR quoted[MAX_PATH + 4];
        DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            wsprintfW(quoted, L"\"%s\"", path);
            RegSetValueExW(key, REG_RUN_VALUE, 0, REG_SZ,
                           (const BYTE *)quoted,
                           (DWORD)((lstrlenW(quoted) + 1) * sizeof(WCHAR)));
        }
    } else {
        RegDeleteValueW(key, REG_RUN_VALUE);
    }
    RegCloseKey(key);
}

/* Monitor identity: the GDI device name (\\.\DISPLAY2) changes when cables move
   or a monitor is unplugged, so per-monitor settings are keyed on the
   connection's device path from the display config instead, and the menu shows
   the name the monitor reports in its EDID. Falls back to the GDI name. */
static void GetMonitorIdentity(const WCHAR *gdiDevice, WCHAR *name, int nameLen,
                               WCHAR *key, int keyLen)
{
    UINT32 np = 0, nm = 0, i;
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    HANDLE heap = GetProcessHeap();

    lstrcpynW(name, L"Monitor", nameLen);
    lstrcpynW(key, gdiDevice, keyLen);

    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &np, &nm) != ERROR_SUCCESS || !np)
        return;
    paths = (DISPLAYCONFIG_PATH_INFO *)HeapAlloc(heap, 0, np * sizeof(*paths));
    modes = (DISPLAYCONFIG_MODE_INFO *)HeapAlloc(heap, 0, (nm ? nm : 1) * sizeof(*modes));
    if (paths && modes &&
        QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &np, paths, &nm, modes, NULL) == ERROR_SUCCESS) {
        for (i = 0; i < np; i++) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME src;
            DISPLAYCONFIG_TARGET_DEVICE_NAME tgt;

            ZeroMemory(&src, sizeof(src));
            src.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            src.header.size      = sizeof(src);
            src.header.adapterId = paths[i].sourceInfo.adapterId;
            src.header.id        = paths[i].sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&src.header) != ERROR_SUCCESS) continue;
            if (lstrcmpiW(src.viewGdiDeviceName, gdiDevice) != 0)        continue;

            ZeroMemory(&tgt, sizeof(tgt));
            tgt.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            tgt.header.size      = sizeof(tgt);
            tgt.header.adapterId = paths[i].targetInfo.adapterId;
            tgt.header.id        = paths[i].targetInfo.id;
            if (DisplayConfigGetDeviceInfo(&tgt.header) != ERROR_SUCCESS) continue;

            if (tgt.monitorFriendlyDeviceName[0])
                lstrcpynW(name, tgt.monitorFriendlyDeviceName, nameLen);
            if (tgt.monitorDevicePath[0])
                lstrcpynW(key, tgt.monitorDevicePath, keyLen);
            break;   /* first target of a cloned source names the screen */
        }
    }
    if (paths) HeapFree(heap, 0, paths);
    if (modes) HeapFree(heap, 0, modes);
}

/* ------------------------------------------------------------------ */
/* Overlay windows (UI thread)                                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    if (msg == WM_NCHITTEST)
        return HTTRANSPARENT;   /* belt and braces on top of WS_EX_TRANSPARENT */
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void UpdateZOrderHooks(void);
static void ScheduleZOrderCheck(UINT delayMs);

static void DestroyOverlay(Screen *s)
{
    if (!s->overlay) return;
    DestroyWindow(s->overlay);
    s->overlay = NULL;
}

/* alpha = 1 - brightness, and brightness is what the eye sees: the dial's own
   fraction times the balance trim. Identical inputs give identical alpha on
   every screen, which is the whole basis of matching. */
static BYTE OverlayAlpha(int level, int trim)
{
    double bright = 1.0 - (100 - level) / 100.0 * OVERLAY_MAX_ALPHA;
    double a;
    bright *= (100 - trim) / 100.0;
    a = 1.0 - bright;
    if (a < 0.0)              a = 0.0;
    if (a > OVERLAY_HARD_CAP) a = OVERLAY_HARD_CAP;
    return (BYTE)(a * 255.0 + 0.5);
}

static void SetOverlay(Screen *s, int level, int trim)
{
    BYTE alpha = OverlayAlpha(level, trim);

    if (alpha == 0) {             /* nothing to dim - release the window entirely */
        DestroyOverlay(s);
        return;
    }

    if (!s->overlay) {
        s->overlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"BrightDimmer", NULL, WS_POPUP,
            s->bounds.left, s->bounds.top,
            s->bounds.right - s->bounds.left,
            s->bounds.bottom - s->bounds.top,
            NULL, NULL, g_hInst, NULL);

        if (!s->overlay) return;

        /* Alpha is set before the window is ever shown, or it flashes fully
           black for a frame before the dim level lands. */
        SetLayeredWindowAttributes(s->overlay, 0, alpha, LWA_ALPHA);
        SetWindowDisplayAffinity(s->overlay, WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(s->overlay, SW_SHOWNOACTIVATE);
    } else {
        SetLayeredWindowAttributes(s->overlay, 0, alpha, LWA_ALPHA);
    }
}

static BOOL IsOurOverlay(HWND h)
{
    int i;
    for (i = 0; i < g_screenCount; i++)
        if (g_screens[i].overlay == h) return TRUE;
    return FALSE;
}

/* True if some visible window that overlaps this overlay sits above it in the
   ordinary desktop band. Windows in higher bands (Start, the Alt+Tab switcher,
   notification flyouts) are above every app's topmost window by design and
   cannot be beaten, so they are not worth a fight. */
static BOOL IsOverlayCovered(HWND overlay)
{
    RECT ro, rw, rx;
    HWND h;

    GetWindowRect(overlay, &ro);
    for (h = GetWindow(overlay, GW_HWNDPREV); h; h = GetWindow(h, GW_HWNDPREV)) {
        if (!IsWindowVisible(h) || IsOurOverlay(h)) continue;
        if (g_getWindowBand) {
            DWORD band = 0;
            if (g_getWindowBand(h, &band) && band > 1) continue;
        }
        if (GetWindowRect(h, &rw) && IntersectRect(&rx, &ro, &rw))
            return TRUE;
    }
    return FALSE;
}

static BOOL RaiseBurstExceeded(void)
{
    DWORD now = GetTickCount();
    DWORD oldest = g_raiseTimes[g_raiseNext];
    /* ring buffer of the last ZORDER_BURST_MAX raise times */
    return oldest != 0 && (now - oldest) < ZORDER_BURST_WINDOW;
}

static void KeepOverlaysOnTop(void)
{
    int i;
    for (i = 0; i < g_screenCount; i++) {
        HWND ov = g_screens[i].overlay;
        if (!ov || !IsOverlayCovered(ov)) continue;
        if (RaiseBurstExceeded()) {
            /* Something else is fighting for the top. Try again later rather
               than ping-ponging with it at full speed. */
            ScheduleZOrderCheck(500);
            return;
        }
        SetWindowPos(ov, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
        g_raiseTimes[g_raiseNext] = GetTickCount();
        g_raiseNext = (g_raiseNext + 1) % ZORDER_BURST_MAX;
    }
}

/* Runs on the UI thread (out-of-context hooks are delivered through its message
   loop). Kept trivial: it only arms the timer, the real check runs once per
   burst of events. */
static void ScheduleZOrderCheck(UINT delayMs)
{
    /* Armed once and left alone until it fires: re-arming on every event would
       let an app that fires events continuously postpone the check forever. */
    if (g_zorderPending) return;
    g_zorderPending = TRUE;
    SetTimer(g_hwndMain, IDT_ZORDER, delayMs, NULL);
}

static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD ev, HWND hwnd,
                                  LONG idObject, LONG idChild, DWORD tid, DWORD time)
{
    (void)hook; (void)idChild; (void)tid; (void)time;
    if (ev == EVENT_OBJECT_SHOW &&
        (idObject != OBJID_WINDOW || !hwnd || GetAncestor(hwnd, GA_PARENT) != GetDesktopWindow()))
        return;   /* a control or child window appearing cannot change top-level z-order */
    ScheduleZOrderCheck(ZORDER_DELAY_MS);
}

/* The hooks exist only while some overlay does: a monitor that is not being
   dimmed has nothing to guard, and the app stays at zero idle cost. */
static void UpdateZOrderHooks(void)
{
    static const DWORD evs[4][2] = {
        { EVENT_SYSTEM_FOREGROUND,   EVENT_SYSTEM_FOREGROUND   },
        { EVENT_SYSTEM_MINIMIZEEND,  EVENT_SYSTEM_MINIMIZEEND  },
        { EVENT_OBJECT_SHOW,         EVENT_OBJECT_SHOW         },
        { EVENT_OBJECT_REORDER,      EVENT_OBJECT_REORDER      },
    };
    BOOL any = FALSE;
    int  i;

    for (i = 0; i < g_screenCount; i++)
        if (g_screens[i].overlay) any = TRUE;

    if (any && g_hookCount == 0) {
        for (i = 0; i < 4; i++) {
            HWINEVENTHOOK h = SetWinEventHook(evs[i][0], evs[i][1], NULL, WinEventProc, 0, 0,
                                              WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
            if (h) g_hooks[g_hookCount++] = h;
        }
    } else if (!any && g_hookCount > 0) {
        for (i = 0; i < g_hookCount; i++) UnhookWinEvent(g_hooks[i]);
        g_hookCount = 0;
        KillTimer(g_hwndMain, IDT_ZORDER);
        g_zorderPending = FALSE;
    }
}

/* ------------------------------------------------------------------ */
/* Screens (UI thread)                                                 */
/* ------------------------------------------------------------------ */

static void ReleaseScreens(void)
{
    int i;
    for (i = 0; i < g_screenCount; i++) DestroyOverlay(&g_screens[i]);
    g_screenCount = 0;
    UpdateZOrderHooks();
}

static BOOL CALLBACK ScreenEnumProc(HMONITOR hMon, HDC hdc, LPRECT clip, LPARAM data)
{
    MONITORINFOEXW mi;
    Screen *s;
    DWORD trim = 0;

    (void)hdc; (void)clip; (void)data;
    if (g_screenCount >= MAX_SCREENS) return FALSE;

    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, (MONITORINFO *)&mi)) return TRUE;

    s = &g_screens[g_screenCount++];
    ZeroMemory(s, sizeof(*s));
    s->bounds = mi.rcMonitor;
    lstrcpynW(s->device, mi.szDevice, 32);
    GetMonitorIdentity(s->device, s->name, 64, s->key, 160);
    if (RegGetDword(REG_TRIM_KEY, s->key, &trim) && trim <= (TRIM_CHOICES - 1) * TRIM_STEP)
        s->trim = (int)trim;
    return TRUE;
}

static void RebuildScreens(void)
{
    g_layoutGen++;
    ReleaseScreens();
    EnumDisplayMonitors(NULL, NULL, ScreenEnumProc, 0);
}

/* Resolves MODE_AUTO against the screens actually present; an explicit user
   choice is returned untouched. */
static BrightnessMode ResolveMode(void)
{
    if (g_modePref != MODE_AUTO) return g_modePref;
    return g_screenCount > 1 ? MODE_MATCHED : MODE_HARDWARE;
}

/* MODE_HARDWARE split of the dial: DDC target percent as the return value, and
   the level the overlay should show through *overlayLevel (100 = none). */
static int MapHardwarePercent(int level, int *overlayLevel)
{
    if (level <= OVERLAY_FLOOR_PCT) {
        *overlayLevel = (level * 100) / OVERLAY_FLOOR_PCT;
        return 0;
    }
    *overlayLevel = 100;
    return ((level - OVERLAY_FLOOR_PCT) * 100 + (100 - OVERLAY_FLOOR_PCT) / 2)
           / (100 - OVERLAY_FLOOR_PCT);
}

/* Whether every panel behind a screen obeys DDC/CI, per the worker's last
   report. Until the worker has reported (a fraction of a second), assume it
   does: guessing the other way double-dims every DDC monitor at each start
   for a visible flicker, while this way only a monitor without DDC waits that
   long for its overlay. */
static BOOL ScreenHasBacklightControl(const Screen *s)
{
    BOOL ok = TRUE;
    int  i;
    EnterCriticalSection(&g_shareLock);
    if (g_shareDdcValid) {
        ok = FALSE;
        for (i = 0; i < g_shareDdcCount; i++) {
            if (lstrcmpiW(g_shareDdc[i].device, s->device) == 0) {
                ok = g_shareDdc[i].allControllable;
                break;
            }
        }
    }
    LeaveCriticalSection(&g_shareLock);
    return ok;
}

/* Overlays are windows, so they are set on the UI thread. In MODE_MATCHED this
   is the entire visible effect of an adjustment - a SetLayeredWindowAttributes
   call per screen, microseconds, nothing that can block on I2C. */
static void ApplyOverlays(void)
{
    BOOL matched = (ResolveMode() == MODE_MATCHED);
    int  i;

    for (i = 0; i < g_screenCount; i++) {
        Screen *s = &g_screens[i];
        if (matched) {
            SetOverlay(s, g_level, s->trim);
        } else if (!ScreenHasBacklightControl(s)) {
            SetOverlay(s, g_level, 0);
        } else {
            int overlayLevel;
            MapHardwarePercent(g_level, &overlayLevel);
            SetOverlay(s, overlayLevel, 0);
        }
    }
    UpdateZOrderHooks();
    KeepOverlaysOnTop();
}

/* ------------------------------------------------------------------ */
/* DDC/CI (worker thread only)                                         */
/* ------------------------------------------------------------------ */

/* Raw VCP 0x10 first: it is what the MCCS standard defines, it is fast, and it
   works on monitors whose capability string is broken or incomplete (which is
   all the high-level API checks before refusing). */
static BOOL PanelRead(Panel *p, DWORD *cur)
{
    if (p->lowLevel) {
        MC_VCP_CODE_TYPE type;
        DWORD c = 0, mx = 0;
        if (!GetVCPFeatureAndVCPFeatureReply(p->h, VCP_BRIGHTNESS, &type, &c, &mx)) return FALSE;
        *cur = c;
        return TRUE;
    } else {
        DWORD mn = 0, c = 0, mx = 0;
        if (!GetMonitorBrightness(p->h, &mn, &c, &mx)) return FALSE;
        *cur = c;
        return TRUE;
    }
}

static BOOL PanelWrite(Panel *p, DWORD val)
{
    return p->lowLevel ? SetVCPFeature(p->h, VCP_BRIGHTNESS, val)
                       : SetMonitorBrightness(p->h, val);
}

static int RawToPct(const Panel *p, DWORD raw)
{
    DWORD span = p->maxB - p->minB;
    if (span == 0) return 100;
    if (raw < p->minB) raw = p->minB;
    if (raw > p->maxB) raw = p->maxB;
    return (int)(((raw - p->minB) * 100 + span / 2) / span);
}

static DWORD PctToRaw(const Panel *p, int pct)
{
    DWORD span = p->maxB - p->minB;
    DWORD v = p->minB + (DWORD)((span * (DWORD)pct + 50) / 100);
    return v > p->maxB ? p->maxB : v;
}

static void ReleasePanels(void)
{
    int i;
    for (i = 0; i < g_panelCount; i++) {
        PHYSICAL_MONITOR pm;
        pm.hPhysicalMonitor = g_panels[i].h;
        pm.szPhysicalMonitorDescription[0] = 0;
        DestroyPhysicalMonitors(1, &pm);
    }
    g_panelCount = 0;
}

static BOOL CALLBACK PanelEnumProc(HMONITOR hMon, HDC hdc, LPRECT clip, LPARAM data)
{
    MONITORINFOEXW mi;
    DWORD physCount = 0, i;
    PHYSICAL_MONITOR pm[MAX_PHYS_PER];
    WCHAR name[64], baseKey[160];

    (void)hdc; (void)clip; (void)data;

    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, (MONITORINFO *)&mi)) return TRUE;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &physCount) || physCount == 0)
        return TRUE;   /* RDP, virtual displays: overlay only, nothing to track */
    if (physCount > MAX_PHYS_PER) physCount = MAX_PHYS_PER;
    if (!GetPhysicalMonitorsFromHMONITOR(hMon, physCount, pm))
        return TRUE;   /* no handle = no DDC: its screen keeps the overlay */

    GetMonitorIdentity(mi.szDevice, name, 64, baseKey, 160);

    for (i = 0; i < physCount; i++) {
        Panel *p;
        MC_VCP_CODE_TYPE type;
        DWORD cur = 0, mx = 0, mn = 0;
        DWORD frozen = 0;

        if (g_panelCount >= MAX_PANELS) {
            /* Out of slots: release the rest rather than leak them. */
            DestroyPhysicalMonitors(physCount - i, &pm[i]);
            break;
        }

        p = &g_panels[g_panelCount++];
        ZeroMemory(p, sizeof(*p));
        p->h = pm[i].hPhysicalMonitor;
        p->lastWritten = MAXDWORD;
        lstrcpynW(p->device, mi.szDevice, 32);
        wsprintfW(p->key, L"%s#%u", baseKey, (unsigned)i);

        if (GetVCPFeatureAndVCPFeatureReply(p->h, VCP_BRIGHTNESS, &type, &cur, &mx) && mx > 0) {
            p->lowLevel = TRUE;
            p->minB = 0;
            p->maxB = mx;
        } else if (GetMonitorBrightness(p->h, &mn, &cur, &mx) && mx > mn) {
            p->lowLevel = FALSE;
            p->minB = mn;
            p->maxB = mx;
        } else {
            p->state = DDC_NONE;
            continue;
        }

        /* A panel already caught ignoring writes is not re-tested on every
           start - the test means writing a value it will not show, which on a
           matched setup briefly drives the other monitors to the wrong level.
           "Re-detect monitors" in the tray clears this. */
        if (!g_forgetFrozen && RegGetDword(REG_FROZEN_KEY, p->key, &frozen) && frozen) {
            p->state      = DDC_IGNORES_WRITES;
            p->stuckVal   = cur;
            p->stuckKnown = TRUE;
        } else {
            p->state = DDC_UNVERIFIED;
        }
    }
    return TRUE;
}

/* Tells the UI which screens have working backlight control. */
static void PublishPanels(void)
{
    int i, j;
    EnterCriticalSection(&g_shareLock);
    g_shareDdcCount = 0;
    for (i = 0; i < g_panelCount; i++) {
        Panel *p = &g_panels[i];
        BOOL ctl = (p->state == DDC_UNVERIFIED || p->state == DDC_WRITABLE);
        ScreenDdc *d = NULL;
        for (j = 0; j < g_shareDdcCount; j++)
            if (lstrcmpiW(g_shareDdc[j].device, p->device) == 0) { d = &g_shareDdc[j]; break; }
        if (!d) {
            if (g_shareDdcCount >= MAX_SCREENS) continue;
            d = &g_shareDdc[g_shareDdcCount++];
            lstrcpynW(d->device, p->device, 32);
            d->allControllable = TRUE;
        }
        if (!ctl) d->allControllable = FALSE;
    }
    g_shareDdcValid = TRUE;
    LeaveCriticalSection(&g_shareLock);
}

/* Brightest level any controllable panel currently shows, so a first run adopts
   where the hardware already is instead of yanking it to an assumed 100%. */
static int ProbePanelsLevel(void)
{
    int i, level = -1;
    for (i = 0; i < g_panelCount; i++) {
        Panel *p = &g_panels[i];
        DWORD cur;
        if (p->state != DDC_UNVERIFIED && p->state != DDC_WRITABLE) continue;
        if (PanelRead(p, &cur)) {
            int lv = RawToPct(p, cur);
            if (lv > level) level = lv;
        }
    }
    return level;
}

/* The common backlight level in MODE_MATCHED: what the dimmest write-ignoring
   panel is stuck showing, since nothing can raise it. A panel whose level could
   not be read is left out - assuming it sits at 0 would black out every other
   monitor's backlight on a guess. */
static int ComputeCeilingPct(void)
{
    int i, ceiling = 100;
    for (i = 0; i < g_panelCount; i++) {
        Panel *p = &g_panels[i];
        int pct;
        if (p->state != DDC_IGNORES_WRITES || !p->stuckKnown) continue;
        pct = RawToPct(p, p->stuckVal);
        if (pct < ceiling) ceiling = pct;
    }
    return ceiling;
}

static BOOL CloseEnough(const Panel *p, DWORD a, DWORD b)
{
    DWORD span = p->maxB - p->minB;
    DWORD tol  = span * DDC_VERIFY_TOLERANCE_PCT / 100;
    DWORD diff = a > b ? a - b : b - a;
    if (tol < 1) tol = 1;
    return diff <= tol;
}

/* One pass over every controllable panel: write the target, then confirm on
   the panels not yet known to obey. Returns TRUE if a panel was caught
   ignoring writes (the ceiling then changes, so the caller runs again). */
static BOOL DriveBacklights(int level, BOOL matched, int ceilingPct)
{
    DWORD before[MAX_PANELS];
    BOOL  verifying[MAX_PANELS];
    BOOL  anyVerify = FALSE, demoted = FALSE;
    int   i, attempt;

    for (i = 0; i < g_panelCount; i++) {
        Panel *p = &g_panels[i];
        DWORD target, cur = 0;
        int   pct, overlayLevel;

        verifying[i] = FALSE;
        if (p->state != DDC_UNVERIFIED && p->state != DDC_WRITABLE) continue;

        pct    = matched ? ceilingPct : MapHardwarePercent(level, &overlayLevel);
        target = PctToRaw(p, pct);

        /* Re-sending a value the panel already holds is pure I2C traffic. A
           read is cheaper and still catches the user having moved the panel
           with its own buttons, which would otherwise leave it out of step
           until the next layout change. */
        if (target == p->lastWritten) {
            DWORD now;
            if (!PanelRead(p, &now) || CloseEnough(p, now, target)) continue;
        }

        /* A write can only be judged if it should move the panel. When it
           already shows the target, a write-ignorer and an obedient panel look
           identical, so verification waits for a write that changes something. */
        if (p->state == DDC_UNVERIFIED) {
            if (PanelRead(p, &cur) && !CloseEnough(p, cur, target)) {
                verifying[i] = TRUE;
                before[i]    = cur;
                anyVerify    = TRUE;
            }
        }
        PanelWrite(p, target);
        p->lastWritten = target;
    }

    for (attempt = 0; anyVerify && attempt < DDC_VERIFY_ATTEMPTS; attempt++) {
        anyVerify = FALSE;
        Sleep(DDC_VERIFY_DELAY_MS);
        if (g_quitWorker) return FALSE;

        for (i = 0; i < g_panelCount; i++) {
            Panel *p = &g_panels[i];
            DWORD cur = 0;
            BOOL  readOk;
            if (!verifying[i]) continue;

            readOk = PanelRead(p, &cur);
            if (readOk && CloseEnough(p, cur, p->lastWritten)) {
                p->state = DDC_WRITABLE;
                verifying[i] = FALSE;
            } else if (attempt + 1 < DDC_VERIFY_ATTEMPTS) {
                PanelWrite(p, p->lastWritten);
                anyVerify = TRUE;
            } else {
                /* It really does ignore writes. Stop sending them, remember it,
                   and record what it is actually showing so the others can be
                   held to it. */
                p->state      = DDC_IGNORES_WRITES;
                p->stuckKnown = readOk;
                p->stuckVal   = readOk ? cur : before[i];
                p->lastWritten = MAXDWORD;
                verifying[i]  = FALSE;
                RegSetDword(REG_FROZEN_KEY, p->key, 1);
                demoted = TRUE;
            }
        }
    }
    return demoted;
}

static void WorkerRun(int level, BOOL matched)
{
    BOOL fresh = FALSE, demotedAny = FALSE;
    int  pass, i;

    if (InterlockedExchange(&g_panelsStale, 0)) {
        if (InterlockedExchange(&g_forgetFrozen, 0)) {
            HKEY key;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP_KEY, 0, KEY_ALL_ACCESS, &key) == ERROR_SUCCESS) {
                RegDeleteKeyW(key, L"IgnoresWrites");
                RegCloseKey(key);
            }
        }
        ReleasePanels();
        EnumDisplayMonitors(NULL, NULL, PanelEnumProc, 0);
        PublishPanels();
        fresh = TRUE;
    }

    /* The OSD buttons of a write-ignoring panel still work, so the level it is
       stuck at can change under us. Re-read it (read only) every run. */
    for (i = 0; i < g_panelCount; i++) {
        Panel *p = &g_panels[i];
        DWORD cur;
        if (p->state == DDC_IGNORES_WRITES && PanelRead(p, &cur)) {
            p->stuckVal   = cur;
            p->stuckKnown = TRUE;
        }
    }

    if (fresh && g_hwndMain) {
        PostMessageW(g_hwndMain, WM_PANELS_READY, (WPARAM)(INT_PTR)ProbePanelsLevel(), 0);
        if (g_adoptPending) return;   /* the UI answers with the level to drive */
    }

    /* A demotion changes the ceiling, so run once more with the new one rather
       than leaving the other panels at the stale level until the next key. */
    for (pass = 0; pass < 2 && !g_quitWorker; pass++) {
        if (!DriveBacklights(level, matched, ComputeCeilingPct())) break;
        demotedAny = TRUE;
        PublishPanels();
    }

    if (demotedAny && g_hwndMain)
        PostMessageW(g_hwndMain, WM_DDC_FALLBACK, 0, 0);
}

static DWORD WINAPI WorkerProc(LPVOID param)
{
    (void)param;
    for (;;) {
        WaitForSingleObject(g_wakeEvent, INFINITE);
        if (g_quitWorker) break;

        for (;;) {
            LONG lvl = InterlockedExchange(&g_reqLevel, -1);
            if (lvl < 0 || g_quitWorker) break;
            WorkerRun((int)lvl, g_reqMatched != 0);
        }
        if (g_quitWorker) break;
    }
    ReleasePanels();
    return 0;
}

static void QueueDdcSync(int level)
{
    if (!g_workerThread) {
        DWORD tid;
        if (!g_wakeEvent) {
            g_wakeEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
            if (!g_wakeEvent) return;
        }
        g_workerThread = CreateThread(NULL, 0, WorkerProc, NULL, 0, &tid);
        if (!g_workerThread) return;   /* g_wakeEvent kept for the next attempt */
    }
    InterlockedExchange(&g_reqMatched, ResolveMode() == MODE_MATCHED ? 1 : 0);
    InterlockedExchange(&g_reqLevel, (LONG)level);
    SetEvent(g_wakeEvent);
}

/* ------------------------------------------------------------------ */
/* Tray icon                                                           */
/* ------------------------------------------------------------------ */

/* A small sun, drawn at runtime at whatever size the shell wants, so the tray
   shows something recognisable without shipping a resource. */
static HICON CreateSunIcon(void)
{
    int        cx = GetSystemMetrics(SM_CXSMICON), cy = GetSystemMetrics(SM_CYSMICON);
    BITMAPINFO bi;
    void      *bits = NULL;
    HDC        dc;
    HBITMAP    color, mask, old;
    ICONINFO   ii;
    HICON      icon = NULL;
    HBRUSH     fill;
    HPEN       pen;
    int        i, r, cxm, cym;
    DWORD     *px;

    if (cx <= 0) cx = 16;
    if (cy <= 0) cy = 16;

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = cx;
    bi.bmiHeader.biHeight      = -cy;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    dc = CreateCompatibleDC(NULL);
    if (!dc) return NULL;
    color = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!color) { DeleteDC(dc); return NULL; }

    /* All-zero AND mask: the colour bitmap's own alpha decides transparency.
       CreateBitmap with no bits would leave the mask's contents undefined. */
    {
        int   stride = ((cx + 15) / 16) * 2;     /* mono rows are WORD-aligned */
        void *zeros  = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, (SIZE_T)(stride * cy));
        mask = zeros ? CreateBitmap(cx, cy, 1, 1, zeros) : NULL;
        if (zeros) HeapFree(GetProcessHeap(), 0, zeros);
    }

    old  = (HBITMAP)SelectObject(dc, color);
    fill = CreateSolidBrush(RGB(255, 196, 0));
    pen  = CreatePen(PS_SOLID, cx >= 24 ? 2 : 1, RGB(255, 196, 0));

    cxm = cx / 2; cym = cy / 2; r = cx * 5 / 16;
    SelectObject(dc, pen);
    for (i = 0; i < 8; i++) {
        static const int dx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
        static const int dy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
        MoveToEx(dc, cxm + dx[i] * cx / 32, cym + dy[i] * cy / 32, NULL);
        LineTo(dc, cxm + dx[i] * cx / 22, cym + dy[i] * cy / 22);
    }
    SelectObject(dc, fill);
    Ellipse(dc, cxm - r, cym - r, cxm + r, cym + r);
    SelectObject(dc, old);
    GdiFlush();

    /* GDI leaves alpha at 0; opaque wherever something was drawn. */
    px = (DWORD *)bits;
    for (i = 0; i < cx * cy; i++)
        if (px[i] & 0x00FFFFFF) px[i] |= 0xFF000000;

    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon    = TRUE;
    ii.hbmColor = color;
    ii.hbmMask  = mask;
    if (mask) icon = CreateIconIndirect(&ii);

    DeleteObject(fill);
    DeleteObject(pen);
    DeleteObject(color);
    if (mask) DeleteObject(mask);
    DeleteDC(dc);
    return icon;
}

static void FillTrayData(NOTIFYICONDATAW *nid)
{
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize           = sizeof(*nid);
    nid->hWnd             = g_hwndMain;
    nid->uID              = 1;
    nid->uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid->uCallbackMessage = WM_TRAYICON;
    nid->hIcon            = g_trayIcon ? g_trayIcon : LoadIconW(NULL, IDI_APPLICATION);
}

static void UpdateTrayTip(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    FillTrayData(&nid);
    wsprintfW(nid.szTip, L"Desktop Brightness: %d%%", g_level);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/* A silent failure the user cannot see is indistinguishable from the app being
   broken - every degraded path gets a balloon saying what happened. */
static void ShowBalloon(const WCHAR *title, const WCHAR *text)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    FillTrayData(&nid);
    nid.uFlags     |= NIF_INFO;
    nid.dwInfoFlags = NIIF_WARNING;
    lstrcpynW(nid.szInfoTitle, title, sizeof(nid.szInfoTitle) / sizeof(WCHAR));
    lstrcpynW(nid.szInfo,      text,  sizeof(nid.szInfo)      / sizeof(WCHAR));
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void AddTrayIcon(void)
{
    NOTIFYICONDATAW nid;
    FillTrayData(&nid);
    wsprintfW(nid.szTip, L"Desktop Brightness: %d%%", g_level);
    g_trayAdded = Shell_NotifyIconW(NIM_ADD, &nid) ? TRUE : FALSE;
}

static void RemoveTrayIcon(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    FillTrayData(&nid);
    Shell_NotifyIconW(NIM_DELETE, &nid);
    g_trayAdded = FALSE;
}

static void ShowTrayMenu(void)
{
    HMENU menu = CreatePopupMenu();
    HMENU modes, balance = NULL;
    POINT pt;
    WCHAR title[64];
    BrightnessMode effective = ResolveMode();
    int   i, c;
    UINT  cmd, layoutGen;

    if (!menu) return;

    modes = CreatePopupMenu();
    if (modes) {
        /* Auto is annotated with what it currently resolves to, so the menu
           answers "which one am I actually on" without a second click. */
        AppendMenuW(modes, MF_STRING | (g_modePref == MODE_AUTO ? MF_CHECKED : 0),
                    IDM_MODE_AUTO,
                    effective == MODE_MATCHED ? L"Automatic (now: match all monitors)"
                                              : L"Automatic (now: hardware backlight)");
        AppendMenuW(modes, MF_SEPARATOR, 0, NULL);
        AppendMenuW(modes, MF_STRING | (g_modePref == MODE_MATCHED ? MF_CHECKED : 0),
                    IDM_MODE_MATCH, L"Match all monitors");
        AppendMenuW(modes, MF_STRING | (g_modePref == MODE_HARDWARE ? MF_CHECKED : 0),
                    IDM_MODE_HW,    L"Hardware backlight (each monitor on its own)");
    }

    /* Balance only means something when there is more than one screen to match
       and the screens are being matched. */
    if (g_screenCount > 1 && effective == MODE_MATCHED) {
        balance = CreatePopupMenu();
        for (i = 0; balance && i < g_screenCount; i++) {
            HMENU sub = CreatePopupMenu();
            WCHAR label[128];
            if (!sub) continue;
            for (c = 0; c < TRIM_CHOICES; c++) {
                WCHAR item[48];
                if (c == 0) lstrcpyW(item, L"Same as the dial");
                else        wsprintfW(item, L"%d%% dimmer", c * TRIM_STEP);
                AppendMenuW(sub, MF_STRING | (g_screens[i].trim == c * TRIM_STEP ? MF_CHECKED : 0),
                            IDM_TRIM_BASE + i * TRIM_CHOICES + c, item);
            }
            wsprintfW(label, L"%d. %s  (%dx%d)%s", i + 1, g_screens[i].name,
                      (int)(g_screens[i].bounds.right - g_screens[i].bounds.left),
                      (int)(g_screens[i].bounds.bottom - g_screens[i].bounds.top),
                      g_screens[i].trim ? L"  *" : L"");
            AppendMenuW(balance, MF_POPUP, (UINT_PTR)sub, label);
        }
    }

    wsprintfW(title, L"Desktop Brightness %s  -  %d%%", APP_VERSION, g_level);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, title);
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (modes)   AppendMenuW(menu, MF_POPUP, (UINT_PTR)modes,   L"Dimming mode");
    if (balance) AppendMenuW(menu, MF_POPUP, (UINT_PTR)balance, L"Balance monitors");
    AppendMenuW(menu, MF_STRING, IDM_REDETECT, L"Re-detect monitors");
    AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, IDM_RESET, L"Reset to 100%");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    GetCursorPos(&pt);
    /* Required so the menu dismisses when the user clicks elsewhere. */
    SetForegroundWindow(g_hwndMain);
    layoutGen = g_layoutGen;
    cmd = (UINT)TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RETURNCMD | TPM_NONOTIFY,
                               pt.x, pt.y, 0, g_hwndMain, NULL);
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);
    DestroyMenu(menu);   /* destroys the submenus with it */

    /* The menu loop keeps dispatching messages, so a display change can
       rebuild the screen list while it is open. Balance items name a screen by
       its position in that list - drop the choice rather than apply it to
       whichever monitor now holds that position. */
    if (cmd >= IDM_TRIM_BASE && layoutGen != g_layoutGen) cmd = 0;
    if (cmd) SendMessageW(g_hwndMain, WM_COMMAND, cmd, 0);
}

/* ------------------------------------------------------------------ */
/* Brightness application                                              */
/* ------------------------------------------------------------------ */

/* The slow tail, run once things stop moving. */
static void Settle(void)
{
    KillTimer(g_hwndMain, IDT_SETTLE);
    UpdateTrayTip();
    if (g_savePending) {
        g_savePending = FALSE;
        RegSetDword(REG_APP_KEY, L"Level", (DWORD)g_level);
    }
    QueueDdcSync(g_level);
}

/* Non-keypress callers (startup, display change, reset) are one-shot, so they
   take the slow tail immediately rather than waiting on the timer. */
static void ApplyLevelNow(BOOL persist)
{
    if (persist) g_savePending = TRUE;
    ApplyOverlays();
    Settle();
}

static void AdjustBrightness(int delta)
{
    int next = ClampLevel(g_level + delta);
    if (next == g_level) return;   /* already at a rail - skip the whole pipeline */

    g_level        = next;
    g_savePending  = TRUE;
    g_levelTouched = TRUE;

    ApplyOverlays();

    /* In MODE_MATCHED the backlight does not depend on the level, so nothing
       goes to hardware until the settle timer. In MODE_HARDWARE the backlight
       write *is* the visible change and goes out now - the hotkey coalescing
       in MainWndProc stops a held key queueing up behind I2C round-trips. */
    if (ResolveMode() != MODE_MATCHED)
        QueueDdcSync(g_level);

    SetTimer(g_hwndMain, IDT_SETTLE, SETTLE_DELAY_MS, NULL);
}

/* Monitors added, removed, rearranged, re-powered: rebuild the screens here and
   have the worker reopen its physical handles, which go stale across all of
   these. */
static void OnDisplayLayoutChanged(void)
{
    /* The worker's per-screen report is keyed by GDI name, and those names can
       move to different monitors in a new layout - drop it until it reports. */
    EnterCriticalSection(&g_shareLock);
    g_shareDdcValid = FALSE;
    LeaveCriticalSection(&g_shareLock);

    RebuildScreens();
    InterlockedExchange(&g_panelsStale, 1);
    g_toldAboutFallback = FALSE;   /* new layout - a new warning is warranted */
    ApplyLevelNow(FALSE);
}

/* ------------------------------------------------------------------ */
/* Main window                                                         */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarMsg && g_taskbarMsg != 0) {
        /* Explorer restarted and dropped every tray icon - put ours back. */
        g_trayAdded = FALSE;
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
    case WM_HOTKEY: {
        int delta;
        MSG peek;

        if      (wp == HOTKEY_UP_ID) delta = +g_step;
        else if (wp == HOTKEY_DN_ID) delta = -g_step;
        else return 0;

        /* Autorepeat posts WM_HOTKEY every ~32 ms and does not stop the instant
           the key comes up. Folding the queued repeats into this one delta
           means the level lands where the key was released. */
        while (PeekMessageW(&peek, hwnd, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
            if      (peek.wParam == HOTKEY_UP_ID) delta += g_step;
            else if (peek.wParam == HOTKEY_DN_ID) delta -= g_step;
        }

        AdjustBrightness(delta);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_SETTLE) {
            Settle();
        } else if (wp == IDT_ZORDER) {
            KillTimer(hwnd, IDT_ZORDER);
            g_zorderPending = FALSE;
            KeepOverlaysOnTop();
        }
        return 0;

    case WM_DISPLAYCHANGE:
        OnDisplayLayoutChanged();
        return 0;

    case WM_POWERBROADCAST:
        /* After sleep the monitors have been power-cycled: their DDC handles
           are stale and some panels come back at a different level. */
        if (wp == PBT_APMRESUMEAUTOMATIC || wp == PBT_APMRESUMESUSPEND)
            OnDisplayLayoutChanged();
        return TRUE;

    case WM_PANELS_READY: {
        /* First enumeration on a machine with no saved level: adopt where the
           hardware already is, but only where that number means something (a
           single screen in hardware mode) and only if the user has not already
           pressed a key. */
        int  probed     = (int)(INT_PTR)wp;
        BOOL wasPending = InterlockedExchange(&g_adoptPending, 0) != 0;
        if (wasPending && !g_levelTouched && probed >= 0 && ResolveMode() == MODE_HARDWARE)
            g_level = ClampLevel(probed);
        ApplyOverlays();
        UpdateTrayTip();
        if (wasPending) QueueDdcSync(g_level);   /* the worker held back for this */
        return 0;
    }

    case WM_DDC_FALLBACK:
        ApplyOverlays();
        if (!g_toldAboutFallback) {
            g_toldAboutFallback = TRUE;
            ShowBalloon(L"A monitor ignored brightness control",
                        L"It accepted DDC/CI brightness commands but did not act on "
                        L"them, so it is dimmed in software instead and the other "
                        L"monitors are held to its level so they still match. If its "
                        L"on-screen menu has a DDC/CI option, switch it on and use "
                        L"Re-detect monitors in this tray menu.");
        }
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu();
        return 0;

    case WM_COMMAND: {
        UINT id = LOWORD(wp);
        if (id >= IDM_TRIM_BASE && id < IDM_TRIM_BASE + MAX_SCREENS * TRIM_CHOICES) {
            int scr = (int)(id - IDM_TRIM_BASE) / TRIM_CHOICES;
            int c   = (int)(id - IDM_TRIM_BASE) % TRIM_CHOICES;
            if (scr < g_screenCount) {
                g_screens[scr].trim = c * TRIM_STEP;
                if (c == 0) RegDeleteName(REG_TRIM_KEY, g_screens[scr].key);
                else        RegSetDword(REG_TRIM_KEY, g_screens[scr].key, (DWORD)(c * TRIM_STEP));
                ApplyOverlays();
            }
            return 0;
        }
        switch (id) {
        case IDM_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            return 0;
        case IDM_MODE_AUTO:
        case IDM_MODE_HW:
        case IDM_MODE_MATCH:
            g_modePref = (id == IDM_MODE_AUTO) ? MODE_AUTO
                       : (id == IDM_MODE_HW)   ? MODE_HARDWARE
                                               : MODE_MATCHED;
            RegSetDword(REG_APP_KEY, L"Mode", (DWORD)g_modePref);
            ApplyLevelNow(FALSE);
            return 0;
        case IDM_REDETECT:
            InterlockedExchange(&g_forgetFrozen, 1);
            OnDisplayLayoutChanged();
            return 0;
        case IDM_RESET:
            g_level = 100;
            g_levelTouched = TRUE;
            ApplyLevelNow(TRUE);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;
    }

    case WM_DESTROY:
        /* An adjustment may still be sitting on the settle timer - flush it,
           or the last thing the user did is the one thing not remembered. */
        KillTimer(hwnd, IDT_SETTLE);
        KillTimer(hwnd, IDT_ZORDER);
        if (g_savePending) {
            g_savePending = FALSE;
            RegSetDword(REG_APP_KEY, L"Level", (DWORD)g_level);
        }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL RegisterClasses(void)
{
    WNDCLASSEXW wc;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = L"BrightMain";
    if (!RegisterClassExW(&wc)) return FALSE;

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = OverlayWndProc;
    wc.hInstance     = g_hInst;
    wc.lpszClassName = L"BrightDimmer";
    if (!RegisterClassExW(&wc)) return FALSE;

    return TRUE;
}

/* Without this the process is DPI-virtualised: GetMonitorInfo hands back scaled
   coordinates and the overlay ends up covering only part of a high-DPI panel.
   Resolved at runtime so the exe still loads on builds without the newer call. */
static void EnableDpiAwareness(void)
{
    typedef BOOL (WINAPI *PFN_SetCtx)(HANDLE);
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    PFN_SetCtx setCtx = u32
        ? (PFN_SetCtx)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext")
        : NULL;

    /* -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 */
    if (!setCtx || !setCtx((HANDLE)(INT_PTR)-4))
        SetProcessDPIAware();

    /* Undocumented but stable since Windows 8; used only to skip windows in the
       shell's own higher z-bands, which no app window can cover. */
    if (u32)
        g_getWindowBand = (PFN_GetWindowBand)(void *)GetProcAddress(u32, "GetWindowBand");
}

static void Cleanup(void)
{
    BOOL workerStopped = TRUE;

    UnregisterHotKey(g_hwndMain, HOTKEY_UP_ID);
    UnregisterHotKey(g_hwndMain, HOTKEY_DN_ID);
    RemoveTrayIcon();
    ReleaseScreens();

    if (g_workerThread) {
        InterlockedExchange(&g_quitWorker, 1);
        SetEvent(g_wakeEvent);
        workerStopped = (WaitForSingleObject(g_workerThread, 5000) == WAIT_OBJECT_0);
        CloseHandle(g_workerThread);
        g_workerThread = NULL;
    }
    if (g_wakeEvent) {
        CloseHandle(g_wakeEvent);
        g_wakeEvent = NULL;
    }
    if (g_trayIcon) {
        DestroyIcon(g_trayIcon);
        g_trayIcon = NULL;
    }

    /* If the worker is somehow still inside a DDC round-trip, deleting the lock
       underneath it would crash on the way out. The process is exiting anyway -
       leaking is the safer of the two. */
    if (workerStopped) DeleteCriticalSection(&g_shareLock);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    HANDLE mutex;
    MSG    msg;
    BOOL   hotkeysOk;

    (void)hPrev; (void)cmdLine; (void)nShow;
    g_hInst = hInst;

    EnableDpiAwareness();

    /* Per-session, not Global: one instance per logged-in user is the intent. */
    mutex = CreateMutexW(NULL, TRUE, L"Local\\DesktopBrightnessApp_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    LoadSettings();
    InitializeCriticalSection(&g_shareLock);

    if (!RegisterClasses()) {
        CloseHandle(mutex);
        return 1;
    }

    /* A normal (never shown) window rather than HWND_MESSAGE: message-only
       windows do not receive broadcasts, and WM_DISPLAYCHANGE, power events and
       TaskbarCreated are all broadcasts. */
    g_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW, L"BrightMain", L"DesktopBrightness",
                                 WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwndMain) {
        CloseHandle(mutex);
        return 1;
    }

    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");
    g_trayIcon   = CreateSunIcon();
    AddTrayIcon();

    hotkeysOk  = RegisterHotKey(g_hwndMain, HOTKEY_UP_ID, g_hotkeyMod, g_hotkeyUp) ? TRUE : FALSE;
    hotkeysOk &= RegisterHotKey(g_hwndMain, HOTKEY_DN_ID, g_hotkeyMod, g_hotkeyDn) ? TRUE : FALSE;

    if (!hotkeysOk)
        ShowBalloon(L"Hotkeys unavailable",
                    L"The brightness hotkeys could not be registered - another "
                    L"running application already owns that combination. They "
                    L"can be changed under HKCU\\Software\\DesktopBrightness "
                    L"(see the README); brightness can still be reset from this "
                    L"tray icon meanwhile.");

    /* If the exe was moved after "Start with Windows" was ticked, the Run
       entry points at nothing and the app would silently stop starting at
       logon - repair it to this copy. */
    if (IsAutoStartEnabled() && AutoStartTargetMissing()) SetAutoStart(TRUE);

    /* Screens and overlays come up now; the worker opens the physical monitors
       in the background and reports back with WM_PANELS_READY. A saved level
       is restored immediately - overlay dimming is a window, so unlike DDC/CI
       it does not survive the process exiting. */
    if (!g_levelSaved) InterlockedExchange(&g_adoptPending, 1);
    RebuildScreens();
    ApplyLevelNow(FALSE);

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Cleanup();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
