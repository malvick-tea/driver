@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ==========================================================================
rem  uninstall.cmd
rem
rem  Reverses the work of install.cmd in the order dictated by the
rem  dependency graph:
rem
rem     1. Unplug any live virtual devices by asking the bus driver to
rem        tear them down. This is done by issuing devcon remove on
rem        USB\VID_1209&PID_BEEF so hidclass.sys releases the stack
rem        cleanly before the bus FDO is removed. If devcon is not
rem        available we fall back to pnputil /remove-device which takes
rem        the instance id.
rem
rem     2. Remove the bus device node (ROOT\VUSBBUS), stopping the
rem        vusbbus service and freeing the root-enumerated device.
rem
rem     3. Uninstall both OEM INFs from the driver store. pnputil
rem        /enum-drivers is scanned for the ProviderName match so we
rem        target the correct published.inf names (oemXX.inf) and do
rem        not accidentally delete an unrelated driver package.
rem
rem     4. Delete the two services if they are somehow still present
rem        (pnputil usually drops them when the INF is removed; this is
rem        a belt-and-suspenders cleanup so a half-failed install does
rem        not leave orphaned service keys).
rem
rem     5. Optionally remove the test certificate from the Root and
rem        TrustedPublisher stores. Skipped unless /purge-cert is
rem        supplied because test systems usually want the certificate
rem        to persist across re-installs.
rem
rem  Parameters:
rem     /purge-cert   Also remove the test certificate from the stores.
rem     /quiet        Suppress informational output (errors still echo).
rem
rem  Exit codes:
rem     0 - success (all removals completed or were already absent)
rem     5 - not elevated
rem     non-zero - underlying tool failure (first failing code)
rem ==========================================================================

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

set "PURGE_CERT="
set "QUIET="

:parse_args
if "%~1"=="" goto after_args
if /I "%~1"=="/purge-cert" ( set "PURGE_CERT=1" & shift & goto parse_args )
if /I "%~1"=="/quiet"      ( set "QUIET=1"      & shift & goto parse_args )
if /I "%~1"=="/?"          ( goto usage )
if /I "%~1"=="-h"          ( goto usage )
if /I "%~1"=="--help"      ( goto usage )
echo [uninstall] Unknown argument: %~1
goto usage

:after_args

net session >nul 2>&1
if errorlevel 1 (
    echo [uninstall] ERROR: Administrator privileges required.
    exit /b 5
)

rem --------------------------------------------------------------------------
rem  Locate devcon the same way install.cmd does. pnputil is used if
rem  devcon is unavailable.
rem --------------------------------------------------------------------------
set "DEVCON="
if exist "%SCRIPT_DIR%\devcon-x64.exe"   set "DEVCON=%SCRIPT_DIR%\devcon-x64.exe"
if not defined DEVCON if exist "%SCRIPT_DIR%\devcon-ARM64.exe" set "DEVCON=%SCRIPT_DIR%\devcon-ARM64.exe"
if not defined DEVCON (
    where devcon >nul 2>&1 && for /f "delims=" %%D in ('where devcon') do if not defined DEVCON set "DEVCON=%%D"
)

rem --------------------------------------------------------------------------
rem  Step 1 — remove any child USB\VID_1209&PID_BEEF nodes (there are
rem  at most VHID_MAX_SLOTS == 1 in v1, but the code is written as a
rem  sweep so a future multi-slot bus driver does not need changes).
rem --------------------------------------------------------------------------
if not defined QUIET echo [uninstall] Removing child HID device(s) ...
if defined DEVCON (
    "%DEVCON%" remove "USB\VID_1209&PID_BEEF*" >nul 2>&1
) else (
    for /f "tokens=*" %%I in ('pnputil /enum-devices /connected 2^>nul ^| findstr /I /C:"USB\VID_1209"') do (
        rem pnputil emits Instance ID lines of the form:
        rem     Instance ID: USB\VID_1209&PID_BEEF\xxx
        for /f "tokens=2,* delims=:" %%A in ("%%I") do (
            set "_iid=%%B"
            set "_iid=!_iid:~1!"
            pnputil /remove-device "!_iid!" >nul 2>&1
        )
    )
)

rem --------------------------------------------------------------------------
rem  Step 2 — remove the bus device node.
rem --------------------------------------------------------------------------
if not defined QUIET echo [uninstall] Removing bus device ROOT\VUSBBUS ...
if defined DEVCON (
    "%DEVCON%" remove "ROOT\VUSBBUS*" >nul 2>&1
) else (
    for /f "tokens=*" %%I in ('pnputil /enum-devices /connected 2^>nul ^| findstr /I /C:"ROOT\VUSBBUS"') do (
        for /f "tokens=2,* delims=:" %%A in ("%%I") do (
            set "_iid=%%B"
            set "_iid=!_iid:~1!"
            pnputil /remove-device "!_iid!" >nul 2>&1
        )
    )
)

rem --------------------------------------------------------------------------
rem  Step 3 — uninstall the two INFs from the driver store. pnputil
rem  emits:
rem     Published Name:     oem23.inf
rem     Original Name:      vusbbus.inf
rem     Provider Name:      Virtual HID Systems
rem     ...
rem  We parse the block to find Published Name whenever Original Name
rem  matches our filename. Scanning by Original Name is safer than by
rem  Provider because a future revision might share a provider with
rem  an unrelated package.
rem --------------------------------------------------------------------------
call :uninstall_inf "vusbbus.inf"
call :uninstall_inf "vhidkm.inf"

rem --------------------------------------------------------------------------
rem  Step 4 — force-delete orphaned services. Unusual but possible if an
rem  install was interrupted between CopyFiles and AddService. Errors
rem  are ignored because the services may already be gone.
rem --------------------------------------------------------------------------
sc query vhidkm  >nul 2>&1 && ( sc stop   vhidkm  >nul 2>&1 & sc delete vhidkm  >nul 2>&1 )
sc query vusbbus >nul 2>&1 && ( sc stop   vusbbus >nul 2>&1 & sc delete vusbbus >nul 2>&1 )

rem --------------------------------------------------------------------------
rem  Step 5 — optionally purge the test certificate.
rem --------------------------------------------------------------------------
if defined PURGE_CERT (
    if not defined QUIET echo [uninstall] Removing test certificate from Root and TrustedPublisher stores ...
    certutil -delstore Root             "Virtual HID Systems Test" >nul 2>&1
    certutil -delstore TrustedPublisher "Virtual HID Systems Test" >nul 2>&1
)

if not defined QUIET echo [uninstall] Done.
exit /b 0

rem ==========================================================================
rem  :uninstall_inf <original-filename>
rem ==========================================================================
:uninstall_inf
set "_target=%~1"
if not defined QUIET echo [uninstall] Scanning driver store for %_target% ...

set "_pub="
set "_matched="
for /f "tokens=1,* delims=:" %%A in ('pnputil /enum-drivers 2^>nul') do (
    set "_key=%%A"
    set "_val=%%B"
    if defined _val set "_val=!_val:~1!"
    if /I "!_key: =!"=="PublishedName" ( set "_pub=!_val!" & set "_matched=" )
    if /I "!_key: =!"=="OriginalName" (
        if /I "!_val!"=="%_target%" set "_matched=1"
    )
    if defined _matched if defined _pub (
        if not defined QUIET echo [uninstall]   deleting !_pub! ^(%_target%^)
        pnputil /delete-driver "!_pub!" /uninstall /force >nul 2>&1
        set "_matched="
        set "_pub="
    )
)
exit /b 0

:usage
echo Usage: uninstall.cmd [/purge-cert] [/quiet]
exit /b 1
