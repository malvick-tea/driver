# Debugging vhidkm

Operational notes for diagnosing driver-stack problems: WPP trace
decoding, useful WinDbg commands, and recommended Driver Verifier
configuration.

---

## 1. WPP (ETW) tracing

### 1.1 Control GUIDs

| Driver | GUID | Declared in |
|---|---|---|
| `vusbbus.sys` | `{E5C3A2E4-3A13-4A5A-9E2F-B3C2E91F8A10}` | `bus/trace.h` |
| `vhidkm.sys`  | `{F1D9B812-50B4-4E3E-8D1F-0B9A0C4B3511}` | `hid/trace.h` |

### 1.2 Flags and levels

Both drivers partition their traces by subsystem flag. See the
`WPP_DEFINE_BIT(...)` entries in each `trace.h`. Relevant flags:

| Flag (bus) | Meaning |
|---|---|
| `TRC_FLAG_GENERAL` | Generic info / warning / error. |
| `TRC_FLAG_DRIVER`  | `DriverEntry` / unload / EvtDriverDeviceAdd. |
| `TRC_FLAG_PNP`     | FDO / PDO PnP callbacks. |
| `TRC_FLAG_POWER`   | D-state transitions. |
| `TRC_FLAG_IOCTL`   | Control-device IOCTL dispatch. |
| `TRC_FLAG_PDO`     | Child-PDO creation, `IRP_MN_QUERY_ID` replies. |
| `TRC_FLAG_IPC`     | `QueryInterface` vtable publish / consume. |

| Flag (hid) | Meaning |
|---|---|
| `TRC_FLAG_GENERAL` | Generic info / warning / error. |
| `TRC_FLAG_DRIVER`  | DriverEntry / unload. |
| `TRC_FLAG_PNP`     | Device-add / self-managed-io. |
| `TRC_FLAG_POWER`   | D-state + idle-notification handlers. |
| `TRC_FLAG_HID`     | `IOCTL_HID_*` handling (descriptors, attributes, strings). |
| `TRC_FLAG_QUEUE`   | Pending `READ_REPORT` IRP queue. |
| `TRC_FLAG_CTL`     | Control-device IOCTL dispatch. |
| `TRC_FLAG_LED`     | Keyboard output-report parsing, notifications. |
| `TRC_FLAG_IPC`     | Client side of bus IPC interface. |

Levels follow ETW convention:
`CRITICAL=1 ERROR=2 WARNING=3 INFO=4 VERBOSE=5`.

### 1.3 Starting a trace session

From an elevated command prompt:

```cmd
rem full-verbose on both drivers, logs to a circular in-memory buffer
tracelog -start vhidkm-bus -guid #E5C3A2E4-3A13-4A5A-9E2F-B3C2E91F8A10 -level 5 -matchanykw 0xFFFFFFFF -rt
tracelog -start vhidkm-hid -guid #F1D9B812-50B4-4E3E-8D1F-0B9A0C4B3511 -level 5 -matchanykw 0xFFFFFFFF -rt
```

Replace `-rt` with `-f <file.etl>` to capture to disk. Stop with:

```cmd
tracelog -stop vhidkm-bus
tracelog -stop vhidkm-hid
```

### 1.4 Decoding

`tracepdb` extracts the TMF (trace message format) definitions from
the PDBs, and `tracefmt` renders an ETL against them:

```cmd
tracepdb -f ".dist\x64\vusbbus\vusbbus.pdb"
tracepdb -f ".dist\x64\vhidkm\vhidkm.pdb"
tracefmt -p %TEMP%\TMF -o vhidkm.txt -display vhidkm.etl
```

The resulting `vhidkm.txt` contains one line per trace, formatted as:

```
[cpu] PID.TID::ss.ms.us [flag] [Function: filename.c(line)] text
```

### 1.5 Common trace filters

- Only errors across both drivers:
  `-level 2 -matchanykw 0xFFFFFFFF`
- Only PnP events on the bus:
  `-level 5 -matchanykw 0x04` (bit 2 = `TRC_FLAG_PNP`)
- Only IOCTL dispatch on the function driver:
  `-level 5 -matchanykw 0x40` (bit 6 = `TRC_FLAG_CTL`)

The bit index of each flag is the position in the
`WPP_DEFINE_BIT(...)` list in `trace.h`.

---

## 2. WinDbg playbook

### 2.1 Attach

- Kernel debug over net (recommended): configure the target with
  `bcdedit /debug on` + `bcdedit /dbgsettings net ...` as in
  `docs/INSTALL.md` §1.2. Launch WinDbg with
  `windbg -k net:port=50000,key=<key>`.
- Kernel debug over a VM pipe: `windbg -k com:pipe,port=\\.\pipe\com_1,reconnect`.

### 2.2 Load symbols

```
.symfix+ c:\symbols
.sympath+ .dist\x64\vusbbus;.dist\x64\vhidkm;user\
.reload /f
```

`.reload /f vusbbus.sys` and `.reload /f vhidkm.sys` force-reload
after a driver update.

### 2.3 Quick inventory

```
lm mvusbbus             ! module info (base, size, timestamp)
lm mvhidkm
!devnode 0 1            ! full PnP tree
!devobj \Driver\vusbbus ! device objects owned by bus driver
!devobj \Driver\vhidkm
!wdfdevice              ! KMDF device inventory (extension DLL: wdfkd)
```

### 2.4 KMDF-specific commands

KMDF ships with the `wdfkd.dll` debugger extension. Useful
commands:

| Command | Purpose |
|---|---|
| `!wdfkd.wdfldr` | Lists loaded KMDF client modules and their KMDF version. |
| `!wdfkd.wdfdriverinfo <driver>` | Full driver info: objects, queues, DPCs. |
| `!wdfkd.wdfdevice <handle>` | Walks a `WDFDEVICE` handle and prints the extension chain. |
| `!wdfkd.wdfrequest <handle>` | Walks a `WDFREQUEST` and prints IRP, IOSB, completion status. |
| `!wdfkd.wdfqueue <handle>` | Inspects a queue's pending list. Essential for debugging the pending-read path in `hid/report_queue.c`. |

### 2.5 IRP tracing

```
!irpfind          ! every IRP in the system
!irp <address>    ! decode an IRP
!stacks 2 vhidkm  ! every thread with a stack frame inside vhidkm
!stacks 2 vusbbus
```

### 2.6 WPP ring buffer

Each driver keeps a WPP ring buffer via `WppRecorder.h`. Print it
without a tracelog session:

```
!wdfkd.wdftraces /d .dist\x64\vusbbus\vusbbus.pdb vusbbus.sys
!wdfkd.wdftraces /d .dist\x64\vhidkm\vhidkm.pdb   vhidkm.sys
```

The ring buffer is the cheapest diagnostic source for post-mortem
dumps — enable it in production.

### 2.7 Crash-dump triage

A bugcheck in this stack is usually one of:

| Bugcheck | Likely cause in this project |
|---|---|
| `DRIVER_VERIFIER_DETECTED_VIOLATION (0xC4)` | Verifier caught a KMDF contract violation. `!analyze -v` prints the specific sub-code; check `Parameter 1`. |
| `DRIVER_IRQL_NOT_LESS_OR_EQUAL (0xD1)` | Touching pageable memory at `DISPATCH_LEVEL`. Look for a call from `EvtIoInternalDeviceControl` into a paged routine. |
| `KERNEL_SECURITY_CHECK_FAILURE (0x139)` | GS cookie / struct-corruption. Often a `pshpack1`-related off-by-one on a report buffer. |
| `SYSTEM_SERVICE_EXCEPTION (0x3B)` | Almost always a bad user-mode buffer reaching the driver uncoped. The IOCTL layer should never expose this — check that `METHOD_BUFFERED` is used and the kernel buffer was validated. |

### 2.8 Useful breakpoints during development

```
bu vusbbus!BusEvtIoDeviceControl
bu vusbbus!PdoEvtDevicePrepareHardware
bu vhidkm!HidEvtIoInternalDeviceControl
bu vhidkm!CtlEvtIoDeviceControl
bu vhidkm!ReportQueueDequeue
```

---

## 3. Driver Verifier

### 3.1 Recommended flag set

From an elevated prompt on the test machine:

```cmd
verifier /flags 0x9BB /driver vusbbus.sys vhidkm.sys
shutdown /r /t 0
```

`0x9BB` enables:

| Bit | Flag | Purpose |
|---|---|---|
| `0x001` | Special pool | Detects out-of-bounds and use-after-free on allocations. |
| `0x002` | Force IRQL checking | Pageable memory touched at `DISPATCH_LEVEL` faults immediately. |
| `0x008` | Pool tracking | Leak detection on driver unload. |
| `0x010` | I/O verification | IRP lifecycle, IoMarkIrpPending, status-code parity. |
| `0x080` | Enhanced I/O verification | Adds completion-routine verification and IRQL-at-completion checks. |
| `0x100` | Miscellaneous checks | Miscellaneous PnP sanity (`IoReportDetectedDevice` pairing, etc.). |
| `0x200` | DMA verification | Unused here; kept on so adding DMA in a future revision will be Verifier-clean from day one. |
| `0x800` | Deadlock detection | Lock-ordering inversion. |

Checked-build kernel is not required; Verifier works on free kernels.

### 3.2 KMDF-specific verifier

KMDF has its own internal verifier which is orthogonal to the
kernel-wide one. Enable via the per-driver registry key:

```cmd
reg add HKLM\SYSTEM\CurrentControlSet\Services\vusbbus\Parameters\Wdf /v VerifierOn /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\vhidkm\Parameters\Wdf  /v VerifierOn /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\vusbbus\Parameters\Wdf /v VerboseOn  /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\vhidkm\Parameters\Wdf  /v VerboseOn  /t REG_DWORD /d 1 /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\vusbbus\Parameters\Wdf /v TrackHandles /t REG_MULTI_SZ /d * /f
reg add HKLM\SYSTEM\CurrentControlSet\Services\vhidkm\Parameters\Wdf  /v TrackHandles /t REG_MULTI_SZ /d * /f
```

After a reboot the `wdfkd.dll` extension commands emit stack traces
for every KMDF handle creation — invaluable when chasing
"WDFOBJECT leaked on unload" Verifier breaks.

### 3.3 Turning verifier off

```cmd
verifier /reset
shutdown /r /t 0
```

### 3.4 Expected findings

A clean build on a clean checkout should produce **zero** Verifier
findings over a 30-minute stress session running the demo in a
loop. Any finding is a bug — check `MEMORY.DMP` or break into the
debugger when Verifier triggers.

---

## 4. HID-side sanity checks

### 4.1 Descriptor parse

On the target machine, after plug-in:

```
!hidkd.hidtree                          ! full HID device tree
!hidkd.hidparse  <ppd-address>          ! parse a HID preparsed-data blob
!hidkd.hidreport <handle>               ! decode a captured report
```

`hidkd.dll` is part of the WDK debugger extensions. `ppd-address`
can be found by walking `HIDCLASS!g_HidDevices` or by dumping the
preparsed-data pointer returned by `IOCTL_HID_GET_REPORT_DESCRIPTOR`.

### 4.2 HidClient view

The `hclient.exe` sample (WDK redistributable) enumerates every HID
device and displays descriptors in a GUI. Use it to confirm our
descriptor parses cleanly:

1. Launch `hclient`.
2. Select `Virtual HID Keyboard + Mouse`.
3. `Extended` tab — should show two top-level collections, three
   input reports, one output report, and no parse warnings in the
   bottom pane.

### 4.3 RawInput verification

Drop the attached `rawinput_viewer.c` (from `test/`) on the host to
confirm `WM_INPUT` messages originating from our device carry the
right `RIM_TYPEKEYBOARD` / `RIM_TYPEMOUSE` payloads. Raw Input is
the lowest-level user-mode API path and the closest approximation
to "what Windows actually sees".

---

## 5. Troubleshooting recipes

### 5.1 Device does not appear after `PLUG_IN`

1. Confirm the bus service is running (`sc query vusbbus` → `RUNNING`).
2. Enable `TRC_FLAG_PDO` and `TRC_FLAG_IPC` on the bus driver.
3. Plug in again and look for the sequence:
   - `bus: PdoCreate slot=0 ok`
   - `bus: InvalidateDeviceRelations -> hub`
   - `bus: IRP_MN_QUERY_DEVICE_RELATIONS populated (1 entry)`
4. If step 3 is missing the invalidation, the child PDO context is
   not being added to the slot table — look at the return value of
   `PdoCreate` in the bus trace.

### 5.2 `Code 10` on the HID device

1. Enable `TRC_FLAG_IPC` and `TRC_FLAG_HID` on `vhidkm`.
2. Re-plug. The expected trace sequence is:
   - `hid: BusIfaceQuery success -> <ptr>`
   - `hid: BusIfaceGetConfigDescriptor len=34 ok`
   - `hid: HID_GET_DEVICE_DESCRIPTOR returning 9 bytes`
   - `hid: HID_GET_REPORT_DESCRIPTOR returning 175 bytes`
3. If `BusIfaceQuery` fails the bus driver is not publishing its
   `QueryInterface` vtable; check `bus/pdo_iface.c`.
4. If one of the `HID_GET_*` paths returns a short buffer, check
   the descriptor byte arrays in `bus/usbdesc.c` and
   `hid/hid_descriptor.c` for off-by-one updates.

### 5.3 Keystrokes appear but mouse does not move

1. Confirm the mouse collection is enumerated:
   `devcon find "HID\VID_1209&PID_BEEF&Col02"` — should return one entry.
2. Check WPP `TRC_FLAG_QUEUE` on `vhidkm`: every `IOCTL_VHIDKM_MOUSE_REL`
   should produce a `report_queue: enqueue id=2 size=8` followed by
   a `report_queue: complete pending read id=2` within microseconds.
3. If `complete pending read` never fires, `kbdhid.sys` opened a
   handle but `mouhid.sys` did not. Inspect the HID child PDOs
   with `!hidkd.hidtree`.

### 5.4 LED changes are missed

1. `TRC_FLAG_LED` on `vhidkm` — every `hidclass.sys`
   `IOCTL_HID_WRITE_REPORT` with `ReportId==0x01` should produce
   `led: parse leds=0xNN`.
2. If multiple concurrent waiters exist, verify each is waking.
   Waiters share a KEVENT per device; a missed wake usually means
   `KeSetEvent` is being called under the wrong lock ordering —
   confirm the lock pattern in `hid/led_state.c`.
