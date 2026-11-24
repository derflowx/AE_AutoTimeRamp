#pragma once

#define PF_DEEP_COLOR_AWARE 1

#include "AEConfig.h"
#ifdef AE_OS_WIN
    #include <Windows.h>
#endif
#include "entry.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_Macros.h"
#include "Param_Utils.h"
#include "AE_EffectCBSuites.h"
#include "AE_GeneralPlug.h"
#include "AEFX_SuiteHandlerTemplate.h"

#define ATR_NAME "Auto Time Ramp"
#define ATR_MATCH_NAME "DFLW AutoTimeRamp"
#define ATR_CATEGORY "Time"
#define ATR_DESCRIPTION "Keyframe-free speed ramps at the layer in/out points."

#define MAJOR_VERSION 0
#define MINOR_VERSION 2
#define BUG_VERSION 0
#define STAGE_VERSION PF_Stage_DEVELOP
#define BUILD_VERSION 1

// Keep in sync with AutoTimeRampPiPL.r (static_asserts in AutoTimeRamp.cpp check this).
#define ATR_OUT_FLAGS (PF_OutFlag_WIDE_TIME_INPUT | PF_OutFlag_USE_OUTPUT_EXTENT | PF_OutFlag_DEEP_COLOR_AWARE)
#define ATR_OUT_FLAGS2                                                                                             \
    (PF_OutFlag2_SUPPORTS_SMART_RENDER | PF_OutFlag2_FLOAT_COLOR_AWARE | PF_OutFlag2_AUTOMATIC_WIDE_TIME_INPUT |   \
     PF_OutFlag2_I_MIX_GUID_DEPENDENCIES | PF_OutFlag2_SUPPORTS_THREADED_RENDERING)
#define ATR_OUT_FLAGS_HEX 0x02000042
#define ATR_OUT_FLAGS2_HEX 0x08221400

// Parameter order. Never reorder; append new params at the end and give them new disk IDs.
enum
{
    ATR_INPUT = 0,
    ATR_IN_TOPIC,
    ATR_IN_DURATION,
    ATR_IN_SOURCE,
    ATR_IN_EASE,
    ATR_IN_TOPIC_END,
    ATR_OUT_TOPIC,
    ATR_OUT_DURATION,
    ATR_OUT_SOURCE,
    ATR_OUT_EASE,
    ATR_OUT_TOPIC_END,
    ATR_ANCHOR,
    ATR_BLUR_TOPIC,
    ATR_BLUR_ENABLE,
    ATR_BLUR_SHUTTER,
    ATR_BLUR_MAX_SAMPLES,
    ATR_BLUR_TOPIC_END,
    ATR_NUM_PARAMS
};

enum
{
    DISK_IN_TOPIC = 1,
    DISK_IN_DURATION,
    DISK_IN_SOURCE,
    DISK_IN_EASE,
    DISK_IN_TOPIC_END,
    DISK_OUT_TOPIC,
    DISK_OUT_DURATION,
    DISK_OUT_SOURCE,
    DISK_OUT_EASE,
    DISK_OUT_TOPIC_END,
    DISK_ANCHOR,
    DISK_BLUR_TOPIC,
    DISK_BLUR_ENABLE,
    DISK_BLUR_SHUTTER,
    DISK_BLUR_MAX_SAMPLES,
    DISK_BLUR_TOPIC_END,
};

// Popup strings: order must match atr::Ease / atr::Anchor (popup values are 1-based).
#define ATR_EASE_CHOICES "Snap|Soft snap|Hard snap|Whip|Expo snap|Circ snap|Settle|Smooth|Linear"
#define ATR_EASE_COUNT 9
#define ATR_ANCHOR_CHOICES "Sync (real-time part in place)|Head (start at in-point)|Tail (end at out-point)"
#define ATR_ANCHOR_COUNT 3

extern "C"
{
    DllExport PF_Err EffectMain(
        PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra);
}
