# Installing vhidkm

This document walks through preparing a target Windows machine,
deploying the build outputs, and verifying that the two drivers are
running.

---

## 1. Prepare the target machine

### 1.1 Enable test-signing

The drivers shipped by this repository are **test-signed**. A Windows
kernel running in production mode will refuse to load a test-signed
image with `STATUS_INVALID_IMAGE_HASH`.

From an **elevated** command prompt on the target machine:

```cmd
bcdedit /set testsigning on
```

Reboot. After reboot the desktop shows `Test Mode` in the bottom-right
corner — that is the signal the gate is open.

To revert once testing is done:

```cmd
bcdedit /set testsigning off
shutdown /r /t 0
```

Leaving a production machine in test mode is a policy violation in
most managed environments — use a VM or a dedicated dev box.

### 1.2 Enable kernel-debug (optional but recommended for development)

```cmd
bcdedit /debug on
bcdedit /dbgsettings net hostip:<host-ip> port:50000 key:<4-word-key>
```

A kernel debugger attached during install lets you see `DbgPrintEx` /
WPP output at driver-load time and surfaces the real error when an
INF stage fails silently.

### 1.3 Disable Hyper-V-enforced Code Integrity (HVCI / Memory Integrity)

On Windows 11 with HVCI enabled the kernel enforces a strict
sub-set of PE layout rules (no self-modifying code sections, no
`.didat` equivalents, etc.) even in test-signing mode. This repository's
drivers comply, but if you hit `STATUS_INVALID_IMAGE_HASH` on
Windows 11 despite test-signing being enabled, open:

```
Settings -> Privacy & Security -> Windows Security -> Device Security
    -> Core Isolation -> Memory Integrity -> Off
```

and reboot. Re-enable when finished.

---

## 2. Stage the binaries

Build the solution as described in `BUILD.md`. The distribution
directory `.dist\<arch>\` contains everything `install.cmd` needs:

```
.dist\x64\
    vusbbus\
        vusbbus.sys
        vusbbus.inf
        vusbbus.cat
        vusbbus.pdb
    vhidkm\
        vhidkm.sys
        vhidkm.inf
        vhidkm.cat
        vhidkm.pdb
```

Copy the whole `.dist\x64` tree and the `install\` folder to the
target machine. Directory layout must be preserved — the install
script computes paths relative to its own location.

---

## 3. Install

Open an **elevated** command prompt on the target. From the
`install\` folder:

```cmd
install.cmd
```

The script:

1. Checks that it is elevated and test-signing is on.
2. Detects architecture (`x64` or `ARM64`) from `PROCESSOR_ARCHITECTURE`.
3. Imports the test certificate (`install\test-cert\vhidkm-test.cer`)
   into `LocalMachine\Root` and `LocalMachine\TrustedPublisher`.
4. Installs the bus driver by root-enumerating a device with hardware
   id `ROOT\VUSBBUS`. That step causes PnP to load `vusbbus.sys`, run
   `vusbbus.inf`, register the service, and create the bus FDO.
5. Stages `vhidkm.inf` in the driver store via
   `pnputil /add-driver /install`. PnP will demand-load `vhidkm.sys`
   the first time the bus enumerates a child PDO matching
   `USB\VID_1209&PID_BEEF`.

Command-line flags:

| Flag | Meaning |
|---|---|
| `/keep-cert` | Skip certificate import (certificate already trusted). |
| `/arch=x64` or `/arch=ARM64` | Override auto-detected architecture. |
| `/dist=<dir>` | Use a non-default distribution directory. Defaults to `..\.dist\<arch>` relative to `install.cmd`. |
| `/no-devcon` | Use `pnputil` for every step instead of `devcon`. Useful on stripped-down test images that do not ship `devcon`. |

---

## 4. Verify installation

### 4.1 Services

```cmd
sc query vusbbus
sc query vhidkm
```

Both should report `STATE = 4 RUNNING` once a device is plugged (see
§5) and `STOPPED` before first use. The bus service starts on demand
when the root device is created by the PnP installer.

### 4.2 PnP tree

```cmd
devcon find "ROOT\VUSBBUS"
devcon find "USB\VID_1209*"
```

Before plugging a virtual device, only the `ROOT\VUSBBUS` node is
present. After a successful `IOCTL_VUSBBUS_PLUG_IN` call a
`USB\VID_1209&PID_BEEF\<instance>` node appears, with `hidclass.sys`
as the upper filter and `kbdhid`+`kbdclass` / `mouhid`+`mouclass` as
the two child function-device stacks.

### 4.3 Device-interface GUIDs

PowerShell:

```powershell
Get-PnpDevice -Class System        | Where-Object { $_.FriendlyName -like "*Virtual USB*" }
Get-PnpDevice -Class HIDClass       | Where-Object { $_.FriendlyName -like "*Virtual HID*" }
```

Or raw:

```cmd
wmic path Win32_PnPEntity where "HardwareID like '%VID_1209&PID_BEEF%'" get Name,Status,DeviceID
```

### 4.4 Functional test

Run the demo:

```cmd
user\vhid_demo.exe
```

The demo plugs a device, sends a sequence of keyboard and mouse
reports, and unplugs. Watch `notepad.exe` or any focused Win32
window — the keystrokes should appear and the cursor should move.

---

## 5. Uninstall

```cmd
uninstall.cmd
```

The script:

1. Removes any live `USB\VID_1209&PID_BEEF*` child devices.
2. Removes the `ROOT\VUSBBUS*` bus device.
3. Enumerates the driver store via `pnputil /enum-drivers`, finds
   the OEM-published names of `vusbbus.inf` and `vhidkm.inf`, and
   deletes both packages with `/uninstall /force`.
4. Force-deletes the `vhidkm` and `vusbbus` services if `pnputil`
   left anything behind.

Optional flags:

| Flag | Meaning |
|---|---|
| `/purge-cert` | Also remove the test certificate from the stores. Default behaviour is to leave the cert in place so subsequent re-installs do not re-prompt. |
| `/quiet` | Suppress informational output. Errors still echo. |

A reboot is not required. Re-running `uninstall.cmd` on an already-uninstalled
machine is a no-op.

---

## 6. Production deployment (attestation-signed path)

The test-signing workflow documented above is for development only.
Production deployment requires:

1. An **EV Code Signing certificate** issued by a CA on Microsoft's
   approved list.
2. A **Microsoft Hardware Developer Program** account.
3. HLK submission of the driver package with the HID Logo test suite
   under `test\hlk-tests.md`.
4. Microsoft's attestation signing applied to the submitted package.
5. Deployment of the attestation-signed `.cab` via the customer's
   normal driver deployment channel (WSUS, Intune, WUfB, etc.).

None of that workflow is automated by this repository. The INF and
`.vcxproj` files are configured so the same source tree produces an
attestation-ready submission when `<SignMode>OfflineSign</SignMode>`
is set and an EV certificate is pointed at by
`<TestCertificate>`/`<PackageCertificateKeyFile>`. The HLK mapping
notes in `test\hlk-tests.md` list the specific tests the device must
pass for the two collections it exposes.

---

## 7. Common failure modes

| Symptom | Cause | Fix |
|---|---|---|
| `install.cmd` exits with code 6 (`test-signing is not enabled`). | `bcdedit /set testsigning on` was never run, or BitLocker suspended the change. | Run `bcdedit /enum {current}` to confirm `testsigning Yes`; if absent, re-run the `bcdedit` command and reboot. |
| `install.cmd` exits with code 7 (`certutil failed to import ...`). | Caller is non-admin, or the certificate file is corrupted. | Re-run from an elevated prompt. Regenerate the cert following `BUILD.md` §2.1. |
| `devcon install` reports success, but the service fails to start with error 577 (`STATUS_INVALID_IMAGE_HASH`). | Signature is valid but HVCI/VBS is enforcing signer restrictions. | Disable Memory Integrity as in §1.3, or obtain a kernel-EV cert. |
| `devcon install` reports success; no PnP device appears. | The matching INF section does not cover the current Windows build. | Verify `[Vhidkm.NT$ARCH$.10.0...17763]` in `vhidkm.inx` still matches; raise the minimum when Windows drops support for older builds. |
| `Code 10 (Device cannot start)` on the virtual HID device. | The HID minidriver FDO is loading against the PDO, but `QUERY_INTERFACE` to fetch the bus-IPC vtable is failing. | Enable WPP trace flag `TRC_FLAG_IPC` on both drivers (`tracelog` commands in `DEBUGGING.md`) and re-plug. |
| `IOCTL_VUSBBUS_PLUG_IN` returns `STATUS_DEVICE_ALREADY_ATTACHED`. | Previous slot was not cleanly unplugged. | Call `IOCTL_VUSBBUS_UNPLUG` with the known `SlotId`, or close and re-open the bus control handle (the driver cleans up on final handle release). |
