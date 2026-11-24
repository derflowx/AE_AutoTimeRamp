#include "Graph.h"

#include "AEFX_SuiteHelper.h"
#include "Guard.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cwchar>
#include <string>

namespace
{
// Small RAII wrapper over the Drawbot suites for this one draw call.
class Canvas
{
public:
    Canvas(DRAWBOT_Suites& s, DRAWBOT_SupplierRef supplier, DRAWBOT_SurfaceRef surface)
        : s_(s), supplier_(supplier), surface_(surface)
    {
        float size = 11.0f;
        s_.supplier_suiteP->GetDefaultFontSize(supplier_, &size);
        fontSize_ = size;
        s_.supplier_suiteP->NewDefaultFont(supplier_, size, &font_);
    }
    ~Canvas()
    {
        if (font_)
            s_.supplier_suiteP->ReleaseObject(reinterpret_cast<DRAWBOT_ObjectRef>(font_));
    }

    float FontSize() const { return fontSize_; }

    void FillRect(float x, float y, float w, float h, const DRAWBOT_ColorRGBA& c)
    {
        if (w <= 0 || h <= 0)
            return;
        DRAWBOT_PathRef path = nullptr;
        DRAWBOT_BrushRef brush = nullptr;
        s_.supplier_suiteP->NewPath(supplier_, &path);
        s_.supplier_suiteP->NewBrush(supplier_, &c, &brush);
        DRAWBOT_RectF32 r = {x, y, w, h};
        s_.path_suiteP->AddRect(path, &r);
        s_.surface_suiteP->FillPath(surface_, brush, path, kDRAWBOT_FillType_Default);
        Release(brush);
        Release(path);
    }

    void Polyline(const float* xs, const float* ys, int n, const DRAWBOT_ColorRGBA& c, float width)
    {
        if (n < 2)
            return;
        DRAWBOT_PathRef path = nullptr;
        DRAWBOT_PenRef pen = nullptr;
        s_.supplier_suiteP->NewPath(supplier_, &path);
        s_.supplier_suiteP->NewPen(supplier_, &c, width, &pen);
        s_.path_suiteP->MoveTo(path, xs[0], ys[0]);
        for (int i = 1; i < n; ++i)
            s_.path_suiteP->LineTo(path, xs[i], ys[i]);
        s_.surface_suiteP->StrokePath(surface_, pen, path);
        Release(pen);
        Release(path);
    }

    void Line(float x0, float y0, float x1, float y1, const DRAWBOT_ColorRGBA& c, float width)
    {
        const float xs[2] = {x0, x1}, ys[2] = {y0, y1};
        Polyline(xs, ys, 2, c, width);
    }

    void Text(const std::wstring& text, float x, float y, const DRAWBOT_ColorRGBA& c, DRAWBOT_TextAlignment align)
    {
        if (!font_ || text.empty())
            return;
        DRAWBOT_BrushRef brush = nullptr;
        s_.supplier_suiteP->NewBrush(supplier_, &c, &brush);
        DRAWBOT_PointF32 origin = {x, y};
        s_.surface_suiteP->DrawString(surface_,
                                      brush,
                                      font_,
                                      reinterpret_cast<const DRAWBOT_UTF16Char*>(text.c_str()),
                                      &origin,
                                      align,
                                      kDRAWBOT_TextTruncation_None,
                                      0.0f);
        Release(brush);
    }

private:
    template <typename T> void Release(T obj)
    {
        if (obj)
            s_.supplier_suiteP->ReleaseObject(reinterpret_cast<DRAWBOT_ObjectRef>(obj));
    }

    DRAWBOT_Suites& s_;
    DRAWBOT_SupplierRef supplier_;
    DRAWBOT_SurfaceRef surface_;
    DRAWBOT_FontRef font_ = nullptr;
    float fontSize_ = 11.0f;
};

DRAWBOT_ColorRGBA Rgba(float r, float g, float b, float a = 1.0f)
{
    DRAWBOT_ColorRGBA c;
    c.red = r;
    c.green = g;
    c.blue = b;
    c.alpha = a;
    return c;
}

std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    std::vswprintf(buf, 256, fmt, args);
    va_end(args);
    return buf;
}

std::wstring Percent(double speed)
{
    return Fmt(L"%.0f%%", speed * 100.0);
}
} // namespace

PF_Err DrawSpeedGraph(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[], PF_EventExtra* event)
{
    PF_Err err = PF_Err_NONE, err2 = PF_Err_NONE;
    if (event->effect_win.area != PF_EA_CONTROL || event->effect_win.index != ATR_GRAPH)
        return err;

    ATR_STEP("graph:data");
    // ---- Data ------------------------------------------------------------------------------------
    Settings s;
    ReadSettingsFromParams(params, s);
    LayerTiming timing;
    if (ReadLayerTiming(in_data, out_data, timing) != PF_Err_NONE)
        return err;
    const double frame = in_data->time_scale ? double(in_data->time_step) / double(in_data->time_scale) : 1.0 / 25.0;
    const atr::Solved solved = BuildRamp(s, timing, frame);
    const double L = std::max(1.0e-6, solved.outPoint - solved.inPoint);

    ATR_STEP("graph:drawbot_setup");
    // ---- Drawbot setup ---------------------------------------------------------------------------
    DRAWBOT_Suites db;
    DRAWBOT_DrawRef drawRef = nullptr;
    DRAWBOT_SupplierRef supplier = nullptr;
    DRAWBOT_SurfaceRef surface = nullptr;
    ERR(AEFX_AcquireDrawbotSuites(in_data, out_data, &db));
    if (err)
        return err;
    {
        AEFX_SuiteScoper<PF_EffectCustomUISuite2> ui(
            in_data, kPFEffectCustomUISuite, kPFEffectCustomUISuiteVersion2, out_data);
        ERR(ui->PF_GetDrawingReference(event->contextH, &drawRef));
    }
    ERR(db.drawbot_suiteP->GetSupplier(drawRef, &supplier));
    ERR(db.drawbot_suiteP->GetSurface(drawRef, &surface));

    if (!err)
    {
        ATR_STEP("graph:draw");
        Canvas c(db, supplier, surface);
        const PF_UnionableRect& fr = event->effect_win.current_frame;
        const float X0 = float(fr.left), Y0 = float(fr.top);
        const float W = float(fr.right - fr.left), H = float(fr.bottom - fr.top);
        const float line = c.FontSize() + 4.0f;
        const float pad = 6.0f;

        const DRAWBOT_ColorRGBA bg = Rgba(0.11f, 0.11f, 0.11f);
        const DRAWBOT_ColorRGBA rampFill = Rgba(0.85f, 0.45f, 0.25f, 0.22f);
        const DRAWBOT_ColorRGBA rtFill = Rgba(0.30f, 0.72f, 0.45f, 0.18f);
        const DRAWBOT_ColorRGBA grid = Rgba(0.45f, 0.45f, 0.45f, 0.8f);
        const DRAWBOT_ColorRGBA curve = Rgba(1.0f, 0.62f, 0.30f);
        const DRAWBOT_ColorRGBA playhead = Rgba(0.35f, 0.62f, 1.0f);
        const DRAWBOT_ColorRGBA text = Rgba(0.82f, 0.82f, 0.82f);
        const DRAWBOT_ColorRGBA dim = Rgba(0.55f, 0.55f, 0.55f);
        const DRAWBOT_ColorRGBA warn = Rgba(1.0f, 0.42f, 0.36f);

        c.FillRect(X0, Y0, W, H, bg);

        // Plot area between a header line and a footer line.
        const float px = X0 + pad, pw = W - 2 * pad;
        const float py = Y0 + pad + line, ph = H - 2 * pad - 2 * line;
        auto xOf = [&](double t) { return px + float((t - solved.inPoint) / L) * pw; };

        // Zones.
        const double rtA = solved.RealTimeStart(), rtB = solved.RealTimeEnd();
        c.FillRect(xOf(solved.inPoint), py, xOf(rtA) - xOf(solved.inPoint), ph, rampFill);
        c.FillRect(xOf(rtA), py, xOf(rtB) - xOf(rtA), ph, rtFill);
        c.FillRect(xOf(rtB), py, xOf(solved.outPoint) - xOf(rtB), ph, rampFill);

        // One sample per pixel column.
        const int n = std::max(2, std::min(2048, int(pw)));
        float xs[2048], ys[2048];
        double srcAt[2048], speedAt[2048];
        double srcMin = 1e300, srcMax = -1e300;
        for (int i = 0; i < n; ++i)
        {
            const double t = solved.inPoint + L * i / (n - 1);
            const atr::Sample smp = atr::Evaluate(solved, t);
            xs[i] = xOf(t);
            srcAt[i] = smp.source;
            speedAt[i] = smp.speed;
            srcMin = std::min(srcMin, smp.source);
            srcMax = std::max(srcMax, smp.source);
        }

        const bool timeMode = params[ATR_GRAPH_MODE] && params[ATR_GRAPH_MODE]->u.pd.value == ATR_GRAPH_TIME;
        if (timeMode)
        {
            // Time graph: source time shown at each layer time (what a Time Remap graph would show).
            // Dashed-looking grey diagonal = untouched 1:1 playback for comparison.
            const double lo = std::min(srcMin, solved.inPoint), hi = std::max(srcMax, solved.outPoint);
            const double span = std::max(1e-6, hi - lo);
            auto yOfS = [&](double s) { return py + ph - float((s - lo) / span) * ph; };
            for (int k = 0; k < 24; k += 2) // dashed 1:1 reference
            {
                const double t0 = solved.inPoint + L * k / 24.0, t1 = solved.inPoint + L * (k + 1) / 24.0;
                c.Line(xOf(t0), yOfS(t0 + solved.shift), xOf(t1), yOfS(t1 + solved.shift), grid, 1.0f);
            }
            for (int i = 0; i < n; ++i)
                ys[i] = yOfS(srcAt[i]);
            c.Polyline(xs, ys, n, curve, 2.0f);
            c.Text(Fmt(L"%.1fs", hi), px + 2.0f, py + c.FontSize(), dim, kDRAWBOT_TextAlignment_Left);
            c.Text(Fmt(L"%.1fs", lo), px + 2.0f, py + ph - 3.0f, dim, kDRAWBOT_TextAlignment_Left);
        }
        else
        {
            // Speed graph on a log axis: 10% .. a bit above the fastest point.
            const double vTop = std::max({2.0, solved.rampIn.peak, solved.rampOut.peak}) * 1.25;
            const double lgMin = std::log10(0.1), lgMax = std::log10(vTop);
            auto yOf = [&](double v) {
                const double lg = std::log10(std::max(v, 0.1));
                return py + ph - float((lg - lgMin) / (lgMax - lgMin)) * ph;
            };
            c.Line(px, yOf(1.0), px + pw, yOf(1.0), grid, 1.0f);
            c.Text(L"100%", px + 2.0f, yOf(1.0) - 2.0f, dim, kDRAWBOT_TextAlignment_Left);
            for (int i = 0; i < n; ++i)
                ys[i] = yOf(speedAt[i]);
            c.Polyline(xs, ys, n, curve, 2.0f);
        }

        // Playhead (current time is in layer time for the effect).
        const double now = double(in_data->current_time) / double(in_data->time_scale);
        if (now >= solved.inPoint && now <= solved.outPoint)
            c.Line(xOf(now), py, xOf(now), py + ph, playhead, 1.0f);

        // Header: per-ramp summary.
        const float headY = Y0 + pad + c.FontSize();
        const atr::SolvedRamp& ri = solved.rampIn;
        const atr::SolvedRamp& ro = solved.rampOut;
        c.Text(Fmt(L"In %.2fs, uses %.1fs, peak %ls", ri.duration, ri.source, Percent(ri.peak).c_str()),
               px, headY, text, kDRAWBOT_TextAlignment_Left);
        c.Text(Fmt(L"Out %.2fs, uses %.1fs, peak %ls", ro.duration, ro.source, Percent(ro.peak).c_str()),
               px + pw, headY, text, kDRAWBOT_TextAlignment_Right);

        // Footer: real-time length + total footage, or a warning.
        const float footY = Y0 + H - pad;
        std::wstring warning;
        const atr::Sample first = atr::Evaluate(solved, solved.inPoint);
        const atr::Sample last = atr::Evaluate(solved, solved.outPoint);
        if (first.outOfRange)
            warning = L"Ramp in needs footage before the clip starts";
        else if (last.outOfRange)
            warning = L"Ramp out runs past the end of the clip";
        else if (ri.clampedToFreeze || ro.clampedToFreeze)
            warning = L"Footage used is below this ease's minimum: starts from a freeze";
        if (!warning.empty())
        {
            c.Text(warning, px, footY, warn, kDRAWBOT_TextAlignment_Left);
        }
        else
        {
            const double rtLen = std::max(0.0, rtB - rtA);
            c.Text(Fmt(L"Real-time %.2fs", rtLen), px, footY, dim, kDRAWBOT_TextAlignment_Left);
            c.Text(Fmt(L"Footage used %.1fs", ri.source + rtLen + ro.source),
                   px + pw, footY, dim, kDRAWBOT_TextAlignment_Right);
        }
    }

    ERR2(AEFX_ReleaseDrawbotSuites(in_data, out_data));
    if (!err)
        event->evt_out_flags = PF_EO_HANDLED_EVENT;
    return err;
}
