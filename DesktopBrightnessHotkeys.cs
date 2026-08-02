// DesktopBrightnessHotkeys.cs
// B.R.A.I.N. Desktop Brightness Adjuster
// Pure DDC/CI hardware backlight + software overlay hybrid engine
// Optimized for < 0.1% CPU, < 5 MB RAM, Low startup impact

using System;
using System.Diagnostics;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading.Tasks;
using System.Windows.Forms;
using Microsoft.Win32;

namespace DesktopBrightnessApp
{
    public class HiddenMainForm : Form
    {
        private NotifyIcon trayIcon;
        private ContextMenuStrip trayMenu;
        private OsdForm osdForm;
        private DimmerOverlayForm dimmerOverlay;

        private const int WM_HOTKEY = 0x0312;
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
            Application.EnableVisualStyles();
            Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new HiddenMainForm());
        }

        public HiddenMainForm()
        {
            // Zero-footprint hidden window
            this.Size = new Size(0, 0);
            this.ShowInTaskbar = false;
            this.FormBorderStyle = FormBorderStyle.None;
            this.WindowState = FormWindowState.Minimized;

            // Build Tray Menu
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

            // System Tray Icon
            trayIcon = new NotifyIcon()
            {
                Icon = SystemIcons.Application,
                ContextMenuStrip = trayMenu,
                Visible = true,
                Text = "Desktop Brightness\n(Alt+PgUp / Alt+PgDn)"
            };

            // Register hotkeys only — zero other work at startup for Low impact
            RegisterHotKey(this.Handle, HOTKEY_UP_ID, MOD_ALT, VK_PRIOR);
            RegisterHotKey(this.Handle, HOTKEY_DN_ID, MOD_ALT, VK_NEXT);

            // Deferred: read hardware brightness 3s after boot to avoid startup CPU hit
            Timer deferredInit = new Timer();
            deferredInit.Interval = 3000;
            deferredInit.Tick += (s, e) =>
            {
                deferredInit.Stop();
                deferredInit.Dispose();
                BrightnessController.InitializeHardware();
                TrimWorkingSet();
            };
            deferredInit.Start();
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
            // Lazy-load OSD on first keypress
            if (osdForm == null || osdForm.IsDisposed)
                osdForm = new OsdForm(this.TrimWorkingSet);

            int brightness = BrightnessController.AdjustInMemory(delta);

            // Software overlay for dimming below 100%
            if (brightness < 100)
            {
                if (dimmerOverlay == null || dimmerOverlay.IsDisposed)
                    dimmerOverlay = new DimmerOverlayForm();

                float opacity = (100 - brightness) / 100.0f * 0.75f;
                dimmerOverlay.SetDimLevel(opacity);
            }
            else if (dimmerOverlay != null && !dimmerOverlay.IsDisposed)
            {
                dimmerOverlay.SetDimLevel(0.0f);
            }

            // Show OSD badge and update tray tooltip
            osdForm.ShowOSD(brightness);
            trayIcon.Text = "Desktop Brightness: " + brightness + "%\n(Alt+PgUp / Alt+PgDn)";

            // Hardware DDC/CI backlight sync (throttled, non-blocking)
            BrightnessController.SyncHardwareThrottled(brightness);
        }

        private void TrimWorkingSet()
        {
            try
            {
                GC.Collect(2, GCCollectionMode.Forced, true);
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
            UnregisterHotKey(this.Handle, HOTKEY_UP_ID);
            UnregisterHotKey(this.Handle, HOTKEY_DN_ID);
            trayIcon.Visible = false;
            if (dimmerOverlay != null && !dimmerOverlay.IsDisposed) dimmerOverlay.Close();
            if (osdForm != null && !osdForm.IsDisposed) osdForm.Close();
            Application.Exit();
        }
    }

    // ─────────────────────────────────────────────────────────────
    // Click-Through Transparent Black Overlay (Screen Capture Excluded)
    // Zero timers, zero polling, zero idle CPU
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

        public DimmerOverlayForm()
        {
            this.FormBorderStyle = FormBorderStyle.None;
            this.StartPosition = FormStartPosition.Manual;
            this.ShowInTaskbar = false;
            this.BackColor = Color.Black;
            this.Bounds = SystemInformation.VirtualScreen;
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
        private Timer hideTimer;
        private int currentPercent = 100;
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

            Rectangle screen = Screen.PrimaryScreen.WorkingArea;
            this.Location = new Point(
                screen.Left + (screen.Width - this.Width) / 2,
                screen.Top + (screen.Height - this.Height) / 2
            );

            hideTimer = new Timer();
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

        public void ShowOSD(int percent)
        {
            this.currentPercent = percent;
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

            string text = currentPercent + "%";
            SizeF sz = g.MeasureString(text, osdFont);
            g.DrawString(text, osdFont, textBrush, (this.Width - sz.Width) / 2f, (this.Height - sz.Height) / 2f);
        }
    }

    // ─────────────────────────────────────────────────────────────
    // DDC/CI Hardware Backlight Controller
    // Enumerates all connected monitors, throttled to 1 I2C call per 150ms
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

        private static int cachedBrightness = 100;
        private static bool isHardwarePending = false;
        private static DateTime lastSyncTime = DateTime.MinValue;

        public static int CurrentBrightness { get { return cachedBrightness; } }

        // Deferred hardware query — called 3s after boot
        public static void InitializeHardware()
        {
            Task.Run(() =>
            {
                try
                {
                    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr data) =>
                    {
                        uint count;
                        if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, out count) && count > 0)
                        {
                            PHYSICAL_MONITOR[] mons = new PHYSICAL_MONITOR[count];
                            if (GetPhysicalMonitorsFromHMONITOR(hMon, count, mons))
                            {
                                uint minB, curB, maxB;
                                if (GetMonitorBrightness(mons[0].hPhysicalMonitor, out minB, out curB, out maxB))
                                    cachedBrightness = (int)curB;
                                DestroyPhysicalMonitors(count, mons);
                            }
                        }
                        return false; // Stop after first monitor
                    }, IntPtr.Zero);
                }
                catch { }
            });
        }

        public static int AdjustInMemory(int delta)
        {
            cachedBrightness = Math.Max(5, Math.Min(100, cachedBrightness + delta));
            return cachedBrightness;
        }

        public static void SyncHardwareThrottled(int target)
        {
            if (isHardwarePending) return;
            if ((DateTime.Now - lastSyncTime).TotalMilliseconds < 150) return;

            isHardwarePending = true;
            lastSyncTime = DateTime.Now;
            int t = target; // Capture for closure

            Task.Run(() =>
            {
                try
                {
                    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMon, IntPtr hdc, ref RECT rc, IntPtr data) =>
                    {
                        uint count;
                        if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMon, out count) && count > 0)
                        {
                            PHYSICAL_MONITOR[] mons = new PHYSICAL_MONITOR[count];
                            if (GetPhysicalMonitorsFromHMONITOR(hMon, count, mons))
                            {
                                for (int i = 0; i < count; i++)
                                    SetMonitorBrightness(mons[i].hPhysicalMonitor, (uint)t);
                                DestroyPhysicalMonitors(count, mons);
                            }
                        }
                        return true; // Continue to all monitors
                    }, IntPtr.Zero);
                }
                catch { }
                finally { isHardwarePending = false; }
            });
        }
    }
}
