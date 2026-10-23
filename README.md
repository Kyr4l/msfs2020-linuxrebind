# msfs-linuxrebind

Bind HOTAS buttons to Microsoft Flight Simulator 2020 cockpit controls **from Linux**, including the
FlyByWire A32NX functions that the in-game controls menu cannot bind (FCU pushes/pulls,
autobrake, EFIS buttons, chrono, ...). Also drives the vibration motors of Winwing devices from
flight data, which MSFS under Proton cannot do by itself.

* Native Linux, no Wine/Proton component: talks the **SimConnect TCP protocol** directly to the sim.
* Reads joysticks straight from **evdev** (`/dev/input/event*`), Winwing devices through **hidraw**.
* **Dear ImGui** UI on **Wayland or X11** (GLFW 3.4+, runtime platform selection), or `--headless`.
* Profiles with press / release actions, repeat-while-held (for FCU knobs), 2-position switches.
* **Axes** as inputs: an axis can send its position as a value (brightness knobs, QNH set...) or act as a +/- knob, with dead zones, inversion and rate limiting.
* Searchable catalogs of cockpit functions for the FlyByWire **A32NX** and **A380X**, generated from the FlyByWire docs, with a per-profile aircraft selector.

## AI-assisted code, real-hardware tested

This code base was written with AI assistance (Claude), under the direction of and reviewed by the
maintainer. That is a statement about how it was produced, not about how it was validated: every
hardware path in it was exercised on the real devices it targets, and no issues were found.

| What | How it was verified |
|---|---|
| SimConnect wire protocol | live handshake and event traffic against MSFS 2020 under Proton (KittyHawk 11.0.282174.999), plus a mock server in `tools/` |
| MobiFlight module path (`L:` set/toggle, readback) | APU MASTER toggled from the tool on a cold & dark FlyByWire A32NX and read back 0 → 1 → 0 |
| WINCTRL URSA MINOR Combat Throttle Metal.EX (`4098:b970`) | controllers discovered over hidraw, both grip motors driven and felt, addresses confirmed by hand, config words dumped before and after |
| WINCTRL URSA MINOR Combat Joystick (`4098:bc29`/`bc2a`) | discovered the same way; motor address from the community kernel driver and X-Plane plugin |
| Axis bindings | verified end to end with a virtual (uinput) joystick through the real app and a mock simulator: value mode sent 10, 20 ... 100, step mode sent the right number of +/- events, and "Listen for input" captured the moved axis in the GUI. Your physical axes were read (ranges and rest positions) but not yet moved by hand with this tool |
| FlyByWire A380X | catalog generated from the FlyByWire A380X docs; the APU, FCU and many overhead names were confirmed present in an installed A380X package. **Not yet exercised on the A380X in flight with this tool**: verify an A380X binding with its *Test* button before relying on it. The docs describe the development branch: the stable build's baro knob uses its own `A380X_EFIS_CP_BARO_PUSH/PULL_<n>` H events and lacks the `A32NX.FCU_EFIS_*_BARO_*` events the docs list, so the A380X catalog has no baro recipes and its EFIS baro entries may do nothing on older builds |
| Device safety | the tool only sends identity queries, config *reads* and volatile motor levels; it never writes flash, restarts or re-flashes a device (`--dump-winwing-config` gives a baseline to diff) |

If you have a Winwing device that is not listed, discovery plus `--motor-sweep` will usually find
its motor in a minute; please report the result so it can be added to the defaults.

## How it works

| Action kind | Transport | Needs |
|---|---|---|
| `K:` event (`GEAR_UP`, `A32NX.FCU_AP_1_PUSH`, ...) | SimConnect `MapClientEventToSimEvent` + `TransmitClientEvent` | nothing |
| `H:` event (`A32NX_EFIS_L_CHRONO_PUSHED`) | calculator code through the MobiFlight WASM module client-data area | MobiFlight WASM module |
| `L:` var set (`A32NX_AUTOBRAKES_ARMED_MODE_SET = 2`) | same | MobiFlight WASM module |
| `L:` var toggle (`A32NX_OVHD_APU_MASTER_SW_PB_IS_ON`) | RPN `(L:X) ! (>L:X)`, evaluated in the sim | MobiFlight WASM module |
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
`--host`, `--port`, `--profile NAME`, `--config FILE`, `--catalog FILE`, `--quiet`,
`--probe-winwing`, `--motor-sweep [DEV]`, `--dump-winwing-config`,
`--fire KIND:NAME[=V]` (one-shot action, e.g. `--fire lvar_toggle:A32NX_OVHD_APU_MASTER_SW_PB_IS_ON`),
`--read NAME` (print an `L:` var).

### AppImage

```sh
./packaging/build-appimage.sh      # -> dist/msfs-linuxrebind-<version>-x86_64.AppImage
```

The script builds with libstdc++/libgcc linked statically, bundles GLFW (3.4+ is often missing on
older distributions) and downloads `appimagetool` plus the AppImage runtime into `packaging/tools/`
on first use. libGL, the X11/Wayland client libraries and glibc come from the host. Icons live in
`packaging/icons/` (`make_icons.py` regenerates the PNG sizes and the embedded window icon from the
same shapes as the SVG). `sudo ninja -C build install` installs the binary, catalog, `.desktop`
file and hicolor icons the classic way instead.

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

**Proton cannot bind port 500**: Linux reserves ports below 1024, so MSFS silently falls back to
its "Dynamic IP4 port" entry and picks a random port on every launch (it does not record it
anywhere Linux can read). The tool handles this: when the configured port refuses, it scans the
loopback listeners owned by `wineserver`, sends the SimConnect Open handshake to each, and uses the
one that answers (the status bar shows the detected port). Setting a fixed `<Port>` above 1024 in
`SimConnect.xml` (e.g. 5500) and in the tool's settings skips the scan. The tool reconnects
automatically whenever the sim is (re)started.

## Usage

1. Start MSFS (any point, even the main menu) and this tool.
2. **Devices** tab: press buttons on the HOTAS to see their numbers.
3. **Bindings** tab: *Add binding* → *Listen for input* → press the button → add actions by hand or
   *from catalog* → *Save binding*. *Test* fires the press actions immediately.
4. Leave the same buttons **unassigned in the MSFS controls menu**, otherwise both fire.

### Axes

Move an axis while *Listen for input* is on (about a quarter of its travel; small drift is ignored) and
the binding becomes an axis binding. The **Devices** tab shows every axis live, which is the easy way to
find out which one is which. Two modes:

* **Value**: the position is rescaled to the output range you set (default 0 to 100, whole numbers) and
  used as the parameter: the value of a `K:` event, the value written by *L: var set*, or `{v}` inside
  RPN code, for example `{v} (>L:A32NX_SOME_BRIGHTNESS)`. Updates are rate limited (40 ms by default) and
  the last position is never lost.
* **Steps**: moving up runs the *up* actions and moving down the *down* actions, once per step of travel
  (default 10 %). Pair `A32NX.FCU_EFIS_L_BARO_INC` and `_DEC` for a QNH knob from a slider.

**Sim axis events need their own range.** Events such as `THROTTLE1_AXIS_SET_EX1`, `AILERON_SET` or
`AXIS_LEFT_BRAKE_SET` take values around -16384 to 16384 (throttles: full reverse to full thrust), not
0 to 100. The catalog stores the range the FlyByWire docs give for each of them: a new axis binding for
such an event gets it automatically, the editor warns (with a *Set range* button) when the output range is
wrong, and the Bindings tab shows a *Fix the ranges* banner for existing bindings (the tool logs them at
start-up too). *Output range* also has presets (percent, sim axis with or without reverse, 0 to 1). Leave
the same physical axis unassigned in the MSFS controls menu, otherwise both inputs fight.

*Use travel from/to* turns the ends of the physical travel into dead zones and *Invert* flips the
direction. `tools/virtual_joystick.py demo` creates a fake joystick to try all of this without hardware.

Tips:

* Momentary cockpit push buttons: one `K:` event on press (e.g. `A32NX.FCU_AP_1_PUSH`).
* Overhead push buttons that are `L:` vars (APU MASTER, APU START, most of the overhead): use
  *L: var toggle*, which flips the switch each press whatever changed it in between. The catalog
  picks toggle automatically for `0|1` vars. The Bindings table shows the live value next to the action.
* FCU knobs on a rotary/hat: `A32NX.FCU_HDG_INC` with *Repeat while held*.
* Two-position switches (e.g. an autobrake toggle): `A32NX.AUTOBRAKE_SET 2` on press and
  `A32NX.AUTOBRAKE_SET 0` on release.
* `A32NX.FCU_ALT_INCREMENT_SET 1000` / `100` to pick the ALT knob step.
* Baro / QNH on three buttons: filter the catalog for `baro`. *Baro STD / QNH toggle* (captain,
  first officer, or both) is an RPN recipe that reads the current mode and pushes or pulls the knob
  accordingly; `A32NX.FCU_EFIS_L_BARO_INC` / `_DEC` with *repeat while held* make two buttons act
  as the knob. Tick *Both sides* before adding a captain-side (`_L_`) entry to get the first
  officer twin in the same binding, or use the *(both sides)* recipes.

## Aircraft profiles

Each profile belongs to an aircraft (A32NX or A380X). The aircraft decides which catalog the editor and
the *Catalog* tab offer; it never changes existing bindings, so you can re-assign a profile at any time.

* **New profile** asks for the aircraft; the default is the aircraft currently loaded in the sim.
* When the loaded aircraft does not match the active profile, the Bindings tab says so and offers a
  one-click *Switch to "<profile>"* (or *Create a profile for it*). Nothing is switched automatically.
* *Catalog* has an aircraft selector for browsing the other aircraft's list ("browsing only" is shown).
* Numbered controls such as `ENG {NUM} BLEED` are expanded into one entry per unit (1 to 4, as the
  FlyByWire docs define the placeholder; the ADIRS and the air-conditioning packs stop at 3). A number
  the aircraft does not have simply does nothing.
* Catalog files are `data/<aircraft>_catalog.json`; any `*_catalog.json` placed in
  `~/.config/msfs-linuxrebind/` overrides the shipped one with the same name, and `--catalog FILE`
  adds one for a single run. `tools/build_catalog.py --all` regenerates the shipped ones.

### Upgrading does not break your configuration

The config is versioned (`version` in `config.json`; 2 since 0.2.0). Profiles written by older builds
have no aircraft and load as A32NX profiles with every binding, haptics setting and device address
intact. The first time a newer build saves over an older file it keeps the original once as
`config.json.v1.bak` (never overwritten afterwards), so rolling back is a file copy. Unknown or newer
fields are ignored rather than rejected. `ctest` runs a round-trip test of exactly this (a schema-1
fixture, optionally your own config: `build/config_roundtrip ~/.config/msfs-linuxrebind/config.json`,
which only ever works on a copy), plus a test that every documented command-line option is accepted.

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
* `tools/build_catalog.py --all` regenerates `data/a32nx_catalog.json` and `data/a380x_catalog.json` from the FlyByWire docs.
* `ctest --test-dir build` runs the config compatibility, command-line option and axis tests.
* `tools/virtual_joystick.py` is a uinput joystick for exercising bindings without hardware.
* `_cpr/` is git-ignored: put copies of files from the (read-only) MSFS install there if you need
  to edit them.

Protocol references: [node-simconnect](https://github.com/EvenAR/node-simconnect) (SimConnect wire
format), [MobiFlight WASM module](https://github.com/MobiFlight/MobiFlight-WASM-Module),
[FlyByWire A32NX flight deck API](https://docs.flybywiresim.com/aircraft/a32nx/a32nx-api/a32nx-flightdeck-api/).

## License

MIT. Dear ImGui (MIT) and nlohmann/json (MIT) are vendored under `third_party/`.
