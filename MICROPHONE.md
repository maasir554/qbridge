# Host microphone forwarding

QEMU forwards the default Windows recording device to the guest through its
DirectSound backend and an emulated Intel HDA microphone.

```text
Default host microphone
  -> Windows DirectSound capture
  -> QEMU audio backend
  -> Intel HDA microphone
  -> guest audio applications
```

Launcher options:

```text
-audiodev dsound,id=hostaudio
-device ich9-intel-hda,id=hda
-device hda-micro,bus=hda.0,audiodev=hostaudio
```

The guest uses its built-in Intel High Definition Audio driver. QEMU captures
the Windows default recording device selected when the VM starts.

To change the source, select another default input device in Windows Sound
settings before starting the VM.
