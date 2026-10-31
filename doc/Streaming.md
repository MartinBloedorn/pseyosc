# OSC streaming

`pseyosc-stream` pairs and connects controllers the same way `pseyosc-pair` does (see the [README](../README.md#how-pairing-works)), and streams their input as OSC over UDP. The address layout follows wiimosc's, with `move/` in place of `mote/`.

```
pseyosc-stream --host 127.0.0.1 --port 4321 --prefix pseyosc [--in-port 4322] [--max-rate 60]
```

| Option | Default | Meaning |
|---|---|---|
| `--host` | `127.0.0.1` | Target: host name, IPv4 address, or broadcast address (`255.255.255.255`, `192.168.1.255`). `0.0.0.0` is rejected, since it's a listening address. |
| `--port` | `4321` | Target UDP port |
| `--prefix` | `pseyosc` | Address prefix. Normalized: `pseyosc`, `/pseyosc/` → `/pseyosc`; may be empty. |
| `--in-port <port>` | off | Receives OSC on this UDP port, on all interfaces, to control the controllers. See [Input](#input---in-port). |
| `--max-rate` | `0` | Max. rate (Hz) of continuous values per controller; `0` = unlimited. Buttons, battery and status messages are never limited. |
| `--repair` | off | As for `pseyosc-pair`: also registers controllers on USB that are already paired with this computer again. |
| `--id <id>=<address>` | | Always gives the controller with this Bluetooth address this id (0–7) and its orb color. Repeatable; also available in `pseyosc-pair`. |
| `--shutdown-on-exit` | off | Switches off all connected controllers when quitting, by dropping their Bluetooth connections. |
| `--idle-timeout <minutes>` | `0` (never) | Switches off controllers that have been idle this long. Only button and trigger changes count as activity so far, not motion. Fractions work (`0.5`). |
| `--track [options]` | off | Tracks the orbs with the PS3 Eye camera, see [Tracking](#tracking---track). Options, comma-separated: `exposure=<0-1>` (default `0.3`), `mirror`, `hue`, `fov=<degrees>` (default `75`); e.g. `--track exposure=0.2,mirror`. |

Pairing controllers plugged in over USB needs Administrator rights on Windows, as for `pseyosc-pair`; without them, controllers paired before still connect when PS is pressed.

## Addresses

`<p>` is `<prefix>/<id>`, e.g. `/pseyosc/0`. `<id>` is 0-based (0–7), the same number the terminal output shows (`[0] Motion Controller 00:13:8A:9A:FE:2E connected`); the orb lights up in its id's color (0 red, 1 green, 2 blue, 3 yellow, 4 magenta, 5 cyan, 6 white, 7 orange). Fixed ids work as in wiimosc: on Ctrl+C, the tool prints the `--id` arguments for the connected controllers.

| Address | Arguments | Sent |
|---|---|---|
| `<p>/status` | string `move` / `none` | on connect / disconnect, including quitting, `--idle-timeout` and `--shutdown-on-exit` |
| `<p>/battery` | float 0–1 | on change; in steps of 0.2 (the controller reports levels 0–5). While charging, the level isn't known and the last one stays; `1` when charged on the charger. |
| `<p>/move/button/{triangle,circle,cross,square,select,start,ps,move}` | int 0/1 | on change |
| `<p>/move/button/trigger` | float 0–1 | continuous: the analog trigger. There's no digital trigger address; threshold this value instead. |
| `<p>/move/rpy` | 3 floats, degrees: roll, pitch, yaw | **placeholder**: `0, 0, 0`, sent once on connect. Sensor fusion comes in a later step. |
| `<p>/move/quat` | 4 floats: w, x, y, z | **placeholder**: `1, 0, 0, 0` (no rotation), sent once on connect |
| `<p>/move/accel` | 3 floats, g: x, y, z | continuous, calibrated by psmoveapi (factory calibration read over USB when pairing) |
| `<p>/move/gyro` | 3 floats, deg/s: x, y, z | continuous: rotation rates about the controller's x, y, z axes, calibrated by psmoveapi |

With `--track` (see [Tracking](#tracking---track)):

| Address | Arguments | Sent |
|---|---|---|
| `<p>/track/found` | int 0/1 | on change: `1` while the orb is found in the camera image, `0` while it isn't (also before it's calibrated, and while calibrating) |
| `<p>/track/pos` | 3 floats, cm: x, y, z | continuous, while found: the orb's center relative to the camera's lens. z points out of the lens (through the image's center), y up, x to the camera's left (i.e. to the right of someone facing the camera); a right-handed system. Doesn't change with `mirror`. Not sent while not found: the last position stays. See [Position](#position). |
| `<p>/track/xyz` | 3 floats: x, y −1–1, z 0–1 | continuous, while found (like `track/pos`, in the same frame, also unaffected by `mirror`): x and y are the orb's center in the image, −1 to 1 from edge to edge, 0 in the center (x positive to the camera's left, y up); z is `track/pos`'s z as a fraction of the tracking range, 0 at the lens, 1 as far as an orb can be tracked (~2.8 m). For pointing, like wiimosc's `mote/ir/xyz`. x and y span the image's width and height, so they aren't to the same scale (4:3). |

"Continuous" values are sent whenever they change, at most `--max-rate` times per second. After connecting, every value is sent once, so receivers start with a complete state.

**Axes** are psmoveapi's sensor axes, which aren't the Wiimote's, so `accel`/`gyro` don't map one-to-one onto wiimosc's. According to psmoveapi's docs, y points along the controller towards the orb (`accel` y ≈ 1 with the orb up); the directions of x and z still need checking on hardware, and belong to the sensor fusion step.

## Input (`--in-port`)

| Address | Arguments | Effect |
|---|---|---|
| `<p>/move/rumble/trigger` | number, 0–1 | Rumble at that intensity until the next rumble request; `0` switches it off. Any numeric type works (float, int, double, `T`/`F`). |
| `<p>/move/rumble/pulse` | 2 numbers: 0–1, ms | Rumble at that intensity for the given duration (capped at 3000 ms), then off. An intensity or duration of 0 switches it off right away. |
| `<p>/move/led` | 3 numbers: red, green, blue, 0–1 each | Sets the orb's color (replacing the id color) until the next request or a reconnect. `0 0 0` switches it off. Ignored with `--track`. |

- **Rumble has intensity levels** (unlike the Wiimote's on/off motor, hence no 0.5 threshold as in wiimosc). The motor doesn't start below about half power (on a ZCM1, psmoveapi's 102 of 255 did nothing, 128 rumbled), so any intensity above 0 maps onto 128–255: 0.01 is the weakest rumble that still turns the motor, 1 the strongest.
- **The last rumble request wins**, as in wiimosc: `trigger` and `pulse` replace whatever is ongoing.
- **Timing**: a request takes effect with the controller's next input report (~10 ms). The hub's connection rumble (300 ms) ends before streaming starts, so requests sent meanwhile take effect afterwards. Requests for ids that aren't connected are dropped; a controller that reconnects starts with the motor off and its id color.
- **Quitting** (Ctrl+C) switches off the orb and the motor of every connected controller.
- **Ignored input** is reported once per address in the terminal (`[osc] ignoring /pseyosc/0/move/bogus: unknown address`): unknown addresses (including another prefix), ids above 7, and missing or wrong arguments.
- Testing: `python tools/oscsend.py 4322 /pseyosc/0/move/led 0 0.5 1` (in Git Bash, prefix it with `MSYS_NO_PATHCONV=1`, otherwise the address is turned into a Windows path).

## Tracking (`--track`)

Needs the camera set up (README: *Camera setup*) and pseyosc built with OpenCV (otherwise `--track` reports that it isn't available). A preview window shows the camera image with the tracked orbs; it can't be closed (it reopens), Ctrl+C quits.

- **Calibration**: when a controller connects, the tracker blinks its orb to learn how its color looks in the camera image, so the orb must be in view then. Other tracked orbs are switched off meanwhile, and nothing is tracked. On failure, or after the lighting changed, **press Select and Start together** to calibrate that controller again.
- **`hue`**: the first controller to be calibrated runs psmoveapi's hue calibration instead: it blinks through many colors, and keeps the ones that track well. Controllers get one of those right away, without blinking, so joining doesn't disturb the others. Select + Start runs the hue calibration again with that controller, and all controllers get new colors. (If it finds no colors, controllers are calibrated by blinking.)
- **Orb colors**: the tracker chooses them (psmoveapi's presets: magenta, cyan, yellow, red, blue, green; or the hues found), and `move/led` input is ignored. Orbs are off until calibrated, so a dark orb means "press Select + Start".
- **Limits**: psmoveapi tracks at most 7 controllers (the hub connects 8), and without `hue` there are only 6 colors, so a 7th controller fails to calibrate.
- `exposure` sets the camera's fixed exposure: lower leaves less but the orbs visible. `mirror` mirrors the preview image, like a mirror facing the camera (the positions stay the same).
- `fov=<degrees>` sets the lens's diagonal field of view: `75` for the blue dot (default), `56` for the red dot. See [Position](#position).
- Rumble requests are applied after an ongoing calibration (see Implementation).

### Position

psmoveapi gives the orb's center and radius in the image (pixels), and estimates its distance from the radius (cm; a curve fitted by psmoveapi for the PS Eye's blue dot setting). `track/pos` turns this into a position with a pinhole camera model: the pixel's offset from the image's center, divided by the focal length, gives the direction of the ray through the orb, and the distance is laid off along it. The focal length follows from the field of view: f = (half the image's diagonal) / tan(fov / 2), i.e. ~521 px at 640x480 with 75°.

- **The field of view is assumed**: Sony gives 75° and 56° for the two lens settings, taken here as diagonal; published calibrations of the PS Eye's blue dot report ~540 px, i.e. ~73°. To check: hold the orb at a measured offset from the lens's axis and compare x/y; adjust `fov` if they're off by a constant factor.
- **With the red dot**, `fov=56` corrects the directions, but psmoveapi's distance curve is fitted for the blue dot, so z (and with it x, y) will be off.
- **`track/xyz`'s z**: the tracking range is the distance psmoveapi's curve gives for the smallest orb its tracker accepts (radius 4.7 px, `tracker_quality_t3`): ~277 cm. The tool prints it on start. Closer than the range's end, the orb is still found; beyond it, it isn't, so z stays below 1 (clamped).
- **Accuracy**: the distance comes from a radius of a few to a few dozen pixels, so it's the noisiest part, especially far away; x and y scale with it. Lens distortion isn't corrected (psmoveapi can undistort with a camera calibration file, not used yet).

## Notes & caveats

- **Rate**: a ZCM1 connected over Bluetooth sends ~90 input reports/s (measured with a resting controller). `accel` and `gyro` change on nearly every one, so that's ~180 messages/s per controller. Each report holds two sensor samples; the streamer sends the newer one.
- **Battery** was seen to change from 0.8 to 0.6 within seconds of connecting; it may flicker between two levels at a step boundary. Not yet investigated.
- **Magnetometer**: not streamed yet. psmoveapi warns `Magnetometer ... not yet calibrated`; it belongs to the sensor fusion step.
- **Testing**: only one program should receive on the target port, see wiimosc's notes. `python tools/oscdump.py 4321 20` prints a summary per address, `python tools/oscrate.py 4321 20` the messages per second.

## Implementation

- `src/hub/MoveHub`: shared by both tools, see [DevelopmentNotes](DevelopmentNotes.md#3-pairing--hub-findings-and-decisions). It calls the listener from each controller's own thread: `connected()` after the connection rumble, `update()` after each input report, `disconnected()` when it goes away or the tool quits. After each `update()`, the hub sends changed LEDs/rumble (`psmove_update_leds()`).
- **Output and threads**: the hub's controller threads read input in `update()`, and set the output (LEDs, rumble) in the listener's `updateOutput()`, holding the hub's output mutex, then send it. With `--track`, that mutex is the tracker's (`MoveHub::setOutputMutex()`).
- `src/tracking/MoveTracker`: psmoveapi's tracker on a thread of its own, which also owns the preview window. Controllers are added/removed by the streamer from their threads (`connected()`/`disconnected()`; the hub calls `disconnected()` before closing the handle, and `removeController()` waits until the tracker lets go of it). psmoveapi's handles aren't thread-safe, and the tracker uses them only while calibrating (blinking the orb, switching the others off): it holds the output mutex meanwhile, and the controller threads skip their output (but keep reading input; reads and writes don't share state in psmoveapi or hidapi on Windows). Afterwards, the tracker's LED updates are off (`psmove_tracker_set_auto_update_leds()`), and the controller threads set the orb color it chose (`orbColor()`). Positions are reported per frame to the streamer (`AbstractTrackerListener::tracked()`), which sends them from the tracker's thread, with its own change detection. Without OpenCV, `MoveTracker_none.cpp` is built instead.
- `src/osc`: copied unchanged from wiimosc (`OscSender`: minimal OSC 1.0 encoder + UDP; `OscReceiver`: parser + UDP receiver thread).
- `src/stream/MoveStreamer`: the listener that maps controller state to OSC, sending a value only if it differs from the last one sent on that address (cached per controller). OSC input is stored per id under a mutex by the receiver's thread, and applied with `psmove_set_rumble()`/`psmove_set_leds()` by the controller's thread on its next `update()`, since only that thread touches its `PSMove`.
- `tools/`: `oscdump.py`, `oscsend.py`, `oscrate.py`, copied from wiimosc.

## Status

| Item | State |
|---|---|
| status, buttons (initial state), trigger, accel, gyro, battery, placeholder rpy/quat | ✅ received with `oscdump.py` from a resting ZCM1 (~90 reports/s) |
| `--in-port`: parsing, ignored-input message | ✅ |
| `--track`: calibration on connect and on Select + Start, `hue`, `mirror`, streaming of found/position | ✅ user-confirmed (with the earlier image coordinates) |
| `--track`: `track/found` (renamed), `track/pos` in cm, `track/xyz`, `fov` | ✅ user-confirmed: axis directions, independent of `mirror`; scale holds with the default field of view (75°). ⏳ `fov=` (e.g. red dot) not yet tested |
| `--track`: several controllers, joining while tracking, quitting during a calibration | ⏳ not yet confirmed |
| `--in-port`: `move/led`, `move/rumble/pulse`, `move/rumble/trigger` | ✅ user-confirmed on a ZCM1; below 0.5 the motor didn't turn, hence the mapping onto 128–255 (user-confirmed) |
| `status none` on quit, button presses, `--max-rate`, reconnect while streaming | ⏳ not yet confirmed |
| Axis directions of `accel`/`gyro` | ⏳ from psmoveapi's docs, not verified |
