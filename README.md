# sidecar-emu

The Sidecar emulator: QEMU cut down to the one machine and the devices the
Sidecar engine gates need. It boots the Sidecar Circle engine
(`kernel8-rpi4.img`) on an emulated Raspberry Pi 4B, reaches it over
Ethernet, puts an Analog Rytm MKII stand-in on its USB-A side and gives its
USB-C gadget a raw MIDI TCP port, so an OS upgrade or an elkbench run can go
through the emulated Pi into analog-emu.

This is a local experiment, not a QEMU fork meant for upstream. QEMU's
policy (`docs/devel/code-provenance.rst` upstream) does not accept
AI-generated code, and the Sidecar changes here were written with an AI
assistant, so none of it may be submitted to QEMU.

## What is in the tree

- **Upstream base.** QEMU commit `f29ccc287a6cfdd51d3a4fd17fbe9af3310a0ac5`
  (11.1.50, https://gitlab.com/qemu-project/qemu), but only the files that
  configuring and building `qemu-system-aarch64` for `raspi4b` uses: about
  2,100 of 11,700. The other targets, the tests, the docs, the tools and the
  firmware blobs are gone. The history shows the cut in steps: the unchanged
  upstream import, the Sidecar commits, then the build-system cut.
- **Sidecar changes** (one commit each):
  1. **raspi4b-net.** The engine boots and answers `/status` over GENET:
     UART clock, mailbox clock tags, the PCIe outbound window, xHCI sub-word
     reads, a `vl805` xHCI on PCIe, GENET status-block handling.
  2. **usb-rytm** (`hw/usb/dev-rytm.c`). A Rytm-shaped high-speed USB device
     (1935:0B48) whose other end is analog-emu's `--ob-link` on a chardev:
     Overbridge audio blocks, USB MIDI, vendor requests answered from
     properties. VL805 port order and Configure Endpoint fixes.
  3. **GENET TX wrap.** The TX consumer index wraps at 16 bits (QEMU hung
     after 65,536 packets).
  4. **dwc2-gadget** (`hw/usb/dwc2-gadget.c`). DWC2 device mode with a
     built-in USB host that enumerates the engine's gadget and carries its
     USB MIDI to a chardev as raw MIDI bytes; `gadget-out-rate` paces the
     way in.
  5. **xHCI Evaluate Context** ignores drop flags (full-speed devices
     enumerate), and `usb-hub,fail-intr=on` (test only) makes a hub's
     status-change endpoint fail like the VL805 hub in T103.
  6. **Generic timer counter without the BQL.** CNTPCT/CNTVCT reads skip the
     global lock when icount is off (`ARM_CP_LOCKLESS_READ`). The engine
     polls the counter from every core; with the lock, the vCPUs queued on
     it. The M4 OS upgrade runs about twice as fast. The full dev engine
     still hits the watchdog (cross-core wake-ups also take the lock).
  7. **Watchdog off switch.** `-global bcm2835-powermgt.watchdog=off`
     (`WATCHDOG=off` for run-sidecar.sh) stops the Pi watchdog firing on a
     missed pat, so the full dev engine can run. Reboots the guest asks for
     (a timeout under 0.25 s) still happen, including the engine's own
     render-core guard.
  8. **Minimal build.** The `raspi` Kconfig entry selects what it needs, and
     `configs/devices/aarch64-softmmu/sidecar.mak` enables only raspi4b,
     usb-rytm (and usb-a4, usb-digitakt), usb-teensy, usb-hub and usb-hid.
  9. **usb-teensy** (`hw/usb/dev-teensy.c`). A stand-in for the Teensy 4.1
     mirror relay, not an emulation of its chip or firmware: the link
     (1209:0002, protocol 1.02 of sidecar-firmware `pi/engine/relay_proto.h`:
     bulk event pipe, link audio, four channels, the forwarded control
     requests, credits, stats, log, the bootloader request) and HalfKay
     (16C0:0478: 1088-byte SET_REPORT blocks, erase refusals after block 0,
     the start report, then the relay back with a new build). Its optional
     chardev is the computer behind the relay (`sidecar/tools/teensystub.py`).
     Fault options for the flows that failed on the unit: `bl-ignore`,
     `bl-stay`, `drop-block=N`, `leave-quiet-ms=N` (every transfer ends in a
     transaction error that long before the Teensy leaves), `start-halfkay`;
     timings `enter-ms`, `halfkay-ms`, `erase-ms`, `boot-ms`, `connect-ms`;
     builds `build=` and `flash-build=`.
  10. **usb-hub,high-speed=on.** A USB 2.0 hub, as the Pi 4's on-board one
     behind the VL805's single USB 2 port, so that usb-rytm and usb-teensy
     (both high speed) can be plugged at once. QEMU's hub stays full speed
     without it. It carries the board hub's ids (VIA Labs 2109:3431).
  11. **SD card on EMMC2.** `-drive if=sd` goes into EMMC2, the Pi 4's card
     slot (what Circle drives), instead of the legacy sdhci/sdhost mux, so
     the engine has a card (`CARD=` for run-sidecar.sh,
     `sidecar/tools/mkcard.sh` makes a 256 MB FAT32 one).
  12. **usb-a4** (in `hw/usb/dev-rytm.c`). An Analog Four MKII stand-in
     (1935:0b47, product "Analog Four MKII") on the same link protocol as
     usb-rytm: alternate 3 blocks of 256 B in (8 channels) and 200 B out
     (6 channels), its own serial and uid. With usb-rytm it makes the
     two-box Pi (`A4_LINK=` for run-sidecar.sh). Each box attaches when its
     link connects and detaches when it closes.
  13. **usb-digitakt** (in `hw/usb/dev-rytm.c`). A Digitakt (mk1) stand-in
     (1935:0b2c, product "Digitakt") on the same link protocol: alternate 3
     blocks of 368 B in (12 channels: MAIN, tracks 1..8, INPUT) and 88 B out
     (2 channels, MAIN), its own serial and uid, on port 4 of the board hub
     (`DT_LINK=` for run-sidecar.sh; `LINK=` empty leaves the Rytm out, for
     an A4 + Digitakt pair). The layout is sidecar-firmware's
     docs/device-profiles.md "Audio" table; nothing of the Digitakt's
     firmware is in it, and no Digitakt emulator stands behind the link
     (a stublink, sine on MAIN).

## Build

```
git clone https://github.com/Iced-Cream/sidecar-emu.git
cd sidecar-emu
sudo apt install build-essential ninja-build python3-venv pkg-config libglib2.0-dev libslirp-dev
sidecar/build.sh            # -> build/qemu-system-aarch64, about 1,500 compile steps
```

Works on Linux and in WSL. On the PC, clone it into the WSL home (not
`/mnt/d`): the build is much faster on the Linux filesystem. Only the `aarch64-softmmu` target with the
`sidecar` device config can be configured from this tree.

## Run

```
sidecar/run-sidecar.sh path/to/kernel8-rpi4.img "usbmidi=os:on"
```

That is the M4 command line: the engine's port 80 on `127.0.0.1:18080`, the
gadget's raw MIDI on `127.0.0.1:7400` (`elkflash --port tcp:127.0.0.1:7400`,
one client at a time), and usb-rytm connecting to analog-emu's link on
`127.0.0.1:7301`. Environment variables in the script change the ports, turn
the gadget or usb-rytm off, or add QEMU arguments.

The full dev engine needs `WATCHDOG=off CPUS=0-1`: on a 16-CPU WSL host,
pinning QEMU to 2 CPUs took the web server from over 5 minutes to 3.6 s
after boot.

## Tests

None of these need firmware; `sidecar/tools/stublink.py` stands in for
analog-emu's link.

Several test runs can share a host. The gate scripts link their stubs over
unix sockets in a private temporary folder (`LINK`, `A4_LINK` and `TEENSY`
take `unix:PATH`), and every port they forward is the usual one when free,
otherwise a free one (`sidecar/tools/freeport.py`); an explicit `HTTP_PORT`
is used as given. Give each run its own outdir and, on a many-core host,
its own `CPUS` (e.g. `0-1` and `2-3`).

- `sidecar/smoke-test.sh <kernel>`: two boots. A USB keyboard on the VL805
  enumerates and `/status` answers; then usb-rytm enumerates against the stub
  link and the gadget's endpoints come up.
- `sidecar/tools/gadget-roundtrip.py [port] [tries]`: with run-sidecar.sh and
  stublink.py running, an identity request into the gadget comes back as the
  stub's 49-byte reply.
- `sidecar/tools/hubloop.sh <kernel> <outdir> [cycles]`: hub and keyboard
  plugged and unplugged over QMP.
- `sidecar/tools/hubfail.sh <kernel> <outdir> [cycles]`: a `fail-intr` hub
  unplugged at varied times; the engine must keep answering.
- `sidecar/tools/teensystub.py [--port 7302]`: the computer behind usb-teensy.
  With `TEENSY=127.0.0.1:7302` run-sidecar.sh plugs a high-speed hub with
  usb-rytm on port 1 and usb-teensy on port 2; when the Pi shows the box
  through the relay the stub enumerates it and sends the Rytm's Overbridge
  vendor requests, which go relay -> Pi -> box and back. `TEENSY=none` plugs
  usb-teensy with no computer. A Teensy flash through the Pi
  (sidecar-firmware `tools/teensy_flash.py`, with `paircode=0` in the
  emulator's cmdline) runs against usb-teensy as against the real relay.

- `sidecar/tools/twobox-gate.py <kernel> <outdir> [--steps ...]
  [--expect-folders] [--expect-slots]`: two boxes on one Pi (two of
  usb-rytm, usb-a4 and usb-digitakt behind the board hub, each on its own
  stublink, a fresh card). A step names two boxes in attach order: `rytm-a4`
  is the Rytm at boot, the A4 plugged second, unplugged and plugged back;
  `a4-rytm`, `rytm-dt`, `dt-rytm`, `a4-dt`, `dt-a4` the same for those;
  `again` boots on rytm-a4's card, `again:X-Y` on step X-Y's. Default
  steps `rytm-a4,a4-rytm,again`. It judges the emulator (QEMU's attached
  boxes at each step, /status, a readable card, no panic/assert) and
  records the engine's /status, UART lines and card folders (`/.rytm`,
  `/.a4`, `/.dt`, `/.sidecar`) in each step's `summary.json`;
  `--expect-folders` adds the step's boxes' folders to the pass;
  `--expect-slots` adds the engine's `slots` (first box slot 0, second
  slot 1, the first kept while the second is out) and, on a kernel with
  CPU admission, each box asking its profile's voices (Rytm 8, A4 4,
  Digitakt 8). The A4's stub says hello with box id 3, the Digitakt's with
  2, the Rytm's none.

Kernels, analog-emu cards and anything else built from instrument firmware
stay out of this repository.

## Repository

https://github.com/Iced-Cream/sidecar-emu is GitHub's fork of qemu/qemu,
renamed. Its only branch, `main`, holds this cut-down tree, with history
starting at the upstream import. Updating the base means importing the same
file set at a newer upstream commit and replaying the Sidecar commits on it.

## Licence

GPL-2.0, as QEMU (see `COPYING` and `LICENSE`). The Sidecar device files
carry `SPDX-License-Identifier: GPL-2.0-or-later`. QEMU's own README is
`README.qemu.rst`.
