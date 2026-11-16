import os, sys, time, subprocess, ctypes
from ctypes import wintypes
from PIL import Image

user32 = ctypes.windll.user32
gdi32 = ctypes.windll.gdi32
kernel32 = ctypes.windll.kernel32
user32.SetProcessDPIAware()

class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [
        ('biSize', wintypes.DWORD),
        ('biWidth', wintypes.LONG),
        ('biHeight', wintypes.LONG),
        ('biPlanes', wintypes.WORD),
        ('biBitCount', wintypes.WORD),
        ('biCompression', wintypes.DWORD),
        ('biSizeImage', wintypes.DWORD),
        ('biXPelsPerMeter', wintypes.LONG),
        ('biYPelsPerMeter', wintypes.LONG),
        ('biClrUsed', wintypes.DWORD),
        ('biClrImportant', wintypes.DWORD)
    ]

def capture_window(hwnd, out_path):
    rect = wintypes.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(rect))
    w = rect.right - rect.left
    h = rect.bottom - rect.top
    if w <= 0 or h <= 0:
        return False

    hdc_screen = user32.GetDC(0)
    hdc_mem = gdi32.CreateCompatibleDC(hdc_screen)
    hbm = gdi32.CreateCompatibleBitmap(hdc_screen, w, h)
    old_bm = gdi32.SelectObject(hdc_mem, hbm)

    user32.ShowWindow(hwnd, 5) # SW_SHOW
    user32.SetForegroundWindow(hwnd)
    time.sleep(0.5)

    res = user32.PrintWindow(hwnd, hdc_mem, 2)
    if not res:
        gdi32.BitBlt(hdc_mem, 0, 0, w, h, hdc_screen, rect.left, rect.top, 0x00CC0020)

    bmp_header = BITMAPINFOHEADER()
    bmp_header.biSize = ctypes.sizeof(BITMAPINFOHEADER)
    bmp_header.biWidth = w
    bmp_header.biHeight = -h
    bmp_header.biPlanes = 1
    bmp_header.biBitCount = 32
    bmp_header.biCompression = 0

    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(hdc_mem, hbm, 0, h, buf, ctypes.byref(bmp_header), 0)

    gdi32.SelectObject(hdc_mem, old_bm)
    gdi32.DeleteObject(hbm)
    gdi32.DeleteDC(hdc_mem)
    user32.ReleaseDC(0, hdc_screen)

    img = Image.frombuffer('RGBA', (w, h), buf, 'raw', 'BGRA', 0, 1).convert('RGB')
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    img.save(out_path, quality=95)
    print(f'Captured {w}x{h} to {out_path}')
    return True

WNDENUMPROC = ctypes.WINFUNCTYPE(ctypes.c_bool, wintypes.HWND, wintypes.LPARAM)
def find_window(cls):
    found = []
    def enum_cb(hwnd, lparam):
        buf = ctypes.create_unicode_buffer(256)
        user32.GetClassNameW(hwnd, buf, 256)
        if buf.value == cls and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True
    user32.EnumWindows(WNDENUMPROC(enum_cb), 0)
    return found[0] if found else None

# First stop any existing instances
subprocess.run(['powershell', '-Command', "Stop-Process -Name 'radium','Radium' -Force -ErrorAction SilentlyContinue"], capture_output=True)
time.sleep(0.5)

proc = subprocess.Popen([r'C:\Users\sayed\Downloads\Radium\dist\Radium.exe'])
time.sleep(2.0)
hwnd = find_window('Radium_MainWindowClass')
print('Found hwnd:', hwnd)
if hwnd:
    capture_window(hwnd, r'C:\Users\sayed\.gemini\antigravity\brain\2edbb647-4e27-4c7b-aa5c-bbf101ff3796\live_app_capture_v3.png')
