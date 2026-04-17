/*
 * bus/trace.h
 *
 * WPP (Windows Software Trace Preprocessor) configuration for
 * vusbbus.sys. WPP generates tracing code at build time from the
 * macro invocations in this file; the runtime support lives in
 * wmlib.lib / tracedrivers.
 *
 * Each driver declares its own control GUID so traces can be
 * decoded independently with `tracelog -start MyTrace -guid
 * {...}`. The bus driver's control GUID is:
 *
 *     {E5C3A2E4-3A13-4A5A-9E2F-B3C2E91F8A10}
 *
 * Trace flags partition the output by subsystem so a developer can
 * enable only the noise relevant to the area they are debugging.
 * Trace levels use the standard WPP level macros: FATAL / ERROR /
 * WARNING / INFORMATION / VERBOSE.
 *
 * Build rule: the .vcxproj must declare
 *     <WppEnabled>true</WppEnabled>
 *     <WppScanConfigurationData>trace.h</WppScanConfigurationData>
 * so tracewpp scans this file for WPP_DEFINE_* directives and emits
 * vusbbus.tmh into the intermediate directory. Each .c source that
 * calls a trace macro includes "trace.h" followed by
 * "vusbbus.tmh" (the generated header, included via #include "*.tmh"
 * pattern used across the project).
 */

#pragma once
#ifndef VHID_BUS_TRACE_H_
#define VHID_BUS_TRACE_H_

#include <evntrace.h>
#include <WppRecorder.h>

/*
 * Control GUID for vusbbus.sys.
 *     {E5C3A2E4-3A13-4A5A-9E2F-B3C2E91F8A10}
 *
 * Registered with the ETW trace controller on driver load. The
 * GUID is also quoted verbatim in docs/DEBUGGING.md so testers can
 * start traces without looking at source.
 */

// {E5C3A2E4-3A13-4A5A-9E2F-B3C2E91F8A10}
#define WPP_CONTROL_GUIDS                                      \
    WPP_DEFINE_CONTROL_GUID(                                   \
        VusbBusTraceGuid,                                      \
        (E5C3A2E4,3A13,4A5A,9E2F,B3C2E91F8A10),                \
        WPP_DEFINE_BIT(TRC_FLAG_GENERAL)                       \
        WPP_DEFINE_BIT(TRC_FLAG_DRIVER)                        \
        WPP_DEFINE_BIT(TRC_FLAG_PNP)                           \
        WPP_DEFINE_BIT(TRC_FLAG_POWER)                         \
        WPP_DEFINE_BIT(TRC_FLAG_IOCTL)                         \
        WPP_DEFINE_BIT(TRC_FLAG_PDO)                           \
        WPP_DEFINE_BIT(TRC_FLAG_IPC)                           \
        )

#define WPP_LEVEL_FLAGS_LOGGER(lvl,flags)   WPP_LEVEL_LOGGER(flags)
#define WPP_LEVEL_FLAGS_ENABLED(lvl, flags) \
    (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_ ## flags).Level >= lvl)

/*
 * Ergonomic wrappers. WPP parses the begin_wpp / end_wpp comments
 * below and synthesizes TraceInfo/TraceError/... with the right
 * level+flag combination.
 *
 * begin_wpp config
 * FUNC TraceInfo{LEVEL=TRACE_LEVEL_INFORMATION, FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceWarn{LEVEL=TRACE_LEVEL_WARNING,     FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceError{LEVEL=TRACE_LEVEL_ERROR,      FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceVerbose{LEVEL=TRACE_LEVEL_VERBOSE,  FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceEntry{LEVEL=TRACE_LEVEL_VERBOSE,    FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceExit{LEVEL=TRACE_LEVEL_VERBOSE,     FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceDrv{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_DRIVER}(MSG, ...);
 * FUNC TracePnp{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_PNP}(MSG, ...);
 * FUNC TracePwr{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_POWER}(MSG, ...);
 * FUNC TraceIoctl{LEVEL=TRACE_LEVEL_INFORMATION,FLAGS=TRC_FLAG_IOCTL}(MSG, ...);
 * FUNC TracePdo{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_PDO}(MSG, ...);
 * FUNC TraceIpc{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_IPC}(MSG, ...);
 * end_wpp
 */

#endif /* VHID_BUS_TRACE_H_ */
