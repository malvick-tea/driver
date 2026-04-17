@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ==========================================================================
rem  install.cmd
rem
rem  Installs the vhidkm driver stack on a development or test machine.
rem
rem  Sequence, matching the dependency order of the PnP tree:
rem     1. Confirm the script is running elevated. Driver installation
rem        requires SeLoadDriverPrivilege, which is only exposed to
rem        Administrators.
rem     2. Confirm test-signing mode is active. Without it the kernel
rem        will refuse to load the test-signed images and the
rem        resulting failure is silent from devcon's perspective
rem        (devcon reports success; the service later fails to start
rem        with STATUS_INVALID_IMAGE_HASH).
rem     3. Locate the self-signed test-certificate produced by the
rem        build step and inject it into the Local Machine "Root" and
rem        "TrustedPublisher" stores. Both are required: Root makes
rem        Windows trust the chain; TrustedPublisher suppresses the
rem        per-install "Install driver from unknown publisher?" prompt.
rem     4. Install the bus driver first with devcon and the
rem        ROOT\VUSBBUS hardware id. The bus is root-enumerated, so
rem        the hardware id is hard-coded; the INF's [Manufacturer]
rem        section matches the same string.
rem     5. Install the HID minidriver INF into the driver store via
rem        pnputil /add-driver /install. vhidkm does NOT get an explicit
rem        devcon install — it is demand-loaded by PnP the first time
rem        the bus enumerates a child PDO matching USB\VID_1209&PID_BEEF.
rem
rem  Parameters:
rem     /keep-cert     Skip certificate import (certificate is already trusted).
rem     /arch=x64      Override auto-detected architecture (x64 | ARM64).
rem     /dist=<dir>    Use a non-default output directory for binaries.
rem                    Default: %~dp0..\.dist\<arch>
rem     /no-devcon     Use pnputil for every step (devcon is deprecated; the
rem                    shipped fallback is pnputil /add-driver for both INFs
rem                    plus pnputil /enable-device for the bus).
rem
rem  Exit codes mirror the underlying tool: 0 on success, non-zero is the
rem  first failing tool's code. Successful runs log every step to stdout
rem  so CI systems can capture a transcript.
rem ==========================================================================

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
set "REPO_ROOT=%SCRIPT_DIR%\.."
set "CERT_DIR=%SCRIPT_DIR%\test-cert"

set "KEEP_CERT="
set "USE_DEVCON=1"
set "ARCH="
set "DIST="

:parse_args
if "%~1"=="" goto after_args
if /I "%~1"=="/keep-cert" ( set "KEEP_CERT=1" & shift & goto parse_args )
if /I "%~1"=="/no-devcon" ( set "USE_DEVCON=" & shift & goto parse_args )
if /I "%~1"=="/?"         ( goto usage )
if /I "%~1"=="-h"         ( goto usage )
if /I "%~1"=="--help"     ( goto usage )
set "_arg=%~1"
if /I "%_arg:~0,6%"=="/arch=" ( set "ARCH=%_arg:~6%" & shift & goto parse_args )
if /I "%_arg:~0,6%"=="/dist=" ( set "DIST=%_arg:~6%" & shift & goto parse_args )
echo [install] Unknown argument: %~1
goto usage

:after_args

rem --------------------------------------------------------------------------
rem  Step 1 — Elevation check. `net session` succeeds only for Administrators.
rem  fltmc is an equally popular idiom but requires the filter manager
rem  service which is disabled in some minimal WDK build images.
rem --------------------------------------------------------------------------
net session >nul 2>&1
if errorlevel 1 (
    echo [install] ERROR: Administrator privileges required. Re-run from an elevated prompt.
    exit /b 5
)

rem --------------------------------------------------------------------------
rem  Step 2 — Verify test-signing is enabled. bcdedit output is parsed for
rem  the testsigning Yes entry. This check is cosmetic only: the actual
rem  gate is performed by the kernel when the driver image is loaded.
rem --------------------------------------------------------------------------
for /f "tokens=1,2" %%A in ('bcdedit /enum {current} ^| findstr /I /C:"testsigning"') do (
    set "TS_VALUE=%%B"
)
if /I not "%TS_VALUE%"=="Yes" (
    echo [install] ERROR: test-signing is not enabled on this boot entry.
    echo [install]        Run:  bcdedit /set testsigning on
    echo [install]        Then reboot and retry.
    exit /b 6
)

rem --------------------------------------------------------------------------
rem  Step 3 — Architecture detection. PROCESSOR_ARCHITECTURE reports the
rem  actual platform on ARM64 Windows (AMD64 for x64, ARM64 for ARM64).
rem  PROCESSOR_ARCHITEW6432 is the old WOW64 reveal-path which we ignore
rem  because this script must never be invoked from a 32-bit shell.
rem --------------------------------------------------------------------------
if not defined ARCH (
    if /I "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "ARCH=x64"
    if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "ARCH=ARM64"
)
if not defined ARCH (
    echo [install] ERROR: Unable to determine target architecture. Pass /arch=x64 or /arch=ARM64.
    exit /b 2
)

if /I not "%ARCH%"=="x64" if /I not "%ARCH%"=="ARM64" (
    echo [install] ERROR: Unsupported architecture "%ARCH%". Expected x64 or ARM64.
    exit /b 2
)

if not defined DIST set "DIST=%REPO_ROOT%\.dist\%ARCH%"
set "BUS_INF=%DIST%\vusbbus\vusbbus.inf"
set "BUS_SYS=%DIST%\vusbbus\vusbbus.sys"
set "HID_INF=%DIST%\vhidkm\vhidkm.inf"
set "HID_SYS=%DIST%\vhidkm\vhidkm.sys"

echo [install] Architecture     : %ARCH%
echo [install] Distribution dir : %DIST%
echo [install] Bus INF          : %BUS_INF%
echo [install] HID INF          : %HID_INF%

if not exist "%BUS_INF%" (
    echo [install] ERROR: %BUS_INF% not found. Build the solution first.
    exit /b 3
)
if not exist "%BUS_SYS%" (
    echo [install] ERROR: %BUS_SYS% not found. Build the solution first.
    exit /b 3
)
if not exist "%HID_INF%" (
    echo [install] ERROR: %HID_INF% not found. Build the solution first.
    exit /b 3
)
if not exist "%HID_SYS%" (
    echo [install] ERROR: %HID_SYS% not found. Build the solution first.
    exit /b 3
)

rem --------------------------------------------------------------------------
rem  Step 4 — Locate devcon. Preferred source is the WDK redistributable
rem  under %WindowsSdkDir%Tools\<ver>\<arch>\devcon.exe. Fall back to a
rem  copy under install\ if the repository ships one (not committed by
rem  default). If neither is present fall back to pnputil.
rem --------------------------------------------------------------------------
set "DEVCON="
if defined USE_DEVCON (
    if exist "%SCRIPT_DIR%\devcon-%ARCH%.exe" set "DEVCON=%SCRIPT_DIR%\devcon-%ARCH%.exe"
    if not defined DEVCON (
        where devcon >nul 2>&1 && for /f "delims=" %%D in ('where devcon') do if not defined DEVCON set "DEVCON=%%D"
    )
    if not defined DEVCON (
        if defined WindowsSdkDir (
            for /f "delims=" %%D in ('dir /b /s "%WindowsSdkDir%Tools\devcon.exe" 2^>nul') do if not defined DEVCON set "DEVCON=%%D"
        )
    )
)
if not defined DEVCON (
    echo [install] devcon.exe not located — falling back to pnputil.
    set "USE_DEVCON="
) else (
    echo [install] devcon binary    : %DEVCON%
)

rem --------------------------------------------------------------------------
rem  Step 5 — Certificate import. Optional: callers who have already
rem  trusted the certificate (or are using EV-signed binaries) pass
rem  /keep-cert. The cert name is the stable SHA-derived subject name
rem  produced by the build's one-shot `makecert`-equivalent helper.
rem --------------------------------------------------------------------------
if defined KEEP_CERT goto skip_cert

if not exist "%CERT_DIR%\vhidkm-test.cer" (
    echo [install] WARNING: %CERT_DIR%\vhidkm-test.cer not found — skipping certificate import.
    echo [install]          If driver load fails with STATUS_INVALID_IMAGE_HASH, import the
    echo [install]          certificate manually (certmgr -add -c -s -r localMachine Root ^<cert^>).
    goto skip_cert
)

echo [install] Importing test certificate into LocalMachine\Root ...
certutil -f -addstore Root "%CERT_DIR%\vhidkm-test.cer"
if errorlevel 1 (
    echo [install] ERROR: certutil failed to import Root certificate.
    exit /b 7
)

echo [install] Importing test certificate into LocalMachine\TrustedPublisher ...
certutil -f -addstore TrustedPublisher "%CERT_DIR%\vhidkm-test.cer"
if errorlevel 1 (
    echo [install] ERROR: certutil failed to import TrustedPublisher certificate.
    exit /b 7
)

:skip_cert

rem --------------------------------------------------------------------------
rem  Step 6 — Install the bus driver.
rem
rem  devcon install creates a new root-enumerated device with the given
rem  hardware id, which triggers PnP to load vusbbus.sys and run the INF.
rem  Running install twice produces a second device node; we therefore
rem  probe for an existing ROOT\VUSBBUS node and skip creation if found,
rem  using devcon update to refresh the binary in place.
rem --------------------------------------------------------------------------
echo [install] Stage 1/2 - installing vusbbus bus driver ...

set "BUS_EXISTS="
if defined DEVCON (
    "%DEVCON%" findall "ROOT\VUSBBUS" 2>nul | findstr /I /C:"ROOT\VUSBBUS" >nul && set "BUS_EXISTS=1"
) else (
    pnputil /enum-devices /class System 2>nul | findstr /I /C:"ROOT\VUSBBUS" >nul && set "BUS_EXISTS=1"
)

if defined BUS_EXISTS (
    echo [install]   existing ROOT\VUSBBUS node present — updating in place.
    if defined DEVCON (
        "%DEVCON%" update "%BUS_INF%" "ROOT\VUSBBUS"
    ) else (
        pnputil /add-driver "%BUS_INF%" /install
    )
) else (
    if defined DEVCON (
        "%DEVCON%" install "%BUS_INF%" "ROOT\VUSBBUS"
    ) else (
        pnputil /add-driver "%BUS_INF%" /install
    )
)
if errorlevel 1 (
    echo [install] ERROR: bus driver installation failed.
    exit /b 8
)

rem --------------------------------------------------------------------------
rem  Step 7 — Stage the HID minidriver INF in the driver store.
rem
rem  We intentionally do NOT use devcon install for the HID minidriver.
rem  The minidriver matches on USB\VID_1209&PID_BEEF which the bus driver
rem  will enumerate when a caller issues IOCTL_VUSBBUS_PLUG_IN. pnputil
rem  /add-driver /install places the INF into the driver store and
rem  rebuilds the device-INF cache; PnP then picks it up automatically
rem  on the next child enumeration.
rem --------------------------------------------------------------------------
echo [install] Stage 2/2 - staging vhidkm HID minidriver in the driver store ...
pnputil /add-driver "%HID_INF%" /install
if errorlevel 1 (
    echo [install] ERROR: pnputil failed for %HID_INF%.
    exit /b 9
)

echo [install] Success. Stack installed.
echo [install]   Bus service     : vusbbus    (SERVICE_DEMAND_START)
echo [install]   Function service: vhidkm     (SERVICE_DEMAND_START)
echo [install]
echo [install] To create a virtual device, run the SDK demo or call
echo [install]   IOCTL_VUSBBUS_PLUG_IN via the bus control interface.
exit /b 0

:usage
echo Usage: install.cmd [/keep-cert] [/no-devcon] [/arch=x64^|ARM64] [/dist=^<dir^>]
exit /b 1
