# pseyosc

Cross-platform utility that pairs PlayStation Move motion controllers (and later handles the PlayStation 3 Eye camera), and turns their input into OSC messages.

> **Status:** early. `pseyosc-pair` (pairing & testing) and `pseyosc-stream` (OSC) work on Windows with a CECH-ZCM1E controller; no sensor fusion yet (orientation is a placeholder). `pseyosc-video` shows the PS3 Eye camera's live image, and `pseyosc-track` tracks the controllers' orbs with it (Windows only so far; positions aren't streamed as OSC yet). See [doc/Streaming.md](doc/Streaming.md). See [doc/DevelopmentNotes.md](doc/DevelopmentNotes.md).

## How pairing works

A Move can't be found by a Bluetooth scan. It stores the address of one host, written over USB, and connects to that host when its PS button is pressed. So pairing takes one USB plug-in per controller:

1. Plug the controller in over USB. pseyosc writes this computer's Bluetooth address into it and registers it with the OS.
2. Unplug it and press its PS button. It connects over Bluetooth, rumbles briefly, and its orb lights up in its id's color.
3. From then on, pressing PS reconnects it. It only needs pairing again after it was used with a PlayStation or another computer.

On Windows, pairing needs **Administrator** rights (psmoveapi edits the registry); without them, controllers paired before are still picked up.

## Building

Requirements: CMake ≥ 3.16 and a C++17 compiler (Visual Studio 2022 on Windows). [psmoveapi](https://github.com/thp/psmoveapi) is included as a submodule under `external/psmoveapi`, and its core is built as a static library. Its submodules hidapi (controllers), and libusb and PS3EYEDriver (camera, Windows) are built from source too.

```
git submodule update --init
git -C external/psmoveapi submodule update --init external/hidapi external/libusb-1.0 external/PS3EYEDriver
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

On Linux, psmoveapi needs the development packages of D-Bus, libudev and BlueZ. Neither Linux nor macOS has been built yet; `pseyosc-video` and `pseyosc-track` are only built on Windows.

`pseyosc-track` (psmoveapi's tracker) also needs OpenCV 4. It isn't installed system-wide, but built in the workspace's shared `lib` folder (next to `pseyosc`), see [lib/README.md](../lib/README.md):

```
sh ../lib/opencv/build.sh
```

CMake looks for it only in `PSEYOSC_OPENCV_DIR`, by default `../lib/opencv/install`; to use another OpenCV 4 install, pass e.g. `-DPSEYOSC_OPENCV_DIR=C:/opencv/build` (the folder containing `OpenCVConfig.cmake`). Without OpenCV, CMake warns and skips `pseyosc-track`.

## Camera setup (Windows)

The PS3 Eye is read directly over USB (PS3EYEDriver, via libusb), which needs the WinUSB driver on the camera's video interface. Once per computer (and USB port, if Windows treats it as a new device):

1. Download [Zadig](https://zadig.akeo.ie/) ([2.9](https://github.com/pbatard/libwdi/releases/download/v1.5.1/zadig-2.9.exe)) and run it (it asks for Administrator rights).
2. Enable *Options → List All Devices*.
3. Select **USB Camera-B4.09.24.1 (Interface 0)**: USB ID `1415 2000 00`. Not *Interface 1* (the microphone, which keeps working with Windows' driver), nor the entry without an interface (the composite parent device).
4. Choose **WinUSB** as the target driver, and click *Install Driver*.

Device Manager then lists the camera under "Universal Serial Bus devices". To undo: uninstall that device in Device Manager, with *Delete the driver software for this device*, and replug the camera.

## Usage

### `pseyosc-stream`: OSC output

```
build\Release\pseyosc-stream.exe --host 127.0.0.1 --port 4321 --prefix pseyosc --in-port 4322
```

This pairs and connects controllers like `pseyosc-pair` (it takes the same options), and streams every controller's input to the OSC target, for example:

```
/pseyosc/0/status              "move"
/pseyosc/0/move/button/cross   1
/pseyosc/0/move/button/trigger 0.42
/pseyosc/0/move/accel          0.54 0.03 0.83
/pseyosc/0/move/gyro           0.44 0.73 -0.45
/pseyosc/0/battery             0.8
```

With `--in-port`, it also takes `/pseyosc/0/move/rumble/trigger 0.5`, `.../rumble/pulse 1 300` and `/pseyosc/0/move/led 0 0.5 1` (orb color). `move/rpy` and `move/quat` are placeholders until sensor fusion is added. See [doc/Streaming.md](doc/Streaming.md) for all addresses and options. To check the output: `python tools/oscdump.py 4321 20`.

### `pseyosc-pair`: pairing & testing

Run it from a terminal opened as Administrator (Windows):

```
build\Release\pseyosc-pair.exe
```

It pairs every controller plugged in over USB that isn't paired with this computer yet, prints a line for each controller that connects over Bluetooth, and one for each button press:

```
Bluetooth radio 44:85:00:BF:B6:D8
[usb] Motion Controller 00:13:8A:9A:FE:2E: pairing (was paired with A8:47:4A:EC:B6:BE)
[usb] Motion Controller 00:13:8A:9A:FE:2E: paired
[0] Motion Controller 00:13:8A:9A:FE:2E connected
[0] trigger pressed
```

Options:

- `--repair` registers controllers with the OS again even if they already store this computer's address, e.g. after removing them in the Bluetooth settings.
- `--shutdown-on-exit` switches off all connected controllers when quitting. A Move has no power button: like on a PlayStation, it stays on while connected, and the host switches it off by dropping its Bluetooth connection. Pressing PS brings it back.
- `--idle-timeout <minutes>` switches off controllers that have gone unused that long. For now only buttons and the trigger count as use, not motion.
- `--id <id>=<address>` always gives this controller this id (and orb color), e.g. `--id 0=00:13:8A:9A:FE:2E`. On Ctrl+C, the tool prints these arguments for the connected controllers.

### `pseyosc-video`: camera viewer

```
build\Release\pseyosc-video.exe [--camera 0] [--resolution 640x480|320x240] [--fps 60]
```

Opens the camera (see [Camera setup](#camera-setup-windows)) and shows its live image in a window, with automatic gain, exposure and white balance.

### `pseyosc-track`: tracking test

```
build\Release\pseyosc-track.exe [--exposure 0.3] [--mirror]
```

Tracks the orbs of all controllers connected over Bluetooth (pair them with `pseyosc-pair` first and press PS; no other pseyosc tool may be running) with psmoveapi's tracker. Each orb blinks briefly while the tracker learns its color in the camera image; then the window shows the darkened camera image with the tracked orbs, and the terminal prints their positions ten times a second:

```
[0] x  312.4 y  201.7 r  14.2 px,   93 cm
```

`x`/`y` is the orb's center in the image and `r` its radius, in pixels; the distance from the camera is estimated from the radius, and is calibrated by psmoveapi for the lens's wide-angle setting (blue dot). The numbers `[0]`, `[1]` are psmoveapi's order of the controllers, not `pseyosc-stream`'s ids. R calibrates again (e.g. after the lighting changed), Esc or Ctrl+C quits. `--exposure` (0-1) sets the camera's fixed exposure: lower values leave less but the orbs visible. The image is shown as the camera sees it, like `pseyosc-video`; `--mirror` flips it horizontally, like a mirror (positions included). The window title shows the measured frame rate. The camera supports up to 75 Hz at 640x480 and 187 Hz at 320x240 (higher requests are capped); other rates become the next lower one it supports (e.g. 60, 50, 40, 30 at 640x480). Esc, closing the window, or Ctrl+C quits.
