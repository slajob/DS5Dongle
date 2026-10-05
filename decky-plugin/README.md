# DS5 Bridge: DS5Dongle without the dongle (Decky Loader plugin)

Connect a DualSense (or DualSense Edge) to the Steam Deck over its built-in
Bluetooth and make it behave like a USB-connected controller:

- 🎮 HD haptics (the audio-driven vibration games use on a wired DualSense)
- 🔊 Controller speaker and 3.5 mm headset jack
- 🎤 Controller / headset microphone
- 🎯 Adaptive triggers, light bar, player LEDs and mute LED through the normal
  wired output reports

It does the same job as the DS5Dongle firmware in this repository, but runs on
the Deck itself instead of a Raspberry Pi Pico 2 W.

> **Status: experimental.** The daemon is covered by an end-to-end test-suite
> that simulates the kernel (USB/IP) and the controller, but it has not been
> validated on real Steam Deck hardware yet. Please report what works and what
> does not, including the log (see [Troubleshooting](#troubleshooting)).

## How it works

```
 DualSense ──Bluetooth──► BlueZ + hid-playstation ──► /dev/hidrawN
                                                         │  (hidden from Steam)
                                                         ▼
                                                   ds5bridged  ◄── Decky plugin (settings, status)
                                                         │  USB/IP
                                                         ▼
                                     vhci-hcd: virtual USB "DualSense Wireless Controller"
                                         │                         │
                                  snd-usb-audio              usbhid + hid-playstation
                       (4ch out: speaker + haptics,     (input, adaptive triggers, LEDs)
                         2ch in: microphone)                       │
                                         └──────── Steam / games ──┘
```

Over Bluetooth a DualSense has no standard audio profile. Haptics, speaker and
mic audio travel in proprietary HID reports, which SteamOS does not implement.
Games only use those features when they find a *wired* DualSense with its
USB audio interface.

The Pico firmware solves this by being a Bluetooth host for the controller and
a USB device for the PC. `ds5bridged` does the same thing in software:

| Firmware (Pico)                         | Plugin (Steam Deck)                                        |
|-----------------------------------------|------------------------------------------------------------|
| BTstack + L2CAP to the controller       | BlueZ/HIDP, through `/dev/hidrawN`                          |
| TinyUSB device with the DS5 descriptors | Same descriptors, attached as a virtual USB device via `vhci-hcd` (USB/IP) |
| `audio.cpp`: 48 kHz → 3 kHz haptics, Opus speaker, Opus mic | Same pipeline (same Opus version, settings and report layout) |
| `main.cpp`: USB report 0x02 → BT 0x31   | Same translation, same config overrides                    |
| Web config                              | Quick Access panel                                         |

Because the virtual controller is a real USB device as far as the kernel is
concerned, everything downstream (snd-usb-audio, PipeWire, Steam Input, Proton)
sees exactly what it would see with the Pico plugged in.

## Requirements

- SteamOS 3.5 or newer with [Decky Loader](https://decky.xyz)
- The `vhci-hcd` kernel module (part of the stock kernel's USB/IP support).
  Check from Desktop Mode → Konsole: `sudo modprobe vhci-hcd && ls /sys/devices/platform/vhci_hcd.0`
- A DualSense / DualSense Edge paired with the Deck over Bluetooth
  (Settings → Bluetooth, hold **PS + Create** until the light bar flashes)

## Install

1. Get `ds5-bridge.zip`: download the `ds5-bridge-plugin` artifact of the
   latest *Build Decky plugin* GitHub Actions run (GitHub wraps it in another
   zip, so extract that once), or build it yourself (below).
2. Decky → ⚙️ Settings → General → enable **Developer mode**.
3. Decky → ⚙️ Settings → Developer → **Install plugin from ZIP** and pick the zip.
4. Open the Quick Access menu → DS5 Bridge. Turn on the controller.

## Using it

The panel shows each bridged controller (battery, whether the virtual wired
controller is active, and whether haptics/audio and the mic are streaming).
The settings match the firmware's web config:

| Setting | Firmware field | Meaning |
|---|---|---|
| Haptics strength | `haptics_gain` | 100–200 % |
| Adaptive trigger strength reduction | `trigger_reduce` | 0 = game decides, 1–10 = weaker |
| Speaker output | `speaker_select` | Auto / controller speaker / headset / off |
| Speaker boost | `speaker_gain` | 0 = game decides, 1–7 |
| Microphone | `mic_select` | Auto / controller / headset / off |
| Lock volume | `lock_volume` | Ignore volume/mute changes from games |
| Controller audio buffer | `audio_buffer_length` | Raise if audio/haptics stutter |
| Present as | `controller_mode` | Auto / DualSense / DualSense Edge |
| Turn off when idle | `inactive_time` | Minutes, 0 = never |
| Light bar on connect | – | Colour set when the controller connects |

Turning the bridge off gives the controller back to SteamOS as a normal
Bluetooth controller immediately (no re-pairing needed).

### What the plugin changes on your system

- Loads the `vhci-hcd` kernel module.
- Installs `/etc/udev/rules.d/72-ds5bridge.rules`. While the bridge runs (the
  file `/run/ds5bridge/active` exists) it makes the *raw Bluetooth* controller
  root-only, so Steam and games see only the bridged wired controller instead of
  two copies. With the bridge off the rule does nothing. Uninstalling the plugin
  removes it.

## Troubleshooting

- **Log:** panel → *Show log*, or `~/homebrew/logs/DS5 Bridge/ds5bridged.log`.
- **"vhci-hcd" error:** the kernel module is missing or failed to load; see
  Requirements.
- **Controller not listed:** it must be connected over Bluetooth (not USB).
  Turn it off and on again after enabling the bridge.
- **Two controllers in Steam:** turn the controller off and on again, so its
  Bluetooth device nodes are recreated with the hiding rule in place.
- **Haptics/speaker stutter:** raise *Controller audio buffer*. The Deck's
  Wi-Fi and Bluetooth share one radio; heavy Wi-Fi traffic (streaming,
  downloads) competes with the ~26 KB/s the controller audio needs.
- **No haptics in a game:** the game must support DualSense haptics on PC
  *and* enable them for a wired controller under Proton. The bridge gives the
  game the same thing a USB cable or the Pico would.

## Building

```sh
cd decky-plugin
./build.sh            # daemon + tests + frontend → out/ds5-bridge.zip
```

Needs cmake, a C++17 compiler, python3, node and pnpm. The daemon uses the
firmware's Opus submodule (`lib/opus`, fetched automatically).

With the [Decky CLI](https://github.com/SteamDeckHomebrew/cli),
`decky plugin build decky-plugin` builds the daemon inside the SteamOS
(holo) container via `backend/Dockerfile`.

### Tests

```sh
make -C backend test
```

`backend/tests/` contains a fake kernel (speaks USB/IP like `vhci-hcd`) and a
fake DualSense. The tests check enumeration and descriptors byte-for-byte
against the firmware, feature/input/output report translation, the UAC volume
and mute controls, the 1 ms isochronous pacing, the haptics resampling, the
Opus speaker stream, the microphone path, Edge handling and the control socket.

### Layout

```
main.py                 Decky backend: daemon supervision, udev rule, settings
src/index.tsx           Quick Access panel
backend/src/
  main.cpp              daemon: epoll loop, controller discovery/hiding, control socket
  bridge.cpp            one controller: the firmware's report/audio logic
  virtual_device.cpp    USB device emulation (control, interrupt, paced iso endpoints)
  usbip.cpp             USB/IP protocol + vhci-hcd attach
  usb_descriptors.cpp   descriptors (HID report descriptors generated from the firmware)
  audio.cpp             haptics resampler, Opus speaker encoder, Opus mic decoder
  bt_link.cpp           hidraw link, evdev grab, node hiding
```

## Limitations

- DualSense Edge profile editing (Sony's PC app) is not bridged, same as the
  firmware's default.
- Feature-report writes from the host are dropped, as in the firmware.
- One virtual USB device per controller; several controllers are supported
  (up to the 8 ports of `vhci-hcd`).
