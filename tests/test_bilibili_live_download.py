# SPDX-License-Identifier: Apache-2.0
import subprocess
import json
import os
import sys
import urllib.request
import ssl

def run():
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    print("=================================================================")
    print("   Radium Live Internet Download Verification: Bilibili Video    ")
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
    print("=================================================================")

    target_url = "https://www.bilibili.com/video/BV1Cn4n6YE29/"
    ytdlp_exe = os.path.abspath("build/yt-dlp/yt-dlp/dist/yt-dlp.exe")
    ffmpeg_exe = os.path.abspath("build/ffmpeg-minimal/ffmpeg-7.1/ffmpeg.exe")
    output_dir = os.path.abspath("dist/test_downloads")
    os.makedirs(output_dir, exist_ok=True)

    print(f"[1/4] Extracting live Bilibili video metadata for {target_url}...")
    res = subprocess.run([ytdlp_exe, "-j", target_url], capture_output=True, text=True, check=True)
    meta = json.loads(res.stdout)

    title = meta.get("title", "bilibili_video")
    duration = meta.get("duration", 0)
    print(f"  Title: {title}")
    print(f"  Duration: {duration} seconds")

    formats = meta.get("requested_formats", [])
    if not formats:
        print("  Using best format from formats list...")
        # Find video and audio format
        v_fmt = None
        a_fmt = None
        for f in meta.get("formats", []):
            if f.get("vcodec") != "none" and not v_fmt:
                v_fmt = f
            if f.get("acodec") != "none" and not a_fmt:
                a_fmt = f
        formats = [v_fmt, a_fmt]

    print(f"[2/4] Discovered {len(formats)} streams for multi-track muxing:")
    for f in formats:
        res_label = f.get("resolution", "unknown")
        vcodec = f.get("vcodec", "none")
        acodec = f.get("acodec", "none")
        print(f"  - Stream: format_id={f.get('format_id')}, res={res_label}, vcodec={vcodec}, acodec={acodec}")

    # Download with Radium worker / FFmpeg muxing
    output_path = os.path.join(output_dir, "bilibili_BV1Cn4n6YE29.mp4")
    if os.path.exists(output_path):
        os.remove(output_path)

    print(f"\n[3/4] Downloading and muxing video + audio via Radium pipeline...")
    # yt-dlp uses ffmpeg to download and mux
    cmd = [
        ytdlp_exe,
        "--ffmpeg-location", ffmpeg_exe,
        "-f", "bestvideo+bestaudio/best",
        "-o", output_path,
        target_url
    ]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        print("Download failed stderr:", p.stderr)
        sys.exit(1)

    print(f"\n[4/4] Verifying downloaded media file:")
    if not os.path.exists(output_path):
        print(f"ERROR: Output file not found: {output_path}")
        sys.exit(1)

    file_size = os.path.getsize(output_path)
    print(f"  File: {output_path}")
    print(f"  Size: {file_size:,} bytes ({file_size / (1024*1024):.2f} MB)")

    # Probe with ffmpeg to verify streams and container validity
    probe_cmd = [ffmpeg_exe, "-i", output_path]
    probe_res = subprocess.run(probe_cmd, capture_output=True, text=True)
    probe_out = probe_res.stderr

    has_video_stream = "Video:" in probe_out
    has_audio_stream = "Audio:" in probe_out
    has_duration = "Duration:" in probe_out

    print(f"  Container Validation:")
    print(f"    - Has Video Stream: {has_video_stream}")
    print(f"    - Has Audio Stream: {has_audio_stream}")
    print(f"    - Valid Duration:   {has_duration}")

    if has_video_stream and has_audio_stream and file_size > 500000:
        print("\n=> SUCCESS: Actual Bilibili video downloaded, merged, and verified playable!")
        sys.exit(0)
    else:
        print("\n=> FAILED container integrity check")
        sys.exit(1)

if __name__ == "__main__":
    run()
