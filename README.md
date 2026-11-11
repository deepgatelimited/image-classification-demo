# Image classification on STM32H747I-DISCO with DeepGate

Capture images with the board's camera, train a classifier on
[DeepGate](https://deepgate.ai), and run it live on the same board.
The example task: is the person in frame wearing a helmet?

**Video walkthrough:** https://youtu.be/yI2VfBmD9mY

## Hardware

- STM32H747I-DISCO
- B-CAMS-OMV camera bundle (OV5640)
- USB cable to the ST-LINK port (CN2)

## Setup (macOS)

- The arm-none-eabi compiler: from the
  [Arm GNU Toolchain downloads](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads),
  get the macOS (Apple silicon) bare-metal tarball,
  `arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi.tar.xz`, and extract it:

  ```bash
  mkdir -p ~/.local/toolchains
  tar -xf ~/Downloads/arm-gnu-toolchain-15.3.rel1-darwin-arm64-arm-none-eabi.tar.xz -C ~/.local/toolchains
  ```

  `env.sh` expects it there; if your version differs, edit `ARM_GCC_ROOT` in it.
- `brew install open-ocd cmake ninja uv`

```bash
tools/fetch_third_party.sh
```

## Steps

**1. Capture.** Flashes the capture firmware; the blue button starts and stops
a burst. Images land in `tasks/helmet/data/raw/session-N/`.

```bash
./capture.sh
```

**2. Train and build on DeepGate.** Zip the session, upload it as a dataset,
train `mobilenet-v1`, then build the model for the **STM32H747XI** (the
board's microcontroller), float ABI **hard**. Download the model spec (`*model_spec*.json`) and the build
(`deepgate-build-hard-*.zip`) into a new folder, e.g. `tasks/helmet/models/v1/`.

**3. Deploy.** Builds the classify firmware with your model and flashes it.

```bash
./deploy.sh tasks/helmet/models/v1
```

**4. Mirror.** Shows the board's screen in your browser (`--record take.mp4`
to record it).

```bash
./mirror.sh
```

## Your own task

Copy `tasks/helmet/` to `tasks/<task>/`, edit `labels.h` (one entry per label
in your dataset), then `./capture.sh <task>` and put models in
`tasks/<task>/models/`.

## Layout

```
firmware/apps/capture/    camera -> LCD + frames to the laptop
firmware/apps/classify/   camera -> model -> LCD (+ screen to the laptop)
firmware/common/          board, display, camera, serial link, screen mirror
tools/                    host side: capture.py, mirror.py, gen_model_meta.py, flashing
tasks/<task>/             labels.h; data/ and models/ stay local (gitignored)
```
