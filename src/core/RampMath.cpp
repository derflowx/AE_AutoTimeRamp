#include "RampMath.h"

#include <algorithm>
#include <cmath>

namespace atr
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kLn2 = 0.69314718055994530942;
constexpr double kExpoNorm = 1.0 - 0.0009765625; // 1 - 2^-10, so expo reaches exactly 1 at u=1
constexpr double kEps = 1.0e-9;

// Settle dip bump b(u) = k * u^8 * (1-u)^2, peak 1.0 at u = 0.8, flat at both ends.
constexpr double kDipK = 149.01161193847656; // 1 / (0.8^8 * 0.2^2)
constexpr double kSettleDepth = 0.25;        // dip of up to 25% under real-time speed

double Clamp01(double u)
{
    return u < 0.0 ? 0.0 : (u > 1.0 ? 1.0 : u);
}

// e(u) = 1 - (1-u)^n
double PowOut(double u, int n)
{
    return 1.0 - std::pow(1.0 - u, n);
}

// integral_0^u of 1 - (1-s)^n ds
double PowOutInt(double u, int n)
{
    return u - (1.0 - std::pow(1.0 - u, n + 1)) / (n + 1);
}
} // namespace

double EaseValue(Ease ease, double u)
{
    u = Clamp01(u);
    switch (ease)
    {
    case Ease::SoftSnap: return PowOut(u, 2);
    case Ease::Snap: return PowOut(u, 3);
    case Ease::HardSnap: return PowOut(u, 4);
    case Ease::Settle: return PowOut(u, 4);
    case Ease::Whip: return PowOut(u, 5);
    case Ease::ExpoSnap: return (1.0 - std::pow(2.0, -10.0 * u)) / kExpoNorm;
    case Ease::CircSnap:
    {
        const double w = u - 1.0;
        return std::sqrt(std::max(0.0, 1.0 - w * w));
    }
    case Ease::Smooth: return 0.5 * (1.0 - std::cos(kPi * u));
    case Ease::Linear:
    default: return u;
    }
}

double EaseIntegral(Ease ease, double u)
{
    u = Clamp01(u);
    switch (ease)
    {
    case Ease::SoftSnap: return PowOutInt(u, 2);
    case Ease::Snap: return PowOutInt(u, 3);
    case Ease::HardSnap: return PowOutInt(u, 4);
    case Ease::Settle: return PowOutInt(u, 4);
    case Ease::Whip: return PowOutInt(u, 5);
    case Ease::ExpoSnap: return (u - (1.0 - std::pow(2.0, -10.0 * u)) / (10.0 * kLn2)) / kExpoNorm;
    case Ease::CircSnap:
    {
        // integral of sqrt(1 - w^2) dw from -1 to u-1
        const double w = u - 1.0;
        const double root = std::sqrt(std::max(0.0, 1.0 - w * w));
        return 0.5 * (w * root + std::asin(std::max(-1.0, std::min(1.0, w)))) + kPi / 4.0;
    }
    case Ease::Smooth: return 0.5 * u - std::sin(kPi * u) / (2.0 * kPi);
    case Ease::Linear:
    default: return 0.5 * u * u;
    }
}

double DipDepth(Ease ease)
{
    return ease == Ease::Settle ? kSettleDepth : 0.0;
}

double DipValue(double u)
{
    u = Clamp01(u);
    const double a = 1.0 - u;
    return kDipK * std::pow(u, 8) * a * a;
}

double DipIntegral(double u)
{
    u = Clamp01(u);
    // k * integral of (s^8 - 2 s^9 + s^10)
    return kDipK * (std::pow(u, 9) / 9.0 - std::pow(u, 10) / 5.0 + std::pow(u, 11) / 11.0);
}

double SolvePeak(Ease ease, double duration, double source)
{
    if (duration <= kEps)
        return 1.0;
    const double mean = EaseIntegral(ease, 1.0);
    const double dip = DipDepth(ease) * DipIntegral(1.0);
    // S/R = vPeak + (1 - vPeak) * mean - dip
    return (source / duration + dip - mean) / (1.0 - mean);
}

double MinSource(Ease ease, double duration)
{
    const double mean = EaseIntegral(ease, 1.0);
    const double dip = DipDepth(ease) * DipIntegral(1.0);
    return std::max(0.0, duration * (mean - dip));
}

namespace
{
SolvedRamp SolveRamp(const RampSpec& spec, double duration, double source)
{
    SolvedRamp r;
    r.ease = spec.ease;
    r.duration = std::max(0.0, duration);
    r.source = std::max(0.0, source);
    if (r.duration <= kEps)
    {
        r.duration = 0.0;
        r.source = 0.0;
        r.peak = 1.0;
        return r;
    }
    double peak = SolvePeak(spec.ease, r.duration, r.source);
    if (peak < 0.0)
    {
        peak = 0.0;
        r.source = MinSource(spec.ease, r.duration);
        r.clampedToFreeze = true;
    }
    r.peak = peak;
    return r;
}
} // namespace

Solved Solve(const RampInput& in)
{
    Solved s;
    s.inPoint = in.inPoint;
    s.outPoint = std::max(in.outPoint, in.inPoint);
    s.sourceMin = in.sourceMin;
    s.sourceMax = std::max(in.sourceMax, in.sourceMin);

    const double length = s.outPoint - s.inPoint;
    double durIn = std::max(0.0, in.rampIn.duration);
    double durOut = std::max(0.0, in.rampOut.duration);
    double srcIn = std::max(0.0, in.rampIn.source);
    double srcOut = std::max(0.0, in.rampOut.source);

    // Ramps longer than the layer: shrink both proportionally, keeping each ramp's compression ratio.
    const double total = durIn + durOut;
    if (total > length && total > kEps)
    {
        const double k = length / total;
        durIn *= k;
        durOut *= k;
        srcIn *= k;
        srcOut *= k;
    }

    s.rampIn = SolveRamp(in.rampIn, durIn, srcIn);
    s.rampOut = SolveRamp(in.rampOut, durOut, srcOut);

    switch (in.anchor)
    {
    case Anchor::Head: s.shift = s.rampIn.source - s.rampIn.duration; break;
    case Anchor::Tail: s.shift = -(s.rampOut.source - s.rampOut.duration); break;
    case Anchor::Sync:
    default: s.shift = 0.0; break;
    }
    return s;
}

double RampSpeed(const SolvedRamp& r, double u)
{
    return r.peak + (1.0 - r.peak) * EaseValue(r.ease, u) - DipDepth(r.ease) * DipValue(u);
}

double RampConsumed(const SolvedRamp& r, double tau)
{
    if (r.duration <= 0.0)
        return 0.0;
    const double u = Clamp01(tau / r.duration);
    return r.duration *
           (r.peak * u + (1.0 - r.peak) * EaseIntegral(r.ease, u) - DipDepth(r.ease) * DipIntegral(u));
}

Sample Evaluate(const Solved& s, double t)
{
    Sample out;
    const double rtStart = s.inPoint + s.rampIn.duration;
    const double rtEnd = s.outPoint - s.rampOut.duration;

    double src;
    if (s.rampIn.duration > 0.0 && t < rtStart)
    {
        const double tau = std::max(0.0, t - s.inPoint);
        // Sync: the ramp ends exactly on natural time at rtStart, having eaten rampIn.source before it.
        src = rtStart - s.rampIn.source + RampConsumed(s.rampIn, tau);
        out.speed = RampSpeed(s.rampIn, tau / s.rampIn.duration);
        out.zone = Zone::RampIn;
    }
    else if (s.rampOut.duration > 0.0 && t > rtEnd)
    {
        const double tau = std::max(0.0, s.outPoint - t); // distance back from the out-point cut
        src = rtEnd + s.rampOut.source - RampConsumed(s.rampOut, tau);
        out.speed = RampSpeed(s.rampOut, tau / s.rampOut.duration);
        out.zone = Zone::RampOut;
    }
    else
    {
        src = t;
        out.speed = 1.0;
        out.zone = Zone::RealTime;
    }

    src += s.shift;
    out.outOfRange = src < s.sourceMin - kEps || src > s.sourceMax + kEps;
    out.source = std::min(std::max(src, s.sourceMin), s.sourceMax);
    return out;
}

int ShutterSamples(const Solved& s, double t, double frameDuration, const MotionBlur& mb, double* sourceTimes)
{
    sourceTimes[0] = Evaluate(s, t).source;
    if (!mb.enabled || frameDuration <= 0.0 || mb.shutterAngle <= 0.0 || mb.maxSamples <= 1)
        return 1;

    const double open = frameDuration * std::min(mb.shutterAngle, 720.0) / 360.0;
    const double a = t - 0.5 * open;
    const double b = t + 0.5 * open;
    const double sweptFrames = std::fabs(Evaluate(s, b).source - Evaluate(s, a).source) / frameDuration;
    if (sweptFrames < 1.0 + 1e-6)
        return 1;

    const int cap = std::min(mb.maxSamples, kMaxShutterSamples);
    const int n = std::min(cap, static_cast<int>(std::ceil(sweptFrames - 1e-6)));
    for (int i = 0; i < n; ++i)
        sourceTimes[i] = Evaluate(s, a + open * (i + 0.5) / n).source;
    return n;
}

const char* EaseName(Ease ease)
{
    switch (ease)
    {
    case Ease::Snap: return "Snap";
    case Ease::SoftSnap: return "Soft snap";
    case Ease::HardSnap: return "Hard snap";
    case Ease::Whip: return "Whip";
    case Ease::ExpoSnap: return "Expo snap";
    case Ease::CircSnap: return "Circ snap";
    case Ease::Settle: return "Settle";
    case Ease::Smooth: return "Smooth";
    case Ease::Linear: return "Linear";
    default: return "?";
    }
}

} // namespace atr
