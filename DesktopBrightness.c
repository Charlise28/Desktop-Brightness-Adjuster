/*
 * DesktopBrightness.c
 * Desktop Brightness Adjuster - native Win32, external monitors over DDC/CI.
 *
 * Links only against DLLs Windows has already loaded
 * (user32/gdi32/shell32/dxva2/advapi32), so process start costs nothing beyond
 * the exe itself - the point of dropping the earlier .NET/WinForms build, which
 * pulled in the CLR at launch and was rated "High" startup impact no matter how
 * much work was deferred inside the app.
 *
 * Scope: desktop monitors on DDC/CI. Laptop internal panels use a different
 * mechanism (WMI) and already have working brightness keys; they are not a
 * target here. A laptop's external monitors still work.
 *
 * ------------------------------------------------------------------
 * Two strategies, chosen from what the hardware can actually do
 * ------------------------------------------------------------------
 * Monitors vary wildly, and a surprising number of panels accept a DDC/CI
 * brightness write, report success, and then ignore it. A panel like that is
 * frozen at whatever its own on-screen menu is set to, and no software can
 * raise it. So the strategy has to depend on the hardware present:
 *
 *   MODE_HARDWARE - every monitor accepts real brightness writes.
 *     Drive each backlight across its own reported [min..max]. This is real
 *     dimming: less backlight power, full contrast retained. Below
 *     OVERLAY_FLOOR_PCT the panel is held at its true minimum and a dimming
 *     overlay continues past it, so a monitor whose own floor is still bright
 *     can reach a properly dark level.
 *
 *   MODE_MATCHED - at least one monitor is frozen, and at least one is not.
 *     A frozen panel caps what the set can agree on, so every controllable
 *     monitor is pinned to that same fixed percent and the whole dial is
 *     realized by the software overlay instead. Every monitor then gets an
 *     identical alpha, which is what makes mismatched panels track each other
 *     at every step. Backlight PWM curves differ per panel, so driving them by
 *     DDC and hoping they agree only ever matches at the two ends of the dial.
 *
 * Mode is auto-selected per monitor layout and can be overridden from the tray
 * menu; the choice is remembered. If no monitor supports DDC/CI at all, the
 * overlay does everything and the distinction does not arise.
 *
 * Build: build_brightness_app.ps1
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0A00

#include <windows.h>
#include <shellapi.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

#define HOTKEY_UP_ID   9001
#define HOTKEY_DN_ID   9002

/* Defaults only. Alt+PgUp/PgDn is free on a stock Windows install but plenty of
   apps claim it, and key preference is personal, so all four are overridable
   from the registry - see LoadSettings and the README. */
#define DEF_HOTKEY_MOD MOD_ALT
#define DEF_HOTKEY_UP  VK_PRIOR   /* Page Up   */
#define DEF_HOTKEY_DN  VK_NEXT    /* Page Down */
#define DEF_STEP       5          /* percent per keypress - fast enough to notice,
                                      still fine enough to dial in. No HUD reads it
                                      back out loud; the eye is the only judge. */
#define MAX_MONITORS   16
#define MAX_PHYS_PER   8

#define WM_TRAYICON    (WM_APP + 1)
#define WM_DDC_FALLBACK (WM_APP + 2)  /* worker found a panel that ignores writes */

/* One-off settle time before reading a write back. Paid once per adjustment
   round, not once per monitor - see ApplyDdcBrightness. */
#define DDC_VERIFY_DELAY_MS 120

/* A single dropped I2C round-trip is not proof of a broken panel. Give the
   monitor this many write+read attempts before writing it off as unwritable,
   otherwise one flaky reply permanently disables real backlight control. */
#define DDC_VERIFY_ATTEMPTS 3

#define IDM_AUTOSTART  100
#define IDM_RESET      101
#define IDM_EXIT       102
#define IDM_MODE_AUTO  103
#define IDM_MODE_HW    104
#define IDM_MODE_MATCH 105

/* A keypress must only ever touch overlay alpha. Everything slower - the tray
   IPC round-trip, the registry write, and above all the DDC/CI traffic at
   ~60 ms per transaction - is coalesced onto this timer, which restarts on
   every adjustment. Holding a key therefore pays that cost once, after
   release, instead of once per repeat. */
#define IDT_SETTLE       1
#define SETTLE_DELAY_MS  250

/* Overlay can never go fully black, or a non-DDC monitor would be unusable. */
#define OVERLAY_MAX_ALPHA 0.85

/* Bottom slice of the dial reserved for the overlay in MODE_HARDWARE. Plenty of
   panels are still uncomfortably bright at their own DDC minimum; below this
   point the backlight sits at that minimum and the overlay keeps going. */
#define OVERLAY_FLOOR_PCT 20

#define REG_APP_KEY    L"Software\\DesktopBrightness"
#define REG_RUN_KEY    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define REG_RUN_VALUE  L"DesktopBrightness"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/* Some panels answer GetMonitorBrightness happily and then ignore every write,
   returning success with no error. Reads alone therefore prove nothing - the
   first write to each monitor is read back to find out what it really does. */
typedef enum { DDC_UNVERIFIED = 0, DDC_WRITABLE, DDC_IGNORES_WRITES } DdcWriteState;

/* What the user asked for. MODE_AUTO resolves against the monitors actually
   present - see ResolveMode - and is the only value that adapts to a layout
   change; the other two are held even if the hardware would suggest otherwise,
   because a user who overrode the choice meant it. */
typedef enum { MODE_AUTO = 0, MODE_HARDWARE, MODE_MATCHED } BrightnessMode;

typedef struct {
    HANDLE hPhysical;   /* DDC handle - note 0 is a VALID value, see hasPhysical */
    BOOL   hasPhysical; /* whether hPhysical holds a real handle to destroy   */
    BOOL   ddcOk;       /* cleared if the panel turns out to ignore writes   */
    DdcWriteState writeState;
    DWORD  minB, maxB;  /* panel's own brightness range, only valid if ddcOk */
    DWORD  pendingVal;  /* raw value last written, awaiting read-back        */
    DWORD  stuckVal;    /* raw value it's frozen at, only if DDC_IGNORES_WRITES */
    RECT   bounds;      /* virtual-desktop coords, used to place an overlay  */
    HWND   overlay;     /* fallback dimmer, only for !ddcOk monitors         */
} MonitorInfo;

static MonitorInfo      g_mons[MAX_MONITORS];
static int              g_monCount  = 0;
static BOOL             g_monsValid = FALSE;
static CRITICAL_SECTION g_monLock;   /* guards g_mons: UI thread rebuilds, worker reads */

static HINSTANCE g_hInst      = NULL;
static HWND      g_hwndMain   = NULL;
static BOOL      g_trayAdded  = FALSE;
static UINT      g_taskbarMsg = 0;   /* "TaskbarCreated" - re-add icon if Explorer restarts */
static DWORD     g_uiThreadId = 0;   /* only this thread may destroy overlay windows */

static int  g_level     = 100;
static BOOL g_initDone  = FALSE;     /* deferred: registry read + monitor enumeration */
static BOOL g_levelSaved = FALSE;    /* level came from the registry, not a hw probe */
static BOOL g_savePending = FALSE;   /* an adjustment is waiting to be written out */

static BrightnessMode g_modePref = MODE_AUTO;  /* what the user picked */

/* Runtime-configurable so a hotkey clash or a taste in step size does not need
   a rebuild. Loaded once at startup - see LoadSettings. */
static UINT g_hotkeyMod = DEF_HOTKEY_MOD;
static UINT g_hotkeyUp  = DEF_HOTKEY_UP;
static UINT g_hotkeyDn  = DEF_HOTKEY_DN;
static int  g_step      = DEF_STEP;
static BOOL g_toldAboutFallback = FALSE; /* balloon shown once per monitor layout */

/* Worker thread: DDC/CI writes are slow I2C round-trips, so they never run on
   the UI thread. g_pendingLevel is "latest value wins" - rapid keypresses
   coalesce, but the final value is always applied (the old C# throttle could
   drop the last one, leaving hardware and OSD disagreeing). */
static volatile LONG g_pendingLevel = -1;
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

static void SaveLevel(int level)
{
    HKEY  key;
    DWORD val = (DWORD)level;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"Level", 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
        RegCloseKey(key);
    }
}

static BOOL LoadLevel(int *out)
{
    HKEY  key;
    DWORD val = 100, size = sizeof(val), type = 0;
    BOOL  found = FALSE;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP_KEY, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, L"Level", NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS
            && type == REG_DWORD) {
            *out  = ClampLevel((int)val);
            found = TRUE;
        }
        RegCloseKey(key);
    }
    return found;
}

/* Reads a DWORD from the app's key, leaving *out alone if absent or wrong type,
   so every setting keeps its compiled-in default until deliberately overridden. */
static void LoadDword(HKEY key, const WCHAR *name, DWORD *out)
{
    DWORD val = 0, size = sizeof(val), type = 0;
    if (RegQueryValueExW(key, name, NULL, &type, (BYTE *)&val, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        *out = val;
}

static void SaveMode(BrightnessMode mode)
{
    HKEY  key;
    DWORD val = (DWORD)mode;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, REG_APP_KEY, 0, NULL, 0,
                        KEY_SET_VALUE, NULL, &key, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(key, L"Mode", 0, REG_DWORD, (const BYTE *)&val, sizeof(val));
        RegCloseKey(key);
    }
}

static void LoadSettings(void)
{
    HKEY  key;
    DWORD mod = g_hotkeyMod, up = g_hotkeyUp, dn = g_hotkeyDn;
    DWORD step = (DWORD)g_step, mode = (DWORD)g_modePref;

    if (RegOpenKeyExW(HKEY_CURRENT_USER, REG_APP_KEY, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return;

    LoadDword(key, L"HotkeyMod",    &mod);
    LoadDword(key, L"HotkeyVkUp",   &up);
    LoadDword(key, L"HotkeyVkDown", &dn);
    LoadDword(key, L"Step",         &step);
    LoadDword(key, L"Mode",         &mode);
    RegCloseKey(key);

    /* A zero modifier is legal to RegisterHotKey but would swallow a bare key
       system-wide, and a step outside 1..50 makes the dial useless - ignore
       either rather than honouring an obviously broken value. */
    if (mod != 0)               g_hotkeyMod = (UINT)mod;
    if (up  != 0)               g_hotkeyUp  = (UINT)up;
    if (dn  != 0)               g_hotkeyDn  = (UINT)dn;
    if (step >= 1 && step <= 50) g_step     = (int)step;
    if (mode <= (DWORD)MODE_MATCHED) g_modePref = (BrightnessMode)mode;
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

/* ------------------------------------------------------------------ */
/* Monitor discovery                                                   */
/* ------------------------------------------------------------------ */

static void DestroyOverlay(MonitorInfo *m)
{
    if (!m->overlay) return;

    /* A window may only be destroyed by the thread that created it; the worker
       calling DestroyWindow here would fail silently and strand the overlay on
       screen. Hand it back to the UI thread instead - DefWindowProc turns
       WM_CLOSE into a DestroyWindow on the owning thread. */
    if (GetCurrentThreadId() == g_uiThreadId)
        DestroyWindow(m->overlay);
    else
        PostMessageW(m->overlay, WM_CLOSE, 0, 0);

    m->overlay = NULL;
}

/* Caller must hold g_monLock. */
static void ReleaseMonitors(void)
{
    int i;
    for (i = 0; i < g_monCount; i++) {
        DestroyOverlay(&g_mons[i]);
        if (g_mons[i].hasPhysical) {
            PHYSICAL_MONITOR pm;
            pm.hPhysicalMonitor = g_mons[i].hPhysical;
            pm.szPhysicalMonitorDescription[0] = 0;
            DestroyPhysicalMonitors(1, &pm);
            g_mons[i].hPhysical   = NULL;
            g_mons[i].hasPhysical = FALSE;
        }
    }
    g_monCount  = 0;
    g_monsValid = FALSE;
}

static BOOL CALLBACK MonitorEnumProc(HMONITOR hMon, HDC hdc, LPRECT clip, LPARAM data)
{
    MONITORINFO mi;
    DWORD       physCount = 0;

    (void)hdc; (void)clip; (void)data;

    if (g_monCount >= MAX_MONITORS) return FALSE;

    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return TRUE;

    if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, &physCount) && physCount > 0) {
        PHYSICAL_MONITOR pm[MAX_PHYS_PER];
        DWORD i;

        if (physCount > MAX_PHYS_PER) physCount = MAX_PHYS_PER;

        if (GetPhysicalMonitorsFromHMONITOR(hMon, physCount, pm)) {
            for (i = 0; i < physCount; i++) {
                MonitorInfo *m;
                DWORD mn = 0, cur = 0, mx = 0;

                if (g_monCount >= MAX_MONITORS) {
                    /* Out of slots: the handles past this point will never be
                       tracked, so release them here rather than leak them. */
                    DestroyPhysicalMonitors(physCount - i, &pm[i]);
                    break;
                }

                m = &g_mons[g_monCount++];
                m->hPhysical   = pm[i].hPhysicalMonitor;
                m->hasPhysical = TRUE;
                m->bounds      = mi.rcMonitor;
                m->overlay     = NULL;
                m->writeState  = DDC_UNVERIFIED;
                m->minB        = 0;
                m->maxB        = 0;
                m->pendingVal  = 0;
                m->stuckVal    = 0;

                /* The only reliable capability test is asking for the range.
                   The old C# build imported this call but never made it, then
                   assumed every panel used 0..100 - wrong on any monitor that
                   reports a different scale. */
                if (GetMonitorBrightness(pm[i].hPhysicalMonitor, &mn, &cur, &mx) && mx > mn) {
                    m->ddcOk = TRUE;
                    m->minB  = mn;
                    m->maxB  = mx;
                } else {
                    m->ddcOk = FALSE;
                }
            }
        }
    } else {
        /* No physical monitor behind this HMONITOR (RDP, some virtual displays):
           still track it so the overlay fallback can cover it. */
        MonitorInfo *m = &g_mons[g_monCount++];
        m->hPhysical   = NULL;
        m->hasPhysical = FALSE;
        m->ddcOk       = FALSE;
        m->bounds      = mi.rcMonitor;
        m->overlay     = NULL;
        m->writeState  = DDC_IGNORES_WRITES;
        m->minB        = 0;
        m->maxB        = 0;
        m->pendingVal  = 0;
        m->stuckVal    = 0;
    }
    return TRUE;
}

/* Caller must hold g_monLock. */
static void EnsureMonitors(void)
{
    if (g_monsValid) return;
    ReleaseMonitors();
    EnumDisplayMonitors(NULL, NULL, MonitorEnumProc, 0);
    g_monsValid = TRUE;
}

/* Caller must hold g_monLock. Resolves MODE_AUTO against the monitors actually
   present; an explicit user choice is returned untouched.

   Matching only becomes a problem when the set is mixed: one monitor frozen and
   another still controllable will drift apart unless the controllable one is
   held back. If every monitor is controllable there is nothing to work around,
   so real backlight control wins. If none are, the overlay is doing everything
   on every monitor regardless and the modes coincide. */
static BrightnessMode ResolveMode(void)
{
    int i, controllable = 0, frozen = 0;

    if (g_modePref != MODE_AUTO) return g_modePref;

    for (i = 0; i < g_monCount; i++) {
        MonitorInfo *m = &g_mons[i];
        if (m->ddcOk && m->hasPhysical) controllable++;
        else                            frozen++;
    }

    return (controllable > 0 && frozen > 0) ? MODE_MATCHED : MODE_HARDWARE;
}

/* Caller must hold g_monLock. The percent a write-ignoring panel is frozen
   showing - every other monitor is capped to this, so a panel that cannot be
   turned down any further never gets left looking brighter than the rest. */
static int ComputeCeilingPct(void)
{
    int i, ceiling = 100;
    for (i = 0; i < g_monCount; i++) {
        MonitorInfo *m = &g_mons[i];
        DWORD span = m->maxB - m->minB;
        int   pct;

        if (m->writeState != DDC_IGNORES_WRITES || !m->hasPhysical || span == 0)
            continue;

        pct = (int)(((m->stuckVal - m->minB) * 100 + span / 2) / span);
        if (pct < 0)   pct = 0;
        if (pct > 100) pct = 100;
        if (pct < ceiling) ceiling = pct;
    }
    return ceiling;
}

/* MODE_HARDWARE split of the dial: DDC target percent as the return value, and
   the level to hand UpdateOverlay through *overlayLevel (100 = no overlay).
   Above the floor the backlight does the work alone; below it the panel sits at
   its own minimum and the overlay carries on into the dark. */
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

/* ------------------------------------------------------------------ */
/* Overlay fallback window                                             */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK OverlayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_ERASEBKGND) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, (HBRUSH)GetStockObject(BLACK_BRUSH));
        return 1;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void UpdateOverlay(MonitorInfo *m, int level)
{
    BYTE alpha;
    double f;

    if (level >= 100) {           /* nothing to dim - release the window entirely */
        DestroyOverlay(m);
        return;
    }

    f     = (100 - level) / 100.0 * OVERLAY_MAX_ALPHA;
    alpha = (BYTE)(f * 255.0 + 0.5);

    if (!m->overlay) {
        m->overlay = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE |
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
            L"BrightDimmer", NULL, WS_POPUP,
            m->bounds.left, m->bounds.top,
            m->bounds.right - m->bounds.left,
            m->bounds.bottom - m->bounds.top,
            NULL, NULL, g_hInst, NULL);

        if (!m->overlay) return;

        /* Alpha is set before the window is ever shown. The old C# overlay
           called Show() while still at Opacity 1.0, flashing the screen
           fully black for a frame before the dim level was applied. */
        SetLayeredWindowAttributes(m->overlay, 0, alpha, LWA_ALPHA);
        SetWindowDisplayAffinity(m->overlay, WDA_EXCLUDEFROMCAPTURE);
        ShowWindow(m->overlay, SW_SHOWNOACTIVATE);
    } else {
        SetLayeredWindowAttributes(m->overlay, 0, alpha, LWA_ALPHA);
    }
}

/* ------------------------------------------------------------------ */
/* DDC/CI worker thread                                                */
/* ------------------------------------------------------------------ */

static void ApplyDdcBrightness(int level)
{
    int  i, attempt, ceilingPct;
    BOOL demoted      = FALSE;
    BOOL anyUnverified = FALSE;
    BOOL matched;

    EnterCriticalSection(&g_monLock);
    EnsureMonitors();

    /* Based on whichever monitors were already known stuck as of the previous
       adjustment - see the refresh loop below for why that lag is fine. */
    ceilingPct = ComputeCeilingPct();
    matched    = (ResolveMode() == MODE_MATCHED);

    for (i = 0; i < g_monCount; i++) {
        MonitorInfo *m = &g_mons[i];
        DWORD span;
        int   targetPct, overlayLevel;

        if (!m->ddcOk || !m->hasPhysical) continue;

        if (matched) {
            /* Pinned at the same fixed percent every controllable panel is held
               to, regardless of the dial - the overlay (identical on every
               monitor) is what the dial actually drives. */
            targetPct = ceilingPct;
        } else {
            /* Real backlight control: the dial is the brightness. */
            targetPct = MapHardwarePercent(level, &overlayLevel);
            (void)overlayLevel;  /* the UI thread's overlay pass recomputes it */
        }

        span = m->maxB - m->minB;
        m->pendingVal = m->minB + (DWORD)((span * (DWORD)targetPct + 50) / 100);
        if (m->pendingVal > m->maxB) m->pendingVal = m->maxB;

        SetMonitorBrightness(m->hPhysical, m->pendingVal);

        if (m->writeState == DDC_UNVERIFIED) anyUnverified = TRUE;
    }

    /* Verification costs a settle delay, so one sleep covers every panel being
       checked. Sleeping per monitor instead stalled the UI thread on the lock
       for delay x monitor-count on the first adjustment. */
    for (attempt = 0; anyUnverified && attempt < DDC_VERIFY_ATTEMPTS; attempt++) {
        anyUnverified = FALSE;
        Sleep(DDC_VERIFY_DELAY_MS);
        if (g_quitWorker) break;

        for (i = 0; i < g_monCount; i++) {
            MonitorInfo *m = &g_mons[i];
            DWORD mn2 = 0, cur2 = 0, mx2 = 0;
            BOOL  readOk;

            if (!m->ddcOk || !m->hasPhysical)      continue;
            if (m->writeState != DDC_UNVERIFIED)   continue;

            readOk = GetMonitorBrightness(m->hPhysical, &mn2, &cur2, &mx2) ? TRUE : FALSE;
            if (readOk && cur2 == m->pendingVal) {
                m->writeState = DDC_WRITABLE;
            } else if (attempt + 1 < DDC_VERIFY_ATTEMPTS) {
                SetMonitorBrightness(m->hPhysical, m->pendingVal);
                anyUnverified = TRUE;
            } else {
                /* Panel really does ignore writes - stop issuing pointless I2C
                   traffic and let the UI thread put a dimming overlay on it.
                   Record what it's actually stuck showing so every other
                   monitor's ceiling can be capped to match it. */
                m->writeState = DDC_IGNORES_WRITES;
                m->ddcOk      = FALSE;
                m->stuckVal   = readOk ? cur2 : m->minB;
                demoted       = TRUE;
            }
        }
    }

    /* Panels already marked as ignoring writes might still be moved by the
       user's own OSD buttons at any time - re-read (no write, no verify
       needed) so ComputeCeilingPct tracks reality instead of freezing at
       whatever it was the moment the panel was first demoted. */
    for (i = 0; i < g_monCount; i++) {
        MonitorInfo *m = &g_mons[i];
        DWORD mn3 = 0, cur3 = 0, mx3 = 0;
        if (m->writeState == DDC_IGNORES_WRITES && m->hasPhysical &&
            GetMonitorBrightness(m->hPhysical, &mn3, &cur3, &mx3))
            m->stuckVal = cur3;
    }

    LeaveCriticalSection(&g_monLock);

    if (demoted && g_hwndMain)
        PostMessageW(g_hwndMain, WM_DDC_FALLBACK, 0, 0);
}

static DWORD WINAPI WorkerProc(LPVOID param)
{
    (void)param;
    for (;;) {
        WaitForSingleObject(g_wakeEvent, INFINITE);
        if (g_quitWorker) break;

        for (;;) {
            LONG lvl = InterlockedExchange(&g_pendingLevel, -1);
            if (lvl < 0 || g_quitWorker) break;
            ApplyDdcBrightness((int)lvl);
        }
        if (g_quitWorker) break;
    }
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
    InterlockedExchange(&g_pendingLevel, (LONG)level);
    SetEvent(g_wakeEvent);
}

/* ------------------------------------------------------------------ */
/* Tray icon                                                           */
/* ------------------------------------------------------------------ */

static void FillTrayData(NOTIFYICONDATAW *nid)
{
    ZeroMemory(nid, sizeof(*nid));
    nid->cbSize           = sizeof(*nid);
    nid->hWnd             = g_hwndMain;
    nid->uID              = 1;
    nid->uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid->uCallbackMessage = WM_TRAYICON;
    nid->hIcon            = LoadIconW(NULL, IDI_APPLICATION);
}

static void UpdateTrayTip(void)
{
    NOTIFYICONDATAW nid;
    if (!g_trayAdded) return;
    FillTrayData(&nid);
    /* szTip is 128 wchars; this text is far shorter, but build it with
       wsprintfW into the fixed buffer rather than assuming a length. */
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
    wsprintfW(nid.szTip, L"Desktop Brightness Adjuster");
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
    HMENU modes;
    POINT pt;
    BrightnessMode effective;

    if (!menu) return;

    EnterCriticalSection(&g_monLock);
    EnsureMonitors();
    effective = ResolveMode();
    LeaveCriticalSection(&g_monLock);

    modes = CreatePopupMenu();
    if (modes) {
        /* Auto is annotated with what it currently resolves to, so the menu
           answers "which one am I actually on" without a second click. */
        AppendMenuW(modes, MF_STRING | (g_modePref == MODE_AUTO ? MF_CHECKED : 0),
                    IDM_MODE_AUTO,
                    effective == MODE_MATCHED ? L"Automatic (now: matched)"
                                              : L"Automatic (now: hardware)");
        AppendMenuW(modes, MF_SEPARATOR, 0, NULL);
        AppendMenuW(modes, MF_STRING | (g_modePref == MODE_HARDWARE ? MF_CHECKED : 0),
                    IDM_MODE_HW,    L"Hardware backlight");
        AppendMenuW(modes, MF_STRING | (g_modePref == MODE_MATCHED ? MF_CHECKED : 0),
                    IDM_MODE_MATCH, L"Match all monitors (software)");
    }

    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Desktop Brightness Adjuster");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (modes) AppendMenuW(menu, MF_POPUP, (UINT_PTR)modes, L"Dimming mode");
    AppendMenuW(menu, MF_STRING | (IsAutoStartEnabled() ? MF_CHECKED : 0),
                IDM_AUTOSTART, L"Start with Windows");
    AppendMenuW(menu, MF_STRING, IDM_RESET, L"Reset to 100%");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"Exit");

    GetCursorPos(&pt);
    /* Required so the menu dismisses when the user clicks elsewhere. */
    SetForegroundWindow(g_hwndMain);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0, g_hwndMain, NULL);
    PostMessageW(g_hwndMain, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

/* ------------------------------------------------------------------ */
/* Brightness application                                              */
/* ------------------------------------------------------------------ */

/* Reads where the hardware actually sits, so a first run adopts the panel's
   current brightness instead of yanking it to an assumed 100%. Returns -1 when
   no DDC-capable monitor answered. */
static int ProbeHardwareLevel(void)
{
    int i, level = -1;

    EnterCriticalSection(&g_monLock);
    EnsureMonitors();
    for (i = 0; i < g_monCount; i++) {
        MonitorInfo *m = &g_mons[i];
        DWORD mn, cur, mx;

        if (!m->ddcOk || !m->hasPhysical) continue;
        if (GetMonitorBrightness(m->hPhysical, &mn, &cur, &mx) && mx > mn) {
            int lv;
            if (cur < mn) cur = mn;
            if (cur > mx) cur = mx;
            lv = (int)(((cur - mn) * 100 + (mx - mn) / 2) / (mx - mn));
            /* Take the brightest panel rather than the first. If the displays
               disagree, adopting the dimmest would yank the others down. */
            if (lv > level) level = lv;
        }
    }
    LeaveCriticalSection(&g_monLock);
    return level;
}

static void EnsureInit(void)
{
    if (g_initDone) return;
    g_initDone = TRUE;

    /* DDC/CI brightness lives in the monitor's own NVRAM and survives a reboot,
       so nothing has to be pushed to hardware when the level is only a guess.
       A level the user actually chose is different - see wWinMain. */
    if (LoadLevel(&g_level)) {
        g_levelSaved = TRUE;
    } else {
        int hw = ProbeHardwareLevel();
        g_level = (hw >= 0) ? hw : 100;
    }
}

/* Overlays are windows, so they must be touched on the UI thread. In
   MODE_MATCHED this is the entire visible effect of an adjustment and all a
   keypress does - a couple of SetLayeredWindowAttributes calls per monitor,
   microseconds, nothing that can block on I2C. In MODE_HARDWARE the backlight
   is doing the work and the overlay only appears below the floor. */
static BOOL ApplyOverlays(void)
{
    int i;
    BOOL matched;

    EnterCriticalSection(&g_monLock);
    EnsureMonitors();
    matched = (ResolveMode() == MODE_MATCHED);

    for (i = 0; i < g_monCount; i++) {
        MonitorInfo *m = &g_mons[i];

        if (matched || !m->ddcOk || !m->hasPhysical) {
            /* Either the whole dial is being done in software, or this panel
               has no working backlight control of its own - identical overlay
               formula in both cases, which is what keeps mismatched panels
               tracking each other at every step rather than only at the ends. */
            UpdateOverlay(m, g_level);
        } else {
            int overlayLevel, ddcPct;
            ddcPct = MapHardwarePercent(g_level, &overlayLevel);
            (void)ddcPct;  /* the worker owns the DDC half */
            UpdateOverlay(m, overlayLevel);
        }
    }
    LeaveCriticalSection(&g_monLock);
    return matched;
}

/* The slow tail, run once things stop moving. The DDC sync here is not about
   the level - the pinned value does not depend on it - it re-reads what a
   write-ignoring panel's OSD is currently set to, so the ceiling every other
   monitor matches stays correct if the user turned that panel up by hand. */
static void Settle(void)
{
    KillTimer(g_hwndMain, IDT_SETTLE);

    UpdateTrayTip();

    if (g_savePending) {
        g_savePending = FALSE;
        SaveLevel(g_level);
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
    int next;
    EnsureInit();

    next = ClampLevel(g_level + delta);
    if (next == g_level) return;   /* already at a rail - skip the whole pipeline */

    g_level       = next;
    g_savePending = TRUE;

    /* In MODE_MATCHED the DDC value does not depend on the level, so there is
       nothing to send to hardware until things stop moving and the settle timer
       fires. In MODE_HARDWARE the backlight write *is* the visible change and
       has to go out now - the hotkey coalescing in MainWndProc is what stops a
       held key queueing up behind those ~60 ms round-trips. */
    if (!ApplyOverlays())
        QueueDdcSync(g_level);

    SetTimer(g_hwndMain, IDT_SETTLE, SETTLE_DELAY_MS, NULL);
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
        UpdateTrayTip();
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
           the key comes up - whatever is already queued still has to be
           handled. Folding the queued repeats into this one delta means the
           level lands where the key was released, rather than drifting on
           afterwards while a backlog drains. */
        while (PeekMessageW(&peek, NULL, WM_HOTKEY, WM_HOTKEY, PM_REMOVE)) {
            if      (peek.wParam == HOTKEY_UP_ID) delta += g_step;
            else if (peek.wParam == HOTKEY_DN_ID) delta -= g_step;
        }

        AdjustBrightness(delta);
        return 0;
    }

    case WM_TIMER:
        if (wp == IDT_SETTLE) Settle();
        return 0;

    case WM_DISPLAYCHANGE:
        /* Monitors added/removed/rearranged: drop cached DDC handles and
           overlays so they are rebuilt against the new layout. Runs on the UI
           thread, unlike the old SystemEvents handler which touched windows
           from the wrong thread. */
        EnterCriticalSection(&g_monLock);
        ReleaseMonitors();
        LeaveCriticalSection(&g_monLock);
        g_toldAboutFallback = FALSE;   /* new layout - a new warning is warranted */
        if (g_initDone) ApplyLevelNow(FALSE);
        return 0;

    case WM_DDC_FALLBACK:
        /* A monitor turned out to ignore DDC writes; re-run so it gets an
           overlay, and say so - otherwise "brightness does nothing" is the only
           symptom the user ever sees. */
        if (g_initDone) ApplyLevelNow(FALSE);
        if (!g_toldAboutFallback) {
            g_toldAboutFallback = TRUE;
            ShowBalloon(L"Monitor ignored brightness control",
                        L"A monitor accepted DDC/CI brightness commands but did not "
                        L"act on them, so it is being dimmed in software instead. "
                        L"While automatic mode is on, the other monitors are held to "
                        L"its brightness so they still match. Look for a DDC/CI "
                        L"option in that monitor's on-screen menu - if it can be "
                        L"switched on, real backlight control returns by itself.");
        }
        return 0;

    case WM_TRAYICON:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
            ShowTrayMenu();
        return 0;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDM_AUTOSTART:
            SetAutoStart(!IsAutoStartEnabled());
            return 0;

        case IDM_MODE_AUTO:
        case IDM_MODE_HW:
        case IDM_MODE_MATCH:
            g_modePref = (LOWORD(wp) == IDM_MODE_AUTO)  ? MODE_AUTO
                       : (LOWORD(wp) == IDM_MODE_HW)    ? MODE_HARDWARE
                                                        : MODE_MATCHED;
            SaveMode(g_modePref);
            EnsureInit();
            /* Switching strategy changes what the backlight should be sitting
               at, not just what the overlay draws, so push both. */
            ApplyLevelNow(FALSE);
            return 0;
        case IDM_RESET:
            EnsureInit();
            g_level = 100;
            ApplyLevelNow(TRUE);
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd);
            return 0;
        }
        return 0;

    case WM_DESTROY:
        /* An adjustment may still be sitting on the settle timer - flush it,
           or the last thing the user did is the one thing not remembered. */
        KillTimer(hwnd, IDT_SETTLE);
        if (g_savePending) {
            g_savePending = FALSE;
            SaveLevel(g_level);
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
}

static void Cleanup(void)
{
    BOOL workerStopped = TRUE;

    UnregisterHotKey(g_hwndMain, HOTKEY_UP_ID);
    UnregisterHotKey(g_hwndMain, HOTKEY_DN_ID);
    RemoveTrayIcon();

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

    /* If the worker is somehow still inside a DDC round-trip, tearing down the
       monitor handles and the lock underneath it would crash on the way out.
       The process is exiting anyway - leaking is the safer of the two. */
    if (!workerStopped) return;

    EnterCriticalSection(&g_monLock);
    ReleaseMonitors();
    LeaveCriticalSection(&g_monLock);
    DeleteCriticalSection(&g_monLock);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    HANDLE mutex;
    MSG    msg;
    BOOL   hotkeysOk;

    (void)hPrev; (void)cmdLine; (void)nShow;
    g_hInst      = hInst;
    g_uiThreadId = GetCurrentThreadId();

    EnableDpiAwareness();
    LoadSettings();   /* hotkeys, step and mode, before anything consumes them */

    /* Per-session, not Global: one instance per logged-in user is the intent,
       and a Global name lets another user's session block this one from
       starting at all (and needs privileges this app has no reason to want). */
    mutex = CreateMutexW(NULL, TRUE, L"Local\\DesktopBrightnessApp_SingleInstance");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    InitializeCriticalSection(&g_monLock);

    if (!RegisterClasses()) {
        CloseHandle(mutex);
        return 1;
    }

    /* A normal (never shown) window rather than HWND_MESSAGE: message-only
       windows do not receive broadcasts, and both WM_DISPLAYCHANGE and
       TaskbarCreated are broadcasts. */
    g_hwndMain = CreateWindowExW(WS_EX_TOOLWINDOW, L"BrightMain", L"DesktopBrightness",
                                 WS_POPUP, 0, 0, 0, 0, NULL, NULL, hInst, NULL);
    if (!g_hwndMain) {
        CloseHandle(mutex);
        return 1;
    }

    g_taskbarMsg = RegisterWindowMessageW(L"TaskbarCreated");

    AddTrayIcon();

    /* Registered against a window this app creates once and never recreates,
       so the binding cannot be silently lost the way it could under WinForms
       handle recreation. Another app already owning the combination is the
       one failure that looks exactly like the app being dead, so it is called
       out rather than ignored. */
    hotkeysOk  = RegisterHotKey(g_hwndMain, HOTKEY_UP_ID, g_hotkeyMod, g_hotkeyUp) ? TRUE : FALSE;
    hotkeysOk &= RegisterHotKey(g_hwndMain, HOTKEY_DN_ID, g_hotkeyMod, g_hotkeyDn) ? TRUE : FALSE;

    if (!hotkeysOk)
        ShowBalloon(L"Hotkeys unavailable",
                    L"The brightness hotkeys could not be registered - another "
                    L"running application already owns that combination. They "
                    L"can be changed under HKCU\\Software\\DesktopBrightness "
                    L"(see the README); brightness can still be reset from this "
                    L"tray icon meanwhile.");

    /* A level the user explicitly chose is restored at startup. This matters
       for overlay-dimmed monitors above all: their dimming is a window, so
       unlike DDC/CI it does not survive the process exiting. */
    EnsureInit();
    if (g_levelSaved)
        ApplyLevelNow(FALSE);
    else
        UpdateTrayTip();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    Cleanup();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}
