#include "Settings.h"

#include <algorithm>

namespace
{
// Raw values in param-index order; filled by each reader, then converted once.
struct RawValues
{
    double v[ATR_NUM_PARAMS] = {};
};

atr::Ease ToEase(double popupValue)
{
    int e = static_cast<int>(popupValue + 0.5) - 1; // popups are 1-based
    if (e < 0 || e >= static_cast<int>(atr::Ease::Count))
        e = 0;
    return static_cast<atr::Ease>(e);
}

void FromRaw(const RawValues& r, Settings& s)
{
    s.ramp.rampIn.duration = r.v[ATR_IN_DURATION];
    s.ramp.rampIn.source = r.v[ATR_IN_SOURCE];
    s.ramp.rampIn.ease = ToEase(r.v[ATR_IN_EASE]);
    s.ramp.rampIn.bezier[0] = r.v[ATR_IN_X1];
    s.ramp.rampIn.bezier[1] = r.v[ATR_IN_Y1];
    s.ramp.rampIn.bezier[2] = r.v[ATR_IN_X2];
    s.ramp.rampIn.bezier[3] = r.v[ATR_IN_Y2];

    s.ramp.linkOut = r.v[ATR_OUT_LINK] != 0.0;
    s.ramp.rampOut.duration = r.v[ATR_OUT_DURATION];
    s.ramp.rampOut.source = r.v[ATR_OUT_SOURCE];
    s.ramp.rampOut.ease = ToEase(r.v[ATR_OUT_EASE]);
    s.ramp.rampOut.bezier[0] = r.v[ATR_OUT_X1];
    s.ramp.rampOut.bezier[1] = r.v[ATR_OUT_Y1];
    s.ramp.rampOut.bezier[2] = r.v[ATR_OUT_X2];
    s.ramp.rampOut.bezier[3] = r.v[ATR_OUT_Y2];

    int anchor = static_cast<int>(r.v[ATR_ANCHOR] + 0.5) - 1;
    if (anchor < 0 || anchor >= static_cast<int>(atr::Anchor::Count))
        anchor = 0;
    s.ramp.anchor = static_cast<atr::Anchor>(anchor);

    s.blur.enabled = r.v[ATR_BLUR_ENABLE] != 0.0;
    s.blur.shutterAngle = r.v[ATR_BLUR_SHUTTER];
    s.blur.maxSamples = std::max(1, std::min(static_cast<int>(r.v[ATR_BLUR_MAX_SAMPLES] + 0.5), atr::kMaxShutterSamples));
    s.marker = r.v[ATR_MARKER] != 0.0;

    s.xfIn.scale = r.v[ATR_IN_XF_SCALE];
    s.xfIn.rotation = r.v[ATR_IN_XF_ROTATION];
    s.xfIn.posX = r.v[ATR_IN_XF_POS_X];
    s.xfIn.posY = r.v[ATR_IN_XF_POS_Y];
    s.xfIn.opacity = r.v[ATR_IN_XF_OPACITY];
    s.xfOut.scale = r.v[ATR_OUT_XF_SCALE];
    s.xfOut.rotation = r.v[ATR_OUT_XF_ROTATION];
    s.xfOut.posX = r.v[ATR_OUT_XF_POS_X];
    s.xfOut.posY = r.v[ATR_OUT_XF_POS_Y];
    s.xfOut.opacity = r.v[ATR_OUT_XF_OPACITY];
    s.direction = static_cast<int>(r.v[ATR_OUT_DIRECTION] + 0.5) == 2 ? atr::OutDirection::Mirror
                                                                       : atr::OutDirection::Continue;
    const int edges = static_cast<int>(r.v[ATR_EDGES] + 0.5);
    s.edges = (edges >= ATR_EDGE_MIRROR && edges <= ATR_EDGE_REPEAT) ? edges : ATR_EDGE_MIRROR;
}

bool IsValueParam(int i)
{
    switch (i)
    {
    case ATR_INPUT:
    case ATR_GRAPH:
    case ATR_IN_TOPIC:
    case ATR_IN_TOPIC_END:
    case ATR_OUT_TOPIC:
    case ATR_OUT_TOPIC_END:
    case ATR_BLUR_TOPIC:
    case ATR_BLUR_TOPIC_END:
    case ATR_IN_XF_TOPIC:
    case ATR_IN_XF_TOPIC_END:
    case ATR_OUT_XF_TOPIC:
    case ATR_OUT_XF_TOPIC_END: return false;
    default: return true;
    }
}

double ValueOf(const PF_ParamDef& p)
{
    switch (p.param_type)
    {
    case PF_Param_FLOAT_SLIDER: return p.u.fs_d.value;
    case PF_Param_SLIDER: return p.u.sd.value;
    case PF_Param_POPUP: return p.u.pd.value;
    case PF_Param_CHECKBOX: return p.u.bd.value ? 1.0 : 0.0;
    default: return 0.0;
    }
}
} // namespace

double ToSeconds(const A_Time& t)
{
    return t.scale ? static_cast<double>(t.value) / static_cast<double>(t.scale) : 0.0;
}

PF_Err ReadSettingsPF(PF_InData* in_data, Settings& s)
{
    PF_Err err = PF_Err_NONE;
    RawValues raw;
    for (int i = 0; i < ATR_NUM_PARAMS && !err; ++i)
    {
        if (!IsValueParam(i))
            continue;
        PF_ParamDef p;
        AEFX_CLR_STRUCT(p);
        // In SmartFX PRE_RENDER, AE checks these back in automatically; elsewhere callers don't use this.
        ERR(PF_CHECKOUT_PARAM(in_data, i, in_data->current_time, in_data->time_step, in_data->time_scale, &p));
        if (!err)
            raw.v[i] = ValueOf(p);
    }
    if (!err)
        FromRaw(raw, s);
    return err;
}

void ReadSettingsFromParams(PF_ParamDef* params[], Settings& s)
{
    RawValues raw;
    for (int i = 0; i < ATR_NUM_PARAMS; ++i)
        if (IsValueParam(i) && params[i])
            raw.v[i] = ValueOf(*params[i]);
    FromRaw(raw, s);
}

A_Err ReadSettingsAEGP(AEGP_PluginID id, AEGP_EffectRefH effectH, const A_Time& layerTime, Settings& s)
{
    // Called from the idle hook / UI thread.
    A_Err err = A_Err_NONE;
    AEFX_SuiteScoper<AEGP_StreamSuite7> streams(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion7);
    RawValues raw;
    for (int i = 0; i < ATR_NUM_PARAMS && !err; ++i)
    {
        if (!IsValueParam(i))
            continue;
        AEGP_StreamRefH streamH = nullptr;
        err = streams->AEGP_GetNewEffectStreamByIndex(id, effectH, i, &streamH);
        if (err || !streamH)
            break;
        AEGP_StreamValue2 val;
        AEFX_CLR_STRUCT(val);
        err = streams->AEGP_GetNewStreamValue(id, streamH, AEGP_LTimeMode_LayerTime, &layerTime, FALSE, &val);
        if (!err)
        {
            raw.v[i] = val.val.one_d;
            streams->AEGP_DisposeStreamValue(&val);
        }
        streams->AEGP_DisposeStream(streamH);
    }
    if (!err)
        FromRaw(raw, s);
    return err;
}

A_Err ReadLayerTimingAEGP(SPBasicSuite* pica, AEGP_LayerH layerH, LayerTiming& timing)
{
    A_Err err = A_Err_NONE;
    AEFX_SuiteScoper<AEGP_LayerSuite9> layerSuite(pica, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    AEFX_SuiteScoper<AEGP_ItemSuite9> itemSuite(pica, kAEGPItemSuite, kAEGPItemSuiteVersion9);

    A_Time inT = {0, 1}, durT = {0, 1};
    err = layerSuite->AEGP_GetLayerInPoint(layerH, AEGP_LTimeMode_LayerTime, &inT);
    if (!err)
        err = layerSuite->AEGP_GetLayerDuration(layerH, AEGP_LTimeMode_LayerTime, &durT);
    if (!err)
    {
        timing.inPoint = ToSeconds(inT);
        timing.outPoint = timing.inPoint + ToSeconds(durT);
    }

    // Source duration (footage / precomp). Layers without a source item (text, shape) have none.
    AEGP_ItemH itemH = nullptr;
    if (!err && layerSuite->AEGP_GetLayerSourceItem(layerH, &itemH) == A_Err_NONE && itemH)
    {
        A_Time itemDur = {0, 1};
        if (itemSuite->AEGP_GetItemDuration(itemH, &itemDur) == A_Err_NONE)
        {
            const double d = ToSeconds(itemDur);
            timing.sourceDur = d > 0.0 ? d : -1.0;
        }
    }
    return err;
}

PF_Err ReadLayerTiming(PF_InData* in_data, PF_OutData* out_data, LayerTiming& timing)
{
    AEFX_SuiteScoper<AEGP_PFInterfaceSuite1> pfInterface(
        in_data, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1, out_data);
    AEGP_LayerH layerH = nullptr;
    PF_Err err = pfInterface->AEGP_GetEffectLayer(in_data->effect_ref, &layerH);
    if (!err && layerH)
        err = ReadLayerTimingAEGP(in_data->pica_basicP, layerH, timing);
    return err;
}

atr::Solved BuildRamp(Settings& s, const LayerTiming& timing, double frameSeconds)
{
    s.ramp.inPoint = timing.inPoint;
    s.ramp.outPoint = timing.outPoint;
    s.ramp.sourceMin = 0.0;
    s.ramp.sourceMax =
        timing.sourceDur > 0.0 ? std::max(0.0, timing.sourceDur - std::max(0.0, frameSeconds)) : kMaxSourceSeconds;
    return atr::Solve(s.ramp);
}
