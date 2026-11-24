// Effect settings and layer timing, readable from three places:
//   - PF params during render / UI events (ReadSettingsPF)
//   - a raw PF_ParamDef* array (ReadSettingsFromParams, UI thread)
//   - AEGP effect streams from the idle hook (ReadSettingsAEGP)
#pragma once

#include "AutoTimeRamp.h"
#include "../core/RampMath.h"

struct Settings
{
    atr::RampInput ramp; // in/out points and source range are filled by BuildRamp()
    atr::MotionBlur blur;
    bool marker = true;

    // Ramp transform (scale/rotation/position/opacity at each cut, linked to the ramp ease).
    atr::RampTransform xfIn;
    atr::RampTransform xfOut;         // as entered; use XfOutEffective() for rendering
    atr::OutDirection direction = atr::OutDirection::Continue;
    int edges = ATR_EDGE_MIRROR;

    atr::RampTransform XfOutEffective() const
    {
        return atr::EffectiveOutTransform(xfIn, xfOut, ramp.linkOut, direction);
    }
};

// Layer timing that affects the render but is not an effect param (mixed into the cache GUID).
struct LayerTiming
{
    double inPoint = 0.0;    // layer time, seconds
    double outPoint = 0.0;   // layer time, seconds
    double sourceDur = -1.0; // seconds; < 0 = unknown / unlimited (stills, text, shapes)
};

// Set in GlobalSetup; used by code that runs outside effect commands (idle hook).
extern SPBasicSuite* g_pica;
extern AEGP_PluginID g_pluginId;

constexpr double kMaxSourceSeconds = 3.0e5; // keeps time values inside A_long at fine time scales

double ToSeconds(const A_Time& t);

PF_Err ReadSettingsPF(PF_InData* in_data, Settings& s);         // checks params out at current_time
void ReadSettingsFromParams(PF_ParamDef* params[], Settings& s); // UI-thread param array
A_Err ReadSettingsAEGP(AEGP_PluginID id, AEGP_EffectRefH effectH, const A_Time& layerTime, Settings& s);

A_Err ReadLayerTimingAEGP(SPBasicSuite* pica, AEGP_LayerH layerH, LayerTiming& timing);
PF_Err ReadLayerTiming(PF_InData* in_data, PF_OutData* out_data, LayerTiming& timing);

// Completes s.ramp with timing; frameSeconds is used to keep the last source frame valid.
atr::Solved BuildRamp(Settings& s, const LayerTiming& timing, double frameSeconds);
