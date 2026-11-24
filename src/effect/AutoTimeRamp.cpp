// Auto Time Ramp — SmartFX effect.
//
// Re-times its own layer: for every output frame it computes a source time with atr::Evaluate
// (from the layer's in/out points and the ramp params) and checks out the input layer at that time.
// No Time Remap property, no keyframes.

#include "AutoTimeRamp.h"
#include "../core/RampMath.h"

#include <algorithm>
#include <cmath>
#include <cstring>

static_assert(ATR_OUT_FLAGS == ATR_OUT_FLAGS_HEX, "Update AE_Effect_Global_OutFlags in AutoTimeRampPiPL.r");
static_assert(ATR_OUT_FLAGS2 == ATR_OUT_FLAGS2_HEX, "Update AE_Effect_Global_OutFlags_2 in AutoTimeRampPiPL.r");

namespace
{
constexpr A_long kCheckoutId = 0;

// Layer timing that affects the render but is not an effect param, so it is mixed into AE's cache GUID.
struct LayerTiming
{
    double inPoint = 0.0;     // layer time, seconds
    double outPoint = 0.0;    // layer time, seconds
    double sourceDur = -1.0;  // seconds; < 0 = unknown / unlimited (stills, text, shapes)
};

double ToSeconds(const A_Time& t)
{
    return t.scale ? static_cast<double>(t.value) / static_cast<double>(t.scale) : 0.0;
}

PF_Err ReadLayerTiming(PF_InData* in_data, PF_OutData* out_data, LayerTiming& timing)
{
    PF_Err err = PF_Err_NONE;
    AEFX_SuiteScoper<AEGP_PFInterfaceSuite1> pfInterface(
        in_data, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1, out_data);
    AEFX_SuiteScoper<AEGP_LayerSuite9> layerSuite(in_data, kAEGPLayerSuite, kAEGPLayerSuiteVersion9, out_data);
    AEFX_SuiteScoper<AEGP_ItemSuite9> itemSuite(in_data, kAEGPItemSuite, kAEGPItemSuiteVersion9, out_data);

    AEGP_LayerH layerH = nullptr;
    ERR(pfInterface->AEGP_GetEffectLayer(in_data->effect_ref, &layerH));

    A_Time inT = {0, 1}, durT = {0, 1};
    ERR(layerSuite->AEGP_GetLayerInPoint(layerH, AEGP_LTimeMode_LayerTime, &inT));
    ERR(layerSuite->AEGP_GetLayerDuration(layerH, AEGP_LTimeMode_LayerTime, &durT));
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

PF_Err ReadParams(PF_InData* in_data, atr::RampInput& ramp)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef p;
    const A_long t = in_data->current_time, step = in_data->time_step;
    const A_u_long scale = in_data->time_scale;

    // In SmartFX, AE checks params checked out during PRE_RENDER back in automatically.
    auto fl = [&](int idx, double& dst) {
        AEFX_CLR_STRUCT(p);
        ERR(PF_CHECKOUT_PARAM(in_data, idx, t, step, scale, &p));
        if (!err)
            dst = p.u.fs_d.value;
    };
    auto pop = [&](int idx, int& dst) {
        AEFX_CLR_STRUCT(p);
        ERR(PF_CHECKOUT_PARAM(in_data, idx, t, step, scale, &p));
        if (!err)
            dst = p.u.pd.value - 1; // popups are 1-based
    };

    int easeIn = 0, easeOut = 0, anchor = 0;
    fl(ATR_IN_DURATION, ramp.rampIn.duration);
    fl(ATR_IN_SOURCE, ramp.rampIn.source);
    pop(ATR_IN_EASE, easeIn);
    fl(ATR_OUT_DURATION, ramp.rampOut.duration);
    fl(ATR_OUT_SOURCE, ramp.rampOut.source);
    pop(ATR_OUT_EASE, easeOut);
    pop(ATR_ANCHOR, anchor);

    if (easeIn < 0 || easeIn >= static_cast<int>(atr::Ease::Count))
        easeIn = 0;
    if (easeOut < 0 || easeOut >= static_cast<int>(atr::Ease::Count))
        easeOut = 0;
    if (anchor < 0 || anchor >= static_cast<int>(atr::Anchor::Count))
        anchor = 0;
    ramp.rampIn.ease = static_cast<atr::Ease>(easeIn);
    ramp.rampOut.ease = static_cast<atr::Ease>(easeOut);
    ramp.anchor = static_cast<atr::Anchor>(anchor);
    return err;
}

PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
    PF_SPRINTF(out_data->return_msg, "%s v%d.%d\r%s", ATR_NAME, MAJOR_VERSION, MINOR_VERSION, ATR_DESCRIPTION);
    (void)in_data;
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData*, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);
    out_data->out_flags = ATR_OUT_FLAGS;
    out_data->out_flags2 = ATR_OUT_FLAGS2;
    return PF_Err_NONE;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Ramp in", DISK_IN_TOPIC);
    PF_ADD_FLOAT_SLIDERX("Duration (s)", 0, 60, 0, 5, 1.0, PF_Precision_HUNDREDTHS, 0, 0, DISK_IN_DURATION);
    PF_ADD_FLOAT_SLIDERX("Footage used (s)", 0, 3600, 0, 30, 5.0, PF_Precision_HUNDREDTHS, 0, 0, DISK_IN_SOURCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Ease", ATR_EASE_COUNT, 1, ATR_EASE_CHOICES, DISK_IN_EASE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(DISK_IN_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Ramp out", DISK_OUT_TOPIC);
    PF_ADD_FLOAT_SLIDERX("Duration (s)", 0, 60, 0, 5, 1.0, PF_Precision_HUNDREDTHS, 0, 0, DISK_OUT_DURATION);
    PF_ADD_FLOAT_SLIDERX("Footage used (s)", 0, 3600, 0, 30, 5.0, PF_Precision_HUNDREDTHS, 0, 0, DISK_OUT_SOURCE);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Ease", ATR_EASE_COUNT, 1, ATR_EASE_CHOICES, DISK_OUT_EASE);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(DISK_OUT_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Anchor", ATR_ANCHOR_COUNT, 1, ATR_ANCHOR_CHOICES, DISK_ANCHOR);

    out_data->num_params = ATR_NUM_PARAMS;
    return err;
}

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;

    LayerTiming timing;
    ERR(ReadLayerTiming(in_data, out_data, timing));
    if (!err && extra->cb->GuidMixInPtr)
        ERR(extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(timing), &timing));

    atr::RampInput ramp;
    ERR(ReadParams(in_data, ramp));

    A_long sourceTime = in_data->current_time;
    if (!err)
    {
        ramp.inPoint = timing.inPoint;
        ramp.outPoint = timing.outPoint;
        ramp.sourceMin = 0.0;
        const double frame = in_data->time_scale ? double(in_data->time_step) / double(in_data->time_scale) : 0.0;
        ramp.sourceMax = timing.sourceDur > 0.0 ? std::max(0.0, timing.sourceDur - frame) : 1.0e9;

        const double t = double(in_data->current_time) / double(in_data->time_scale);
        const atr::Sample s = atr::Evaluate(atr::Solve(ramp), t);
        sourceTime = static_cast<A_long>(std::llround(s.source * double(in_data->time_scale)));
    }

    PF_RenderRequest req = extra->input->output_request;
    req.preserve_rgb_of_zero_alpha = TRUE;
    PF_CheckoutResult in_result;
    AEFX_CLR_STRUCT(in_result);
    ERR(extra->cb->checkout_layer(in_data->effect_ref,
                                  ATR_INPUT,
                                  kCheckoutId,
                                  &req,
                                  sourceTime,
                                  in_data->time_step,
                                  in_data->time_scale,
                                  &in_result));

    if (!err)
    {
        extra->output->result_rect = in_result.result_rect;
        extra->output->max_result_rect = in_result.max_result_rect;
    }
    return err;
}

void ClearWorld(PF_EffectWorld* world)
{
    if (!world || !world->data)
        return;
    char* row = reinterpret_cast<char*>(world->data);
    const size_t bytes = static_cast<size_t>(world->rowbytes);
    for (A_long y = 0; y < world->height; ++y, row += world->rowbytes)
        std::memset(row, 0, bytes);
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    PF_EffectWorld *inputP = nullptr, *outputP = nullptr;

    ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, kCheckoutId, &inputP));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outputP));

    if (!err && outputP)
    {
        if (inputP && inputP->data)
        {
            AEFX_SuiteScoper<PF_WorldTransformSuite1> xform(
                in_data, kPFWorldTransformSuite, kPFWorldTransformSuiteVersion1, out_data);
            ERR(xform->copy(in_data->effect_ref, inputP, outputP, nullptr, nullptr));
        }
        else
        {
            ClearWorld(outputP); // no source pixels at that time: transparent
        }
    }

    ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, kCheckoutId));
    return err ? err : err2;
}
} // namespace

extern "C" DllExport PF_Err PluginDataEntryFunction2(PF_PluginDataPtr inPtr,
                                                     PF_PluginDataCB2 inPluginDataCallBackPtr,
                                                     SPBasicSuite* inSPBasicSuitePtr,
                                                     const char* inHostName,
                                                     const char* inHostVersion)
{
    (void)inSPBasicSuitePtr;
    (void)inHostName;
    (void)inHostVersion;
    PF_Err result = PF_Err_INVALID_CALLBACK;
    result = PF_REGISTER_EFFECT_EXT2(inPtr,
                                     inPluginDataCallBackPtr,
                                     ATR_NAME,
                                     ATR_MATCH_NAME,
                                     ATR_CATEGORY,
                                     AE_RESERVED_INFO,
                                     "EffectMain",
                                     "https://github.com/derflowx/AE_AutoTimeRamp");
    return result;
}

PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;
    try
    {
        switch (cmd)
        {
        case PF_Cmd_ABOUT: err = About(in_data, out_data, params, output); break;
        case PF_Cmd_GLOBAL_SETUP: err = GlobalSetup(in_data, out_data, params, output); break;
        case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data, params, output); break;
        case PF_Cmd_SMART_PRE_RENDER: err = PreRender(in_data, out_data, static_cast<PF_PreRenderExtra*>(extra)); break;
        case PF_Cmd_SMART_RENDER: err = SmartRender(in_data, out_data, static_cast<PF_SmartRenderExtra*>(extra)); break;
        default: break;
        }
    }
    catch (PF_Err& thrown)
    {
        err = thrown; // never throw into AE
    }
    catch (...)
    {
        err = PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    return err;
}
