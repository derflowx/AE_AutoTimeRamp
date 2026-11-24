// Standalone tests for RampMath. Build: see build.ps1 (-Tests).
#include "../src/core/RampMath.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace atr;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK_NEAR(a, b, tol, msg)                                                                         \
    do                                                                                                     \
    {                                                                                                      \
        ++g_checks;                                                                                        \
        const double va = (a), vb = (b);                                                                   \
        if (!(std::fabs(va - vb) <= (tol)))                                                                \
        {                                                                                                  \
            ++g_failures;                                                                                  \
            std::printf("FAIL %s: %.9f vs %.9f (line %d)\n", msg, va, vb, __LINE__);                       \
        }                                                                                                  \
    } while (0)

#define CHECK(cond, msg)                                                                                   \
    do                                                                                                     \
    {                                                                                                      \
        ++g_checks;                                                                                        \
        if (!(cond))                                                                                       \
        {                                                                                                  \
            ++g_failures;                                                                                  \
            std::printf("FAIL %s (line %d)\n", msg, __LINE__);                                             \
        }                                                                                                  \
    } while (0)

static Ease AllEases(int i)
{
    return static_cast<Ease>(i);
}

// Analytic integrals must match numeric integration of the shape functions.
static void TestIntegrals()
{
    for (int i = 0; i < static_cast<int>(Ease::Count); ++i)
    {
        const Ease e = AllEases(i);
        CHECK_NEAR(EaseValue(e, 0.0), 0.0, 1e-12, EaseName(e));
        CHECK_NEAR(EaseValue(e, 1.0), 1.0, 1e-12, EaseName(e));
        for (double u : {0.1, 0.37, 0.5, 0.8, 1.0})
        {
            const int n = 20000;
            double sum = 0.0;
            for (int k = 0; k < n; ++k)
                sum += EaseValue(e, (k + 0.5) / n * u) * (u / n);
            CHECK_NEAR(EaseIntegral(e, u), sum, 1e-6, EaseName(e));
        }
    }
    const int n = 20000;
    double sum = 0.0;
    for (int k = 0; k < n; ++k)
        sum += DipValue((k + 0.5) / n) / n;
    CHECK_NEAR(DipIntegral(1.0), sum, 1e-6, "dip integral");
    CHECK_NEAR(DipValue(0.8), 1.0, 1e-9, "dip peak");
}

// 1 s ramp eating 5 s of footage with cubic snap: peak = (5 - 0.75) / 0.25 = 17.
static void TestPeakSolve()
{
    CHECK_NEAR(SolvePeak(Ease::Snap, 1.0, 5.0), 17.0, 1e-9, "cubic snap peak");
    CHECK_NEAR(SolvePeak(Ease::Linear, 2.0, 2.0), 1.0, 1e-9, "linear 1:1 is real-time");
    for (int i = 0; i < static_cast<int>(Ease::Count); ++i)
    {
        const Ease e = AllEases(i);
        SolvedRamp r;
        r.ease = e;
        r.duration = 1.3;
        r.source = 7.0;
        r.peak = SolvePeak(e, r.duration, r.source);
        CHECK_NEAR(RampConsumed(r, r.duration), r.source, 1e-9, EaseName(e));
        CHECK_NEAR(RampSpeed(r, 1.0), 1.0, 1e-9, "lands on 100%");
        CHECK_NEAR(RampSpeed(r, 0.0), r.peak, 1e-9, "peak at the cut");
    }
}

// Speed never goes negative (no reverse playback), including Settle with a freeze start.
static void TestNoReverse()
{
    for (int i = 0; i < static_cast<int>(Ease::Count); ++i)
    {
        const Ease e = AllEases(i);
        for (double src : {0.0, 0.3, 1.0, 5.0, 40.0})
        {
            RampInput in;
            in.inPoint = 0.0;
            in.outPoint = 7.0;
            in.rampIn = {1.0, src, e};
            in.rampOut = {1.0, src, e};
            const Solved s = Solve(in);
            double prev = Evaluate(s, 0.0).source;
            for (int k = 1; k <= 7000; ++k)
            {
                const Sample smp = Evaluate(s, k / 1000.0);
                CHECK(smp.speed >= -1e-9, "speed >= 0");
                if (smp.source < prev - 1e-9)
                {
                    CHECK(false, "source time monotonic");
                    break;
                }
                prev = smp.source;
            }
        }
    }
}

// Continuity at zone boundaries + the user's example: 1 s ramp / 5 s real-time / 1 s ramp.
static void TestMapping()
{
    RampInput in;
    in.inPoint = 10.0;
    in.outPoint = 17.0;
    in.rampIn = {1.0, 5.0, Ease::Snap};
    in.rampOut = {1.0, 5.0, Ease::Snap};
    in.anchor = Anchor::Sync;
    const Solved s = Solve(in);

    // Sync: real-time zone is untouched.
    CHECK_NEAR(Evaluate(s, 11.0).source, 11.0, 1e-9, "rt start natural");
    CHECK_NEAR(Evaluate(s, 13.5).source, 13.5, 1e-9, "rt middle natural");
    CHECK_NEAR(Evaluate(s, 16.0).source, 16.0, 1e-9, "rt end natural");
    // Ramp in starts 5 s of footage before real-time begins; ramp out ends 5 s after.
    CHECK_NEAR(Evaluate(s, 10.0).source, 6.0, 1e-9, "ramp in start");
    CHECK_NEAR(Evaluate(s, 17.0).source, 21.0, 1e-9, "ramp out end");
    // Continuity just either side of the boundaries.
    CHECK_NEAR(Evaluate(s, 11.0 - 1e-7).source, Evaluate(s, 11.0 + 1e-7).source, 1e-5, "in boundary");
    CHECK_NEAR(Evaluate(s, 16.0 - 1e-7).source, Evaluate(s, 16.0 + 1e-7).source, 1e-5, "out boundary");
    CHECK_NEAR(Evaluate(s, 10.0).speed, 17.0, 1e-9, "peak speed in");
    CHECK_NEAR(Evaluate(s, 17.0).speed, 17.0, 1e-9, "peak speed out");

    // Head anchor: first frame is the in-point frame.
    in.anchor = Anchor::Head;
    CHECK_NEAR(Evaluate(Solve(in), 10.0).source, 10.0, 1e-9, "head anchor");
    in.anchor = Anchor::Tail;
    CHECK_NEAR(Evaluate(Solve(in), 17.0).source, 17.0, 1e-9, "tail anchor");

    // Out of footage: clamp and flag.
    in.anchor = Anchor::Sync;
    in.sourceMin = 8.0;
    const Sample clamped = Evaluate(Solve(in), 10.0);
    CHECK_NEAR(clamped.source, 8.0, 1e-9, "clamped to source min");
    CHECK(clamped.outOfRange, "flagged out of range");
}

static void TestEdgeCases()
{
    // Ramps longer than the layer shrink to fit.
    RampInput in;
    in.inPoint = 0.0;
    in.outPoint = 1.0;
    in.rampIn = {1.0, 5.0, Ease::Snap};
    in.rampOut = {1.0, 5.0, Ease::Snap};
    const Solved s = Solve(in);
    CHECK_NEAR(s.rampIn.duration + s.rampOut.duration, 1.0, 1e-9, "ramps fit layer");
    CHECK_NEAR(s.rampIn.source / s.rampIn.duration, 5.0, 1e-9, "compression ratio kept");

    // Zero-length ramp is a hard cut at real-time.
    in.outPoint = 5.0;
    in.rampIn = {0.0, 3.0, Ease::Snap};
    CHECK_NEAR(Evaluate(Solve(in), 0.0).source, 0.0, 1e-9, "no ramp in");

    // Requested footage below the freeze minimum clamps to a freeze at the cut.
    in.rampIn = {2.0, 0.1, Ease::Snap};
    const Solved f = Solve(in);
    CHECK(f.rampIn.clampedToFreeze, "freeze clamp flagged");
    CHECK_NEAR(f.rampIn.peak, 0.0, 1e-12, "freeze peak");
    CHECK_NEAR(f.rampIn.source, MinSource(Ease::Snap, 2.0), 1e-12, "freeze source");
}

static void TestShutter()
{
    RampInput in;
    in.inPoint = 10.0;
    in.outPoint = 17.0;
    in.rampIn = {1.0, 5.0, Ease::Snap};
    in.rampOut = {1.0, 5.0, Ease::Snap};
    const Solved s = Solve(in);
    const double fd = 1.0 / 25.0;
    double st[kMaxShutterSamples];
    MotionBlur mb; // on, 180 degrees, 16 samples

    // Real-time zone: single sample at exactly t (image untouched).
    CHECK(ShutterSamples(s, 13.0, fd, mb, st) == 1, "real-time single sample");
    CHECK_NEAR(st[0], 13.0, 1e-12, "real-time sample is t");

    // At the cut the speed is 17x: half a frame of shutter sweeps ~8.5 source frames -> 9 samples.
    const int n = ShutterSamples(s, 10.0 + 0.5 * fd, fd, mb, st);
    CHECK(n >= 8 && n <= 10, "sample count follows speed");
    for (int i = 1; i < n; ++i)
        CHECK(st[i] > st[i - 1], "samples increase in time");

    // Cap and disable.
    mb.maxSamples = 4;
    CHECK(ShutterSamples(s, 10.0 + 0.5 * fd, fd, mb, st) == 4, "max samples cap");
    mb.enabled = false;
    CHECK(ShutterSamples(s, 10.0 + 0.5 * fd, fd, mb, st) == 1, "disabled");
    CHECK_NEAR(st[0], Evaluate(s, 10.0 + 0.5 * fd).source, 1e-12, "disabled sample is unblurred");

    // Wider shutter sweeps proportionally more.
    mb.enabled = true;
    mb.maxSamples = 64;
    const int n180 = ShutterSamples(s, 10.2, fd, mb, st);
    mb.shutterAngle = 360.0;
    const int n360 = ShutterSamples(s, 10.2, fd, mb, st);
    CHECK(n360 > n180, "360 shutter takes more samples");
}

int main()
{
    TestShutter();
    TestIntegrals();
    TestPeakSolve();
    TestNoReverse();
    TestMapping();
    TestEdgeCases();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
