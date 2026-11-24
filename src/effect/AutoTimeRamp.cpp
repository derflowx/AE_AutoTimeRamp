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
#include <vector>

static_assert(ATR_OUT_FLAGS == ATR_OUT_FLAGS_HEX, "Update AE_Effect_Global_OutFlags in AutoTimeRampPiPL.r");
static_assert(ATR_OUT_FLAGS2 == ATR_OUT_FLAGS2_HEX, "Update AE_Effect_Global_OutFlags_2 in AutoTimeRampPiPL.r");

namespace
{
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

// Passed from PreRender to SmartRender: where each shutter sample's buffer sits in layer space.
struct RenderPlan
{
    int samples = 1;
    PF_LRect outRect = {0, 0, 0, 0};
    PF_LRect sampleRect[atr::kMaxShutterSamples];
};

void DeletePlan(void* p)
{
    delete static_cast<RenderPlan*>(p);
}

bool IsEmpty(const PF_LRect& r)
{
    return r.left >= r.right || r.top >= r.bottom;
}

// dst = union(dst, src); empty rects are ignored.
void UnionLRect(const PF_LRect* src, PF_LRect* dst)
{
    if (IsEmpty(*src))
        return;
    if (IsEmpty(*dst))
    {
        *dst = *src;
        return;
    }
    dst->left = std::min(dst->left, src->left);
    dst->top = std::min(dst->top, src->top);
    dst->right = std::max(dst->right, src->right);
    dst->bottom = std::max(dst->bottom, src->bottom);
}

PF_Err ReadParams(PF_InData* in_data, atr::RampInput& ramp, atr::MotionBlur& blur)
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

    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, ATR_BLUR_ENABLE, t, step, scale, &p));
    if (!err)
        blur.enabled = p.u.bd.value != 0;
    fl(ATR_BLUR_SHUTTER, blur.shutterAngle);
    AEFX_CLR_STRUCT(p);
    ERR(PF_CHECKOUT_PARAM(in_data, ATR_BLUR_MAX_SAMPLES, t, step, scale, &p));
    if (!err)
        blur.maxSamples = std::max(1, std::min<int>(p.u.sd.value, atr::kMaxShutterSamples));

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

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Motion blur", DISK_BLUR_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable", TRUE, 0, DISK_BLUR_ENABLE);
    PF_ADD_FLOAT_SLIDERX("Shutter angle", 0, 720, 0, 360, 180, PF_Precision_INTEGER, 0, 0, DISK_BLUR_SHUTTER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Max samples", 1, atr::kMaxShutterSamples, 1, 32, 16, DISK_BLUR_MAX_SAMPLES);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(DISK_BLUR_TOPIC_END);

    out_data->num_params = ATR_NUM_PARAMS;
    return err;
}

// Requested source times are passed to AE on a finer grid than the comp's, so shutter samples can
// land between frames (precomps render sub-frame content; footage picks the nearest frame).
constexpr double kMinTimeScale = 6000.0;
constexpr double kMaxSourceSeconds = 3.0e5; // keeps time values inside A_long at the fine scale

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;

    LayerTiming timing;
    ERR(ReadLayerTiming(in_data, out_data, timing));
    if (!err && extra->cb->GuidMixInPtr)
        ERR(extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(timing), &timing));

    atr::RampInput ramp;
    atr::MotionBlur blur;
    ERR(ReadParams(in_data, ramp, blur));
    if (err)
        return err;

    const double scale = double(in_data->time_scale);
    const double frame = scale > 0.0 ? double(in_data->time_step) / scale : 0.0;
    ramp.inPoint = timing.inPoint;
    ramp.outPoint = timing.outPoint;
    ramp.sourceMin = 0.0;
    ramp.sourceMax = timing.sourceDur > 0.0 ? std::max(0.0, timing.sourceDur - frame) : kMaxSourceSeconds;

    const double t = double(in_data->current_time) / scale;
    double sourceTimes[atr::kMaxShutterSamples];
    const int n = atr::ShutterSamples(atr::Solve(ramp), t, frame, blur, sourceTimes);

    const A_long k = std::max<A_long>(1, static_cast<A_long>(std::ceil(kMinTimeScale / scale)));
    const A_u_long fineScale = in_data->time_scale * static_cast<A_u_long>(k);
    const A_long fineStep = in_data->time_step * k;

    RenderPlan* plan = new RenderPlan();
    plan->samples = n;

    PF_RenderRequest req = extra->input->output_request;
    req.preserve_rgb_of_zero_alpha = TRUE;
    bool first = true;
    for (int i = 0; i < n && !err; ++i)
    {
        const double src = std::min(std::max(sourceTimes[i], 0.0), kMaxSourceSeconds);
        const A_long when = static_cast<A_long>(std::llround(src * double(fineScale)));
        PF_CheckoutResult res;
        AEFX_CLR_STRUCT(res);
        ERR(extra->cb->checkout_layer(in_data->effect_ref, ATR_INPUT, i, &req, when, fineStep, fineScale, &res));
        if (err)
            break;
        plan->sampleRect[i] = res.result_rect;
        if (first)
        {
            extra->output->result_rect = res.result_rect;
            extra->output->max_result_rect = res.max_result_rect;
            first = false;
        }
        else
        {
            UnionLRect(&res.result_rect, &extra->output->result_rect);
            UnionLRect(&res.max_result_rect, &extra->output->max_result_rect);
        }
    }

    if (err)
    {
        delete plan;
        return err;
    }
    plan->outRect = extra->output->result_rect;
    extra->output->pre_render_data = plan;
    extra->output->delete_pre_render_data_func = DeletePlan;
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

// Pixel traits: channel type, white value, and rounding back from the float accumulator.
template <typename PixelT> struct Traits;
template <> struct Traits<PF_Pixel8>
{
    static float Store(float v) { return std::min(255.0f, std::max(0.0f, v + 0.5f)); }
};
template <> struct Traits<PF_Pixel16>
{
    static float Store(float v) { return std::min(32768.0f, std::max(0.0f, v + 0.5f)); }
};
template <> struct Traits<PF_PixelFloat>
{
    static float Store(float v) { return v; }
};

// Box-filter average of the shutter samples. AE buffers are premultiplied, so a plain average of
// all four channels is correct. Each sample buffer may cover a different rect in layer space.
template <typename PixelT>
void Accumulate(const RenderPlan& plan, PF_EffectWorld* const* inputs, PF_EffectWorld* out)
{
    using ChanT = decltype(PixelT::red);
    const A_long w = out->width, h = out->height;
    const float inv = 1.0f / float(plan.samples);
    std::vector<float> acc(static_cast<size_t>(w) * 4);

    for (A_long y = 0; y < h; ++y)
    {
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int i = 0; i < plan.samples; ++i)
        {
            const PF_EffectWorld* in = inputs[i];
            if (!in || !in->data)
                continue;
            const A_long dx = plan.outRect.left - plan.sampleRect[i].left;
            const A_long sy = y + plan.outRect.top - plan.sampleRect[i].top;
            if (sy < 0 || sy >= in->height)
                continue;
            const PixelT* row =
                reinterpret_cast<const PixelT*>(reinterpret_cast<const char*>(in->data) + sy * in->rowbytes);
            const A_long x0 = std::max<A_long>(0, -dx), x1 = std::min<A_long>(w, in->width - dx);
            for (A_long x = x0; x < x1; ++x)
            {
                const PixelT& p = row[x + dx];
                float* a = &acc[static_cast<size_t>(x) * 4];
                a[0] += float(p.alpha);
                a[1] += float(p.red);
                a[2] += float(p.green);
                a[3] += float(p.blue);
            }
        }
        PixelT* orow = reinterpret_cast<PixelT*>(reinterpret_cast<char*>(out->data) + y * out->rowbytes);
        for (A_long x = 0; x < w; ++x)
        {
            const float* a = &acc[static_cast<size_t>(x) * 4];
            orow[x].alpha = static_cast<ChanT>(Traits<PixelT>::Store(a[0] * inv));
            orow[x].red = static_cast<ChanT>(Traits<PixelT>::Store(a[1] * inv));
            orow[x].green = static_cast<ChanT>(Traits<PixelT>::Store(a[2] * inv));
            orow[x].blue = static_cast<ChanT>(Traits<PixelT>::Store(a[3] * inv));
        }
    }
}

PF_Err SmartRender(PF_InData* in_data, PF_OutData* out_data, PF_SmartRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    const RenderPlan* plan = static_cast<const RenderPlan*>(extra->input->pre_render_data);
    const int n = plan ? plan->samples : 1;

    PF_EffectWorld* inputs[atr::kMaxShutterSamples] = {};
    PF_EffectWorld* outputP = nullptr;
    for (int i = 0; i < n && !err; ++i)
        ERR(extra->cb->checkout_layer_pixels(in_data->effect_ref, i, &inputs[i]));
    ERR(extra->cb->checkout_output(in_data->effect_ref, &outputP));

    if (!err && outputP && outputP->data)
    {
        if (n == 1)
        {
            if (inputs[0] && inputs[0]->data)
            {
                AEFX_SuiteScoper<PF_WorldTransformSuite1> xform(
                    in_data, kPFWorldTransformSuite, kPFWorldTransformSuiteVersion1, out_data);
                ERR(xform->copy(in_data->effect_ref, inputs[0], outputP, nullptr, nullptr));
            }
            else
            {
                ClearWorld(outputP); // no source pixels at that time: transparent
            }
        }
        else
        {
            AEFX_SuiteScoper<PF_WorldSuite2> worldSuite(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
            PF_PixelFormat format = PF_PixelFormat_INVALID;
            ERR(worldSuite->PF_GetPixelFormat(outputP, &format));
            if (!err)
            {
                switch (format)
                {
                case PF_PixelFormat_ARGB128: Accumulate<PF_PixelFloat>(*plan, inputs, outputP); break;
                case PF_PixelFormat_ARGB64: Accumulate<PF_Pixel16>(*plan, inputs, outputP); break;
                case PF_PixelFormat_ARGB32: Accumulate<PF_Pixel8>(*plan, inputs, outputP); break;
                default: err = PF_Err_BAD_CALLBACK_PARAM; break;
                }
            }
        }
    }

    for (int i = 0; i < n; ++i)
        ERR2(extra->cb->checkin_layer_pixels(in_data->effect_ref, i));
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
