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
  7. **Minimal build.** The `raspi` Kconfig entry selects what it needs, and
     `configs/devices/aarch64-softmmu/sidecar.mak` enables only raspi4b,
     usb-rytm, usb-hub and usb-hid.

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

## Tests

None of these need firmware; `sidecar/tools/stublink.py` stands in for
analog-emu's link.

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
