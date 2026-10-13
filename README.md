# msfs-linuxrebind

Bind HOTAS buttons to Microsoft Flight Simulator 2020 cockpit controls **from Linux**, including the
FlyByWire A32NX functions that the in-game controls menu cannot bind (FCU pushes/pulls,
autobrake, EFIS buttons, chrono, ...). Also drives the vibration motors of Winwing devices from
flight data, which MSFS under Proton cannot do by itself.

* Native Linux, no Wine/Proton component: talks the **SimConnect TCP protocol** directly to the sim.
* Reads joysticks straight from **evdev** (`/dev/input/event*`), Winwing devices through **hidraw**.
* **Dear ImGui** UI on **Wayland or X11** (GLFW 3.4+, runtime platform selection), or `--headless`.
* Profiles with press / release actions, repeat-while-held (for FCU knobs), 2-position switches.
* Searchable catalog of A32NX functions generated from the FlyByWire docs.

## How it works

| Action kind | Transport | Needs |
|---|---|---|
| `K:` event (`GEAR_UP`, `A32NX.FCU_AP_1_PUSH`, ...) | SimConnect `MapClientEventToSimEvent` + `TransmitClientEvent` | nothing |
| `H:` event (`A32NX_EFIS_L_CHRONO_PUSHED`) | calculator code through the MobiFlight WASM module client-data area | MobiFlight WASM module |
| `L:` var set (`A32NX_AUTOBRAKES_ARMED_MODE_SET = 2`) | same | MobiFlight WASM module |
| RPN calculator code (`1 (>L:FOO)`) | same | MobiFlight WASM module |

The A32NX `A32NX.*` custom events cover the FCU, EFIS and autobrake and need **no** extra module.
For `H:` events and `L:` vars install the
[MobiFlight WASM module](https://github.com/MobiFlight/MobiFlight-WASM-Module/releases)
(unzip `mobiflight-event-module` into the MSFS `Community` folder). The status bar shows whether it
was detected.

## Requirements

* Linux, GCC 12+ (C++20), CMake 3.20+, Ninja or Make
* GLFW >= 3.4 development package (`glfw-devel` / `libglfw3-dev`), OpenGL/EGL, pkg-config
* Read access to your joystick's `/dev/input/event*` node and read/write to its `/dev/hidraw*` node
  (most distributions grant both to the logged-in user via udev `uaccess`; otherwise see
  [Permissions](#permissions))

## Build

```sh
git clone --recursive https://github.com/Kyr4l/msfs-2020-linuxrebind.git
cd msfs-2020-linuxrebind
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
ninja -C build
./build/msfs-linuxrebind
```

Options: `--wayland` / `--x11` (force a platform), `--headless` (no window, run the active profile),
`--host`, `--port`, `--profile NAME`, `--config FILE`, `--catalog FILE`, `--quiet`.

Config is stored in `~/.config/msfs-linuxrebind/config.json` and saved automatically.

## MSFS side: SimConnect over TCP

MSFS listens where its `SimConnect.xml` says. Under Steam/Proton that file is

```
<steam library>/steamapps/compatdata/1250410/pfx/drive_c/users/steamuser/AppData/Roaming/Microsoft Flight Simulator/SimConnect.xml
```

and must contain an IPv4 block with a fixed port (the default file shipped by MSFS already has
`<Port>500</Port>` with `<Scope>local</Scope>`, which is all this tool needs):

```xml
<SimConnect.Comm>
    <Descr>Static IP4 port</Descr>
    <Protocol>IPv4</Protocol>
    <Scope>local</Scope>
    <Port>500</Port>
    <MaxClients>64</MaxClients>
    <MaxRecvSize>41088</MaxRecvSize>
</SimConnect.Comm>
```

The tool connects to `127.0.0.1:500` by default and reconnects automatically whenever the sim is
(re)started.

## Usage

1. Start MSFS (any point, even the main menu) and this tool.
2. **Devices** tab: press buttons on the HOTAS to see their numbers.
3. **Bindings** tab: *Add binding* → *Listen for input* → press the button → add actions by hand or
   *from catalog* → *Save binding*. *Test* fires the press actions immediately.
4. Leave the same buttons **unassigned in the MSFS controls menu**, otherwise both fire.

Tips:

* Momentary cockpit push buttons: one `K:` event on press (e.g. `A32NX.FCU_AP_1_PUSH`).
* FCU knobs on a rotary/hat: `A32NX.FCU_HDG_INC` with *Repeat while held*.
* Two-position switches (e.g. an autobrake toggle): `A32NX.AUTOBRAKE_SET 2` on press and
  `A32NX.AUTOBRAKE_SET 0` on release.
* `A32NX.FCU_ALT_INCREMENT_SET 1000` / `100` to pick the ALT knob step.

## Vibration (Winwing URSA MINOR and friends)

MSFS only vibrates XInput gamepads and DirectInput force-feedback devices; the Winwing kernel
driver does not expose the URSA MINOR motors as force feedback, so under Proton the game cannot
reach them. Instead, this tool subscribes to flight data (ground speed, touchdown, stall,
overspeed, gear/flaps motion, reversers, speed brake, G load, braking) and writes motor levels to
the device with Winwing's own HID protocol (output report `0x02`, opcode `0x49`), the way
SimAppPro does on Windows. Enable it in the **Vibration** tab; everything is off by default.

Motor addressing per product comes from community reverse engineering
([winctrl-xplane-plugin](https://github.com/rswilem/winctrl-xplane-plugin),
[WinctrlConfig](https://github.com/f3nr1l/WinctrlConfig), the kernel `hid-winwing` rumble patch):

| Product id | Device | Controllers (device / family) | Motors (device, SET_LEDX index) |
|---|---|---|---|
| `b970` | URSA MINOR Combat Throttle Metal.EX | grip L `0x04`, grip R `0x08`, base `0x40` / `0xb9` | left (`0x04`, `0x0e`), right (`0x08`, `0x10`) |
| `b920`, `b930` | URSA MINOR 32 Throttle Metal L/R | `0x10` / `0xb9` | left (`0x10`, `0x0e`), right (`0x10`, `0x10`) |
| `bc27`, `bc28` | URSA MINOR Airline Joystick L/R | grip `0x07`/`0x08` / `0xbf`, lights `0x20` / `0xbb` | single (grip, 0) |
| `bc29`, `bc2a` | URSA MINOR Fighter Joystick | grip `0x09`/`0x0a` / `0xbf`, lights `0x20` / `0xbb` | single (grip, 0) |

Unknown products are handled by discovery: a request to the broadcast address (device `0x01`,
family `0x00`) makes every controller behind the USB device answer with its id, and the product
name is then read from its flash. Controllers named "... L" / "... R" become the left/right
motors (index 0, which may need adjusting: run `--motor-sweep` to find the index that buzzes). *Discover controllers* in the Vibration tab shows the list and lets you pick
which one is which; the *LED test* (index 0 on the base = backlight) and *Pulse* buttons confirm
it. `msfs-linuxrebind --probe-winwing` prints the same from the command line.

## Permissions

If the Log tab reports "permission denied":

```
# /etc/udev/rules.d/70-msfs-linuxrebind.rules
# evdev joystick nodes and Winwing hidraw nodes for the logged-in user
SUBSYSTEM=="input", ENV{ID_INPUT_JOYSTICK}=="1", TAG+="uaccess"
KERNEL=="hidraw*", ATTRS{idVendor}=="4098", TAG+="uaccess"
```

then `sudo udevadm control --reload && sudo udevadm trigger` and re-plug the device.

## Development

* `tools/mock_simconnect.py --port 5500` is a fake SimConnect server (answers Open, logs events,
  emulates the MobiFlight areas); run the app with `--port 5500` to test without MSFS.
* `tools/build_catalog.py` regenerates `data/a32nx_catalog.json` from the FlyByWire docs.
* `_cpr/` is git-ignored: put copies of files from the (read-only) MSFS install there if you need
  to edit them.

Protocol references: [node-simconnect](https://github.com/EvenAR/node-simconnect) (SimConnect wire
format), [MobiFlight WASM module](https://github.com/MobiFlight/MobiFlight-WASM-Module),
[FlyByWire A32NX flight deck API](https://docs.flybywiresim.com/aircraft/a32nx/a32nx-api/a32nx-flightdeck-api/).

## License

MIT. Dear ImGui (MIT) and nlohmann/json (MIT) are vendored under `third_party/`.
