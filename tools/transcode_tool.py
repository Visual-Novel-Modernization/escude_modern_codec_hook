"""
Escu:de Visual Novel Modernization Tool: JXL & AV1 Transcoder
Supports:
  1. Haison (Haison Shoujo ~Ayashimadoi no Kago no Sato~)
  2. Haison Gaiden (Haison Shoujo Gaiden ~Kyoraku Mugen~)
  3. Haison 2 (Haison Shoujo 2 ~Kageri Izanau Hime no Hako~)
"""

import os
import sys
import subprocess
import shutil
import glob
from escarc import parse

CJXL_PATH = os.environ.get("CJXL_PATH", "cjxl")
FFMPEG_PATH = os.environ.get("FFMPEG_PATH", "ffmpeg")

def convert_video_to_av1(input_video, output_video, crf=28, preset=8):
    if not os.path.exists(input_video):
        print(f"[!] Input video not found: {input_video}")
        return False
    print(f"[*] Transcoding {input_video} -> {output_video} (AV1 libsvtav1 CRF {crf})...")
    cmd = [
        FFMPEG_PATH, "-i", input_video,
        "-c:v", "libsvtav1", "-preset", str(preset), "-crf", str(crf),
        "-c:a", "aac", "-b:a", "192k",
        output_video, "-y"
    ]
    res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if res.returncode == 0 and os.path.exists(output_video):
        orig_sz = os.path.getsize(input_video) / (1024 * 1024)
        new_sz = os.path.getsize(output_video) / (1024 * 1024)
        ratio = (1.0 - (new_sz / orig_sz)) * 100
        print(f"[+] Done: {orig_sz:.1f} MB -> {new_sz:.1f} MB ({ratio:.1f}% reduction)")
        return True
    else:
        print(f"[!] FFmpeg error: {res.stderr.decode('utf-8', 'ignore')[-500:]}")
        return False

def extract_and_convert_jxl(bin_path, out_dir, cjxl_path=CJXL_PATH, distance=1.0):
    bin_name = os.path.basename(bin_path)
    prefix = os.path.splitext(bin_name)[0]
    ents, count, nsz = parse(bin_path)
    print(f"[*] Processing {bin_name}: {count} entries...")
    
    png_count = 0
    with open(bin_path, "rb") as f:
        for name, off, sz in ents:
            if name.lower().endswith(".png"):
                f.seek(off)
                raw_png = f.read(sz)
                
                # Loose path inside out_dir: e.g. prefix / name
                rel_jxl = os.path.splitext(name)[0] + ".jxl"
                loose_path = os.path.join(out_dir, prefix, rel_jxl)
                os.makedirs(os.path.dirname(loose_path), exist_ok=True)
                
                temp_png = loose_path + ".tmp.png"
                with open(temp_png, "wb") as pf:
                    pf.write(raw_png)
                
                cmd = [cjxl_path, temp_png, loose_path, "-d", str(distance), "--effort", "7"]
                res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                if os.path.exists(temp_png):
                    os.remove(temp_png)
                
                png_count += 1
                if png_count % 50 == 0:
                    print(f"    Transcoded {png_count} images to JXL...")

    print(f"[+] {bin_name}: Finished {png_count} images.")

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python transcode_tool.py video <input.wmv> <output.mp4>")
        print("  python transcode_tool.py images <input.bin> <game_root_dir>")
        sys.exit(1)
        
    action = sys.argv[1].lower()
    if action == "video":
        inp = sys.argv[2]
        out = sys.argv[3]
        convert_video_to_av1(inp, out)
    elif action == "images":
        inp_bin = sys.argv[2]
        game_root = sys.argv[3]
        extract_and_convert_jxl(inp_bin, game_root)
