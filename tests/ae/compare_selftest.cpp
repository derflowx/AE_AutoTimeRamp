// Compares selftest_result.csv (source times AE actually rendered) against RampMath.
// Usage: compare_selftest.exe selftest_result.csv
#include "../../src/core/RampMath.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace
{
constexpr double kFps = 25.0;
constexpr double kSrcDur = 120.0;

std::vector<std::string> Split(const std::string& s)
{
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, ','))
        out.push_back(item);
    return out;
}

struct Case
{
    std::string name;
    atr::Solved solved;
    int bpc = 32;
    atr::MotionBlur blur;
    int frames = 0;
    double maxErrFrames = 0.0; // vs exact (unquantised) source time
    double worstT = 0.0;
    int offByMoreThanHalfFrame = 0;
    double maxGreenErr = 0.0;  // green = 0.5 + 0.5 sin(2 pi t), averaged over the shutter samples
    int blurVisibleFrames = 0; // frames where the result clearly differs from an unblurred render
};

double Green(double t)
{
    return 0.5 + 0.5 * std::sin(2.0 * 3.14159265358979323846 * t);
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("usage: compare_selftest <csv>\n");
        return 2;
    }
    std::ifstream in(argv[1]);
    if (!in)
    {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }

    std::vector<Case> cases;
    std::string line;
    bool failed = false;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const auto f = Split(line);
        if (f[0] == "#param")
        {
            std::printf("param %s: %s (%s)\n", f[1].c_str(), f[2].c_str(), f[3].c_str());
            continue;
        }
        if (f[0] == "#error" || f[0] == "#cleanup error")
        {
            std::printf("AE %s\n", line.c_str());
            failed = true;
            continue;
        }
        if (f[0] == "#info")
        {
            std::printf("AE %s\n", line.c_str());
            continue;
        }
        if (f[0] != "#case" && (f[0].empty() || f[0][0] == '#' || f.size() < 3))
            continue; // #done and anything unknown
        if (f[0] == "#case")
        {
            atr::RampInput ri;
            ri.inPoint = std::atof(f[2].c_str());
            ri.outPoint = std::atof(f[3].c_str());
            ri.rampIn = {std::atof(f[4].c_str()), std::atof(f[5].c_str()), atr::Ease(std::atoi(f[6].c_str()) - 1)};
            ri.rampOut = {std::atof(f[7].c_str()), std::atof(f[8].c_str()), atr::Ease(std::atoi(f[9].c_str()) - 1)};
            ri.anchor = atr::Anchor(std::atoi(f[10].c_str()) - 1);
            if (f.size() > 23) // link + in/out bezier (v0.3+)
            {
                ri.linkOut = std::atoi(f[15].c_str()) != 0;
                for (int b = 0; b < 4; ++b)
                {
                    ri.rampIn.bezier[b] = std::atof(f[16 + b].c_str());
                    ri.rampOut.bezier[b] = std::atof(f[20 + b].c_str());
                }
            }
            ri.sourceMin = 0.0;
            ri.sourceMax = kSrcDur - 1.0 / kFps;
            Case c;
            c.name = f[1];
            c.solved = atr::Solve(ri);
            c.bpc = std::atoi(f[11].c_str());
            c.blur.enabled = f.size() > 14 && std::atoi(f[12].c_str()) != 0;
            if (f.size() > 14)
            {
                c.blur.shutterAngle = std::atof(f[13].c_str());
                c.blur.maxSamples = std::atoi(f[14].c_str());
            }
            cases.push_back(c);
            continue;
        }
        if (cases.empty())
            continue;
        Case& c = cases.back();
        const double t = std::atof(f[1].c_str());
        const double got = std::atof(f[2].c_str());
        // Expected pixel = average of the shutter samples (red channel is linear in source time).
        double st[atr::kMaxShutterSamples];
        const int n = atr::ShutterSamples(c.solved, t, 1.0 / kFps, c.blur, st);
        double want = 0.0;
        for (int i = 0; i < n; ++i)
            want += st[i] / n;
        const double errFrames = std::fabs(got - want) * kFps;
        if (f.size() > 3)
        {
            double wantG = 0.0;
            for (int i = 0; i < n; ++i)
                wantG += Green(st[i]) / n;
            const double gotG = std::atof(f[3].c_str());
            c.maxGreenErr = std::max(c.maxGreenErr, std::fabs(gotG - wantG));
            const double unblurredG = Green(atr::Evaluate(c.solved, t).source);
            if (std::fabs(wantG - unblurredG) > 0.05 && std::fabs(gotG - unblurredG) > 0.05)
                ++c.blurVisibleFrames;
        }
        ++c.frames;
        if (errFrames > c.maxErrFrames)
        {
            c.maxErrFrames = errFrames;
            c.worstT = t;
        }
        if (errFrames > 0.5 + 1e-3)
            ++c.offByMoreThanHalfFrame;
    }

    std::printf("\n%-16s %4s %6s %13s %8s %10s %8s %s\n", "case", "bpc", "frames", "max err (fr)", "at t",
                "green err", "blurred", "result");
    for (const Case& c : cases)
    {
        // Effect rounds the requested time to AE's time grid (<= half a frame). 8 bpc can only encode
        // 120 s / 255 steps, so it is a smoke test with a coarse tolerance.
        const double tol = c.bpc == 8 ? (kSrcDur / 255.0) * kFps : 0.5 + 1e-3;
        // Green tolerance covers 8-bit quantisation (1/255) plus float noise.
        const bool greenOk = c.maxGreenErr <= 0.01;
        // A blur case must visibly differ from an unblurred render somewhere; a no-blur case never.
        const bool blurOk = c.blur.enabled ? c.blurVisibleFrames > 0 : c.blurVisibleFrames == 0;
        const bool ok = c.frames > 0 && c.maxErrFrames <= tol && greenOk && blurOk;
        failed = failed || !ok;
        std::printf("%-16s %4d %6d %13.3f %8.3f %10.4f %8d %s\n", c.name.c_str(), c.bpc, c.frames, c.maxErrFrames,
                    c.worstT, c.maxGreenErr, c.blurVisibleFrames, ok ? "PASS" : "FAIL");
    }
    return failed ? 1 : 0;
}
