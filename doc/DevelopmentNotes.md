# Development notes

This is a handover for agents continuing work on pseyosc. It collects the technical decisions, findings, the current state, and open work. `../wiimosc` is the sister project for Wiimotes; its structure (hub, listener, tools) is mirrored here, and its docs are a reference for what is planned (OSC streaming, fusion, idle timeout, shutdown).

Last updated: 2026-10-30.

## 1. State of the project

| Area | State |
|---|---|
| Windows build (VS 2022, x64) | ✅ builds without warnings |
| Detecting a controller on USB, reading its address and stored host | ✅ works on hardware (CECH-ZCM1E) |
| Refusing to pair without Administrator rights | ✅ works |
| Calibration read over USB and saved (`%APPDATA%\.psmoveapi\<addr>.calibration`) | ✅ works |
| Pairing as Administrator (write host address, register with Windows) | ✅ works: "Connection verified" after unplugging and pressing PS |
| Bluetooth pickup, LED/rumble feedback, button presses, disconnect on quit | ✅ works (user-confirmed); an idle controller keeps sending reports, so the 3 s timeout didn't fire |
| Reconnecting a paired controller by PS without Administrator rights | ✅ works |
| `--shutdown-on-exit`, `--idle-timeout` (drop the Bluetooth link, Windows only) | ✅ works (user-confirmed): the controller switches off, PS brings it back |
| Linux, macOS | ⏳ CMake written, never built |
| OSC output, camera | ⏳ not started |

**Development hardware** (user's machine, Windows 10, VS 2022, CMake 3.30):

- Radio: Intel Wireless Bluetooth, `44:85:00:BF:B6:D8`.
- Move controller CECH-ZCM1E (PS3 model, USB PID `0x03d5`): `00:13:8A:9A:FE:2E`. Before pseyosc, it was paired with `A8:47:4A:EC:B6:BE` (probably a PlayStation).

## 2. Layout & build

```
src/hub/       MoveHub + AbstractHubListener: USB pairing, Bluetooth pickup, threads, feedback, idle timeout;
               BluetoothLink: drops a device's Bluetooth link (BluetoothLink_win.cpp; _other.cpp is a stub)
src/pairtool/  pseyosc-pair: listener that prints button presses + CLI
external/psmoveapi/  thp/psmoveapi (BSD), submodule, PATCHED (see §4); only its hidapi submodule is needed
doc/           this file
```

- psmoveapi is **not** built through its own CMakeLists: that also builds the OpenCV tracker, libusb, the examples and utilities, requires all its submodules, and changes global compiler flags (`/W4` etc.). pseyosc's CMakeLists builds a static `psmoveapi` target from the core: `psmove.c`, `psmove_calibration.c`, `psmove_orientation.cpp`, `daemon/moved_client.cpp` (referenced by `psmove.c`), `math/*`, the platform port and hidapi. `psmove_config.h` is generated into `build/psmoveapi/`. The tracker (camera) will need more of it later.
- The target's include directories are public, including psmoveapi's `src/` (private headers: `psmove_private.h` for `psmove_connect_internal`, `_psmove_read_btaddrs`, the VID/PIDs; `psmove_port.h` for the host address and the permission check).
- **Running the tool from Git Bash**: `timeout -s INT` doesn't reach a Windows console program (it gets killed, exit 124), and piping into `head` buffers all output until the program exits.

## 3. Pairing & hub: findings and decisions

- **A Move is never discoverable.** Pairing means writing the host's address into the controller over USB (feature report `PSMove_Req_SetBTAddr`), and registering it with the OS. psmoveapi's `psmove_pair()` does both. So the hub's "scan" is a poll of the HID devices on USB, not a Bluetooth inquiry.
- **`psmove_pair()` on Windows blocks without a timeout** (`psmove_port_windows.c`, `handle_windows8_and_later`): it makes the radio connectable and discoverable, then loops until the controller has connected over Bluetooth, sets `VirtuallyCabled` under `HKLM\SYSTEM\CurrentControlSet\Services\HidBth\Parameters\Devices\<radio><move>` (hence Administrator), and enables the HID service. After ~24 s without a connection it removes the device and starts over. It prints its own instructions ("Unplug the controller..."). The hub therefore runs it on a detached thread per controller, and remembers which addresses are being paired, so plugging one in again doesn't start a second loop. ZCM2 controllers additionally get authenticated (`BluetoothAuthenticateDeviceEx`); not relevant for the ZCM1.
- **Controllers already storing this computer's address aren't paired again** (they'd start the blocking loop every time they're plugged in, e.g. for charging). `--repair` overrides this, for when Windows forgot the device.
- **Finder**: every 500 ms, `hid_enumerate` for VID `0x054c`, PIDs `0x03d5` (ZCM1) and `0x0c5e` (ZCM2). On Windows each controller appears as three collections; psmoveapi wants the `&col01#` path (it derives the `&col02#` one for the address feature report). New paths are opened with `psmove_connect_internal()`, which tells USB from Bluetooth (on Windows by the serial: `"0"` on USB, the address over Bluetooth). USB paths are remembered until unplugged; Bluetooth ones are skipped while a device thread has them open.
- **Device threads** poll with `psmove_poll()` (non-blocking) and call `psmove_update_leds()` every iteration: it sends changed LEDs/rumble, and resends unchanged ones every 4 s, since the controller switches them off otherwise. A disconnection is assumed after 3 s without input reports, or when the LED write fails. An idle Move keeps sending reports (verified on hardware).
- psmoveapi logs to stderr with ANSI colors; `pseyosc-pair` enables `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on stderr, otherwise the Windows 10 console prints them as `←[33m`.
- **Switching controllers off.** A Move has no power button. Unconnected, it gives up after a few seconds of trying and switches off; connected, it stays on (the status LED stayed lit after quitting `pseyosc-pair`, which only closes the HID handle). A PlayStation switches it off by dropping the link, so the hub does the same (`BluetoothLink`: `IOCTL_BTH_DISCONNECT_DEVICE` on the radio, ported from wiimosc; no Administrator rights needed). With `--shutdown-on-exit` the device thread switches the orb off and drops the link when quitting; with `--idle-timeout` it drops it after that long without button or trigger changes, and ends right away (a dropped link isn't otherwise noticed for 3 s: `psmove_poll()` returns 0 for read errors and for no data alike). Motion doesn't count as activity yet; the accelerometer/gyro noise thresholds still need measuring. macOS and Linux: not implemented (`BluetoothLink_other.cpp` returns false).
- psmoveapi's `moved` remote controllers are disabled (`psmove_set_remote_config(PSMove_OnlyLocal)`), so it doesn't try to reach a daemon.

## 4. psmoveapi patches

`external/psmoveapi` is modified locally; every change is marked with `pseyosc patch`. The changes aren't committed yet: the user will fork psmoveapi and push them there.

- `include/psmove.h`: `PSMOVE_STATIC` defines `ADDAPI` empty on Windows. Without it, the header declares `dllimport` unless `BUILDING_SHARED_LIBRARY` is set, and static linking fails (the same issue as `WIIUSE_STATIC` in wiimosc).

## 5. Open work

1. Test pairing as Administrator, then the Bluetooth connection, feedback, buttons and disconnection.
2. OSC streaming (`pseyosc-stream`), modelled on `wiimosc-stream`: buttons, trigger, accelerometer, gyro, magnetometer, orientation (psmoveapi's AHRS, or Fusion as in wiimosc), battery; LED and rumble as OSC input.
3. Magnetometer calibration (psmoveapi warns "not yet calibrated"; it has a `magnetometer_calibration` utility).
4. Add motion to the idle check once the sensor noise is measured.
5. Linux and macOS builds.
6. The PS3 Eye camera.
