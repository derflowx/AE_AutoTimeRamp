// Auto Time Ramp â€” SmartFX effect.
//
// Re-times its own layer: for every output frame it computes a source time with atr::Evaluate
// (from the layer's in/out points and the ramp params) and checks out the input layer at that time.
// No Time Remap property, no keyframes.
//
// Files: Settings (param/timing readers), Graph (Effect Controls speed graph),
//        Marker (real-time layer marker + idle hook), this file (params, UI updates, render).

#include "AutoTimeRamp.h"
#include "Graph.h"
#include "Guard.h"
#include "Marker.h"
#include "Settings.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

static_assert(ATR_OUT_FLAGS == ATR_OUT_FLAGS_HEX, "Update AE_Effect_Global_OutFlags in AutoTimeRampPiPL.r");
static_assert(ATR_OUT_FLAGS2 == ATR_OUT_FLAGS2_HEX, "Update AE_Effect_Global_OutFlags_2 in AutoTimeRampPiPL.r");

SPBasicSuite* g_pica = nullptr;
AEGP_PluginID g_pluginId = 0;

namespace
{
// Passed from PreRender to SmartRender: where each shutter sample's buffer sits in layer space.
struct SampleXform
{
    // Inverse mapping from output pixel centre (layer coords) to source position: q = M * p + c.
    double m00 = 1, m01 = 0, m10 = 0, m11 = 1, c0 = 0, c1 = 0;
    float opacity = 1.0f;
};

struct RenderPlan
{
    int samples = 1;
    PF_LRect outRect = {0, 0, 0, 0};
    PF_LRect sampleRect[atr::kMaxShutterSamples];
    // Ramp transform (only when some sample isn't identity).
    bool transformed = false;
    int edges = ATR_EDGE_MIRROR;
    A_long layerW = 0, layerH = 0; // full layer size at the current downsample
    SampleXform xf[atr::kMaxShutterSamples];
};

PF_LRect Intersect(const PF_LRect& a, const PF_LRect& b)
{
    PF_LRect r;
    r.left = std::max(a.left, b.left);
    r.top = std::max(a.top, b.top);
    r.right = std::min(a.right, b.right);
    r.bottom = std::min(a.bottom, b.bottom);
    if (r.right < r.left)
        r.right = r.left;
    if (r.bottom < r.top)
        r.bottom = r.top;
    return r;
}

// Inverse of: p = pivot + S*R(theta)*(q - pivot) + d, computed in square-pixel space (x * par).
SampleXform MakeXform(const atr::TransformState& st, double pivX, double pivY, double dx, double dy, double par)
{
    const double th = st.rotation * 3.14159265358979323846 / 180.0;
    const double s = std::max(1e-4, st.scale);
    const double a = std::cos(th) / s, b = std::sin(th) / s, c = -std::sin(th) / s, d = std::cos(th) / s;
    const double PvX = pivX * par, PvY = pivY, DX = dx * par, DY = dy;
    SampleXform x;
    x.m00 = a;
    x.m01 = b / par;
    x.c0 = (PvX - a * (PvX + DX) - b * (PvY + DY)) / par;
    x.m10 = c * par;
    x.m11 = d;
    x.c1 = PvY - c * (PvX + DX) - d * (PvY + DY);
    x.opacity = static_cast<float>(std::min(1.0, std::max(0.0, st.opacity)));
    return x;
}

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


PF_Err About(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
    PF_SPRINTF(out_data->return_msg, "%s v%d.%d\r%s", ATR_NAME, MAJOR_VERSION, MINOR_VERSION, ATR_DESCRIPTION);
    (void)in_data;
    return PF_Err_NONE;
}

PF_Err GlobalSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
    out_data->my_version = PF_VERSION(MAJOR_VERSION, MINOR_VERSION, BUG_VERSION, STAGE_VERSION, BUILD_VERSION);
    out_data->out_flags = ATR_OUT_FLAGS;
    out_data->out_flags2 = ATR_OUT_FLAGS2;

    if (in_data->appl_id != kAppID_Premiere && !g_pluginId)
    {
        g_pica = in_data->pica_basicP;
        try
        {
            AEFX_SuiteScoper<AEGP_UtilitySuite6> util(in_data, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6, out_data);
            if (util->AEGP_RegisterWithAEGP(nullptr, ATR_NAME, &g_pluginId) == A_Err_NONE)
                RegisterMarkerIdleHook();
        }
        catch (...)
        {
            g_pluginId = 0; // marker features off; rendering still works
        }
    }
    return PF_Err_NONE;
}

PF_Err AddBezierSliders(PF_InData* in_data, int diskX1, int diskY1, int diskX2, int diskY2)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    const A_long sup = PF_ParamFlag_SUPERVISE;
    PF_ADD_FLOAT_SLIDERX("Bezier x1", 0, 1, 0, 1, atr::kDefaultBezier[0], PF_Precision_HUNDREDTHS, 0, sup, diskX1);
    PF_ADD_FLOAT_SLIDERX("Bezier y1", -1, 2, 0, 1, atr::kDefaultBezier[1], PF_Precision_HUNDREDTHS, 0, sup, diskY1);
    PF_ADD_FLOAT_SLIDERX("Bezier x2", 0, 1, 0, 1, atr::kDefaultBezier[2], PF_Precision_HUNDREDTHS, 0, sup, diskX2);
    PF_ADD_FLOAT_SLIDERX("Bezier y2", -1, 2, 0, 1, atr::kDefaultBezier[3], PF_Precision_HUNDREDTHS, 0, sup, diskY2);
    return err;
}

// "Transform" subgroup: values at the cut, easing to identity with the ramp.
PF_Err AddTransformGroup(PF_InData* in_data, int topic, int scale, int rotation, int posX, int posY, int opacity, int topicEnd)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Transform at cut", topic);
    PF_ADD_FLOAT_SLIDERX("Scale (%)", 1, 1000, 25, 400, 100, PF_Precision_TENTHS, 0, 0, scale);
    PF_ADD_FLOAT_SLIDERX("Rotation (deg)", -3600, 3600, -180, 180, 0, PF_Precision_TENTHS, 0, 0, rotation);
    PF_ADD_FLOAT_SLIDERX("Position X (px)", -20000, 20000, -2000, 2000, 0, PF_Precision_TENTHS, 0, 0, posX);
    PF_ADD_FLOAT_SLIDERX("Position Y (px)", -20000, 20000, -2000, 2000, 0, PF_Precision_TENTHS, 0, 0, posY);
    PF_ADD_FLOAT_SLIDERX("Opacity (%)", 0, 100, 0, 100, 100, PF_Precision_TENTHS, 0, 0, opacity);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(topicEnd);
    return err;
}

PF_Err ParamsSetup(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef*[], PF_LayerDef*)
{
    PF_Err err = PF_Err_NONE;
    PF_ParamDef def;
    const A_long sup = PF_ParamFlag_SUPERVISE; // so USER_CHANGED_PARAM fires (marker + graph refresh)

    // Which curve the graph shows: speed over time, or source time over time (time-remap view).
    AEFX_CLR_STRUCT(def);
    def.flags = sup | PF_ParamFlag_CANNOT_TIME_VARY;
    PF_ADD_POPUP("Graph", ATR_GRAPH_MODE_COUNT, ATR_GRAPH_SPEED, ATR_GRAPH_MODE_CHOICES, DISK_GRAPH_MODE);

    // Graph: a colour param whose Effect Controls UI is drawn by us. Its value is unused.
    AEFX_CLR_STRUCT(def);
    def.flags = PF_ParamFlag_CANNOT_TIME_VARY;
    def.ui_flags = PF_PUI_CONTROL | PF_PUI_DONT_ERASE_CONTROL;
    def.ui_width = ATR_GRAPH_WIDTH;
    def.ui_height = ATR_GRAPH_HEIGHT;
    PF_ADD_COLOR("Ramp graph", 0, 0, 0, DISK_GRAPH);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Ramp in", DISK_IN_TOPIC);
    PF_ADD_FLOAT_SLIDERX("Duration (s)", 0, 60, 0, 5, 1.0, PF_Precision_HUNDREDTHS, 0, sup, DISK_IN_DURATION);
    PF_ADD_FLOAT_SLIDERX("Footage used (s)", 0, 3600, 0, 30, 5.0, PF_Precision_HUNDREDTHS, 0, sup, DISK_IN_SOURCE);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Ease", ATR_EASE_COUNT, 1, ATR_EASE_CHOICES, DISK_IN_EASE);
    ERR(AddBezierSliders(in_data, DISK_IN_X1, DISK_IN_Y1, DISK_IN_X2, DISK_IN_Y2));
    ERR(AddTransformGroup(in_data, DISK_IN_XF_TOPIC, DISK_IN_XF_SCALE, DISK_IN_XF_ROTATION, DISK_IN_XF_POS_X,
                          DISK_IN_XF_POS_Y, DISK_IN_XF_OPACITY, DISK_IN_XF_TOPIC_END));
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(DISK_IN_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Ramp out", DISK_OUT_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Same as ramp in", TRUE, sup, DISK_OUT_LINK);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Motion direction", ATR_DIRECTION_COUNT, 1, ATR_DIRECTION_CHOICES, DISK_OUT_DIRECTION);
    PF_ADD_FLOAT_SLIDERX("Duration (s)", 0, 60, 0, 5, 1.0, PF_Precision_HUNDREDTHS, 0, sup, DISK_OUT_DURATION);
    PF_ADD_FLOAT_SLIDERX("Footage used (s)", 0, 3600, 0, 30, 5.0, PF_Precision_HUNDREDTHS, 0, sup, DISK_OUT_SOURCE);
    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Ease", ATR_EASE_COUNT, 1, ATR_EASE_CHOICES, DISK_OUT_EASE);
    ERR(AddBezierSliders(in_data, DISK_OUT_X1, DISK_OUT_Y1, DISK_OUT_X2, DISK_OUT_Y2));
    ERR(AddTransformGroup(in_data, DISK_OUT_XF_TOPIC, DISK_OUT_XF_SCALE, DISK_OUT_XF_ROTATION, DISK_OUT_XF_POS_X,
                          DISK_OUT_XF_POS_Y, DISK_OUT_XF_OPACITY, DISK_OUT_XF_TOPIC_END));
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(DISK_OUT_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    def.flags = sup;
    PF_ADD_POPUP("Anchor", ATR_ANCHOR_COUNT, 1, ATR_ANCHOR_CHOICES, DISK_ANCHOR);
    AEFX_CLR_STRUCT(def);
    PF_ADD_POPUP("Transform edges", ATR_EDGES_COUNT, ATR_EDGE_MIRROR, ATR_EDGES_CHOICES, DISK_EDGES);

    AEFX_CLR_STRUCT(def);
    PF_ADD_TOPIC("Motion blur", DISK_BLUR_TOPIC);
    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Enable", FALSE, 0, DISK_BLUR_ENABLE);
    PF_ADD_FLOAT_SLIDERX("Shutter angle", 0, 720, 0, 360, 180, PF_Precision_INTEGER, 0, 0, DISK_BLUR_SHUTTER);
    AEFX_CLR_STRUCT(def);
    PF_ADD_SLIDER("Max samples", 1, atr::kMaxShutterSamples, 1, 32, 16, DISK_BLUR_MAX_SAMPLES);
    AEFX_CLR_STRUCT(def);
    PF_END_TOPIC(DISK_BLUR_TOPIC_END);

    AEFX_CLR_STRUCT(def);
    PF_ADD_CHECKBOXX("Real-time marker", TRUE, sup, DISK_MARKER);

    out_data->num_params = ATR_NUM_PARAMS;

    if (!err)
    {
        PF_CustomUIInfo ci;
        AEFX_CLR_STRUCT(ci);
        ci.events = PF_CustomEFlag_EFFECT;
        err = (*(in_data->inter.register_ui))(in_data->effect_ref, &ci);
    }
    return err;
}

// Show the bezier sliders only for Custom eases; grey out ramp-out controls while linked.
PF_Err UpdateParamsUI(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[])
{
    PF_Err err = PF_Err_NONE;
    if (in_data->appl_id == kAppID_Premiere || !g_pluginId)
        return err;

    const bool linked = params[ATR_OUT_LINK]->u.bd.value != 0;
    const bool inCustom = params[ATR_IN_EASE]->u.pd.value == ATR_EASE_CUSTOM;
    const bool outCustom = params[ATR_OUT_EASE]->u.pd.value == ATR_EASE_CUSTOM;

    ATR_STEP("update_params_ui:disable");
    AEFX_SuiteScoper<PF_ParamUtilsSuite3> paramUtils(in_data, kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, out_data);
    // Ramp-out values are greyed out while linked; the motion direction only matters while linked.
    struct Dis
    {
        int idx;
        bool disable;
    };
    const Dis dis[] = {{ATR_OUT_DURATION, linked},    {ATR_OUT_SOURCE, linked},      {ATR_OUT_EASE, linked},
                       {ATR_OUT_X1, linked},          {ATR_OUT_Y1, linked},          {ATR_OUT_X2, linked},
                       {ATR_OUT_Y2, linked},          {ATR_OUT_XF_SCALE, linked},    {ATR_OUT_XF_ROTATION, linked},
                       {ATR_OUT_XF_POS_X, linked},    {ATR_OUT_XF_POS_Y, linked},    {ATR_OUT_XF_OPACITY, linked},
                       {ATR_OUT_DIRECTION, !linked}};
    for (const Dis& d : dis)
    {
        const int idx = d.idx;
        const bool isDisabled = (params[idx]->ui_flags & PF_PUI_DISABLED) != 0;
        if (isDisabled != d.disable)
        {
            PF_ParamDef copy = *params[idx];
            if (d.disable)
                copy.ui_flags |= PF_PUI_DISABLED;
            else
                copy.ui_flags &= ~PF_PUI_DISABLED;
            ERR(paramUtils->PF_UpdateParamUI(in_data->effect_ref, idx, &copy));
        }
    }

    // Visibility goes through dynamic stream flags in AE.
    ATR_STEP("update_params_ui:visibility");
    AEFX_SuiteScoper<AEGP_PFInterfaceSuite1> pfi(in_data, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1, out_data);
    AEFX_SuiteScoper<AEGP_StreamSuite6> streams(in_data, kAEGPStreamSuite, kAEGPStreamSuiteVersion6, out_data);
    AEFX_SuiteScoper<AEGP_DynamicStreamSuite4> dyn(
        in_data, kAEGPDynamicStreamSuite, kAEGPDynamicStreamSuiteVersion4, out_data);
    AEGP_EffectRefH effH = nullptr;
    if (pfi->AEGP_GetNewEffectForEffect(g_pluginId, in_data->effect_ref, &effH) != A_Err_NONE || !effH)
        return err;
    struct Vis
    {
        int idx;
        bool hide;
    };
    const Vis vis[] = {{ATR_IN_X1, !inCustom},  {ATR_IN_Y1, !inCustom},  {ATR_IN_X2, !inCustom},
                       {ATR_IN_Y2, !inCustom},  {ATR_OUT_X1, !outCustom}, {ATR_OUT_Y1, !outCustom},
                       {ATR_OUT_X2, !outCustom}, {ATR_OUT_Y2, !outCustom}};
    for (const Vis& v : vis)
    {
        AEGP_StreamRefH streamH = nullptr;
        if (streams->AEGP_GetNewEffectStreamByIndex(g_pluginId, effH, v.idx, &streamH) != A_Err_NONE || !streamH)
            continue;
        AEGP_DynStreamFlags flags = 0;
        dyn->AEGP_GetDynamicStreamFlags(streamH, &flags);
        const bool hidden = (flags & AEGP_DynStreamFlag_HIDDEN) != 0;
        if (hidden != v.hide)
            dyn->AEGP_SetDynamicStreamFlag(streamH, AEGP_DynStreamFlag_HIDDEN, FALSE, v.hide);
        streams->AEGP_DisposeStream(streamH);
    }
    AEFX_SuiteScoper<AEGP_EffectSuite5> fx(in_data, kAEGPEffectSuite, kAEGPEffectSuiteVersion5, out_data);
    fx->AEGP_DisposeEffect(effH);
    return err;
}

// A supervised param changed: update the marker inside the same undo step and redraw the graph.
PF_Err UserChangedParam(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[])
{
    PF_Err err = PF_Err_NONE;
    out_data->out_flags |= PF_OutFlag_REFRESH_UI;
    if (in_data->appl_id == kAppID_Premiere || !g_pluginId)
        return err;

    AEFX_SuiteScoper<AEGP_PFInterfaceSuite1> pfi(in_data, kAEGPPFInterfaceSuite, kAEGPPFInterfaceSuiteVersion1, out_data);
    AEGP_LayerH layerH = nullptr;
    if (pfi->AEGP_GetEffectLayer(in_data->effect_ref, &layerH) != A_Err_NONE || !layerH)
        return err;
    LayerTiming timing;
    if (ReadLayerTimingAEGP(in_data->pica_basicP, layerH, timing) != A_Err_NONE)
        return err;
    Settings s;
    ReadSettingsFromParams(params, s);
    const double frame = in_data->time_scale ? double(in_data->time_step) / double(in_data->time_scale) : 1.0 / 25.0;
    const atr::Solved solved = BuildRamp(s, timing, frame);
    ATR_STEP("user_changed_param:sync_marker");
    SyncMarker(layerH, ComputeMarkerZone(s, solved), frame);
    return err;
}

PF_Err HandleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* extra)
{
    if (extra->e_type == PF_Event_DRAW)
        return DrawSpeedGraph(in_data, out_data, params, extra);
    return PF_Err_NONE;
}

// Requested source times are passed to AE on a finer grid than the comp's, so shutter samples can
// land between frames (precomps render sub-frame content; footage picks the nearest frame).
constexpr double kMinTimeScale = 6000.0;

PF_Err PreRender(PF_InData* in_data, PF_OutData* out_data, PF_PreRenderExtra* extra)
{
    PF_Err err = PF_Err_NONE;

    LayerTiming timing;
    ERR(ReadLayerTiming(in_data, out_data, timing));
    if (!err && extra->cb->GuidMixInPtr)
        ERR(extra->cb->GuidMixInPtr(in_data->effect_ref, sizeof(timing), &timing));

    Settings settings;
    ERR(ReadSettingsPF(in_data, settings));
    if (err)
        return err;

    const double scale = double(in_data->time_scale);
    const double frame = scale > 0.0 ? double(in_data->time_step) / scale : 0.0;
    const atr::Solved solved = BuildRamp(settings, timing, frame);

    const double t = double(in_data->current_time) / scale;
    double sourceTimes[atr::kMaxShutterSamples], timelineTimes[atr::kMaxShutterSamples];
    const int n = atr::ShutterSamples(solved, t, frame, settings.blur, sourceTimes, timelineTimes);

    // Ramp transform per shutter sample (so transform motion is blurred too).
    const atr::RampTransform xfIn = settings.xfIn, xfOut = settings.XfOutEffective();
    atr::TransformState states[atr::kMaxShutterSamples];
    bool anyXf = false;
    for (int i = 0; i < n; ++i)
    {
        states[i] = atr::TransformAt(solved, xfIn, xfOut, timelineTimes[i]);
        anyXf = anyXf || !states[i].IsIdentity();
    }


    const A_long k = std::max<A_long>(1, static_cast<A_long>(std::ceil(kMinTimeScale / scale)));
    const A_u_long fineScale = in_data->time_scale * static_cast<A_u_long>(k);
    const A_long fineStep = in_data->time_step * k;

    RenderPlan* plan = new RenderPlan();
    plan->samples = n;

    PF_RenderRequest req = extra->input->output_request;
    req.preserve_rgb_of_zero_alpha = TRUE;

    PF_LRect layerRect = {0, 0, 0, 0};
    if (anyXf)
    {
        // A transformed sample can pull pixels from anywhere in the layer: request all of it.
        const double dsx =
            in_data->downsample_x.den ? double(in_data->downsample_x.num) / in_data->downsample_x.den : 1.0;
        const double dsy =
            in_data->downsample_y.den ? double(in_data->downsample_y.num) / in_data->downsample_y.den : 1.0;
        layerRect.right = static_cast<A_long>(std::ceil(in_data->width * dsx));
        layerRect.bottom = static_cast<A_long>(std::ceil(in_data->height * dsy));
        req.rect = layerRect;
        plan->transformed = layerRect.right > 0 && layerRect.bottom > 0;
        plan->edges = settings.edges;
        plan->layerW = layerRect.right;
        plan->layerH = layerRect.bottom;
        double par = in_data->pixel_aspect_ratio.den
                         ? double(in_data->pixel_aspect_ratio.num) / in_data->pixel_aspect_ratio.den
                         : 1.0;
        if (par <= 0.0)
            par = 1.0;
        for (int i = 0; i < n; ++i)
            plan->xf[i] = MakeXform(states[i], 0.5 * layerRect.right, 0.5 * layerRect.bottom,
                                    states[i].dx * dsx, states[i].dy * dsy, par);
    }
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
    if (plan->transformed)
    {
        // Output covers what AE asked for, within the layer (edges are synthesised by the edge mode).
        extra->output->result_rect = Intersect(extra->input->output_request.rect, layerRect);
        extra->output->max_result_rect = layerRect;
    }
    plan->outRect = extra->output->result_rect;
    {
        const PF_LRect& q = extra->input->output_request.rect;
        const PF_LRect& o = extra->output->result_rect;
        AtrDebugLog("pre t=%ld n=%d xf=%d req=(%ld,%ld,%ld,%ld) out=(%ld,%ld,%ld,%ld) s0=(%ld,%ld,%ld,%ld) layer=%ldx%ld",
                    (long)in_data->current_time, n, plan->transformed ? 1 : 0, (long)q.left, (long)q.top, (long)q.right,
                    (long)q.bottom, (long)o.left, (long)o.top, (long)o.right, (long)o.bottom,
                    (long)plan->sampleRect[0].left, (long)plan->sampleRect[0].top, (long)plan->sampleRect[0].right,
                    (long)plan->sampleRect[0].bottom, (long)plan->layerW, (long)plan->layerH);
    }
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

// Map an integer layer coordinate into the layer by the edge mode; false = outside (transparent).
inline bool WrapEdge(A_long& v, A_long size, int mode)
{
    if (v >= 0 && v < size)
        return true;
    if (mode == ATR_EDGE_TRANSPARENT || size <= 0)
        return false;
    if (mode == ATR_EDGE_REPEAT)
    {
        v %= size;
        if (v < 0)
            v += size;
        return true;
    }
    const A_long period = 2 * size; // mirror
    A_long m = v % period;
    if (m < 0)
        m += period;
    v = m < size ? m : period - 1 - m;
    return true;
}

// Transformed shutter samples: each output pixel is mapped back through the sample's inverse
// transform, sampled bilinearly with the edge mode, scaled by the sample's opacity, and averaged.
template <typename PixelT>
void AccumulateTransformed(const RenderPlan& plan, PF_EffectWorld* const* inputs, PF_EffectWorld* out)
{
    using ChanT = decltype(PixelT::red);
    const A_long w = out->width, h = out->height;
    const float inv = 1.0f / float(plan.samples);

    auto fetch = [&](const PF_EffectWorld* in, const PF_LRect& r, A_long x, A_long y, float* px) {
        if (!WrapEdge(x, plan.layerW, plan.edges) || !WrapEdge(y, plan.layerH, plan.edges))
            return false;
        const A_long bx = x - r.left, by = y - r.top;
        if (bx < 0 || by < 0 || bx >= in->width || by >= in->height)
            return false;
        const PixelT& p =
            reinterpret_cast<const PixelT*>(reinterpret_cast<const char*>(in->data) + by * in->rowbytes)[bx];
        px[0] = float(p.alpha);
        px[1] = float(p.red);
        px[2] = float(p.green);
        px[3] = float(p.blue);
        return true;
    };

    for (A_long y = 0; y < h; ++y)
    {
        PixelT* orow = reinterpret_cast<PixelT*>(reinterpret_cast<char*>(out->data) + y * out->rowbytes);
        const double py = plan.outRect.top + y + 0.5;
        for (A_long x = 0; x < w; ++x)
        {
            const double px = plan.outRect.left + x + 0.5;
            float acc[4] = {0, 0, 0, 0};
            for (int i = 0; i < plan.samples; ++i)
            {
                const PF_EffectWorld* in = inputs[i];
                const SampleXform& m = plan.xf[i];
                if (!in || !in->data || m.opacity <= 0.0f)
                    continue;
                // Source position in pixel-index space (pixel centres at +0.5).
                const double qx = m.m00 * px + m.m01 * py + m.c0 - 0.5;
                const double qy = m.m10 * px + m.m11 * py + m.c1 - 0.5;
                const double fx0 = std::floor(qx), fy0 = std::floor(qy);
                const A_long x0 = static_cast<A_long>(fx0), y0 = static_cast<A_long>(fy0);
                const float fx = static_cast<float>(qx - fx0), fy = static_cast<float>(qy - fy0);
                const float wts[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
                const A_long xs[4] = {x0, x0 + 1, x0, x0 + 1};
                const A_long ys[4] = {y0, y0, y0 + 1, y0 + 1};
                for (int k = 0; k < 4; ++k)
                {
                    float p[4];
                    if (wts[k] > 0.0f && fetch(in, plan.sampleRect[i], xs[k], ys[k], p))
                    {
                        const float wk = wts[k] * m.opacity;
                        acc[0] += p[0] * wk;
                        acc[1] += p[1] * wk;
                        acc[2] += p[2] * wk;
                        acc[3] += p[3] * wk;
                    }
                }
            }
            orow[x].alpha = static_cast<ChanT>(Traits<PixelT>::Store(acc[0] * inv));
            orow[x].red = static_cast<ChanT>(Traits<PixelT>::Store(acc[1] * inv));
            orow[x].green = static_cast<ChanT>(Traits<PixelT>::Store(acc[2] * inv));
            orow[x].blue = static_cast<ChanT>(Traits<PixelT>::Store(acc[3] * inv));
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
    if (outputP)
        AtrDebugLog("render t=%ld out=%ldx%ld origin=(%ld,%ld) in0=%ldx%ld planOut=(%ld,%ld)", (long)in_data->current_time,
                    (long)outputP->width, (long)outputP->height, (long)in_data->output_origin_x,
                    (long)in_data->output_origin_y, inputs[0] ? (long)inputs[0]->width : -1L,
                    inputs[0] ? (long)inputs[0]->height : -1L, plan ? (long)plan->outRect.left : 0L,
                    plan ? (long)plan->outRect.top : 0L);

    if (!err && outputP && outputP->data && plan && plan->transformed)
    {
        AEFX_SuiteScoper<PF_WorldSuite2> worldSuite(in_data, kPFWorldSuite, kPFWorldSuiteVersion2, out_data);
        PF_PixelFormat format = PF_PixelFormat_INVALID;
        ERR(worldSuite->PF_GetPixelFormat(outputP, &format));
        if (!err)
        {
            switch (format)
            {
            case PF_PixelFormat_ARGB128: AccumulateTransformed<PF_PixelFloat>(*plan, inputs, outputP); break;
            case PF_PixelFormat_ARGB64: AccumulateTransformed<PF_Pixel16>(*plan, inputs, outputP); break;
            case PF_PixelFormat_ARGB32: AccumulateTransformed<PF_Pixel8>(*plan, inputs, outputP); break;
            default: err = PF_Err_BAD_CALLBACK_PARAM; break;
            }
        }
    }
    else if (!err && outputP && outputP->data)
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

static PF_Err EffectMainImpl(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
    PF_Err err = PF_Err_NONE;
    try
    {
        ATR_STEP(cmd == PF_Cmd_SMART_PRE_RENDER ? "prerender" : cmd == PF_Cmd_SMART_RENDER ? "render"
                 : cmd == PF_Cmd_EVENT ? "event" : cmd == PF_Cmd_UPDATE_PARAMS_UI ? "update_params_ui"
                 : cmd == PF_Cmd_USER_CHANGED_PARAM ? "user_changed_param" : cmd == PF_Cmd_GLOBAL_SETUP ? "global_setup"
                 : cmd == PF_Cmd_PARAMS_SETUP ? "params_setup" : "other_cmd");
        switch (cmd)
        {
        case PF_Cmd_ABOUT: err = About(in_data, out_data, params, output); break;
        case PF_Cmd_GLOBAL_SETUP: err = GlobalSetup(in_data, out_data, params, output); break;
        case PF_Cmd_PARAMS_SETUP: err = ParamsSetup(in_data, out_data, params, output); break;
        case PF_Cmd_UPDATE_PARAMS_UI: err = UpdateParamsUI(in_data, out_data, params); break;
        case PF_Cmd_USER_CHANGED_PARAM: err = UserChangedParam(in_data, out_data, params); break;
        case PF_Cmd_EVENT: err = HandleEvent(in_data, out_data, params, static_cast<PF_EventExtra*>(extra)); break;
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

// SEH wrapper: a crash inside the plug-in is logged (see Guard.h) and reported as an error to AE.
PF_Err EffectMain(
    PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_LayerDef* output, void* extra)
{
    __try
    {
        return EffectMainImpl(cmd, in_data, out_data, params, output, extra);
    }
    __except (AtrCrashFilter(GetExceptionInformation(), "EffectMain"))
    {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
}
