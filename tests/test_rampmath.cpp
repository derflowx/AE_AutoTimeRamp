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
    for (int i = 0; i < static_cast<int>(Ease::Custom); ++i) // presets only
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
        RampInput in;
        in.outPoint = 10.0;
        in.rampIn = {1.3, 7.0, e};
        const SolvedRamp r = Solve(in).rampIn;
        if (e != Ease::Custom)
            CHECK_NEAR(r.peak, SolvePeak(e, 1.3, 7.0), 1e-9, "solve matches SolvePeak");
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

static void TestBezier()
{
    const double lin[4] = {0.0, 0.0, 1.0, 1.0};
    const double easeInOut[4] = {0.42, 0.0, 0.58, 1.0};
    const double overshoot[4] = {0.3, 1.8, 0.6, 1.4};
    for (double u = 0.0; u <= 1.0; u += 0.05)
    {
        CHECK_NEAR(BezierEase(lin, u), u, 1e-6, "bezier (0,0,1,1) is linear");
        CHECK_NEAR(BezierEase(easeInOut, u) + BezierEase(easeInOut, 1.0 - u), 1.0, 1e-6, "ease-in-out symmetric");
        const double v = BezierEase(overshoot, u);
        CHECK(v >= 0.0 && v <= 1.0, "bezier clamped to [0,1]");
    }
    // CSS ease (0.25, 0.1, 0.25, 1) at x=0.5 is ~0.8024 (reference value).
    const double cssEase[4] = {0.25, 0.1, 0.25, 1.0};
    CHECK_NEAR(BezierEase(cssEase, 0.5), 0.8024033877399112, 1e-4, "css ease reference");

    // Table integral matches dense numeric integration.
    EaseTable t;
    BuildEaseTable(kDefaultBezier, t);
    const int n = 50000;
    double sum = 0.0;
    for (int k = 0; k < n; ++k)
        sum += BezierEase(kDefaultBezier, (k + 0.5) / n) / n;
    // The default curve is infinitely steep at u=0, so a 512-point table is within ~5e-5 of the exact
    // integral. Budgets are solved against the same table, so ramps still consume exactly S.
    CHECK_NEAR(t.integral[kEaseTableSize], sum, 1e-4, "table integral");

    // Custom ramp consumes exactly its footage budget and lands on 100%.
    RampInput in;
    in.inPoint = 0.0;
    in.outPoint = 8.0;
    in.rampIn = {1.0, 6.0, Ease::Custom};
    const Solved s = Solve(in);
    CHECK_NEAR(RampConsumed(s.rampIn, s.rampIn.duration), 6.0, 1e-9, "custom budget");
    CHECK_NEAR(RampSpeed(s.rampIn, 1.0), 1.0, 1e-9, "custom lands on 100%");
    CHECK_NEAR(Evaluate(s, 1.0).source, 1.0, 1e-9, "custom sync boundary");
}

static void TestLink()
{
    RampInput in;
    in.inPoint = 0.0;
    in.outPoint = 10.0;
    in.rampIn = {1.5, 9.0, Ease::Whip};
    in.rampOut = {0.3, 0.5, Ease::Linear};
    in.linkOut = true;
    in.sourceMin = -100.0; // ramp in reaches 7.5 s before the in-point; don't clamp for this check
    const Solved s = Solve(in);
    CHECK_NEAR(s.rampOut.duration, 1.5, 1e-12, "linked duration");
    CHECK_NEAR(s.rampOut.source, 9.0, 1e-12, "linked footage");
    CHECK(s.rampOut.ease == Ease::Whip, "linked ease");
    CHECK_NEAR(s.rampOut.peak, s.rampIn.peak, 1e-12, "linked peak");
    // Mirror symmetry around the layer centre (sync anchor): source(t) + source(L - t) = L.
    for (double t = 0.0; t <= 10.0; t += 0.25)
        CHECK_NEAR(Evaluate(s, t).source + Evaluate(s, 10.0 - t).source, 10.0, 1e-9, "linked mirror");
}

static void TestTransform()
{
    RampInput in;
    in.inPoint = 0.0;
    in.outPoint = 8.0;
    in.rampIn = {1.0, 5.0, Ease::Snap};
    in.rampOut = {1.0, 5.0, Ease::Snap};
    in.sourceMin = -100.0; // ramp in reaches before 0 s; don't clamp
    const Solved s = Solve(in);

    RampTransform tin;
    tin.scale = 130.0;
    tin.rotation = 20.0;
    tin.posX = 400.0;
    tin.posY = -50.0;
    tin.opacity = 0.0;
    RampTransform tout; // ignored when linked

    // At the cut: the full transform. In real-time: identity. At the boundary: identity (continuous).
    const RampTransform outCont = EffectiveOutTransform(tin, tout, true, OutDirection::Continue);
    TransformState a = TransformAt(s, tin, outCont, 0.0);
    CHECK_NEAR(a.scale, 1.3, 1e-12, "scale at cut");
    CHECK_NEAR(a.rotation, 20.0, 1e-12, "rotation at cut");
    CHECK_NEAR(a.dx, 400.0, 1e-12, "x at cut");
    CHECK_NEAR(a.opacity, 0.0, 1e-12, "opacity at cut");
    CHECK(TransformAt(s, tin, outCont, 4.0).IsIdentity(), "identity in real-time");
    CHECK(TransformAt(s, tin, outCont, 1.0).IsIdentity(), "identity at ramp-in boundary");
    const TransformState justBefore = TransformAt(s, tin, outCont, 1.0 - 1e-6);
    CHECK_NEAR(justBefore.dx, 0.0, 1e-6, "continuous into real-time");

    // Linked + continue: ramp out moves the other way at the cut (keeps going through the cut).
    const TransformState z = TransformAt(s, tin, outCont, 8.0);
    CHECK_NEAR(z.scale, 1.0 / 1.3, 1e-12, "continue: inverse scale");
    CHECK_NEAR(z.rotation, -20.0, 1e-12, "continue: opposite rotation");
    CHECK_NEAR(z.dx, -400.0, 1e-12, "continue: opposite position");
    CHECK_NEAR(z.opacity, 0.0, 1e-12, "continue: opacity dips the same way");

    // Linked + mirror: same as ramp in.
    const RampTransform outMirror = EffectiveOutTransform(tin, tout, true, OutDirection::Mirror);
    CHECK_NEAR(TransformAt(s, tin, outMirror, 8.0).dx, 400.0, 1e-12, "mirror: same position");

    // Not linked: ramp out uses its own values.
    tout.rotation = 90.0;
    CHECK_NEAR(TransformAt(s, tin, EffectiveOutTransform(tin, tout, false, OutDirection::Continue), 8.0).rotation,
               90.0, 1e-12, "unlinked out uses own values");

    // The weight follows the speed: (speed - 1) / (peak - 1) == 1 - e(u) == w.
    for (double t = 0.05; t < 1.0; t += 0.1)
    {
        const double w = (Evaluate(s, t).speed - 1.0) / (s.rampIn.peak - 1.0);
        CHECK_NEAR(TransformAt(s, tin, outCont, t).rotation, 20.0 * w, 1e-9, "transform linked to speed");
    }

    // Shutter samples report their timeline times.
    double st[kMaxShutterSamples], tt[kMaxShutterSamples];
    MotionBlur mb;
    const int n = ShutterSamples(s, 0.02, 0.04, mb, st, tt);
    CHECK(n > 1, "blurred at the cut");
    for (int i = 0; i < n; ++i)
        CHECK_NEAR(Evaluate(s, tt[i]).source, st[i], 1e-12, "timeline time matches source sample");
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
    TestBezier();
    TestLink();
    TestTransform();
    TestShutter();
    TestIntegrals();
    TestPeakSolve();
    TestNoReverse();
    TestMapping();
    TestEdgeCases();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
