# Quickstart & Verification Guide: Sub-100ms Instant Download Startup

## 1. Quick Verification
To check startup latency for any download:
```powershell
python print_startup_log.py
```
Look for:
- `Download task created`
- `Scheduler dispatches task`
- `First data bytes received`
- **Total Startup Time (Click -> First Byte)**: Must be `< 100 ms` (target `<= 10 ms`).

## 2. Testing Procedure
1. Open Chrome with Radium Extension loaded.
2. Navigate to YouTube (e.g. `https://www.youtube.com/watch?v=lvJrFoglKL0` or any video).
3. Click "Download [quality]" on the video overlay badge or popup.
4. Run `python print_startup_log.py` in powershell.
5. Confirm `Total Startup Time` is strictly under 100 ms without yt-dlp delay.
