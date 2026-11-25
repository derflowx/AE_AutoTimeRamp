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

// Ramp-transform case: a static horizontal gradient (value = x / W) seen through the transform.
struct XfCase
{
    std::string name;
    atr::Solved solved;
    atr::RampTransform in, outEff;
    int edges = 1; // 1 mirror, 2 transparent, 3 repeat
    double W = 200, H = 100;
    double pivX = -1, pivY = -1; // < 0: layer centre
    int points = 0, alphaOk = 0, valueOkU = 0, valueOkP = 0, valueChecked = 0;
    double maxAlphaErr = 0;
};

// Expected (value, alpha) at layer pixel position (px, py): inverse of the renderer's transform.
void ExpectXf(const XfCase& c, double t, double px, double py, double& value, double& alpha, bool& nearEdge)
{
    const atr::TransformState st = atr::TransformAt(c.solved, c.in, c.outEff, t);
    const double th = st.rotation * 3.14159265358979323846 / 180.0;
    const double s = std::max(1e-4, st.scale);
    const double a = std::cos(th) / s, b = std::sin(th) / s, cc = -std::sin(th) / s, d = std::cos(th) / s;
    const double pvx = c.pivX >= 0 ? c.pivX : c.W / 2, pvy = c.pivY >= 0 ? c.pivY : c.H / 2;
    const double ux = px - pvx - st.dx, uy = py - pvy - st.dy;
    double qx = pvx + a * ux + b * uy;
    double qy = pvy + cc * ux + d * uy;
    nearEdge = false;
    auto wrap = [&](double& v, double size) {
        // Edges are anti-aliased: bilinear sampling + sampleImage's averaging blend over ~2 output px,
        // i.e. ~2/scale source px.
        const double m = 1.0 + 1.5 / s;
        if (std::fabs(v) < m || std::fabs(v - size) < m)
            nearEdge = true;
        if (v >= 0 && v <= size)
            return true;
        if (c.edges == 2)
            return false;
        if (c.edges == 3)
        {
            v = std::fmod(v, size);
            if (v < 0)
                v += size;
            if (v < m || v > size - m)
                nearEdge = true;
            return true;
        }
        double m2 = std::fmod(v, 2 * size);
        if (m2 < 0)
            m2 += 2 * size;
        v = m2 <= size ? m2 : 2 * size - m2;
        return true;
    };
    const bool inside = wrap(qx, c.W) && wrap(qy, c.H);
    alpha = inside ? st.opacity : 0.0;
    value = qx / c.W;
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
    std::vector<XfCase> xfCases;
    std::string line;
    bool failed = false;
    while (std::getline(in, line))
    {
        if (line.empty())
            continue;
        const auto f = Split(line);
        if (f[0] == "#xfcase" && f.size() >= 25)
        {
            // name, in, out, inDur, inSrc, inEase, outDur, outSrc, outEase, link, dir, edges, W, H, xin[5], xout[5]
            XfCase c;
            c.name = f[1];
            atr::RampInput ri;
            ri.inPoint = std::atof(f[2].c_str());
            ri.outPoint = std::atof(f[3].c_str());
            ri.rampIn = {std::atof(f[4].c_str()), std::atof(f[5].c_str()), atr::Ease(std::atoi(f[6].c_str()) - 1)};
            ri.rampOut = {std::atof(f[7].c_str()), std::atof(f[8].c_str()), atr::Ease(std::atoi(f[9].c_str()) - 1)};
            ri.linkOut = std::atoi(f[10].c_str()) != 0;
            ri.sourceMin = -1.0e9;
            c.solved = atr::Solve(ri);
            const atr::OutDirection dir =
                std::atoi(f[11].c_str()) == 2 ? atr::OutDirection::Mirror : atr::OutDirection::Continue;
            c.edges = std::atoi(f[12].c_str());
            c.W = std::atof(f[13].c_str());
            c.H = std::atof(f[14].c_str());
            atr::RampTransform xin, xout;
            double* vi[5] = {&xin.scale, &xin.rotation, &xin.posX, &xin.posY, &xin.opacity};
            double* vo[5] = {&xout.scale, &xout.rotation, &xout.posX, &xout.posY, &xout.opacity};
            for (int k = 0; k < 5; ++k)
            {
                *vi[k] = std::atof(f[15 + k].c_str());
                *vo[k] = std::atof(f[20 + k].c_str());
            }
            if (f.size() >= 27) // optional pivot (layer px)
            {
                c.pivX = std::atof(f[25].c_str());
                c.pivY = std::atof(f[26].c_str());
            }
            c.in = xin;
            c.outEff = atr::EffectiveOutTransform(xin, xout, ri.linkOut, dir);
            xfCases.push_back(c);
            continue;
        }
        if (f[0] == "xf" && f.size() >= 7 && !xfCases.empty())
        {
            XfCase& c = xfCases.back();
            const double t = std::atof(f[2].c_str()), px = std::atof(f[3].c_str()), py = std::atof(f[4].c_str());
            const double gotV = std::atof(f[5].c_str()), gotA = std::atof(f[6].c_str());
            double wantV = 0, wantA = 0;
            bool nearEdge = false;
            ExpectXf(c, t, px, py, wantV, wantA, nearEdge);
            ++c.points;
            const double aErr = std::fabs(gotA - wantA);
            if (!nearEdge)
                c.maxAlphaErr = std::max(c.maxAlphaErr, aErr);
            if (aErr < 0.02 || nearEdge)
                ++c.alphaOk;
            if (wantA > 0.1 && !nearEdge)
            {
                ++c.valueChecked;
                if (std::fabs(gotV - wantV) < 0.02)
                    ++c.valueOkU; // sampleImage returns straight colour
                if (std::fabs(gotV - wantV * wantA) < 0.02)
                    ++c.valueOkP; // ... or premultiplied
            }
            continue;
        }
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
    if (!xfCases.empty())
    {
        std::printf("\n%-24s %7s %9s %12s %14s %s\n", "transform case", "points", "alpha ok", "max a err",
                    "position ok", "result");
        for (const XfCase& c : xfCases)
        {
            const int valueOk = std::max(c.valueOkU, c.valueOkP);
            const bool ok = c.points > 0 && c.alphaOk == c.points && c.valueChecked > 0 &&
                            valueOk >= c.valueChecked * 97 / 100;
            failed = failed || !ok;
            std::printf("%-24s %7d %9d %12.4f %8d/%-5d %s\n", c.name.c_str(), c.points, c.alphaOk, c.maxAlphaErr,
                        valueOk, c.valueChecked, ok ? "PASS" : "FAIL");
        }
    }
    return failed ? 1 : 0;
}
