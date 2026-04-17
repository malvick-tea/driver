# vhidkm — Virtual USB HID Keyboard + Mouse

A production-grade, self-contained Windows kernel driver stack that presents itself to the operating system as a single USB-connected composite HID device combining a full keyboard and a full mouse. The solution is built entirely on KMDF — no use of `vhf.sys`, no undocumented kernel APIs.

## Key Features

- Single virtual device exposing **keyboard + mouse** through two top-level HID collections in one report descriptor.
- Full keyboard: 8 modifier keys plus 6-key rollover across the entire HID Keyboard/Keypad usage page, LED output reports (NumLock / CapsLock / ScrollLock / Compose / Kana).
- Full mouse: 5 buttons (left, right, middle, back, forward), 16-bit **relative** X/Y, 16-bit **absolute** X/Y mapped to the virtual screen, signed 8-bit vertical wheel and horizontal AC-Pan wheel.
- Two purpose-built KMDF drivers:
  - `vusbbus.sys` — virtual USB bus driver that creates a USB-compatible PDO.
  - `vhidkm.sys` — custom HID minidriver that binds on top of the PDO and feeds `hidclass.sys`.
- Separate user-mode control surface exposed through a dedicated device interface GUID and a clean, versioned, admin-gated IOCTL protocol.
- C SDK (`vhid.h`, `vhid.c`) and Python `ctypes` wrapper (`user/python/vhid.py`) with runnable demos for every supported operation.
- Driver Verifier clean, WPP-traced, SAL2-annotated, PREfast-clean, signed for test deployment out of the box.

## Platform Support

- Windows 10, version 1809 and newer.
- Windows 11, including 24H2.
- Architectures: **x64** and **ARM64**.

## Build

Prerequisites, step-by-step build commands, and CI integration notes live in [`docs/BUILD.md`](docs/BUILD.md).

Short form:

```cmd
msbuild vhidkm.sln /p:Configuration=Debug;Platform=x64
```

## Install

Full test-sign, enable-test-mode, and `devcon install` walkthrough is in [`docs/INSTALL.md`](docs/INSTALL.md).

Short form — from an **elevated** Developer Command Prompt, after a **one-time** `bcdedit /set testsigning on` followed by a reboot:

```cmd
install\install.cmd
```

## Use

The user-mode API and every IOCTL are documented in [`docs/USAGE.md`](docs/USAGE.md). Runnable examples live in `user/demo.c` and `user/python/demo.py`.

## Debugging

WinDbg commands, WPP trace decoding, and recommended Driver Verifier flags: [`docs/DEBUGGING.md`](docs/DEBUGGING.md).

## Warning — Test-Signing Mode

This project is configured for **test-signing**. To load the drivers Windows must be running in test-signing mode (`bcdedit /set testsigning on` + reboot) and the self-signed test root must be installed. **Do not enable test-signing on production machines.** Test-signed drivers run with the same privilege as production-signed drivers; a machine in test mode will happily load any test-signed driver. Use a disposable VM or a dedicated dev machine.

Production deployment requires an EV code-signing certificate and Microsoft Hardware Developer attestation. The steps are summarized in [`docs/INSTALL.md`](docs/INSTALL.md) but are not automated by this repository.


## Architecture

The full architecture (driver stack diagram, file structure, USB and HID descriptors, IOCTL protocol, PnP state machine, power management plan, security considerations, Verifier strategy) is documented in [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md).

## License

MIT.