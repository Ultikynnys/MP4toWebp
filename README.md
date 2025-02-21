# MP4toWebp

Console Application that converts MP4 to WebP.

---

## Overview

**MP4toWebp** is a command-line tool designed to convert video files (MP4 or MKV) into animated WebP images. Leveraging FFmpeg for video decoding and WebP libraries for image encoding, this application offers high-quality conversion with customizable settings including adjustable quality, lossless encoding, custom frame rates, and video scaling. It also features multi-threaded frame processing with a real-time progress indicator.

---

## Features

- **Video to WebP Conversion:** Converts MP4 or MKV videos into animated WebP files.
- **Adjustable Quality:** Set output quality (0–100, default is 80) and choose between lossy and lossless encoding.
- **Custom Frame Rate:** Configure the output frame rate (default is 30 FPS).
- **Multi-threaded Processing:** Utilizes a thread pool for concurrent frame conversion.
- **Scaling:** Supports user-defined scaling of the input video.
- **Progress Indicator:** Displays a real-time progress bar showing conversion percentage, frame count, and file size.

### Showcase

#### Drag MP4 files directly to the executable
![](https://raw.githubusercontent.com/Ultikynnys/MP4toWebp/refs/heads/main/Show1.webp)

#### Or Use the CLI
![](https://raw.githubusercontent.com/Ultikynnys/MP4toWebp/refs/heads/main/Show2.webp)

---

## Usage

Run the application from the command line with the following syntax:

```bash
MP4toWebp <input.mp4|input.mkv> [output.webp] [options]
```

### Command-Line Options

- **`-q, --quality <value>`**  
  Set the output quality (0–100, default: 80).

- **`-l, --lossless`**  
  Enable lossless WebP encoding (default is lossy).

- **`-f, --framerate <value>`**  
  Set the output frame rate in FPS (default: 30).

- **`--thread_level <value>`**  
  Set the extra thread level for the WebP encoder (default: 0).

- **`--method <value>`**  
  Choose the quality/speed trade-off for WebP encoding (0 = fast, 6 = slower but better quality; default: 0).

- **`--target_size <value>`**  
  Specify the target file size in MB (internally converted to bytes; default: 0).

- **`--scale <value>`**  
  Set the scale factor for the input video (default: 0.5).

### Example

Convert `sample.mkv` to an animated WebP with custom parameters:

```bash
MP4toWebp sample.mkv output.webp -q 75 -l -f 24 --method 4 --target_size 2 --scale 0.5
```



---

## FFmpeg Disclaimer

This software uses code from [FFmpeg](http://ffmpeg.org) licensed under the [LGPLv2.1](http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html). The FFmpeg source can be downloaded [here](link_to_your_sources).

**Included FFmpeg Libraries Information:**

- **Package:** ffmpeg  
- **Version:** 7.1  
- **Port-Version:** 2  
- **Depends:** vcpkg-cmake-get-vars, vcpkg-pkgconfig-get-modules  
- **Architecture:** x64-windows  
- **Multi-Arch:** same  

FFmpeg 7.1 version source code is distributed as required in the repository.

---

## About

This software uses libraries from the FFmpeg project under the LGPLv2.1 license. By integrating FFmpeg, the application delivers efficient and robust video decoding, ensuring a high-performance conversion process from MP4/MKV to animated WebP.

Enjoy converting your videos to animated WebP with MP4toWebp!