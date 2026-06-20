<div align="center">

<img alt="OrcaStudio" src="resources/images/BambuStudio_192px.png" width="128" height="128">

# OrcaStudio

## Fork of Bambu Studio with OrcaSlicer changes applied.

This version restores BambuNetwork support for Bambu Lab printers through the Linux `bambu_networking` component.

It is not limited to LAN-only mode. It restores the normal Bambu Lab network workflow used by Bambu Studio, including printer monitoring and printing.

</div>

> [!IMPORTANT]
> This version also fixes a slicer issue that appears when using BMCU with Bambu Lab A1 / A1-mini printers on firmware `01.08.01.00` and `01.08.00.00`.
>
> Instead of showing an error and blocking the whole slicer, OrcaStudio ignores that error and retries sending the print.
>
> If an error appears on the printer - stay calm. Wait 5 seconds. The error should disappear by itself and the print should resume.
>
> If this no longer works, it means that Bambu Lab still does not respect BMCU, does not respect customers who bought their printers, and continues to block BMCU users.

> [!WARNING]
> Bambu Lab is limiting local BMCU interoperability through firmware updates.
>
> About how printer updates remove functions that were available at purchase:
> [BMCU vs firmware locks](https://github.com/jarczakpawel/BMCU-C-PJARCZAK/blob/main/bmcu-vs-firmware-locks.md)

## Important note about `bambu_networking` and AGPL

OrcaStudio does not ship the closed `bambu_networking` component.

The application contains the callback/interface code needed to use that component. This code comes directly from the public AGPL v3 Bambu Studio source code.

In my opinion, the closed `bambu_networking` component is not AGPL-compliant. Bambu Studio is AGPL v3, but its public code downloads, installs, dynamically loads and deeply integrates this closed component through ABI structures, callbacks and runtime control flow.

The Linux version of this component is the least problematic path, because it does not require the same Windows/macOS publisher-signature workflow that ties the module to Bambu Lab-signed binaries. On Windows/macOS, Bambu Studio can validate that the module has the same publisher/certificate as the application, with certificate checking enabled by default unless ignored by configuration.

Bambu Lab should publish the complete corresponding source code for `bambu_networking` under AGPL-compatible terms.

Detailed analysis:
https://github.com/jarczakpawel/OrcaSlicer-bambulab/blob/main/bambu_agpl.md

## Installation

### Windows

Windows requires WSL 2.

Open Command Prompt or PowerShell as Administrator and run:

```bat
dism.exe /online /enable-feature /featurename:Microsoft-Windows-Subsystem-Linux /all /norestart
dism.exe /online /enable-feature /featurename:VirtualMachinePlatform /all /norestart
```

Restart Windows, then launch OrcaStudio.

The application imports its own WSL2 runtime named:

```text
BambuStudio-LinuxRuntime
```

If WSL 2 does not work, make sure virtualization is enabled in BIOS/UEFI. Depending on the motherboard, this option may be named Intel Virtualization Technology, Intel VT-x, AMD-V, or SVM Mode.

### Linux

Nothing special is required.

Install and run normally.

### macOS

Nothing needs to be installed manually.

On first use, the application will ask to install the local Linux runtime.

## Removing the runtime

### Windows

To remove only the WSL runtime used by OrcaStudio:

```bat
wsl --terminate BambuStudio-LinuxRuntime
wsl --unregister BambuStudio-LinuxRuntime
rmdir /s /q "%LOCALAPPDATA%\BambuStudio-LinuxRuntime"
rmdir /s /q "%APPDATA%\BambuStudio_OrcaSlicer\ota\plugins"
```

If you do not use WSL for anything else, you can also disable WSL 2:

```bat
dism.exe /online /disable-feature /featurename:Microsoft-Windows-Subsystem-Linux /norestart
dism.exe /online /disable-feature /featurename:VirtualMachinePlatform /norestart
```

Restart Windows after disabling WSL 2.

### Linux

There is no WSL/Lima runtime to remove.

Remove the application package/AppImage and, if needed, remove its user data:

```bash
rm -rf ~/.config/BambuStudio_OrcaSlicer ~/.cache/BambuStudio_OrcaSlicer ~/.local/share/BambuStudio_OrcaSlicer
```

### macOS

To remove the local Lima runtime:

```bash
LIMACTL="$HOME/Library/Application Support/BambuStudio_OrcaSlicer/slicer-linux-runtime/lima/bin/limactl"
if [ -x "$LIMACTL" ]; then
    "$LIMACTL" stop slicer-linux-runtime 2>/dev/null || true
    "$LIMACTL" delete -f slicer-linux-runtime 2>/dev/null || true
elif command -v limactl >/dev/null 2>&1; then
    limactl stop slicer-linux-runtime 2>/dev/null || true
    limactl delete -f slicer-linux-runtime 2>/dev/null || true
fi
rm -rf "$HOME/Library/Application Support/BambuStudio_OrcaSlicer/slicer-linux-runtime"
```

## What was done

* Updated to OrcaSlicer as of June 14, 2026, up to commit [`9bcee518f859205fbcf3455c4f89fce5c606049c`](https://github.com/SoftFever/OrcaSlicer/commit/9bcee518f859205fbcf3455c4f89fce5c606049c) + added OrcaSlicer commits [`e700113b39f39b837175c680929538aa9655a9f9`](https://github.com/SoftFever/OrcaSlicer/commit/e700113b39f39b837175c680929538aa9655a9f9) and [`5ed8f5ef258898a4006677bab8a3f2e412adedec`](https://github.com/SoftFever/OrcaSlicer/commit/5ed8f5ef258898a4006677bab8a3f2e412adedec).
* Restored BambuNetwork support through the Linux `bambu_networking` path.
* Added Windows WSL2 runtime support.
* Added macOS Lima runtime support.
* Fixed macOS runtime integration, including printer connection, file browsing and camera preview.
* Added experimental Bambu Lab A2L printer support - it has not been tested.
* Fixed the issue that appears when using BMCU with A1 / A1-mini printers on firmware `01.08.01.00` and `01.08.00.00`.
* Fixed the known integration issues from this fork.

## **Special thanks**

The macOS version was made possible thanks to support from Daniel Rakowiecki's service.

I have been a huge fan of this channel for many years and I have learned a lot from Daniel. He often repairs difficult service equipment and many other devices that other people often do not attempt to repair or are unable to repair.

Daniel provided the MacBook that made it possible to work on the macOS version, for which I am extremely grateful.

Here is the video showing the repair of the MacBook that was sent to me:
https://www.youtube.com/watch?v=uC3ySN0Fp0w

This is not an advertisement or a sponsored post. I am writing this freely and voluntarily as a thank you.

## BMCU

I also encourage you to use BMCU.

BMCU firmware is available here:
https://github.com/jarczakpawel/BMCU-C-PJARCZAK

BMCU Flasher is available here:
https://github.com/jarczakpawel/BMCU-Flasher

