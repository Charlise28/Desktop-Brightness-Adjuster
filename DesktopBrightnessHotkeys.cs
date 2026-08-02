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
            this.Size = new Size(0, 0);
            this.ShowInTaskbar = false;
            this.FormBorderStyle = FormBorderStyle.None;
            this.WindowState = FormWindowState.Minimized;

            // Build Tray Menu
            trayMenu = new ContextMenuStrip();
            var header = new ToolStripMenuItem("B.R.A.I.N. Desktop Brightness") { Enabled = false };
            trayMenu.Items.Add(header);
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
                Text = "Desktop Brightness: 50%\n(Alt+PgUp / Alt+PgDn)"
            };

            // Register Global Hotkeys (< 1ms CPU footprint)
            RegisterHotKey(this.Handle, HOTKEY_UP_ID, MOD_ALT, VK_PRIOR);
            RegisterHotKey(this.Handle, HOTKEY_DN_ID, MOD_ALT, VK_NEXT);

            // Fetch initial hardware monitor brightness
            BrightnessController.InitializeHardware();

            TrimWorkingSetRAM();
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
                {
                    AdjustBrightness(+1);
                }
                else if (id == HOTKEY_DN_ID)
                {
                    AdjustBrightness(-1);
                }
            }
            base.WndProc(ref m);
        }

        private void AdjustBrightness(int delta)
        {
            if (osdForm == null || osdForm.IsDisposed)
            {
                osdForm = new OsdForm(this.TrimWorkingSetRAM);
            }

            int newBrightness = BrightnessController.AdjustInMemory(delta);

            // 1. Dimming Layer & Software Cursor Dimmer
            if (newBrightness < 100)
            {
                if (dimmerOverlay == null || dimmerOverlay.IsDisposed)
                {
                    dimmerOverlay = new DimmerOverlayForm();
                }
                float overlayOpacity = (100 - newBrightness) / 100.0f * 0.75f;
                dimmerOverlay.UpdateOpacity(overlayOpacity);
            }
            else
            {
                if (dimmerOverlay != null && !dimmerOverlay.IsDisposed)
                {
                    dimmerOverlay.UpdateOpacity(0.0f);
                }
            }

            // 2. Immediate Minimalist Center OSD Render
            osdForm.ShowOSD(newBrightness);
            UpdateToolTip(newBrightness);

            // 3. Smooth Hardware Backlight Curve
            int hwTarget = Math.Max(0, Math.Min(100, newBrightness));
            BrightnessController.SyncHardwareThrottled(hwTarget);
        }

        private void UpdateToolTip(int current)
        {
            trayIcon.Text = "Desktop Brightness: " + current + "%\n(Alt+PgUp / Alt+PgDn)";
        }

        public void TrimWorkingSetRAM()
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
                    return key != null && (key.GetValue("DesktopBrightness") != null || key.GetValue("DesktopBrightnessApp") != null);
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
                    if (key != null)
                    {
                        if (enable)
                            key.SetValue("DesktopBrightness", "\"" + Application.ExecutablePath + "\"");
                        else
                        {
                            key.DeleteValue("DesktopBrightness", false);
                            key.DeleteValue("DesktopBrightnessApp", false);
                        }
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

    // Click-Through Transparent Black Screen Dimmer Overlay with Hardware Mouse Follower & Capture Exclusion
    public class DimmerOverlayForm : Form
    {
        private const int WS_EX_TRANSPARENT = 0x20;
        private const int WS_EX_LAYERED = 0x80000;
        private const int WS_EX_NOACTIVATE = 0x08000000;
        private const int WS_EX_TOPMOST = 0x8;

        [DllImport("user32.dll")]
        private static extern bool SetWindowDisplayAffinity(IntPtr hWnd, uint dwAffinity);

        private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;

        private Timer mouseTrackerTimer;
        private Point currentMousePos;
        private float currentOpacity = 0.0f;

        public DimmerOverlayForm()
        {
            this.FormBorderStyle = FormBorderStyle.None;
            this.StartPosition = FormStartPosition.Manual;
            this.ShowInTaskbar = false;
            this.BackColor = Color.Black;
            this.DoubleBuffered = true;

            Rectangle virtualScreen = SystemInformation.VirtualScreen;
            this.Bounds = virtualScreen;

            this.Show();
            SetWindowDisplayAffinity(this.Handle, WDA_EXCLUDEFROMCAPTURE);

            // Timer to track mouse cursor position and draw software cursor tint over hardware cursor
            mouseTrackerTimer = new Timer();
            mouseTrackerTimer.Interval = 16; // ~60 FPS cursor tracking
            mouseTrackerTimer.Tick += (s, e) =>
            {
                if (this.currentOpacity > 0.05f)
                {
                    Point newPos = Cursor.Position;
                    Point localPos = this.PointToClient(newPos);
                    if (localPos != currentMousePos)
                    {
                        currentMousePos = localPos;
                        this.Invalidate();
                    }
                }
            };
            mouseTrackerTimer.Start();
        }

        protected override CreateParams CreateParams
        {
            get {
                CreateParams cp = base.CreateParams;
                cp.ExStyle |= WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOPMOST;
                return cp;
            }
        }

        protected override bool ShowWithoutActivation
        {
            get { return true; }
        }

        public void UpdateOpacity(float opacity)
        {
            this.currentOpacity = Math.Max(0.0f, Math.Min(0.75f, opacity));
            this.Opacity = this.currentOpacity;
            this.Invalidate();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            if (currentOpacity > 0.05f)
            {
                Graphics g = e.Graphics;
                g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;

                // Draw dark tint circle over hardware cursor location to dim the mouse pointer
                int alpha = (int)(currentOpacity * 220);
                using (SolidBrush cursorDimBrush = new SolidBrush(Color.FromArgb(alpha, 0, 0, 0)))
                {
                    g.FillEllipse(cursorDimBrush, currentMousePos.X - 16, currentMousePos.Y - 16, 32, 32);
                }
            }
        }
    }

    // Ultra-Minimalist Center-Screen Percentage Badge OSD
    public class OsdForm : Form
    {
        private Timer hideTimer;
        private int currentPercent = 50;
        private Action onHideCallback;

        private static readonly Font osdFont = new Font("Segoe UI", 16, FontStyle.Bold);
        private static readonly SolidBrush bgBrush = new SolidBrush(Color.FromArgb(248, 16, 16, 20));
        private static readonly SolidBrush textBrush = new SolidBrush(Color.White);
        private static readonly Pen borderPen = new Pen(Color.FromArgb(50, 50, 58), 1.5f);

        [DllImport("user32.dll")]
        private static extern bool SetWindowDisplayAffinity(IntPtr hWnd, uint dwAffinity);

        private const uint WDA_EXCLUDEFROMCAPTURE = 0x00000011;

        public OsdForm(Action onHide = null)
        {
            this.onHideCallback = onHide;
            this.FormBorderStyle = FormBorderStyle.None;
            this.StartPosition = FormStartPosition.Manual;
            this.ShowInTaskbar = false;
            this.TopMost = true;
            this.Size = new Size(100, 48); // Compact Pill Badge
            this.BackColor = Color.FromArgb(16, 16, 20);
            this.DoubleBuffered = true;

            // Center of Primary Screen
            Rectangle screen = Screen.PrimaryScreen.WorkingArea;
            this.Location = new Point(screen.Left + (screen.Width - this.Width) / 2, screen.Top + (screen.Height - this.Height) / 2);

            hideTimer = new Timer();
            hideTimer.Interval = 900; // Hide after 0.9s
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

        protected override bool ShowWithoutActivation
        {
            get { return true; }
        }

        public void ShowOSD(int percent)
        {
            this.currentPercent = percent;
            this.Invalidate();
            if (!this.Visible)
            {
                this.Show();
            }
            hideTimer.Stop();
            hideTimer.Start();
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            Graphics g = e.Graphics;
            g.SmoothingMode = System.Drawing.Drawing2D.SmoothingMode.AntiAlias;

            // Matte Black Pill Background
            g.FillRectangle(bgBrush, 0, 0, this.Width, this.Height);

            // Subtle Dark Border
            g.DrawRectangle(borderPen, 1, 1, this.Width - 2, this.Height - 2);

            // Bold Centered Percentage Text
            string text = currentPercent + "%";
            SizeF textSize = g.MeasureString(text, osdFont);
            float posX = (this.Width - textSize.Width) / 2.0f;
            float posY = (this.Height - textSize.Height) / 2.0f;
            g.DrawString(text, osdFont, textBrush, posX, posY);
        }
    }

    public static class BrightnessController
    {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
        private struct PHYSICAL_MONITOR
        {
            public IntPtr hPhysicalMonitor;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)]
            public string szPhysicalMonitorDescription;
        }

        [DllImport("user32.dll")]
        private static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr lprcClip, MonitorEnumDelegate lpfnEnum, IntPtr dwData);

        private delegate bool MonitorEnumDelegate(IntPtr hMonitor, IntPtr hdcMonitor, ref RECT lprcMonitor, IntPtr dwData);

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int left;
            public int top;
            public int right;
            public int bottom;
        }

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool GetNumberOfPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, out uint pdwNumberOfPhysicalMonitors);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool GetPhysicalMonitorsFromHMONITOR(IntPtr hMonitor, uint dwPhysicalMonitorArraySize, [Out] PHYSICAL_MONITOR[] pPhysicalMonitorArray);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool GetMonitorBrightness(IntPtr hMonitor, out uint pdwMinimumBrightness, out uint pdwCurrentBrightness, out uint pdwMaximumBrightness);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool SetMonitorBrightness(IntPtr hMonitor, uint dwNewBrightness);

        [DllImport("dxva2.dll", SetLastError = true)]
        private static extern bool DestroyPhysicalMonitors(uint dwPhysicalMonitorArraySize, PHYSICAL_MONITOR[] pPhysicalMonitorArray);

        private static int cachedBrightness = 50;
        private static bool isHardwarePending = false;
        private static DateTime lastSyncTime = DateTime.MinValue;

        public static int CurrentBrightness
        {
            get { return cachedBrightness; }
        }

        public static void InitializeHardware()
        {
            Task.Run(() =>
            {
                try
                {
                    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMonitor, IntPtr hdcMonitor, ref RECT lprcMonitor, IntPtr dwData) =>
                    {
                        uint count = 0;
                        if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, out count) && count > 0)
                        {
                            PHYSICAL_MONITOR[] monitors = new PHYSICAL_MONITOR[count];
                            if (GetPhysicalMonitorsFromHMONITOR(hMonitor, count, monitors))
                            {
                                for (int i = 0; i < count; i++)
                                {
                                    uint minB = 0, curB = 0, maxB = 0;
                                    if (GetMonitorBrightness(monitors[i].hPhysicalMonitor, out minB, out curB, out maxB))
                                    {
                                        cachedBrightness = (int)curB;
                                        break;
                                    }
                                }
                                DestroyPhysicalMonitors(count, monitors);
                            }
                        }
                        return true;
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

        public static void SyncHardwareThrottled(int targetBrightness)
        {
            if (isHardwarePending) return;
            if ((DateTime.Now - lastSyncTime).TotalMilliseconds < 100) return;

            isHardwarePending = true;
            lastSyncTime = DateTime.Now;

            Task.Run(() =>
            {
                try
                {
                    EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, (IntPtr hMonitor, IntPtr hdcMonitor, ref RECT lprcMonitor, IntPtr dwData) =>
                    {
                        uint count = 0;
                        if (GetNumberOfPhysicalMonitorsFromHMONITOR(hMonitor, out count) && count > 0)
                        {
                            PHYSICAL_MONITOR[] monitors = new PHYSICAL_MONITOR[count];
                            if (GetPhysicalMonitorsFromHMONITOR(hMonitor, count, monitors))
                            {
                                foreach (var mon in monitors)
                                {
                                    SetMonitorBrightness(mon.hPhysicalMonitor, (uint)targetBrightness);
                                }
                                DestroyPhysicalMonitors(count, monitors);
                            }
                        }
                        return true;
                    }, IntPtr.Zero);
                }
                catch { }
                finally
                {
                    isHardwarePending = false;
                }
            });
        }
    }
}
