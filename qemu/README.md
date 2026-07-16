# Building the QEMU device

`hw/usb/dev-usbip.c` is the QEMU-facing adapter. It uses QBridge through its
stable C ABI; QEMU itself does not contain any Windows socket or USB/IP wire
code.

For a Windows build, use an MSYS2 UCRT64 shell and build a shared QBridge
library so QEMU does not need to link C++ runtime objects directly:

```sh
cmake -S /c/src/qbridge -B /c/src/qbridge/build-qemu \
  -G Ninja -DQBRIDGE_BUILD_SHARED=ON -DQBRIDGE_BUILD_TESTS=OFF \
  -DCMAKE_INSTALL_PREFIX=/ucrt64
cmake --build /c/src/qbridge/build-qemu
cmake --install /c/src/qbridge/build-qemu
```

Then, from a QEMU checkout:

```sh
cp /c/src/qbridge/qemu/hw/usb/dev-usbip.c hw/usb/dev-usbip.c
git apply /c/src/qbridge/qemu/patches/0001-hw-usb-build-usb-ip-when-qbridge-is-installed.patch
./configure --target-list=x86_64-softmmu
ninja -C build
```

Keep `qbridge.dll` beside `qemu-system-x86_64.exe` (or on `PATH`). Start a VM
with an xHCI controller and the imported device:

```sh
qemu-system-x86_64.exe \
  -device qemu-xhci,id=xhci \
  -device usb-ip,bus=xhci.0,host=127.0.0.1,tcp-port=3240,busid=1-7
```

Useful device policy properties are `vendorid`, `productid`, and `serial`.
Non-loopback hosts are rejected unless `allow-remote=on` is explicitly set.
`reconnect=on` (the default) causes a QEMU USB detach followed by attach after
the same USB/IP bus ID becomes available again. `wait=on` (also the default)
allows QEMU to start before `usbipd bind`; the guest receives a normal USB
hotplug event when that bus ID becomes available.
