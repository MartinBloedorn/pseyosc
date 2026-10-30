# Development notes

This is a handover for agents continuing work on pseyosc. It collects the technical decisions, findings, the current state, and open work. `../wiimosc` is the sister project for Wiimotes; its structure (hub, listener, tools) is mirrored here, and its docs are a reference for what is planned (sensor fusion, camera). Topic docs: [Streaming.md](Streaming.md) (OSC tool and address reference).

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
| `pseyosc-stream` (OSC): status, buttons, trigger, accel, gyro, battery; rumble and LED input | ✅ streams from a ZCM1 (~90 reports/s); LED/rumble input user-confirmed; rumble intensities map onto the motor's working range 128–255 ([Streaming.md](Streaming.md#status)) |
| `pseyosc-video`: live camera image in a Win32 window (PS3EYEDriver + libusb) | ✅ PS3 Eye with WinUSB: 640x480 at 58 fps measured (60 Hz requested) and 69 fps (75 Hz), 320x240 at 186 fps (187 Hz); image quality to be checked by eye |
| Sensor fusion (`move/rpy`, `move/quat` are placeholders), magnetometer, camera tracking | ⏳ not started |

**Development hardware** (user's machine, Windows 10, VS 2022, CMake 3.30):

- Radio: Intel Wireless Bluetooth, `44:85:00:BF:B6:D8`.
- Move controller CECH-ZCM1E (PS3 model, USB PID `0x03d5`): `00:13:8A:9A:FE:2E`. Before pseyosc, it was paired with `A8:47:4A:EC:B6:BE` (probably a PlayStation).
- PS3 Eye camera (USB `1415:2000`), WinUSB installed on interface 0 with Zadig 2.9.

## 2. Layout & build

```
src/hub/       MoveHub + AbstractHubListener: USB pairing, Bluetooth pickup, threads, feedback, idle timeout;
               BluetoothLink: drops a device's Bluetooth link (BluetoothLink_win.cpp; _other.cpp is a stub)
src/pairtool/  pseyosc-pair: listener that prints button presses + CLI
src/osc/       OscSender / OscReceiver: copied unchanged from wiimosc
src/stream/    pseyosc-stream: MoveStreamer (listener → OSC, OSC input → rumble/LED) + CLI
src/video/     pseyosc-video: camera viewer (PS3EYEDriver capture thread + Win32 window); Windows only
tools/         oscdump.py, oscsend.py, oscrate.py (copied from wiimosc)
external/psmoveapi/  thp/psmoveapi (BSD), submodule, PATCHED (see §4); of its submodules, hidapi (controllers),
                     libusb-1.0 and PS3EYEDriver (camera) are needed; glm is part of its tree
doc/           Streaming.md, this file
```

- psmoveapi is **not** built through its own CMakeLists: that also builds the OpenCV tracker, libusb, the examples and utilities, requires all its submodules, and changes global compiler flags (`/W4` etc.). pseyosc's CMakeLists builds a static `psmoveapi` target from the core: `psmove.c`, `psmove_calibration.c`, `psmove_orientation.cpp`, `daemon/moved_client.cpp` (referenced by `psmove.c`), `math/*`, the platform port and hidapi. `psmove_config.h` is generated into `build/psmoveapi/`. The tracker (camera) will need more of it later.
- The target's include directories are public, including psmoveapi's `src/` (private headers: `psmove_private.h` for `psmove_connect_internal`, `_psmove_read_btaddrs`, the VID/PIDs; `psmove_port.h` for the host address and the permission check).
- **Running the tool from Git Bash**: `timeout -s INT` doesn't reach a Windows console program (it gets killed, exit 124), and piping into `head` buffers all output until the program exits.

## 3. Pairing & hub: findings and decisions

- **A Move is never discoverable.** Pairing means writing the host's address into the controller over USB (feature report `PSMove_Req_SetBTAddr`), and registering it with the OS. psmoveapi's `psmove_pair()` does both. So the hub's "scan" is a poll of the HID devices on USB, not a Bluetooth inquiry.
- **`psmove_pair()` on Windows blocks without a timeout** (`psmove_port_windows.c`, `handle_windows8_and_later`): it makes the radio connectable and discoverable, then loops until the controller has connected over Bluetooth, sets `VirtuallyCabled` under `HKLM\SYSTEM\CurrentControlSet\Services\HidBth\Parameters\Devices\<radio><move>` (hence Administrator), and enables the HID service. After ~24 s without a connection it removes the device and starts over. It prints its own instructions ("Unplug the controller..."). The hub therefore runs it on a detached thread per controller, and remembers which addresses are being paired, so plugging one in again doesn't start a second loop. ZCM2 controllers additionally get authenticated (`BluetoothAuthenticateDeviceEx`); not relevant for the ZCM1.
- **Controllers already storing this computer's address aren't paired again** (they'd start the blocking loop every time they're plugged in, e.g. for charging). `--repair` overrides this, for when Windows forgot the device.
- **Finder**: every 500 ms, `hid_enumerate` for VID `0x054c`, PIDs `0x03d5` (ZCM1) and `0x0c5e` (ZCM2). On Windows each controller appears as three collections; psmoveapi wants the `&col01#` path (it derives the `&col02#` one for the address feature report). New paths are opened with `psmove_connect_internal()`, which tells USB from Bluetooth (on Windows by the serial: `"0"` on USB, the address over Bluetooth). USB paths are remembered until unplugged; Bluetooth ones are skipped while a device thread has them open.
- **Connection rumble**: the device thread rumbles for 300 ms (blocking) before calling the listener's `connected()`, so it can't cut off a rumble the listener requests: psmoveapi can't read back the motor's state. Input reports queue up meanwhile.
- **Device threads** poll with `psmove_poll()` (non-blocking) and call `psmove_update_leds()` every iteration: it sends changed LEDs/rumble, and resends unchanged ones every 4 s, since the controller switches them off otherwise. A disconnection is assumed after 3 s without input reports, or when the LED write fails. An idle Move keeps sending reports (verified on hardware).
- psmoveapi logs to stderr with ANSI colors; `pseyosc-pair` enables `ENABLE_VIRTUAL_TERMINAL_PROCESSING` on stderr, otherwise the Windows 10 console prints them as `←[33m`.
- **Switching controllers off.** A Move has no power button. Unconnected, it gives up after a few seconds of trying and switches off; connected, it stays on (the status LED stayed lit after quitting `pseyosc-pair`, which only closes the HID handle). A PlayStation switches it off by dropping the link, so the hub does the same (`BluetoothLink`: `IOCTL_BTH_DISCONNECT_DEVICE` on the radio, ported from wiimosc; no Administrator rights needed). With `--shutdown-on-exit` the device thread switches the orb off and drops the link when quitting; with `--idle-timeout` it drops it after that long without button or trigger changes, and ends right away (a dropped link isn't otherwise noticed for 3 s: `psmove_poll()` returns 0 for read errors and for no data alike). Motion doesn't count as activity yet; the accelerometer/gyro noise thresholds still need measuring. macOS and Linux: not implemented (`BluetoothLink_other.cpp` returns false).
- psmoveapi's `moved` remote controllers are disabled (`psmove_set_remote_config(PSMove_OnlyLocal)`), so it doesn't try to reach a daemon.

## 3b. Camera: findings and decisions

- **psmoveapi's image pipeline** (`src/tracker/`, not built yet) has three layers: a camera driver per platform (`CameraControl`: PS3EYEDriver on Windows, V4L2 on Linux, AVFoundation or PS3EYEDriver on macOS) delivering BGR frames; the tracker (needs OpenCV 4: blinking color calibration of each orb, then per frame HSV threshold → biggest contour → circle fit within a region of interest, giving pixel x/y and radius, distance from the radius); and `psmove_fusion` (3D position from the PS Eye's field of view, 75° on the lens's blue dot, 56° on the red dot). The tracker sets the orbs' colors itself, and isn't thread-safe; both clash with MoveHub's design (orb = id color, `PSMove` owned by its device thread), to be decided when adding tracking.
- **psmoveapi has no DirectShow/Media Foundation driver**: on Windows the camera is read with PS3EYEDriver over libusb, which needs WinUSB on the camera's interface 0 (Zadig; see the README). Without a driver, Windows lists that interface with an error; interface 1 (microphone array) uses `usbaudio` and keeps working. PS3EYEDriver only lists cameras libusb can open, so without WinUSB it finds none.
- **Build**: libusb and PS3EYEDriver are built from source as static targets (`libusb`: the sources of libusb's `msvc/libusb_static.vcxproj`, without hotplug; `ps3eye`: `ps3eye.cpp` with `/FIwinsock2.h`, as in psmoveapi, for `timeval`). pseyosc uses PS3EYEDriver's C++ class `ps3eye::PS3EYECam` directly, not psmoveapi's `CameraControl` (which needs OpenCV's C API). OpenCV is postponed: the user decides later how to bring it in. The user's environment has `OpenCV_DIR` pointing to an OpenCV 3.1 install that doesn't exist anymore; the tracker needs OpenCV 4.
- **PS3EYEDriver**: `getFrame()` blocks until the next frame (no timeout); its own thread handles the libusb events. `init()` opens 640x480 for any size above 320x240. Frame rates: it takes the highest supported rate at or below the request; above 75 Hz (640x480) and 187 Hz (320x240) its table notes "video is partly corrupt", so `pseyosc-video` caps requests there. Its defaults are manual gain/exposure/white balance; the viewer switches the automatic ones on, tracking will need them off with a low exposure (psmoveapi: auto gain/white balance off, gain 0, exposure 0.3 of the maximum).
- **License**: PS3EYEDriver is MIT, but based on GPL'd code (psmoveapi marks builds with it as GPL).
- **Viewer**: a capture thread copies each frame into the window's buffer (under a mutex) and invalidates it; the window thread paints it with `StretchDIBits` (24-bit top-down DIB, BGR as delivered), scaled to the client area. The message loop also polls the Ctrl+C flag and shows the measured frame rate in the title. Quitting waits for the capture thread's next frame, so an unplugged camera would hang it (not handled yet). Killing the process without `stop()` left the camera usable on the next start.

## 4. psmoveapi patches

`external/psmoveapi` is modified locally; every change is marked with `pseyosc patch`. They are committed on the submodule's `master` (`5702bee`), but only locally: `.gitmodules` still points to upstream `thp/psmoveapi`. Once the user has forked it, point `.gitmodules` to the fork, and push the fork before pseyosc (as for wiimosc's wiiuse fork).

- `include/psmove.h`: `PSMOVE_STATIC` defines `ADDAPI` empty on Windows. Without it, the header declares `dllimport` unless `BUILDING_SHARED_LIBRARY` is set, and static linking fails (the same issue as `WIIUSE_STATIC` in wiimosc).

## 5. Open work

1. The open items in [Streaming.md](Streaming.md#status) (battery flicker, axes).
2. Sensor fusion for `move/rpy` and `move/quat` (psmoveapi's AHRS, or Fusion as in wiimosc), possibly with the magnetometer for yaw.
3. Magnetometer calibration (psmoveapi warns "not yet calibrated"; it has a `magnetometer_calibration` utility).
4. Add motion to the idle check once the sensor noise is measured.
5. Linux and macOS builds.
6. The PS3 Eye camera: decide how to bring in OpenCV 4 (needed by psmoveapi's tracker), then tracking (orb positions to OSC), resolving the orb color and threading clashes noted in §3b. Linux/macOS viewer.
