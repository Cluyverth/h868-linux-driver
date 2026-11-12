# h868-linux-driver

![Language: C](https://img.shields.io/badge/language-C-00599C)
![Platform: Linux](https://img.shields.io/badge/platform-Linux-FCC624)
![Subsystem: DVB](https://img.shields.io/badge/subsystem-DVB-0078D4)
![License: GPL-2.0-or-later](https://img.shields.io/badge/license-GPL--2.0--or--later-blue)
![Status: experimental](https://img.shields.io/badge/status-experimental-orange)

[Português (Brasil)](README.pt-BR.md)

An experimental Linux kernel driver for ISDB-T terrestrial television reception with USB receiver **`07ca:0868`**.

The receiver was not recognized by a Linux driver in the tested setup. This project provides the `h868` kernel module, which initializes the device and exposes it through the Linux DVB API.

> **Warning:** this experimental driver runs in the kernel. A failure can freeze the system, so save your work before testing it.

## Hardware and compatibility

- Supported USB ID: **`07ca:0868`**. Check the device with `lsusb -d 07ca:0868`. The product name alone does not guarantee the same hardware.
- Tuner: TDA18271, using the existing Linux kernel driver.
- Demodulator: the exact model has not been confirmed.
- USB bridge variants with `bcdDevice=0` are rejected because their older protocol has not been validated.

Reception was verified on one AVerTV Hybrid Volar HD PRO H868 (`07ca:0868`) with an external antenna and CachyOS kernel `7.3.3-1-cachyos`. Other devices, kernels and distributions still need validation.

## Features and limitations

The driver registers a DVB adapter, tunes ISDB-T with a 6 MHz bandwidth, reports signal presence and lock, and delivers the digital transport stream. Experimental relative signal strength and quality measurements are available through the DVB statistics API. They are not calibrated dBm or dB values. The driver also provides experimental reporting of the transmission mode and guard interval. The driver accepts frequencies from 45 to 864 MHz, but this does not guarantee reception across the entire range.

The following features are not yet implemented or validated:

- Error counters and calibrated signal strength and quality measurements.
- TMCC layer parameter decoding, individual ISDB-T layer selection and ISDB-Tsb mode.
- Analog input, radio and remote control.

When the receiver is disconnected, processes holding DVB descriptors open can delay release of the old instance. Repeating disconnects and reconnects under these conditions can exhaust the available adapter numbers. To avoid this, close applications using the receiver before disconnecting it.

## DKMS installation

### Prerequisites

Install DKMS, `make`, the compiler and tools used to build the kernel, and prepared headers for the **same kernel version and variant** as the running kernel. The headers must also contain `drivers/media/tuners/tda18271.h`. The kernel must provide the DVB and TDA18271 modules.

The tested CachyOS setup used `dkms`, `linux-cachyos-headers`, `clang` and `llvm`.

### Register and install

From the root of this repository:

```sh
make -C kernel clean
sudo dkms add "$PWD/kernel"
sudo dkms install h868/0.1.0 -k "$(uname -r)"
dkms status h868/0.1.0
```

The `clean` command removes only local build artifacts. `dkms add` copies the sources to `/usr/src/h868-0.1.0`. Later changes in this repository **do not automatically update** that copy. `dkms install` builds and installs the module for the specified kernel.

Remove an older DKMS installation before installing this version to avoid conflicting builds of `h868`. Use `dkms status` to identify the old package and version.

Connect the receiver and check the module and DVB devices:

```sh
lsmod | grep '^h868 '
ls -l /dev/dvb/adapter0/
```

The adapter number may differ from `0`. If the module does not load automatically, run `sudo modprobe h868`. Users need permission to access the DVB devices, depending on the distribution's configuration.

### Kernel updates and Secure Boot

On distributions with DKMS hooks, a kernel update normally triggers a rebuild. Kernel API changes, incompatible headers or missing tools can still prevent the build. Check the update output and `dkms status`.

Secure Boot or lockdown may require a signed module and a trusted key. DKMS may generate a local key and MOK certificate, but that does not mean the firmware trusts the key. If a signature is required, consult your distribution's instructions.

### Removal

Close applications using the receiver and disconnect it. To unload the module and remove all DKMS builds of this driver:

```sh
sudo modprobe -r h868
sudo dkms remove h868/0.1.0 --all
```

Removing the package does not automatically unload a module in use. If another module depends on `h868` or it is still busy, `modprobe -r` may fail. Do not force its removal.

## Source code

- `kernel/h868_usb.c`: USB communication, power, I²C buses and digital stream transport.
- `kernel/h868_frontend.c`: demodulator initialization and DVB frontend integration.
- `kernel/h868_metrics.h`: relative signal metric conversions.
- `kernel/h868_ofdm.h`: transmission mode and guard interval decoding.
- `kernel/h868.h`: internal interface between driver components.
- `kernel/Makefile` and `kernel/dkms.conf`: module build and DKMS configuration.

The conversion tests can be run without a receiver:

```sh
cc -std=c11 -Wall -Wextra -Werror tests/h868_metrics_test.c -o /tmp/h868-metrics-test
/tmp/h868-metrics-test
cc -std=c11 -Wall -Wextra -Werror tests/h868_ofdm_test.c -o /tmp/h868-ofdm-test
/tmp/h868-ofdm-test
```

## License

The driver is licensed under [GPL-2.0-or-later](LICENSE), as indicated by the SPDX identifiers in its source files.
