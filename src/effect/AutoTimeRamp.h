#pragma once

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"
#ifdef AE_OS_WIN
    #include <Windows.h>
#endif
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectUI.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "AE_EffectSuites.h"
#include "AE_AdvEffectSuites.h"
#include "AE_GeneralPlug.h"
#include "AEFX_SuiteHandlerTemplate.h"

#define ATR_NAME "Auto Time Ramp"
#define ATR_MATCH_NAME "DFLW AutoTimeRamp"
#define ATR_CATEGORY "Time"
#define ATR_DESCRIPTION "Keyframe-free speed ramps at the layer in/out points."

#define MAJOR_VERSION 0
#define MINOR_VERSION 3
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

// Keep in sync with AutoTimeRampPiPL.r (static_asserts in AutoTimeRamp.cpp check this).
#define ATR_OUT_FLAGS                                                                                              \
    (PF_OutFlag_WIDE_TIME_INPUT | PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_CUSTOM_UI |                            \
     PF_OutFlag_DEEP_COLOR_AWARE | PF_OutFlag_SEND_UPDATE_PARAMS_UI)
#define ATR_OUT_FLAGS2                                                                                             \
    (PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_AUTOMATIC_WIDE_TIME_INPUT |   \
     PF_OutFlag2_I_MIX_GUID_DEPENDENCIES | PF_OutFlag2_SUPPORTS_THREADED_RENDERING)
#define ATR_OUT_FLAGS_HEX 0x06008042
#define ATR_OUT_FLAGS2_HEX 0x08221400

// Parameter order (indices). AE matches saved params by disk ID, so params can be inserted anywhere
// as long as every param keeps its disk ID and new params get new ones.
enum
{
    ATR_INPUT = 0,
    ATR_GRAPH,
    ATR_IN_TOPIC,
    ATR_IN_DURATION,
    ATR_IN_SOURCE,
    ATR_IN_EASE,
    ATR_IN_X1,
    ATR_IN_Y1,
    ATR_IN_X2,
    ATR_IN_Y2,
    ATR_IN_TOPIC_END,
    ATR_OUT_TOPIC,
    ATR_OUT_LINK,
    ATR_OUT_DURATION,
    ATR_OUT_SOURCE,
    ATR_OUT_EASE,
    ATR_OUT_X1,
    ATR_OUT_Y1,
    ATR_OUT_X2,
    ATR_OUT_Y2,
    ATR_OUT_TOPIC_END,
    ATR_ANCHOR,
    ATR_BLUR_TOPIC,
    ATR_BLUR_ENABLE,
    ATR_BLUR_SHUTTER,
    ATR_BLUR_MAX_SAMPLES,
    ATR_BLUR_TOPIC_END,
    ATR_MARKER,
    ATR_NUM_PARAMS
};

// Disk IDs: never change or reuse. (Also the suffix of each param's scripting match name,
// e.g. "DFLW AutoTimeRamp-0002" = ramp in duration.)
enum
{
    DISK_IN_TOPIC = 1,
    DISK_IN_DURATION = 2,
    DISK_IN_SOURCE = 3,
    DISK_IN_EASE = 4,
    DISK_IN_TOPIC_END = 5,
    DISK_OUT_TOPIC = 6,
    DISK_OUT_DURATION = 7,
    DISK_OUT_SOURCE = 8,
    DISK_OUT_EASE = 9,
    DISK_OUT_TOPIC_END = 10,
    DISK_ANCHOR = 11,
    DISK_BLUR_TOPIC = 12,
    DISK_BLUR_ENABLE = 13,
    DISK_BLUR_SHUTTER = 14,
    DISK_BLUR_MAX_SAMPLES = 15,
    DISK_BLUR_TOPIC_END = 16,
    DISK_IN_X1 = 17,
    DISK_IN_Y1 = 18,
    DISK_IN_X2 = 19,
    DISK_IN_Y2 = 20,
    DISK_OUT_X1 = 21,
    DISK_OUT_Y1 = 22,
    DISK_OUT_X2 = 23,
    DISK_OUT_Y2 = 24,
    DISK_OUT_LINK = 25,
    DISK_GRAPH = 26,
    DISK_MARKER = 27,
};

// Popup strings: order must match atr::Ease / atr::Anchor (popup values are 1-based).
#define ATR_EASE_CHOICES "Snap|Soft snap|Hard snap|Whip|Expo snap|Circ snap|Settle|Smooth|Linear|Custom"
#define ATR_EASE_COUNT 10
#define ATR_EASE_CUSTOM 10
#define ATR_ANCHOR_CHOICES "Sync (real-time part in place)|Head (start at in-point)|Tail (end at out-point)"
#define ATR_ANCHOR_COUNT 3

#define ATR_GRAPH_HEIGHT 150
#define ATR_GRAPH_WIDTH 320

extern "C"
{
    DllExport PF_Err EffectMain(
        PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}
