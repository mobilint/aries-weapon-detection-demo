# Notice

This repository is no longer maintained.  
Ongoing development has moved to `aries-cv-demo`.  
This repository is kept for reference only.

---
# ARIES Weapon Detection Demo

This project is a multi-channel weapon detection demo for MLA100-based systems. The current default configuration uses one `YOLO26` model and 30 local video feeders.

## Overview

- Executable: `build/src/demo/demo`
- Default configuration files:
  - `rc/ModelSetting_MLA100.yaml`
  - `rc/FeederSetting_MLA100.yaml`
  - `rc/LayoutSetting_MLA100.yaml`
- Default layout: 30 worker tiles on a 1920x1080 background
- Default model: `mxq/yolo26s-weapon_uint8_input.mxq`
- Sample inputs: `rc/video/positive`, `rc/video/negative`

## Requirements

### Linux

- Ubuntu 20.04 or later recommended
- `cmake`
- `make` or `build-essential`
- `libopencv-dev`
- Mobilint QB Runtime and MLA100 driver

The build links against `OpenCV`, `OpenMP`, `yaml-cpp`, and `qbruntime`.

### Windows

- Visual Studio 2022
- CMake
- OpenCV 4.x
- Mobilint QB Runtime and MLA100 driver

## Quick Start

If the project is already built:

```bash
./run.sh
```

`run.sh` changes to `$HOME/aries-weapon-detection-demo/build` and launches `src/demo/demo`.

If you need to build manually:

```bash
mkdir -p build
cd build
cmake -DPRODUCT=aries2-v4 -DDRIVER_TYPE=aries2 ..
make -j"$(nproc)"
./src/demo/demo
```

The demo expects to run from the `build` directory. The application opens `../rc/...` and `../mxq/...` paths relative to the current working directory, not by resolving paths from the executable location.

## update.sh

`./update.sh` does all of the following:

- configures Git credential cache for the current user
- runs `git pull`
- installs `libopencv-dev` and `cmake` with `apt-get`
- runs CMake in `build` if the directory does not exist yet
- runs `make -j $(nproc)`
- copies the desktop entry to `$HOME/.local/share/applications/`
- copies the icon to `$HOME/.icons/`

It is an update-and-setup script, not just a build script.

## Configuration Files

At startup, the demo loads these fixed paths:

```text
../rc/LayoutSetting_MLA100.yaml
../rc/ModelSetting_MLA100.yaml
../rc/FeederSetting_MLA100.yaml
```

### ModelSetting

Current example:

```yaml
- model_type: YOLO26
  mxq_path: ../mxq/yolo26s-weapon_uint8_input.mxq
  dev_no: 0
  num_core: 8
```

- Runtime-supported model types in the current code path: `YOLO11`, `YOLO26`
- Parser-accepted model type strings: `YOLO11`, `YOLO26`
- `mxq_path`: path to the model file
- `dev_no`: MLA100 device index
- `num_core`: number of inference threads to start for that model
- `core_id`: also parsed; if `num_core` is omitted, the code uses the number of `core_id` entries

The shipped configuration contains one `YOLO26` model entry.

### FeederSetting

The shipped feeder list contains 30 `VIDEO` inputs.

```yaml
- feeder_type: VIDEO
  src_path: ../rc/video/negative/8.mp4
```

Supported feeder types:

- `CAMERA`: local camera index as a string such as `"0"`
- `IPCAMERA`: stream URL such as RTSP
- `YOUTUBE`: YouTube URL
- `VIDEO`: local video file path

`src_path` is a single string per feeder entry. To add more inputs, add more feeder items.

### LayoutSetting

The shipped layout contains one background image and 30 worker regions.

```yaml
image_layout:
  - path: ../rc/layout/layout.png
    roi: [0, 0, 1920, 1080]

worker_layout:
  - {feeder_index: 0, model_index: 0, roi: [0, 137, 320, 188]}
```

- `image_layout`: background images and their destination rectangles
- `worker_layout`: maps each tile to a `feeder_index` and `model_index`
- `feeder_layout`: parsed by the loader, but not used by the shipped `_MLA100` file

If a `worker_layout` entry references an out-of-range feeder or model index, that worker is skipped.

## Controls

### Keyboard

| Key | Action |
| --- | --- |
| `D` / `d` | Toggle FPS overlay |
| `T` / `t` | Toggle elapsed time overlay |
| `M` / `m` | Toggle fullscreen |
| `C` / `c` | Stop all workers |
| `F` / `f` | Start all workers |
| `1`, `2`, `3` | Switch mode if the extra config files exist |
| `Q` / `q` / `Esc` | Quit |

Notes:

- The key handler lowercases input, so uppercase and lowercase shortcuts behave the same.
- The `1`, `2`, and `3` mode keys try to load `../rc/LayoutSetting.yaml`, `../rc/LayoutSetting2.yaml`, `../rc/LayoutSetting3.yaml`, `../rc/ModelSetting.yaml`, and `../rc/ModelSetting2.yaml`.
- Those extra files are not included in this repository, so mode switching is unsafe unless you add compatible files yourself.

### Mouse

- Left click: start the selected worker
- Right click: stop the selected worker

## Packaging

To create a deployment package:

```bash
./package/package.sh aries2-v4 aries2
```

The script builds dependency artifacts in `build_package`, assembles `demo-package-<PRODUCT>`, creates `demo-package-<PRODUCT>.tar.gz`, and then removes the temporary build and package directories.

The generated archive contains source files, configs, model files, a package-specific `Makefile`, and `package/README.md`.

## Repository Layout

```text
.
├── mxq/                    # model files
├── rc/                     # YAML configs, layout images, sample videos
├── src/demo/               # demo application source code
├── package/                # packaging script and packaged README
├── run.sh
└── update.sh
```
