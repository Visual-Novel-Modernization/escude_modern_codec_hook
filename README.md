# Escu:de Visual Novel Modernization Hook (JXL & LAV Filters AV1)

A modern multimedia decoding enhancement plugin for titles developed on the proprietary **Escu:de** game engine. Through DirectX 9 proxy injection, it enables seamless decoding of high-compression **JPEG XL (JXL)** images and modern high-definition AV1 video via **LAV Filters (DXVA2 / D3D11 GPU Hardware Acceleration)**.

> Current branch: `feat/lavfilters` (in-process registration-free 32-bit LAV Filters architecture, delivering native GPU hardware decoding and maximum smoothness)

---

## Tested Titles (Known working)

- **Haison Shoujo ~Ayashimadoi no Kago no Sato~** (`haison.exe`)
- Compatible engine family: *Haison Shoujo Gaiden ~Kyoraku Mugen~* (`haison_fd.exe`), *Haison Shoujo 2 ~Kageri Izanau Hime no Hako~* (`haison2.exe`)



Other titles from Escu:de may or may not work with this hook depending on their engine version.

## Core Technical Architecture

### 1. In-Process Registration-Free LAV Filters Direct Injection (DirectShow Hardware Decoding)
- **Registration-Free COM Architecture**: No administrator privileges required, no need to run `regsvr32`, and no registry modifications.
- **DirectShow Topology Hijacking**:
  - Intercepts `ole32.dll!CoCreateInstance(CLSID_FilterGraph)` and hooks `IGraphBuilder::RenderFile` (vtable slot 13).
  - When the engine requests video playback, detects matching `.mp4` / `.webm` / `.mkv` files; on match, dynamically loads 32-bit `LAVSplitter.ax`, `LAVVideo.ax`, and `LAVAudio.ax` from the local `lav/` folder.
  - Mounts LAV splitter and audio/video decoders directly into the engine's own `FilterGraph` and renders output pins.
- **Full Hardware Acceleration & Zero Conflict**:
  - `LAV Video Decoder` automatically harnesses discrete GPU **DXVA2 / D3D11** hardware decoding engines (RTX 30/40 series, Intel Arc, AMD RDNA2/3).
  - Output feeds directly into the game's native `IVideoWindow` and DirectSound renderer, preserving original keyboard skipping, window message handling, and A/V sync logic--completely eliminating black screens and stuttering for smooth 60 FPS playback.

### 2. JXL (JPEG XL) Image Decoding Hook
- **Dynamic BPP Adaptive Detection**:
  - Reads `alpha_bits` and `num_extra_channels` from the JXL header.
  - **With Alpha Channel (Sprites / CG / Effects)**: Allocates a 32-bit DIB (`bpp = 32`), written in BGRA format for flawless alpha blending.
  - **Without Alpha Channel (Backgrounds)**: Allocates a 24-bit DIB (`bpp = 24`), written in BGR format, ensuring engine backbuffer clears full frames to prevent UI ghosting/trailing artifacts.
- **Static Embedding & Fallback Compatibility**:
  - Statically linked `libjxl 0.11.1`, supporting both archive replacement and loose file loading.
  - System font glyphs and UI mask files remain untouched in their native palette format.

---

## Directory Structure

```
escude_modern_hook/
├── src/
│   ├── hook_defs.h      # Engine offset tables and function pointer definitions
│   ├── jxl_loader.h     # JXL decoding interface
│   ├── jxl_loader.c     # libjxl decoder and 24/32-bit adaptive conversion
│   ├── lav_hook.h       # LAV Filters registration-free injection interface
│   ├── lav_hook.cpp     # DirectShow FilterGraph hooking and LAV instance creation
│   └── main.cpp         # d3d9 proxy entry point, adaptive hooks
├── tools/
│   ├── escarc.py        # ESC-ARC2 archive decryption and extraction tool
│   ├── batch_jxl_bin.py # Multi-threaded batch JXL transcoding and repacking tool
│   └── transcode_tool.py# Automated single-file transcoding script
├── build.bat            # One-click MinGW32 fully-static build script
├── .gitignore
└── README.md
```

---

## Deployment

The game root directory only requires the following file layout:
```
<game_root>/
├── d3d9.dll                  # Core hook plugin (independent of MSYS runtime)
├── libwinpthread-1.dll       # Runtime support library
├── lav/                      # 32-bit LAV Filters runtime folder
│   ├── LAVSplitter.ax
│   ├── LAVVideo.ax           # GPU hardware accelerated video decoder
│   ├── LAVAudio.ax
│   └── avcodec-lav-63.dll etc.
├── opening.mp4               # AV1 compressed video (~19.5 MB, 89% reduction)
├── opening.wmv.bak           # Original video backup
├── bg.bin / st.bin / ev.bin  # JXL compressed archives
└── gfx.bin                   # Original system font and UI archive
```
