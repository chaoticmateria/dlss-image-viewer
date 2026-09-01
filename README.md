# DLSS Image Viewer

A hardware-accelerated image and video viewer built around Direct3D 12, FFmpeg, and NVIDIA NGX/DLSS. It applies real-time DLSS Super Resolution to your media, featuring an interactive split-screen comparison mode and background exporting.

> **Disclaimer:** This project is highly experimental and unstable. Use at your own risk and Do Your Own Research (DYOR).

## Features

- **Direct3D 12 Renderer**: Fast, hardware-accelerated rendering.
- **NVIDIA NGX DLSS Integration**: Upscales images and video frames using DLSS Super Resolution (`CreateFeature` + `EvaluateFeature_C`).
- **Interactive Split Compare Mode**: Instantly compare original inputs side-by-side with DLSS-processed outputs using a draggable split line.
- **Background Image Exporting**: Instantly save processed frames to disk as PNG without UI freezing or stuttering.
- **FFmpeg Decoder Engine**: Supports decoding common image formats and most video formats (MP4, MKV, WebM, etc.).
- **Live Image Adjustments**: Adjust brightness, contrast, saturation, gamma, temperature, and tint on the fly.
- **Letterboxing & Fill Modes**: Maintains original display aspect ratios correctly with support for cropping/filling.

## System Requirements

### Hardware
- **GPU:** NVIDIA RTX GPU (DLSS support required).
  - *Tested on NVIDIA RTX 4060 Ti 16GB.*
- **OS:** Windows 10 or Windows 11 (64-bit).

### Software
- NVIDIA Display Drivers (latest recommended).
- `nvngx_dlss.dll` runtime library.

## Installation & Usage (For Normal Users)

If you just want to use the application without compiling any code, follow these simple steps:

1. **Download the Release**: Download the latest `DLSS_Image_Viewer_Release.zip` from the Releases tab and extract it to a folder.
2. **Add DLSS Libraries**: The release ZIP includes the application and FFmpeg, but you must provide the DLSS runtime. Place `nvngx_dlss.dll` in the same folder as `DLSSImageViewer.exe`.
3. **Install ReShade (Crucial for DLSS 5)**: Install **ReShade with add-on support** for `DLSSImageViewer.exe` using the **DirectX 10/11/12** rendering API. This is required if you plan to use DLSS 5 Neural Rendering.
4. **(Optional) DLSS 5 Neural Rendering Files**: Download the required DLSS 5 runtime (`nvngx_dlssnr.dll`) and `renodx-dlss5.addon64` from [here](https://app.mediafire.com/folder/sa9zioqbixj7e) and place them in the same folder.
5. **Run the App**: Launch `DLSSImageViewer.exe`. Drag and drop an image or video file onto the window.
6. **Compare & Export**: Use the **Quality** dropdown to set your DLSS mode. Click the **Compare** button to drag the interactive split-screen line, and click **Export** to instantly save the processed frame.

Typical directory layout before running:
```text
DLSSImageViewer.exe
ffmpeg.exe
ffprobe.exe
nvngx_dlss.dll
```

## Controls & Shortcuts

| Action | Shortcut |
| --- | --- |
| Open File | `Ctrl+O` |
| Split Compare Mode | `Compare` Button (UI) |
| Export Image | `Export` Button (UI) |
| Play / Pause | `Space` |
| Mute | `M` |
| Toggle DLSS | `D` |
| Image Adjustments | `Ctrl+E` |
| Fullscreen | `F11` (or double-click) |
| Debug Views (Motion, Depth) | `1`, `2`, `3`, `4`, `5` |

## Build from Source (For Developers)

### Requirements
- Windows 10/11 x64
- Visual Studio 2022 (Desktop Development with C++)
- Windows SDK
- Git for Windows
- NVIDIA RTX GPU

### Instructions
Simply run the included build script:
```bat
build_windows.bat
```
The script will fetch the DLSS SDK, download FFmpeg/FFprobe, and compile the D3D12 pipeline. Output will be generated in `build\Release\`.

## License
This project is open-source and licensed under the **MIT License**. See [LICENSE](LICENSE). 
*NVIDIA DLSS/NGX and FFmpeg are separate components subject to their own licenses.*

## Acknowledgements
This project is a modified fork based on the original **DLSS 5 Image Viewer**.  
Original repository: [https://gitlab.com/JessicaNataliaMods/dlss-5-video-player](https://gitlab.com/JessicaNataliaMods/dlss-5-video-player)
