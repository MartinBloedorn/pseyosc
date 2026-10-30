# pseyosc

Cross-platform utility that pairs PlayStation Move motion controllers (and later handles the PlayStation 3 Eye camera), and turns their input into OSC messages.

> **Status:** early. `pseyosc-pair` (pairing & testing) works on Windows with a CECH-ZCM1E controller. No OSC output yet. See [doc/DevelopmentNotes.md](doc/DevelopmentNotes.md).

## How pairing works

A Move can't be found by a Bluetooth scan. It stores the address of one host, written over USB, and connects to that host when its PS button is pressed. So pairing takes one USB plug-in per controller:

1. Plug the controller in over USB. pseyosc writes this computer's Bluetooth address into it and registers it with the OS.
2. Unplug it and press its PS button. It connects over Bluetooth, rumbles briefly, and its orb lights up in its id's color.
3. From then on, pressing PS reconnects it. It only needs pairing again after it was used with a PlayStation or another computer.

On Windows, pairing needs **Administrator** rights (psmoveapi edits the registry); without them, controllers paired before are still picked up.

## Building

Requirements: CMake ≥ 3.16 and a C++17 compiler (Visual Studio 2022 on Windows). [psmoveapi](https://github.com/thp/psmoveapi) is included as a submodule under `external/psmoveapi`, with its hidapi submodule, and its core is built as a static library.

```
git submodule update --init
git -C external/psmoveapi submodule update --init external/hidapi
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

On Linux, psmoveapi needs the development packages of D-Bus, libudev and BlueZ. Neither Linux nor macOS has been built yet.

## Usage

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
