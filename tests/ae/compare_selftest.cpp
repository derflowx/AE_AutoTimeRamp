// Compares selftest_result.csv (source times AE actually rendered) against RampMath.
// Usage: compare_selftest.exe selftest_result.csv
#include "../../src/core/RampMath.h"

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
    int frames = 0;
    double maxErrFrames = 0.0; // vs exact (unquantised) source time
    double worstT = 0.0;
    int offByMoreThanHalfFrame = 0;
};
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
            ri.sourceMin = 0.0;
            ri.sourceMax = kSrcDur - 1.0 / kFps;
            Case c;
            c.name = f[1];
            c.solved = atr::Solve(ri);
            c.bpc = std::atoi(f[11].c_str());
            cases.push_back(c);
            continue;
        }
        if (cases.empty())
            continue;
        Case& c = cases.back();
        const double t = std::atof(f[1].c_str());
        const double got = std::atof(f[2].c_str());
        const double want = atr::Evaluate(c.solved, t).source;
        const double errFrames = std::fabs(got - want) * kFps;
        ++c.frames;
        if (errFrames > c.maxErrFrames)
        {
            c.maxErrFrames = errFrames;
            c.worstT = t;
        }
        if (errFrames > 0.5 + 1e-3)
            ++c.offByMoreThanHalfFrame;
    }

    std::printf("\n%-16s %4s %6s %14s %10s %s\n", "case", "bpc", "frames", "max err (fr)", "at t", "result");
    for (const Case& c : cases)
    {
        // Effect rounds the requested time to AE's time grid (<= half a frame). 8 bpc can only encode
        // 120 s / 255 steps, so it is a smoke test with a coarse tolerance.
        const double tol = c.bpc == 8 ? (kSrcDur / 255.0) * kFps : 0.5 + 1e-3;
        const bool ok = c.frames > 0 && c.maxErrFrames <= tol;
        failed = failed || !ok;
        std::printf("%-16s %4d %6d %14.3f %10.3f %s\n",
                    c.name.c_str(), c.bpc, c.frames, c.maxErrFrames, c.worstT, ok ? "PASS" : "FAIL");
    }
    return failed ? 1 : 0;
}
