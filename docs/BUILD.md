# Building vhidkm

This document describes the toolchain, prerequisites, and build steps
for the two KMDF drivers (`vusbbus.sys`, `vhidkm.sys`) and the
user-mode SDK.

---

## 1. Prerequisites

### 1.1 Windows host

| Item | Required version |
|---|---|
| Windows host OS | Windows 10 2004+ or Windows 11. A Windows Server 2022 or Server 2025 build host is also supported. |
| Disk space | ~15 GB free (VS + WDK + build outputs). |

The build host does **not** need to be the same machine as the test
target. We routinely cross-build x64 and ARM64 binaries on an x64
development host and deploy them to separate VMs for test.

### 1.2 Visual Studio 2022

Install the **Desktop development with C++** workload, and the
following individual components:

- `MSVC v143 - VS 2022 C++ x64/x86 build tools` (required)
- `MSVC v143 - VS 2022 C++ ARM64 build tools` (required for ARM64 drivers)
- `Windows 11 SDK (10.0.26100.*)` (matches the WDK below)
- `C++ ATL for latest v143 build tools (x86 & x64)` (required by the Spectre-mitigated driver targets)
- `Spectre-mitigated libs for v143 - x64/ARM64` (MSBuild refuses to
  link a KMDF driver without these once `SpectreMitigation` is set in
  the project file)

Professional, Community and BuildTools editions are all supported.
Enterprise is not required.

### 1.3 Windows Driver Kit

Install **WDK 10.0.26100 (Windows 11, version 24H2)**. The WDK
installer is the `wdksetup.exe` bootstrapper from the
[Windows Hardware Developer site](https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk).

After installation verify the extension is present in Visual Studio:

```
Tools -> Options -> Extensions
    "Windows Driver Kit" : Enabled
```

If the option is missing, install `Windows Driver Kit Visual Studio
Extension` from the VS Marketplace (it is included in the WDK
installer but occasionally fails to register when VS is installed
after the WDK).

### 1.4 Test-signing tools (optional, only required for `install.cmd`)

- `signtool.exe` — shipped with the Windows SDK (`C:\Program Files (x86)\Windows Kits\10\bin\<ver>\<arch>\signtool.exe`).
- `makecert.exe` / `pvk2pfx.exe` — shipped with the older Windows SDK
  Enterprise WDK feature. On systems that only have the modern SDK,
  the `New-SelfSignedCertificate` PowerShell cmdlet is an equivalent
  substitute and is preferred going forward because `makecert.exe`
  is deprecated.
- `certutil.exe` — shipped with every supported Windows host.
- `devcon.exe` — `Windows Driver Kit` component. Resides under
  `%WindowsSdkDir%Tools\<ver>\x64\devcon.exe`. `install.cmd` searches
  several candidate locations; the script falls back to `pnputil` when
  `devcon` is not located.

---

## 2. One-time repository setup

Clone the repository and open the solution once from Visual Studio so
that MSBuild materialises the per-user property files:

```cmd
git clone https://example.com/vhidkm.git
cd vhidkm
devenv vhidkm.sln /command "Build.BuildSolution"
exit
```

This first open regenerates `*.vcxproj.user` per-user files. Do not
commit those files — they are in `.gitignore`.

### 2.1 Generating the test certificate

One-time, for test builds only:

```powershell
$cert = New-SelfSignedCertificate `
    -Type CodeSigningCert `
    -Subject "CN=Virtual HID Systems Test" `
    -KeyUsage DigitalSignature `
    -KeyAlgorithm RSA -KeyLength 2048 `
    -HashAlgorithm SHA256 `
    -CertStoreLocation "Cert:\LocalMachine\My" `
    -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3")

$pwd = ConvertTo-SecureString -String "vhidkm-test" -Force -AsPlainText
Export-PfxCertificate -Cert $cert -FilePath install\test-cert\vhidkm-test.pfx -Password $pwd
Export-Certificate   -Cert $cert -FilePath install\test-cert\vhidkm-test.cer
```

The `.pfx` is consumed by MSBuild when `<SignMode>TestSign</SignMode>`
is set and the `TestCertificate` property in `vhidkm.sln` (or in a
`Directory.Build.props` override) points at the file. The `.cer` is
what `install.cmd` imports into the Root and TrustedPublisher stores
on the target machine.

Keep the `.pfx` out of the repository. The `.cer` is safe to share
but is deliberately not committed either — every developer generates
their own to avoid sharing private keys.

---

## 3. Command-line build

From a **Developer Command Prompt for VS 2022** (not a plain cmd —
the developer prompt sets `VCINSTALLDIR`, `WindowsSdkDir`,
`VSCMD_ARG_TGT_ARCH`, and the `PATH` entries MSBuild needs):

### 3.1 Debug x64

```cmd
msbuild vhidkm.sln ^
    /p:Configuration=Debug ^
    /p:Platform=x64 ^
    /m
```

`/m` enables MSBuild parallelism. The build produces:

```
.dist\x64\vusbbus\
    vusbbus.sys
    vusbbus.inf
    vusbbus.cat
    vusbbus.pdb
.dist\x64\vhidkm\
    vhidkm.sys
    vhidkm.inf
    vhidkm.cat
    vhidkm.pdb
.dist\x64\sdk\
    vhid.lib
    vhid.pdb
    vhid_demo.exe
```

### 3.2 Release x64

```cmd
msbuild vhidkm.sln /p:Configuration=Release /p:Platform=x64 /m
```

Release builds enable the full PREfast ruleset, run SDV analysis
(when `/p:RunSdvAnalysis=true` is set, see §5), and strip the
symbol-level information embedded in the .sys image. The .pdb files
are preserved in `.dist` for debugger use.

### 3.3 Debug / Release ARM64

Swap `Platform=x64` for `Platform=ARM64`. Ensure the ARM64
MSVC toolchain and Spectre libraries are installed (see §1.2).

### 3.4 Incremental builds

MSBuild keeps incremental state per `(Configuration, Platform, Project)`
triple. Re-running the same command is safe and fast; changing only
one project triggers a rebuild of that project alone. `/t:Rebuild`
forces a full rebuild when diagnostics in the intermediate
directory are suspected of being stale.

---

## 4. Build outputs and what they mean

| File | Purpose |
|---|---|
| `*.sys` | Kernel driver image. Installed to `%SystemRoot%\System32\drivers`. |
| `*.inf` | Stamped INF. Generated from `*.inx` via `Stampinf`; contains the final `DriverVer` and architecture decorations. |
| `*.cat` | Catalog signed by `inf2cat` + `signtool`. Required for driver install on signed-only systems. |
| `*.pdb` | MSVC debug symbols. Critical for WinDbg — lose the pdb and you lose stack-frame fidelity. Keep every shipping pdb. |
| `*.tmh` | WPP-generated trace message header. Lives under `<project>\Debug\<arch>\tmh\`. Safe to delete; regenerated on next build. |
| `vhid.lib` | User-mode C SDK static library, re-exported from `user/`. |
| `vhid_demo.exe` | Demo executable linked against `vhid.lib`. |

---

## 5. Static analysis

Two analysers run on the driver sources:

- **PREfast** (`/analyze`) runs by default on every build when
  `<RunCodeAnalysis>true</RunCodeAnalysis>` is set. The project files
  declare the rule set `DriverMinimumRules.ruleset` at Debug and
  `DriverRecommendedRules.ruleset` at Release.
- **Static Driver Verifier (SDV)** runs on demand:

```cmd
msbuild vhidkm.sln /p:Configuration=Release /p:Platform=x64 /t:sdv /p:Inputs="/check:default.sdv"
msbuild vhidkm.sln /p:Configuration=Release /p:Platform=x64 /t:sdv /p:Inputs="/clean"
```

Results land in `<project>\sdv\smvcl.log` and `<project>\sdv\sdv-user.lock`.
An SDV pass with no Defect findings is a precondition for a shippable
build. Findings flagged `Internal Error` are usually SDV
infrastructure problems — re-run after `/t:sdv /p:Inputs="/clean"`.

---

## 6. CI integration

A minimal GitHub Actions workflow fragment:

```yaml
jobs:
  build:
    runs-on: windows-2022
    strategy:
      matrix:
        configuration: [Debug, Release]
        platform: [x64, ARM64]
    steps:
      - uses: actions/checkout@v4

      - name: Install WDK
        shell: pwsh
        run: |
          choco install windows-wdk -y --version 10.0.26100

      - uses: microsoft/setup-msbuild@v2

      - name: Build
        run: >
          msbuild vhidkm.sln
          /p:Configuration=${{ matrix.configuration }}
          /p:Platform=${{ matrix.platform }}
          /p:SignMode=Off
          /m

      - name: Upload artefacts
        uses: actions/upload-artifact@v4
        with:
          name: vhidkm-${{ matrix.configuration }}-${{ matrix.platform }}
          path: .dist\${{ matrix.platform }}\
```

`SignMode=Off` tells the WDK MSBuild tasks to skip `signtool` and
`inf2cat` signing passes — the resulting unsigned catalogs are fine
for artefact storage. Production builds re-sign in a gated stage with
an EV code-signing certificate held in a hardware HSM; those
mechanics are outside this document.

---

## 7. Troubleshooting build failures

| Symptom | Likely cause | Fix |
|---|---|---|
| `error MSB3073: the command "inf2cat ..." exited with code 9` | One of the target OSes in the INF `[Manufacturer]` decoration is not in `inf2cat /os:` list. | Update the project's `<InfCatalog>` property or the `/os:` override in `Directory.Build.props`. |
| `error MSB6006: "tracewpp.exe" exited with code 1` | `trace.h` `begin_wpp/end_wpp` block syntactically broken by a recent edit. | Run `tracewpp` manually with `-v` on the offending `.c` to see the real diagnostic. |
| `LNK2019: unresolved external __security_cookie` | Spectre-mitigated libs missing. | Install the matching Spectre libraries for the target architecture (see §1.2). |
| `MSB3073 exited with code 740` on post-build | Post-build step tries to invoke `pnputil` without elevation. | Run MSBuild from an elevated prompt, or disable the post-build step with `/p:InstallAfterBuild=false`. |
| `error 0xE0000241` from `signtool.exe` | `.pfx` password mismatch or the cert has expired. | Regenerate the cert as in §2.1. |

If you hit something else, set `/v:detailed` or `/v:diag` on MSBuild
and inspect the exact task that failed. WDK build failures are
almost always in `PnpxPack`, `Stampinf`, `Inf2Cat`, `SignTool`, or
`TraceWpp`; MSBuild prints the task name next to the `error` line.
