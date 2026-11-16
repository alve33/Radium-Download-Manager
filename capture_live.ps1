$cSource = @"
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public class WindowCapture {
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint nFlags);

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

    [StructLayout(LayoutKind.Sequential)]
    public struct RECT {
        public int Left;
        public int Top;
        public int Right;
        public int Bottom;
    }

    public static void Capture(IntPtr hwnd, string path) {
        RECT rc;
        GetWindowRect(hwnd, out rc);
        int w = rc.Right - rc.Left;
        int h = rc.Bottom - rc.Top;
        if (w <= 0 || h <= 0) { w = 1280; h = 720; }
        using (Bitmap bmp = new Bitmap(w, h)) {
            using (Graphics g = Graphics.FromImage(bmp)) {
                IntPtr hdc = g.GetHdc();
                PrintWindow(hwnd, hdc, 2);
                g.ReleaseHdc(hdc);
            }
            bmp.Save(path, ImageFormat.Png);
        }
    }
}
"@

Add-Type -TypeDefinition $cSource -ReferencedAssemblies System.Drawing

$p = Get-Process -Name 'radium','Radium' -ErrorAction SilentlyContinue
if (-not $p) {
    $p = Start-Process -FilePath 'c:\Users\sayed\Downloads\Radium\dist\Radium.exe' -PassThru
    Start-Sleep -Seconds 2
}

$hwnd = $p[0].MainWindowHandle
if ($hwnd -eq [IntPtr]::Zero) {
    $hwnd = [WindowCapture]::FindWindowW($null, "Radium Download Manager")
}

$outPath = 'C:\Users\sayed\.gemini\antigravity\brain\2edbb647-4e27-4c7b-aa5c-bbf101ff3796\live_app_capture_v2.png'
[WindowCapture]::Capture($hwnd, $outPath)
Write-Output "Captured to: $outPath"
