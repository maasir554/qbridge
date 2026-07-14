# QBridge

QBridge makes a USB device exported by `usbipd-win` appear as an ordinary USB
device on a QEMU USB bus. There is no guest agent, guest driver, virtual camera,
or device-specific emulation: guest USB requests are translated to USB/IP URBs
and their completions are returned to QEMU's existing USB core.

The repository contains:

- `qbridge`, a portable C ABI library with a C++17 implementation for Windows,
  Linux, and macOS;
- `qbridge list` and `qbridge probe`, diagnostic commands for `usbipd-win`;
- `qemu/hw/usb/dev-usbip.c`, the QEMU `-device usb-ip` adapter;
- wire-format tests for discovery, import, submit, unlink, and ISO descriptors.

## Implemented behavior

- USB/IP 1.1.1 device listing and import;
- asynchronous control, bulk, interrupt, and isochronous submissions;
- multiple in-flight URBs with sequence-based, out-of-order completion;
- USB/IP unlink for QEMU packet cancellation;
- device, every configuration, language, manufacturer, product, and serial
  descriptor discovery;
- endpoint type, alternate-setting, max-packet, interval, and SuperSpeed stream
  setup from the physical configuration descriptors;
- disconnect and reconnect callbacks used by QEMU as USB detach/attach events;
- initial wait for a bus ID, allowing `usbipd bind` after QEMU starts;
- VID, PID, and serial allow-list policy;
- loopback-only connections unless remote access is explicitly enabled;
- bounds checks for device counts, transfer sizes, ISO counts, descriptors, and
  bus IDs.

## Build on Windows

Use a Visual Studio Developer PowerShell:

```powershell
cmake -S . -B build -A x64 -DQBRIDGE_BUILD_SHARED=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Or use the MSYS2 UCRT64 procedure in
[`qemu/README.md`](qemu/README.md) when building QEMU itself.

## Prepare a device

In an elevated PowerShell on the QEMU host:

```powershell
usbipd list
usbipd bind --busid 1-7
```

Binding shares the device; do not attach it to WSL, because QBridge is the
USB/IP client. Verify discovery before starting QEMU:

```powershell
build\Release\qbridge.exe list
build\Release\qbridge.exe probe --busid 1-7
```

Then use a QEMU build containing the adapter:

```powershell
qemu-system-x86_64.exe `
  -device qemu-xhci,id=xhci `
  -device usb-ip,bus=xhci.0,host=127.0.0.1,port=3240,busid=1-7
```

Optional policy example:

```text
-device usb-ip,busid=1-7,vendorid=0x046d,productid=0x0825,serial=ABC123
```

## Architecture

The QEMU adapter depends only on the public C transport boundary in
`include/qbridge/qbridge.h`. Socket handling, USB/IP framing, descriptor
management, pending-URB ownership, cancellation, and recovery remain outside
QEMU. This is the intended extension point for a future local-libusb or other
transport implementation without changing the packet-to-QEMU adapter.

The receiver thread never calls the QEMU USB core directly. It records a
completion and schedules a guarded QEMU bottom half; packet completion,
endpoint updates, and hotplug consequently run in QEMU's main-loop context.

## Security and operational limits

USB/IP has no authentication or encryption. QBridge therefore rejects every
non-loopback destination by default. `allow-remote=on` is an explicit opt-in to
plaintext remote USB/IP and should only be used inside a separately secured
network or tunnel. Native TLS is not implemented because it is not negotiated
by the interoperable USB/IP protocol or by `usbipd-win`.

Live migration is disabled for `usb-ip`, because ownership of a physical
server-side USB device cannot be transferred atomically. USB/IP also has no
wire-level bus-reset command; guest resets cancel outstanding URBs and reset
QEMU endpoint state, while reconnecting provides the physical re-enumeration
path.

The latency, CPU, and memory targets in `REQUIREMENTS.md` require benchmarking
on the intended Windows/QEMU/webcam combination. The data path performs no
transcoding or media copies beyond the copies required between QEMU I/O vectors
and USB/IP TCP buffers, and permits many URBs to remain in flight.

## Developer checks

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```
