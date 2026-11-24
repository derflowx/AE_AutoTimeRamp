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

struct RampSpec
{
    double duration = 1.0; // timeline seconds
    double source = 5.0;   // footage seconds consumed
    Ease ease = Ease::Snap;
};

struct RampInput
{
    double inPoint = 0.0;  // layer time, seconds
    double outPoint = 0.0; // layer time, seconds
    RampSpec rampIn;
    RampSpec rampOut;
    Anchor anchor = Anchor::Sync;
    double sourceMin = 0.0;    // valid source range (layer time, seconds)
    double sourceMax = 1.0e12; // pass item duration minus one frame if known
};

struct SolvedRamp
{
    double duration = 0.0; // effective timeline duration (may be shrunk to fit the layer)
    double source = 0.0;   // effective footage used (may be raised to the freeze minimum)
    double peak = 1.0;     // speed at the cut (1.0 = 100%)
    Ease ease = Ease::Snap;
    bool clampedToFreeze = false; // requested source was below the minimum for this ease
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
};

struct Sample
{
    double source = 0.0; // source time to show (layer time, seconds), already clamped
    double speed = 1.0;  // instantaneous speed (1.0 = 100%)
    Zone zone = Zone::RealTime;
    bool outOfRange = false; // unclamped source time fell outside [sourceMin, sourceMax]
};

// Ease shape and its integral from 0..u (u is clamped to [0,1]).
double EaseValue(Ease ease, double u);
double EaseIntegral(Ease ease, double u);

// Settle dip bump and its integral (zero for every ease except Settle).
double DipDepth(Ease ease);
double DipValue(double u);
double DipIntegral(double u);

// Peak speed needed for a ramp of `duration` seconds to consume `source` seconds of footage.
// Returns < 0 when the request is below the freeze minimum (caller clamps).
double SolvePeak(Ease ease, double duration, double source);

// Minimum footage a ramp can consume with this ease (peak = 0, i.e. a freeze at the cut).
double MinSource(Ease ease, double duration);

Solved Solve(const RampInput& in);

// Speed within a solved ramp at normalised position u (0 = cut, 1 = real-time boundary).
double RampSpeed(const SolvedRamp& r, double u);

// Footage consumed from the cut up to timeline offset tau (0..duration) inside the ramp.
double RampConsumed(const SolvedRamp& r, double tau);

// Evaluate the mapping at layer time t (seconds).
Sample Evaluate(const Solved& s, double t);

const char* EaseName(Ease ease);

} // namespace atr
