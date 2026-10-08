# Escu:de Visual Novel Modernization Hook (JXL & AV1)

A modern multimedia decoding enhancement plugin for titles developed on the proprietary **Escu:de** game engine. Through DirectX 9 proxy injection, it enables seamless decoding of high-compression **JPEG XL (JXL)** images and modern high-definition **AV1 (libsvtav1)** video.

---

## Supported Titles

- **Haison Shoujo ~Ayashimadoi no Kago no Sato~** (`haison.exe`)
- **Haison Shoujo Gaiden ~Kyoraku Mugen~** (`haison_fd.exe`)
- **Haison Shoujo 2 ~Kageri Izanau Hime no Hako~** (`haison2.exe`)

---

## Core Technical Architecture

### 1. Dynamic Engine Version Auto-Detection
Upon `DllMain` injection, the plugin parses the host executable's PE header to retrieve `AddressOfEntryPoint`, automatically matching memory offsets across three engine generations:
- **Haison 1**: EntryPoint `0x000F9429` | `LoadPNG` `0x00470850` | `CreateImageBuffer` `0x004654C0`
- **Haison Gaiden**: EntryPoint `0x000F98AC` | `LoadPNG` `0x00470C70` | `CreateImageBuffer` `0x004654E0`
- **Haison 2**: EntryPoint `0x0009D016` | `LoadPNG` `0x0044D590` | `CreateImageBuffer` `0x00441070`

### 2. JXL (JPEG XL) Image Decoding Hook
- **Hook Point**: Intercepts the engine's low-level `LoadPNG` function.
- **Transparency & Color Pipeline**:
  - Supports prioritized loading of loose external `.jxl` files and replacing `.png` byte streams inside `.bin` packages with JXL streams.
  - Decodes images via `libjxl 0.11.1`, automatically inverting RGBA color order and converting to bottom-up `BGRA` scanlines required by native Windows 32-bit DIB Sections.
  - Full support for 8-bit alpha transparency channels, eliminating dark border or color cast artifacts on character sprites.
  - Native untranscoded PNG streams pass through directly to the original libpng pipeline for seamless backward compatibility.

### 3. AV1 Hardware-Accelerated Video Playback
- **Hook Point**: Hooks `VideoPlay` and subclasses the main window to intercept media control messages (`1204` Open, `1205` Close, `1210` Poll Playback, `1211` Volume Control).
- **Decoding & Playback**:
  - Built on Windows Media Foundation and EVR (Enhanced Video Renderer).
  - Native support for AV1 video formats (`.mp4` / `.webm` / `.mkv`), prioritizing modern transcoded media.
  - Bridges DirectShow `EC_COMPLETE` notifications, supporting hotkey skipping and native volume attenuation.

---

## Directory Structure

```
escude_modern_hook/
├── src/
│   ├── hook_defs.h      # Engine offset tables and function pointer definitions
│   ├── jxl_loader.h     # JXL decoding interface
│   ├── jxl_loader.c     # libjxl decoder and BGRA DIB conversion implementation
│   ├── mf_video.h       # Video player interface
│   ├── mf_video.cpp     # Media Foundation / EVR AV1 player implementation
│   └── main.cpp         # d3d9 proxy entry point, adaptive hook, and window subclassing
├── tools/
│   ├── escarc.py        # ESC-ARC2 archive decryption and extraction tool
│   ├── batch_jxl_bin.py # Multi-threaded batch JXL transcoding and repacking tool
│   └── transcode_tool.py# Automated single-file transcoding script
├── build.bat            # One-click MinGW32 build script
├── .gitignore
└── README.md
```

---

## Building

Requirements:
- MinGW32 / MSYS2 (`i686-w64-mingw32-g++`)
- `mingw-w64-i686-libjxl` static library support

Run `build.bat` in the root directory to generate `d3d9.dll` and dependencies in `bin/`.

---

## Deployment & Asset Transcoding

1. Place the compiled `d3d9.dll` and `libwinpthread-1.dll` in the game root directory.
2. Recommended video transcoding parameters (HandBrakeCLI / SVT-AV1):
   ```bash
   HandBrakeCLI.exe -i opening.wmv -o opening.mp4 -e svt_av1 --encoder-preset 5 -q 30 -E av_aac -B 192
   ```
   Rename the original `opening.wmv` to `opening.wmv.bak`.
3. Image transcoding (JXL):
   ```bash
   python tools/batch_jxl_bin.py <bg.bin/st.bin/ev.bin/gfx.bin>
   ```
   The tool automatically extracts PNGs, transcodes them with `effort 7, quality 90`, and repacks them into `.bin` archives with the same name, backing up original files as `.bak`.
