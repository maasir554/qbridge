# Camera passthrough

## Architecture

```text
Host webcam
  -> Windows Media Foundation
  -> Local\QBridgeCamera shared memory
  -> QEMU usb-qbridge-camera device
  -> guest UVC driver
```

The guest identifies the camera as:

- Manufacturer: `Azurewave`
- Product: `USB2.0 HD UVC WebCam`
- VID:PID: `13d3:5463`

No guest-specific driver is required.

## Host capture

`qbridge-media.exe` captures camera index `0` as 640x480 MJPEG at 30 fps.

Relevant files:

- `cli/media.cpp`
- `include/qbridge/media.h`
- `start-qbridge-media.ps1`
- `stop-qbridge-media.ps1`

Frames are published through `Local\QBridgeCamera`.

## QEMU integration

The QEMU device is registered as `usb-qbridge-camera` and is attached with:

```text
-device qemu-xhci,id=xhci
-device usb-qbridge-camera,bus=xhci.0
```

The validated QEMU binary is:

```text
E:\QEMU\qemu-src\build\qemu-system-x86_64-qbridge-media-v9.exe
```

## UVC requirements

- UVC 1.1 34-byte probe/commit control.
- Variable-size 640x480 MJPEG frames at 30 fps.
- High-speed bulk IN endpoint `0x82`, 512-byte maximum packet size.
- One UVC payload header per complete JPEG, not per USB transaction.
- Minimal payload header containing `EOH`, `EOF`, and alternating `FID`.
- No fabricated PTS/SCR timestamps.
- A QEMU timer must wake NAKed bulk requests when a new host frame arrives.
- Frames must follow the producer rate; resending at USB request speed floods
  the guest.

## Usage

Run:

```powershell
E:\QEMU\whpx.cmd
```

The launcher starts `qbridge-media`, boots QEMU, and stops capture when QEMU
exits. Only one application or VM can own the host webcam at a time.
