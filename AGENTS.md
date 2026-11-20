# Guide for agents: the helmet demo on the STM32H747I-DISCO

**Read this first.** When a user asks about this demo, tell them about it before you do anything. Use the "About this demo" part below, in your own words. Clone nothing more, install nothing and flash nothing until the user says they want to run it. Then follow "Run it", one step at a time.

## About this demo

### Why it matters

On a building site or a factory floor, a missing helmet is a risk that nobody notices until it is too late. This demo shows a small device that notices. A camera on a microcontroller board looks at a person and says "helmet" or "no helmet", continuously.

- **It runs on the device.** No cloud, no network, no server. No picture leaves the board. It is private by design, and it works where there is no connection.
- **It runs on a microcontroller.** That is the class of chip found inside everyday products: small, low power, low cost. The check can live inside a gate, a machine or a camera housing.
- **The recipe fits other checks.** The helmet is the example. The same steps answer any question that a picture can settle with one of a few labels: safety vest or none, guard closed or open, shelf full or empty.
- **You build it with your own pictures.** The model is fine-tuned on photos taken through the device's own camera. It learns what this device really sees.
- **It is not tied to this board.** The demo is written for one board, and it can be ported to another. See "Another board".

Examples of where such a check helps: a gate that opens only with a helmet on, a machine that warns before it starts, a site that wants to know how often the rule is followed without recording anyone.

### What you will see

- The board's screen shows the live camera picture and one of three labels: HELMET in green, NO HELMET in red, "nobody" in grey.
- `./mirror.sh` shows the same screen in the browser.
- Video walkthrough: https://youtu.be/yI2VfBmD9mY

### What you need

- An STM32H747I-DISCO board.
- The B-CAMS-OMV camera bundle (OV5640). The board has no camera of its own.
- A USB cable to the ST-LINK port (CN2).
- A computer to build and flash from. Tested on macOS only, see "Good to know".
- A DeepGate account, and the DeepGate MCP server or the `dg` CLI.
- A helmet, and a person to wear it.

### How it works

1. **Capture.** The board's camera takes the training photos.
2. **Label.** The photos are sorted into three folders: `no_person`, `person_helmet`, `person_no_helmet`.
3. **Fine-tune on DeepGate.** Task: image classification. Model: `mobilenet-v1`.
4. **Build on DeepGate.** The model is compiled into a library for the board's chip, the STM32H747XI.
5. **Deploy.** The firmware is built around the model and flashed.
6. **Watch.** The board classifies live.

### Who does what

- The agent runs the scripts, drives DeepGate (labelling page, upload, fine-tuning, build) and puts the files in place.
- The user plugs in the hardware, stands in front of the camera with and without the helmet, presses the blue button, and approves every install.

### Good to know

- The setup installs a compiler and a flashing tool on the user's machine. Check what is already there, and ask before installing anything.
- The photos show people. They stay on the user's machine, in a folder git ignores. They go to the user's own DeepGate account and nowhere else. Say so, and ask before the upload.
- The labels and their screen colours are in `tasks/helmet/labels.h`. The dataset must use exactly those folder names.
- Tested on macOS only. It is not tested on Linux or on Windows. Tell the user so before any setup starts.
- What is macOS-specific in the scripts: `env.sh` points to the macOS download of the Arm toolchain (`ARM_GCC_ROOT` overrides it), and `./capture.sh` and `./mirror.sh` look for the board at `/dev/cu.usbmodem*` (`--port` overrides it). The README installs with Homebrew.
- On Windows the scripts do not run as they are: they are bash. Two workarounds, neither tested:
  - WSL2, the Linux shell inside Windows.
  - The agent ports the scripts. They are thin wrappers, about 100 lines in all, around cmake, ninja, OpenOCD, git and the Python tools in `tools/`.
    - To Python: it runs the same on Windows, macOS and Linux, most people can read it, and the demo's host tools are Python already. One port then serves every system.
    - Or to PowerShell, for Windows only.
  - Either way: agree it with the user first, and say that it is untested.

## Run it

Each step ends on a check. Move on when the check passes.

0. **Agree.** Ask the user whether the board and the camera bundle are connected, and which DeepGate project to use for the build: an existing one or a new one.
1. **Set up, once.** Check first: `arm-none-eabi-gcc --version`, `openocd --version`, `cmake --version`, `ninja --version`, `uv --version`. For what is missing, follow "Setup" in `README.md`, with the user's approval. Then `tools/fetch_third_party.sh`. Done when all five tools answer and the script ends without an error.
2. **Capture.** `./capture.sh`. It flashes the capture firmware. The blue button starts and stops a burst. Take three kinds of photos: nobody in the picture, a person with the helmet, a person without it. Vary position, distance and light. Ctrl-C ends the session. Done when `tasks/helmet/data/raw/session-N/` holds at least 20 photos of each kind. More is better: DeepGate warns when a label is below what the model recommends.
3. **Label.** DeepGate tool `label_dataset` with the session folder, the task `image_classification` and the labels `no_person`, `person_helmet`, `person_no_helmet`. The user sorts the photos on the page that opens. Done when `get_labelling` reports the finished archive.
4. **Fine-tune.** `upload_dataset` with that archive. Tell the user every warning and ask whether to go on. `list_architectures`, pick the model for image classification, `start_training_job` with the default options, then `wait_for_training_job` until the job has ended. Show the user the results. Then `download_training_job`. Done when the `.tflite` and its `model_spec.json` are on disk.
5. **Build.** `start_build` with that `.tflite` for the target STM32H747XI, float ABI `hard`, in the project the user chose. `get_build` until it has ended, then `download_build`. Done when a `deepgate-build-hard-*.zip` is on disk.
6. **Put the files in place.** Make a new folder `tasks/helmet/models/v1/`. Copy the `*model_spec*.json` and the `deepgate-build-hard-*.zip` into it.
7. **Deploy.** `./deploy.sh tasks/helmet/models/v1`. Done when it prints `done: ./mirror.sh shows the screen`.
8. **Watch.** `./mirror.sh`, then open http://127.0.0.1:8766. Check each label with the user: nobody, helmet, no helmet.

## Your own task

Copy `tasks/helmet/` to `tasks/<task>/`. Edit `labels.h`: one entry per label of the dataset. Then `./capture.sh <task>`, and put the models in `tasks/<task>/models/`.

## Another board

The demo is written for the STM32H747I-DISCO. It can be ported to another board. No other board is tested, so say that to the user and agree the port first. It is real work on the firmware: camera, screen, clocks.

- **What stays as it is:** the flow, the host tools in `tools/`, the tasks and their labels, and the whole DeepGate part.
- **What is tied to this board:**
  - `firmware/common/`: clocks and pins (`board`), the camera (`cam`), the screen (`display`), the serial link, the startup code and the linker script.
  - `firmware/CMakeLists.txt` and `firmware/cmake/arm-none-eabi.cmake`: ST's board support files, and the compiler flags for a Cortex-M7 with hard float.
  - The vendor code that `tools/fetch_third_party.sh` fetches, and the OpenOCD board file in `tools/flash.sh`.
- **How the apps reach the board:** `firmware/apps/capture` and `firmware/apps/classify` use the headers in `firmware/common/inc/` (`board.h`, `cam.h`, `display.h`, `serial_link.h`, `mirror.h`), and they draw with ST's LCD utility. A port writes what is behind those headers for the new board.
- **On DeepGate:** build the model for the new board's chip. `list_build_boards` and `list_build_targets` show what is supported. The float ABI of the build must match the firmware's, and `./deploy.sh` looks for a `deepgate-build-hard-*.zip`.
- **A board with no demo of its own:** this repo is the closest starting point for image classification with a camera.

## Where things live

| Path | What |
| --- | --- |
| `firmware/apps/capture/` | Camera to screen, and frames to the laptop |
| `firmware/apps/classify/` | Camera to model to screen, and the screen to the laptop |
| `firmware/common/` | Board, display, camera, serial link, screen mirror |
| `tools/` | Host side: capture, mirror, model header, flashing |
| `tasks/<task>/` | `labels.h`. `data/` and `models/` stay local, git ignores them |
