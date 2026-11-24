// RampMath — pure C++ core of Auto Time Ramp. No AE SDK dependencies.
//
// Timeline (layer time, seconds):
//
//   | ramp in | ......... real-time (100%) ......... | ramp out |
//   inPoint                                               outPoint
//
// Each ramp has a timeline duration R and a footage budget S (seconds of source it consumes).
// Speed inside a ramp, with u running 0 (at the cut) -> 1 (where real-time starts):
//
//   v(u) = vPeak + (1 - vPeak) * e(u) - D * b(u)
//
// e(u) is the ease shape (e(0)=0, e(1)=1), b(u) an optional "settle" dip bump (b(0)=b(1)=0).
// vPeak is solved so the ramp consumes exactly S seconds of footage.

#pragma once

namespace atr
{

enum class Ease : int
{
    Snap = 0,  // cubic out
    SoftSnap,  // quad out
    HardSnap,  // quart out
    Whip,      // quint out
    ExpoSnap,  // exponential out (normalised)
    CircSnap,  // circular out
    Settle,    // quart out + small dip under 100% before landing
    Smooth,    // sine in-out
    Linear,
    Custom,    // cubic bezier (x1, y1, x2, y2), CSS-style, y clamped to [0,1]
    Count
};

enum class Anchor : int
{
    Sync = 0, // real-time zone plays at the layer's natural source time
    Head,     // first frame = source frame at the in-point
    Tail,     // last frame = source frame at the out-point
    Count
};

enum class Zone : int
{
    RampIn = 0,
    RealTime,
    RampOut
};

// Default Custom curve: a snap-like shape (fast change at the cut, gentle landing).
constexpr double kDefaultBezier[4] = {0.0, 0.8, 0.2, 1.0};

struct RampSpec
{
    double duration = 1.0; // timeline seconds
    double source = 5.0;   // footage seconds consumed
    Ease ease = Ease::Snap;
    double bezier[4] = {kDefaultBezier[0], kDefaultBezier[1], kDefaultBezier[2], kDefaultBezier[3]};
};

struct RampInput
{
    double inPoint = 0.0;  // layer time, seconds
    double outPoint = 0.0; // layer time, seconds
    RampSpec rampIn;
    RampSpec rampOut;
    bool linkOut = false;  // ramp out mirrors ramp in (duration, footage, ease)
    Anchor anchor = Anchor::Sync;
    double sourceMin = 0.0;    // valid source range (layer time, seconds)
    double sourceMax = 1.0e12; // pass item duration minus one frame if known
};

// Tabulated ease (used for Custom): e and its exact integral on a uniform grid; e is linear
// between grid points, and the integral is integrated exactly from that.
constexpr int kEaseTableSize = 512;

struct EaseTable
{
    double e[kEaseTableSize + 1];
    double integral[kEaseTableSize + 1];
};

struct SolvedRamp
{
    double duration = 0.0; // effective timeline duration (may be shrunk to fit the layer)
    double source = 0.0;   // effective footage used (may be raised to the freeze minimum)
    double peak = 1.0;     // speed at the cut (1.0 = 100%)
    Ease ease = Ease::Snap;
    bool clampedToFreeze = false; // requested source was below the minimum for this ease
    EaseTable table;              // filled only for Ease::Custom
};

struct Solved
{
    double inPoint = 0.0;
    double outPoint = 0.0;
    SolvedRamp rampIn;
    SolvedRamp rampOut;
    double shift = 0.0; // anchor offset added to every source time
    double sourceMin = 0.0;
    double sourceMax = 1.0e12;

    double RealTimeStart() const { return inPoint + rampIn.duration; }
    double RealTimeEnd() const { return outPoint - rampOut.duration; }
};

struct Sample
{
    double source = 0.0; // source time to show (layer time, seconds), already clamped
    double speed = 1.0;  // instantaneous speed (1.0 = 100%)
    Zone zone = Zone::RealTime;
    bool outOfRange = false; // unclamped source time fell outside [sourceMin, sourceMax]
};

// Preset ease shape and its integral from 0..u (u is clamped to [0,1]). Custom is not handled here
// (it needs bezier parameters); use BezierEase / the solved ramp instead.
double EaseValue(Ease ease, double u);
double EaseIntegral(Ease ease, double u);

// CSS-style cubic bezier from (0,0) to (1,1) with control points (x1,y1), (x2,y2); x's clamped to
// [0,1], result clamped to [0,1] so speed can never run backwards.
double BezierEase(const double bezier[4], double u);
void BuildEaseTable(const double bezier[4], EaseTable& table);

// Settle dip bump and its integral (zero for every ease except Settle).
double DipDepth(Ease ease);
double DipValue(double u);
double DipIntegral(double u);

// Peak speed needed for a ramp of `duration` seconds to consume `source` seconds of footage,
// for preset eases. Returns < 0 when the request is below the freeze minimum (caller clamps).
double SolvePeak(Ease ease, double duration, double source);

// Minimum footage a ramp can consume with this preset ease (peak = 0, i.e. a freeze at the cut).
double MinSource(Ease ease, double duration);

Solved Solve(const RampInput& in);

// Ease shape / integral of a solved ramp (dispatches presets vs. Custom table).
double RampEase(const SolvedRamp& r, double u);
double RampEaseIntegral(const SolvedRamp& r, double u);

// Speed within a solved ramp at normalised position u (0 = cut, 1 = real-time boundary).
double RampSpeed(const SolvedRamp& r, double u);

// Footage consumed from the cut up to timeline offset tau (0..duration) inside the ramp.
double RampConsumed(const SolvedRamp& r, double tau);

// Evaluate the mapping at layer time t (seconds).
Sample Evaluate(const Solved& s, double t);

// ---- Speed-based motion blur --------------------------------------------------------------------
//
// A virtual shutter, centred on the output frame, is open for (shutterAngle / 360) frames of timeline
// time. Samples are spread evenly across it and each is mapped through Evaluate(), so the blur length
// follows the ramp's speed. Sample count = source frames swept by the shutter (rounded up), capped.
// When the shutter sweeps less than one source frame (e.g. the real-time zone at 180 degrees),
// a single sample at exactly t is used, so the image is untouched.

constexpr int kMaxShutterSamples = 64;

struct MotionBlur
{
    bool enabled = true;
    double shutterAngle = 180.0; // degrees, 0..720
    int maxSamples = 16;         // 1..kMaxShutterSamples
};

// Fills `sourceTimes` (capacity kMaxShutterSamples) and returns the sample count (>= 1).
// frameDuration is the comp frame duration in seconds.
int ShutterSamples(const Solved& s, double t, double frameDuration, const MotionBlur& mb, double* sourceTimes);

const char* EaseName(Ease ease);

} // namespace atr
