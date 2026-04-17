/*
 * hid/trace.h
 *
 * WPP trace definitions for vhidkm.sys. Separate control GUID from
 * the bus driver so traces can be enabled independently.
 *
 *     {F1D9B812-50B4-4E3E-8D1F-0B9A0C4B3511}
 */

#pragma once
#ifndef VHID_HID_TRACE_H_
#define VHID_HID_TRACE_H_

#include <evntrace.h>
#include <WppRecorder.h>

// {F1D9B812-50B4-4E3E-8D1F-0B9A0C4B3511}
#define WPP_CONTROL_GUIDS                                      \
    WPP_DEFINE_CONTROL_GUID(                                   \
        VhidkmTraceGuid,                                       \
        (F1D9B812,50B4,4E3E,8D1F,0B9A0C4B3511),                \
        WPP_DEFINE_BIT(TRC_FLAG_GENERAL)                       \
        WPP_DEFINE_BIT(TRC_FLAG_DRIVER)                        \
        WPP_DEFINE_BIT(TRC_FLAG_PNP)                           \
        WPP_DEFINE_BIT(TRC_FLAG_POWER)                         \
        WPP_DEFINE_BIT(TRC_FLAG_HID)                           \
        WPP_DEFINE_BIT(TRC_FLAG_QUEUE)                         \
        WPP_DEFINE_BIT(TRC_FLAG_CTL)                           \
        WPP_DEFINE_BIT(TRC_FLAG_LED)                           \
        WPP_DEFINE_BIT(TRC_FLAG_IPC)                           \
        )

#define WPP_LEVEL_FLAGS_LOGGER(lvl,flags)   WPP_LEVEL_LOGGER(flags)
#define WPP_LEVEL_FLAGS_ENABLED(lvl, flags) \
    (WPP_LEVEL_ENABLED(flags) && WPP_CONTROL(WPP_BIT_ ## flags).Level >= lvl)

/*
 * begin_wpp config
 * FUNC TraceInfo{LEVEL=TRACE_LEVEL_INFORMATION, FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceWarn{LEVEL=TRACE_LEVEL_WARNING,     FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceError{LEVEL=TRACE_LEVEL_ERROR,      FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceVerbose{LEVEL=TRACE_LEVEL_VERBOSE,  FLAGS=TRC_FLAG_GENERAL}(MSG, ...);
 * FUNC TraceDrv{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_DRIVER}(MSG, ...);
 * FUNC TracePnp{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_PNP}(MSG, ...);
 * FUNC TracePwr{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_POWER}(MSG, ...);
 * FUNC TraceHid{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_HID}(MSG, ...);
 * FUNC TraceQue{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_QUEUE}(MSG, ...);
 * FUNC TraceCtl{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_CTL}(MSG, ...);
 * FUNC TraceLed{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_LED}(MSG, ...);
 * FUNC TraceIpc{LEVEL=TRACE_LEVEL_INFORMATION,  FLAGS=TRC_FLAG_IPC}(MSG, ...);
 * end_wpp
 */

#endif /* VHID_HID_TRACE_H_ */
