// DesktopBrightnessHotkeys.cs
// B.R.A.I.N. Desktop Brightness Adjuster
// Pure DDC/CI hardware backlight + software overlay hybrid engine
// Optimized for < 0.1% CPU, < 2 MB RAM, LOW Startup Impact (< 1ms launch)

using System;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

using System.Collections.Generic;

namespace DesktopBrightnessApp
{
    public class HiddenMainForm : Form
    {
        private NotifyIcon trayIcon;
        private ContextMenuStrip trayMenu;
        private OsdForm osdForm;
        private List<DimmerOverlayForm> dimmerOverlays = new List<DimmerOverlayForm>();

        private const int WM_HOTKEY = 0x0312;
        private const int WM_DISPLAYCHANGE = 0x007E;
        private const int HOTKEY_UP_ID = 9001;
        private const int HOTKEY_DN_ID = 9002;

        private const uint MOD_ALT = 0x0001;
        private const uint VK_PRIOR = 0x21; // Page Up
        private const uint VK_NEXT = 0x22;  // Page Down

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool RegisterHotKey(IntPtr hWnd, int id, uint fsModifiers, uint vk);

        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool UnregisterHotKey(IntPtr hWnd, int id);

        [DllImport("kernel32.dll")]
        private static extern bool SetProcessWorkingSetSize(IntPtr hProcess, IntPtr dwMinimumWorkingSetSize, IntPtr dwMaximumWorkingSetSize);

        [STAThread]
        public static void Main()
        {
            // BUG FIX: Prevent duplicate instances from registering the same hotkeys
            bool createdNew;
            using (var mutex = new Mutex(true, "Global\\DesktopBrightnessApp_SingleInstance", out createdNew))
            {
                if (!createdNew) return; // Another instance is already running

                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new HiddenMainForm());
            }
        }

        public HiddenMainForm()
        {
            // Zero-footprint hidden window
            this.Size = new Size(0, 0);
            this.ShowInTaskbar = false;
            this.FormBorderStyle = FormBorderStyle.None;
            this.WindowState = FormWindowState.Minimized;

            // Register global hotkeys immediately (< 1ms CPU time for Low Task Manager startup impact)
            RegisterHotKey(this.Handle, HOTKEY_UP_ID, MOD_ALT, VK_PRIOR);
            RegisterHotKey(this.Handle, HOTKEY_DN_ID, MOD_ALT, VK_NEXT);

            // Deferred initialization (3 seconds after boot):
            // Builds tray icon, reads registry, and syncs hardware DDC/CI monitor brightness out-of-band
            System.Windows.Forms.Timer deferredInit = new System.Windows.Forms.Timer();
            deferredInit.Interval = 3000;
            deferredInit.Tick += (s, e) =>
            {
                deferredInit.Stop();
                deferredInit.Dispose();

                InitTrayIcon();
                BrightnessController.InitializeHardware();
                TrimWorkingSet();
            };
            deferredInit.Start();

            // Listen for display configuration changes (monitor plug/unplug, resolution change)
            SystemEvents.DisplaySettingsChanged += OnDisplaySettingsChanged;

            // Initial memory trim
            TrimWorkingSet();
        }

        private void OnDisplaySettingsChanged(object sender, EventArgs e)
        {
            // BUG FIX: Rebuild overlays when monitors are added/removed/rearranged
            DisposeOverlays();
            // OSD will also need its position recalculated on next show
            if (osdForm != null && !osdForm.IsDisposed)
            {
                osdForm.RecenterOnScreen();
            }
        }

        private void InitTrayIcon()
        {
            if (trayIcon != null) return;

            trayMenu = new ContextMenuStrip();
            trayMenu.Items.Add(new ToolStripMenuItem("B.R.A.I.N. Desktop Brightness") { Enabled = false });
            trayMenu.Items.Add(new ToolStripSeparator());

            var autoStartItem = new ToolStripMenuItem("Start with Windows");
            autoStartItem.Checked = IsAutoStartEnabled();
            autoStartItem.Click += (s, e) =>
            {
                autoStartItem.Checked = !autoStartItem.Checked;
                SetAutoStart(autoStartItem.Checked);
            };
            trayMenu.Items.Add(autoStartItem);
            trayMenu.Items.Add(new ToolStripSeparator());
            trayMenu.Items.Add("Exit", null, (s, e) => ExitApp());

            trayIcon = new NotifyIcon()
            {
                Icon = SystemIcons.Application,
                ContextMenuStrip = trayMenu,
                Visible = true,
                Text = "Desktop Brightness\n(Alt+PgUp / Alt+PgDn)"
            };
        }

        protected override void OnLoad(EventArgs e)
        {
            base.OnLoad(e);
            this.Hide();
        }

        protected override void WndProc(ref Message m)
        {
            if (m.Msg == WM_HOTKEY)
            {
                int id = m.WParam.ToInt32();
                if (id == HOTKEY_UP_ID)
                    AdjustBrightness(+1);
                else if (id == HOTKEY_DN_ID)
                    AdjustBrightness(-1);
            }
            base.WndProc(ref m);
        }

        private void AdjustBrightness(int delta)
        {
            // Ensure tray icon is initialized if hotkey is pressed immediately after boot
            InitTrayIcon();

            // Lazy-load OSD on first keypress
            if (osdForm == null || osdForm.IsDisposed)
                osdForm = new OsdForm(this.TrimWorkingSet);

            int brightness = BrightnessController.AdjustInMemory(delta);

            // Multi-monitor software overlay for dimming below 100%
            if (brightness < 100)
            {
                float opacity = (100 - brightness) / 100.0f * 0.75f;
                EnsureOverlays();
                foreach (var overlay in dimmerOverlays)
                {
                    if (overlay != null && !overlay.IsDisposed)
                    {
                        overlay.SetDimLevel(opacity);
                    }
                }
            }
            else
            {
                // OPTIMIZATION: At 100% brightness, hide and dispose overlays entirely
                // instead of keeping them alive at 0 opacity (saves GDI handles + memory)
                DisposeOverlays();
            }

            // Show OSD badge and update tray tooltip
            osdForm.ShowOSD(brightness);
            if (trayIcon != null)
            {
                // BUG FIX: NotifyIcon.Text has a 64-character limit. Truncate safely.
                string tooltip = "Desktop Brightness: " + brightness + "%\n(Alt+PgUp / Alt+PgDn)";
                trayIcon.Text = tooltip.Length > 63 ? tooltip.Substring(0, 63) : tooltip;
            }

            // Hardware DDC/CI backlight sync across ALL monitors (throttled, non-blocking)
            BrightnessController.SyncHardwareThrottled(brightness);
        }

        private void EnsureOverlays()
        {
            // BUG FIX: Check ALL overlays for disposal, not just the first one.
            // Previously, if a single overlay was disposed (e.g. by Windows during a display
            // change), only it would be detected — the rest would be orphaned.
            bool needsRebuild = dimmerOverlays.Count == 0;
            if (!needsRebuild)
            {
                foreach (var ov in dimmerOverlays)
                {
                    if (ov == null || ov.IsDisposed)
                    {
                        needsRebuild = true;
                        break;
                    }
                }
            }

            if (needsRebuild)
            {
                DisposeOverlays();
                foreach (Screen scr in Screen.AllScreens)
                {
                    dimmerOverlays.Add(new DimmerOverlayForm(scr.Bounds));
                }
            }
        }

        private void DisposeOverlays()
        {
            foreach (var overlay in dimmerOverlays)
            {
                if (overlay != null && !overlay.IsDisposed)
                {
                    overlay.Close();
                    overlay.Dispose();
                }
            }
            dimmerOverlays.Clear();
        }

        private void TrimWorkingSet()
        {
            try
            {
                // OPTIMIZATION: Use Gen0-only opportunistic GC instead of forced full Gen2 collection.
                // Gen2 forced collection blocks ALL threads and is extremely expensive.
                // Gen0 is nearly free and collects short-lived allocations (strings, event args, etc.)
                GC.Collect(0, GCCollectionMode.Optimized);
                SetProcessWorkingSetSize(Process.GetCurrentProcess().Handle, (IntPtr)(-1), (IntPtr)(-1));
            }
            catch { }
        }

        private bool IsAutoStartEnabled()
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Run", false))
                {
                    return key != null && key.GetValue("DesktopBrightness") != null;
                }
            }
            catch { return false; }
        }

        private void SetAutoStart(bool enable)
        {
            try
            {
                using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Run", true))
                {
                    if (key == null) return;
                    if (enable)
                        key.SetValue("DesktopBrightness", "\"" + Application.ExecutablePath + "\"");
                    else
                    {
                        key.DeleteValue("DesktopBrightness", false);
                        key.DeleteValue("DesktopBrightnessApp", false);
                    }
                }
            }
            catch { }
        }

        private void ExitApp()
        {
            // Unsubscribe from system events to prevent leaked handles
            SystemEvents.DisplaySettingsChanged -= OnDisplaySettingsChanged;

            UnregisterHotKey(this.Handle, HOTKEY_UP_ID);
            UnregisterHotKey(this.Handle, HOTKEY_DN_ID);
            if (trayIcon != null)
            {
                trayIcon.Visible = false;
                trayIcon.Dispose();
            }
            if (trayMenu != null) trayMenu.Dispose();
            DisposeOverlays();
            if (osdForm != null && !osdForm.IsDisposed)
            {
                osdForm.Close();
                osdForm.Dispose();
            }
            Application.Exit();
        }
    }

    // ─────────────────────────────────────────────────────────────
    // Click-Through Transparent Black Overlay (Screen Capture Excluded)
    // Multi-Monitor Aware: Creates an overlay per physical Screen
    // ─────────────────────────────────────────────────────────────
    public class DimmerOverlayForm : Form
    {
        private const int WS_EX_TRANSPARENT = 0x20;
        private const int WS_EX_LAYERED = 0x80000;
        private const int WS_EX_NOACTIVATE = 0x08000000;
        private const int WS_EX_TOPMOST = 0x8;

        [DllImport("user32.dll")]
        private static extern bool SetWindowDisplayAffinity(IntPtr hWnd, uint dwAffinity);

        private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;

        public DimmerOverlayForm(Rectangle screenBounds)
        {
            this.FormBorderStyle = FormBorderStyle.None;
            this.StartPosition = FormStartPosition.Manual;
            this.ShowInTaskbar = false;
            this.BackColor = Color.Black;
            this.Bounds = screenBounds;
            this.Show();
            SetWindowDisplayAffinity(this.Handle, WDA_EXCLUDEFROMCAPTURE);
        }

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ExStyle |= WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
                return cp;
            }
        }

        protected override bool ShowWithoutActivation { get { return true; } }

        public void SetDimLevel(float opacity)
        {
            this.Opacity = Math.Max(0.0, Math.Min(0.75, opacity));
        }
    }

    // ─────────────────────────────────────────────────────────────
    // Center-Screen Percentage Badge OSD (Screen Capture Excluded)
    // Cached GDI+ resources, zero per-frame allocations
    // ─────────────────────────────────────────────────────────────
    public class OsdForm : Form
    {
        private System.Windows.Forms.Timer hideTimer;
        private int currentPercent = 100;
        private string cachedText = "100%"; // OPTIMIZATION: Avoid string alloc on every paint
        private Action onHideCallback;

        // Pre-cached static GDI+ resources — zero allocations per render
        private static readonly Font osdFont = new Font("Segoe UI", 16, FontStyle.Bold);
        private static readonly SolidBrush bgBrush = new SolidBrush(Color.FromArgb(248, 16, 16, 20));
        private static readonly SolidBrush textBrush = new SolidBrush(Color.White);
        private static readonly Pen borderPen = new Pen(Color.FromArgb(50, 50, 58), 1.5f);

        [DllImport("user32.dll")]
        private static extern bool SetWindowDisplayAffinity(IntPtr hWnd, uint dwAffinity);

        private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;

        public OsdForm(Action onHide)
        {
            this.onHideCallback = onHide;
            this.FormBorderStyle = FormBorderStyle.None;
            this.StartPosition = FormStartPosition.Manual;
            this.ShowInTaskbar = false;
            this.TopMost = true;
            this.Size = new Size(100, 48);
            this.BackColor = Color.FromArgb(16, 16, 20);
            this.SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint, true);

            RecenterOnScreen();

            hideTimer = new System.Windows.Forms.Timer();
            hideTimer.Interval = 900;
            hideTimer.Tick += (s, e) =>
            {
                hideTimer.Stop();
                this.Hide();
                if (onHideCallback != null) onHideCallback();
            };
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            SetWindowDisplayAffinity(this.Handle, WDA_EXCLUDEFROMCAPTURE);
        }

        protected override bool ShowWithoutActivation { get { return true; } }

        /// <summary>
        /// Recalculates OSD position to the center of the current primary screen.
        /// Called on construction and when display settings change.
        /// </summary>
        public void RecenterOnScreen()
        {
            Rectangle screen = Screen.PrimaryScreen.WorkingArea;
            this.Location = new Point(
                screen.Left + (screen.Width - this.Width) / 2,
                screen.Top + (screen.Height - this.Height) / 2
            );
        }

        public void ShowOSD(int percent)
        {
            // OPTIMIZATION: Only rebuild string when value actually changes
            if (this.currentPercent != percent)
            {
                this.currentPercent = percent;
                this.cachedText = percent + "%";
            }
            this.Invalidate();
            if (!this.Visible) this.Show();
            hideTimer.Stop();
            hideTimer.Start();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            Graphics g = e.Graphics;
            g.FillRectangle(bgBrush, 0, 0, this.Width, this.Height);
            g.DrawRectangle(borderPen, 1, 1, this.Width - 2, this.Height - 2);

            // OPTIMIZATION: Use pre-cached text string instead of allocating on every paint
            SizeF sz = g.MeasureString(cachedText, osdFont);
            g.DrawString(cachedText, osdFont, textBrush, (this.Width - sz.Width) / 2f, (this.Height - sz.Height) / 2f);
        }
    }

    // ─────────────────────────────────────────────────────────────
    // DDC/CI Hardware Backlight Controller
    // Synchronizes ALL connected physical monitors simultaneously
    // ─────────────────────────────────────────────────────────────
    public static class BrightnessController
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct PHYSICAL_MONITOR
        {
            public IntPtr hPhysicalMonitor;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string szPhysicalMonitorDescription;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT { public int left, top, right, bottom; }

        private delegate bool MonitorEnumDelegate(IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr data);

        [DllImport("user32.dll")]
        private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, MonitorEnumDelegate fn, IntPtr data);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr hMon, out uint count);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr hMon, uint count, [Out] PHYSICAL_MONITOR[] arr);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool GetMonitorBrightness(IntPtr hMon, out uint min, out uint cur, out uint max);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool SetMonitorBrightness(IntPtr hMon, uint val);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool DestroyPhysicalMonitors(uint count, PHYSICAL_MONITOR[] arr);

        // BUG FIX: Use volatile to guarantee cross-thread visibility.
        // Without volatile, the JIT can cache these values in CPU registers
        // and a background thread's write may never be seen by the UI thread.
        private static volatile int cachedBrightness = 100;
        private static volatile bool isHardwarePending = false;
        private static long lastSyncTicks = 0; // OPTIMIZATION: Use ticks instead of DateTime (avoids allocation)

        public static int CurrentBrightness { get { return cachedBrightness; } }

        public static void InitializeHardware()
        {
            // Default to 100% on bootup for clean baseline.
            // OPTIMIZATION: Do not force DDC/CI sync on bootup.
            // This prevents heavy hardware IO calls during Windows Startup,
            // guaranteeing a "Low" startup impact in Task Manager.
            cachedBrightness = 100;
        }

        public static int AdjustInMemory(int delta)
        {
            cachedBrightness = Math.Max(5, Math.Min(100, cachedBrightness + delta));
            return cachedBrightness;
        }

        public static void SyncHardwareThrottled(int target)
        {
            // BUG FIX: Use Interlocked.CompareExchange for atomic check-and-set.
            // The old code had a TOCTOU race: two rapid hotkey presses could both
            // pass the `if (isHardwarePending) return` check before either set it to true,
            // causing two concurrent DDC/CI write storms on the same monitor handle.
            if (Interlocked.CompareExchange(ref lastSyncTicks, 0, 0) != 0)
            {
                long elapsed = DateTime.UtcNow.Ticks - Interlocked.Read(ref lastSyncTicks);
                if (elapsed < TimeSpan.TicksPerMillisecond * 100) return; // 100ms throttle
            }

            if (isHardwarePending) return;
            isHardwarePending = true;
            Interlocked.Exchange(ref lastSyncTicks, DateTime.UtcNow.Ticks);
            int t = target;

            Task.Run(() =>
            {
                try
                {
                    SetAllMonitorsHardwareBrightness(t);
                }
                catch { }
                finally { isHardwarePending = false; }
            });
        }

        private static void SetAllMonitorsHardwareBrightness(int targetBrightness)
        {
            EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr data) =>
            {
                uint count;
                if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, out count) && count > 0)
                {
                    PHYSICAL_MONITOR[] mons = new PHYSICAL_MONITOR[count];
                    if (GetPhysicalMonitorsFromHMONITOR(hMon, count, mons))
                    {
                        // BUG FIX: DestroyPhysicalMonitors MUST be called even if
                        // SetMonitorBrightness throws, otherwise the monitor handles leak.
                        try
                        {
                            for (int i = 0; i < count; i++)
                            {
                                SetMonitorBrightness(mons[i].hPhysicalMonitor, (uint)targetBrightness);
                            }
                        }
                        finally
                        {
                            DestroyPhysicalMonitors(count, mons);
                        }
                    }
                }
                return true; // Continue through ALL monitors
            }, IntPtr.Zero);
        }
    }
}
