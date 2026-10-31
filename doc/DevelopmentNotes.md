# Development notes

This is a handover for agents continuing work on pseyosc. It collects the technical decisions, findings, the current state, and open work. `../wiimosc` is the sister project for Wiimotes; its structure (hub, listener, tools) is mirrored here, and its docs are a reference for what is planned (sensor fusion, camera). Topic docs: [Streaming.md](Streaming.md) (OSC tool and address reference, sensor fusion).

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
| `pseyosc-track`: psmoveapi's tracker (OpenCV 4 from `../lib/opencv`), positions printed | ✅ user-confirmed: calibration and tracking work out of the box with one ZCM1; `--mirror` too |
| `pseyosc-stream --track`: tracking thread, calibration on join and on Select + Start, `track/found`, `track/pos` (cm, pinhole model), `track/xyz` (−1–1, z 0–1 of the tracking range) | ✅ user-confirmed with one controller (calibration, hue, mirror, positions and their scale); several controllers and `fov=` not yet ([Streaming.md](Streaming.md#status)) |
| Sensor fusion (`--fusion`: 6-DoF, Fusion as in wiimosc) | ✅ user-confirmed live on a ZCM1, including the Select + Start restart; axes and angle signs verified (§3c) |
| Yaw hints (`move/hint/yaw <yaw> [strength]` over `--in-port`, as in wiimosc) | ✅ on a resting ZCM1 (set, wrap, steps, clamping; scripted). ⏳ pitch gate, while moving |
| Magnetometer (9-DoF) | ⏳ not started |

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
src/track/     pseyosc-track: psmoveapi's tracker test (connects controllers itself, no MoveHub); Windows only
src/fusion/    MotionFusion (rest calibration, time steps → filter), MadgwickFilter, Orientation: from wiimosc, see §3c
src/tracking/  MoveTracker: psmoveapi's tracker on its own thread, for pseyosc-stream --track (_none.cpp: stub
               without OpenCV); design in Streaming.md (Implementation)
tools/         oscdump.py, oscsend.py, oscrate.py (copied from wiimosc)
external/psmoveapi/  thp/psmoveapi (BSD), submodule, unmodified upstream master (see §4); of its submodules, hidapi (controllers),
                     libusb-1.0 and PS3EYEDriver (camera) are needed; glm is part of its tree
external/Fusion/     xioTechnologies Fusion v1.3.3 (MIT), submodule, unmodified: Madgwick's AHRS in C (as wiimosc)
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
- **Build**: libusb and PS3EYEDriver are built from source as static targets (`libusb`: the sources of libusb's `msvc/libusb_static.vcxproj`, without hotplug; `ps3eye`: `ps3eye.cpp` with `/FIwinsock2.h`, as in psmoveapi, for `timeval`). pseyosc uses PS3EYEDriver's C++ class `ps3eye::PS3EYECam` directly, not psmoveapi's `CameraControl` (which needs OpenCV's C API). The viewer doesn't need OpenCV; the tracker does (see below).
- **PS3EYEDriver**: `getFrame()` blocks until the next frame (no timeout); its own thread handles the libusb events. `init()` opens 640x480 for any size above 320x240. Frame rates: it takes the highest supported rate at or below the request; above 75 Hz (640x480) and 187 Hz (320x240) its table notes "video is partly corrupt", so `pseyosc-video` caps requests there. Its defaults are manual gain/exposure/white balance; the viewer switches the automatic ones on, tracking will need them off with a low exposure (psmoveapi: auto gain/white balance off, gain 0, exposure 0.3 of the maximum).
- **License**: PS3EYEDriver is MIT, but based on GPL'd code (psmoveapi marks builds with it as GPL).
- **OpenCV** isn't part of pseyosc: it's built once for the workspace in `synestechia/lib/opencv` (`build.sh`: 4.14.0, static, `/MD`, Debug + Release, modules core, imgproc, imgcodecs, videoio, highgui, calib3d; see `lib/README.md`). CMake looks for it only in `PSEYOSC_OPENCV_DIR` (default `../lib/opencv/install`, `NO_DEFAULT_PATH`, `OpenCV_DIR` cache unset first), so the user's stale `OpenCV_DIR` environment variable (OpenCV 3.1, deleted) and system installs are never used; without it, `pseyosc-track` is skipped with a warning. Pitfalls met: OpenCV's Windows config looks for DLLs unless `OpenCV_STATIC` is ON; and with `BUILD_LIST`, protobuf (dnn) and ade (gapi) were exported as dependencies but not built, so `find_package` failed on missing files until `WITH_PROTOBUF`/`WITH_ADE` were turned off. The build downloads Intel IPP (ICV, Intel's license), linked statically.
- **Tracker build**: `psmoveapi_tracker` is a static target from `src/tracker/` (tracker, hue calibration, helpers, camera control with the PS3EYEDriver driver, fusion), linking `psmoveapi`, `ps3eye` (now with `ps3eye_capi.cpp`, the C API psmoveapi's driver uses) and OpenCV. `PSMOVE_USE_PS3EYE_DRIVER` is set in the generated `psmove_config.h` on Windows, and `CAMERA_CONTROL_USE_PS3EYE_DRIVER` on the target (see §4).
- **pseyosc-track** follows psmoveapi's `utils/test_tracker.cpp`: single-threaded; connects the controllers with `psmove_connect_by_id()` (Bluetooth only), `psmove_tracker_enable()` (blinking calibration) for each, then per loop: drain input reports, `update_image()`, `update()` (also sends the orb colors: the tracker owns the LEDs), `annotate()`, `cv::imshow()` of the tracker's frame. Default settings worked right away (exposure 0.3).
- **Mirroring**: psmoveapi's tracker does *not* mirror by default: `psmove_tracker_settings_set_default()` sets `camera_mirror = false`, although `psmove_tracker.h` documents `[true]`. So by default the tracker's image has the same orientation as `pseyosc-video` (verified by the user); `--mirror` turns it on. With PS3EYEDriver, mirroring is done by the camera (`setFlip()`, sensor register 0x0C): verified with a scratch test that grabbed frames before and after `setFlip(true, false)` (the new frame matched the old one mirrored horizontally: mean difference 8.3 vs. 21.9 unmirrored and 4.7 frame-to-frame noise), both while streaming and when set before `start()`. Defining `CAMERA_CONTROL_USE_PS3EYE_DRIVER` (§4) keeps psmoveapi from flipping it back in software.
- **Viewer**: a capture thread copies each frame into the window's buffer (under a mutex) and invalidates it; the window thread paints it with `StretchDIBits` (24-bit top-down DIB, BGR as delivered), scaled to the client area. The message loop also polls the Ctrl+C flag and shows the measured frame rate in the title. Quitting waits for the capture thread's next frame, so an unplugged camera would hang it (not handled yet). Killing the process without `stop()` left the camera usable on the next start.

## 3c. Sensor fusion: findings and decisions

- **psmoveapi's own fusion** (`psmove_enable_orientation()`, `psmove_orientation.cpp`) was not used: its default on a ZCM1 is a complementary 9-DoF filter that needs a magnetometer calibration file per controller (without it, the reference direction is a zero vector), blends towards accelerometer and magnetometer at a fixed rate even while the controller is swung, doesn't estimate the gyro's bias, and is unmaintained. Instead, wiimosc's fusion (xioTechnologies Fusion) was copied, so both projects stream the same thing; a copy (not a shared library) was the user's choice, since Fusion needed no patches in wiimosc and is small.
- **Copied from wiimosc** (`src/fusion`): `MadgwickFilter` unchanged apart from comments, `AbstractOrientationFilter`, `Orientation` (without `rotate()`), and `MotionFusion` rewritten for the Move: no MotionPlus calibration (psmoveapi already returns g and rad/s from the factory calibration), no IR anchors, two samples per report. Yaw hints (`move/hint/yaw`) are wiimosc's, minus the 1 s hold that overrules the anchors: same strength semantics, 80° pitch gate, dropped before the fusion is ready; a restart clears the offset.
- **Axes**: psmoveapi's sensor axes are the body frame as they are: x right, y towards the orb, z out of the buttons' face, right-handed, gyro right-handed about them. Verified with pose captures: orb up → accel y ≈ 1.02; lying on its right / left side → x ≈ −0.51 / +0.55 with z ≈ 0.84; lifting the orb → gyro x +86 deg/s; rolling from right to left → gyro y −92 deg/s. (A first guess from psmoveapi's "OpenGL" sensor transform, `(x, z, −y)`, was wrong.)
- **Angles**: unlike wiimosc (whose roll keeps wiiuse's accelerometer sign, `roll = −φ`), pseyosc's roll, pitch and yaw are all right-handed rotations about the body axes: roll positive rolling right, pitch positive up, yaw positive left. Whether that matches wiimosc's roll sign hasn't been compared with a Wiimote.
- **Timing**: a ZCM1 sends 87–89 reports/s, two samples each. dt comes from the rate measured at rest and the `psmove_poll()` sequence numbers (passed to the listener's `update()`), not from arrival times (details in Streaming.md, Implementation). The ZCM1's own report timestamp isn't exposed by psmoveapi.
- **Measured at rest** (ZCM1 `00:13:8A:9A:FE:2E`): gyro bias −0.2 to −0.3 / −0.5 to +0.3 / −0.2 to −0.4 deg/s (x/y/z, across five connections), much smaller than the Wiimote's; gyro noise ~±0.6 deg/s; yaw drift after startup ~0.04 deg/s. The rest thresholds (4 deg/s, 0.1 g) and the filter's gains are wiimosc's, not tuned for the Move.
- **Gyro range** unknown (psmoveapi doesn't expose the calibration's scale), so Fusion's overrange handling is off (`gyroRange` 0).

## 4. psmoveapi: no patches

`external/psmoveapi` tracks upstream `thp/psmoveapi` `master` unmodified; keep it that way, and work around issues from pseyosc's CMakeLists. Until 2026-10-31 it carried two local patches, replaced by compile definitions:

- **Static linking on Windows**: `psmove.h` declares its API `dllimport` unless `BUILDING_SHARED_LIBRARY` is set (upstream only builds a DLL), which fails with a static library. `BUILDING_SHARED_LIBRARY` is defined publicly on the `psmoveapi` target, so library and tools see `dllexport`: that links, and only gives the executables an export table (`.lib`/`.exp` files next to them). Was: a `PSMOVE_STATIC` patch defining `ADDAPI` empty.
- **Double mirroring in the tracker**: `psmove_tracker_update_image()` flips the image in software unless `CAMERA_CONTROL_USE_PS3EYE_DRIVER` is defined, which nothing in upstream does; with PS3EYEDriver (which mirrors in hardware), the image was flipped twice with `camera_mirror` on, i.e. not mirrored. The macro is defined on `psmoveapi_tracker` (it's tested nowhere else). Was: a patch testing `PSMOVE_USE_PS3EYE_DRIVER` instead. A candidate for an upstream pull request. (`psmove_tracker.cpp` isn't UTF-8: edit it with `sed`, not tools that re-encode it.)

## 5. Open work

1. The open items in [Streaming.md](Streaming.md#status) (battery flicker; fusion with several controllers, compared with wiimosc).
2. Sensor fusion: 9-DoF with the magnetometer for yaw (Fusion's `FusionAhrsUpdate` with magnetic rejection; needs a hard/soft-iron calibration, psmoveapi has a `magnetometer_calibration` utility); tuning on recordings as in wiimosc; the gyro's range.
4. Add motion to the idle check once the sensor noise is measured.
5. Linux and macOS builds.
6. The PS3 Eye camera: confirm `--track` on hardware ([Streaming.md](Streaming.md#status)); 3D positions (`psmove_fusion`); Linux/macOS viewer and tracker.
