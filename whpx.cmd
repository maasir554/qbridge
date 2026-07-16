@echo off
tasklist /FI "IMAGENAME eq qemu-system-x86_64-qbridge-media-v9.exe" 2>NUL | find /I "qemu-system-x86_64-qbridge-media-v9.exe" >NUL
if not errorlevel 1 (
  echo The QBridge media VM is already running.
  pause
  exit /b 1
)
tasklist /FI "IMAGENAME eq qemu-system-x86_64.exe" 2>NUL | find /I "qemu-system-x86_64.exe" >NUL
if not errorlevel 1 (
  echo Another QEMU VM is already running.
  pause
  exit /b 1
)

set "PATH=E:\QEMU\qbridge\install-fixed\bin;C:\msys64\ucrt64\bin;%PATH%"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\QEMU\stop-qbridge-media.ps1"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\QEMU\start-qbridge-media.ps1"
if errorlevel 1 (
  echo Host webcam capture failed.
  type "E:\QEMU\logs\qbridge-media.log" 2>NUL
  pause
  exit /b 1
)

"E:\QEMU\qemu-src\build\qemu-system-x86_64-qbridge-media-v9.exe" ^
-L "C:\Program Files\qemu\share" ^
-machine q35,accel=whpx ^
-cpu Skylake-Client-v4 ^
-m 8G ^
-smp 4 ^
-smbios type=0,vendor="American Megatrends International",version="K3605ZF.316" ^
-smbios type=1,manufacturer="ASUSTeK COMPUTER INC.",product="Vivobook_ASUSLaptop K3605ZF_K3605ZF",version="1.0" ^
-smbios type=2,manufacturer="ASUSTeK COMPUTER INC.",product="K3605ZF",version="1.0" ^
-drive if=pflash,format=raw,readonly=on,file="E:\QEMU\firmware\OVMF_CODE.fd" ^
-drive if=pflash,format=raw,file="E:\QEMU\firmware\OVMF_VARS.fd" ^
-device ich9-ahci,id=sata ^
-drive id=disk0,if=none,file="E:\QEMU\images\windows11.qcow2",format=qcow2 ^
-device ide-hd,drive=disk0,bus=sata.0 ^
-drive id=virtio,if=none,media=cdrom,file="E:\QEMU\iso\virtio-win.iso" ^
-device ide-cd,drive=virtio,bus=sata.2 ^
-netdev user,id=net0 ^
-device e1000e,netdev=net0 ^
-audiodev dsound,id=hostaudio ^
-device ich9-intel-hda,id=hda ^
-device hda-micro,bus=hda.0,audiodev=hostaudio ^
-device qemu-xhci,id=xhci ^
-device usb-qbridge-camera,bus=xhci.0 ^
-display gtk ^
-vga std

powershell.exe -NoProfile -ExecutionPolicy Bypass -File "E:\QEMU\stop-qbridge-media.ps1"
pause
