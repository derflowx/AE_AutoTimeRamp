# Auto Time Ramp

After Effects effect (native C++, AE SDK) that speed-ramps a layer into and out of real-time at its
in/out points. No Time Remap, no keyframes: everything is derived from the layer's trimmed length.
Design: [docs/DESIGN.md](docs/DESIGN.md).

Status: **v0.1 spike**: ramps, ease presets and anchors work; no motion blur, Effect Controls graph
or markers yet.

## Layout

| Path | What |
|---|---|
| `src/core/RampMath.*` | Pure C++ ramp math (no SDK). Speed curves, peak solve, source-time mapping. |
| `src/effect/` | SmartFX effect + PiPL resource. |
| `tests/test_rampmath.cpp` | Standalone unit tests for the math. |
| `win/AutoTimeRamp.vcxproj` | VS2022 (v143) project. |
| `build.ps1` | Build helper. |

## Build (Windows)

Needs Visual Studio 2022 with the C++ desktop workload and the AE SDK (not in this repo).

1. Copy `win\local.props.example` to `win\local.props` and set `AE_SDK` to the SDK folder
   (the one containing `Examples\Headers`).
2. Build:

```powershell
.\build.ps1            # Release .aex -> build\Release\AutoTimeRamp.aex
.\build.ps1 -Tests     # RampMath unit tests
.\build.ps1 -Config Debug
```

## Install for development

AE's plug-in folder needs admin rights. Once, from an **admin** PowerShell, link the build folder into AE:

```powershell
New-Item -ItemType Junction -Path "C:\Program Files\Adobe\Adobe After Effects 2026\Support Files\Plug-ins\AutoTimeRamp" -Target "<repo>\build\Release"
```

After that every build is picked up on the next AE launch. Close AE before rebuilding: AE locks the `.aex`.

The effect appears under **Effect > Time > Auto Time Ramp**.

## Effect controls

- **Ramp in / Ramp out**
  - *Duration (s)*: timeline length of the ramp.
  - *Footage used (s)*: how much source the ramp runs through. More than the duration means a
    fast whoosh ramp (peak speed is solved automatically); less means a slow ramp, down to a freeze.
  - *Ease*: Snap, Soft snap, Hard snap, Whip, Expo snap, Circ snap, Settle, Smooth, Linear.
- **Anchor**
  - *Sync*: the real-time part plays at its natural time. Pick the content by slip-editing
    (Alt-drag the layer content). The ramps use footage before and after it.
  - *Head*: starts on the in-point frame.
  - *Tail*: ends on the out-point frame.

Real-time length = layer length − ramp in − ramp out. If the ramps don't fit, both shrink proportionally.

## Spike test checklist

Use a precomp that shows its own time, e.g. a 60 s comp with a solid + the **Timecode** effect, placed
in a main comp and trimmed to 7 s with plenty of handle on both sides. Apply Auto Time Ramp with the
defaults (1 s / 5 s / Snap on both sides).

1. **Handles**: during the first second the timecode races through ~5 s of time and lands on
   natural speed. Black or frozen frames here mean reading frames outside the trim doesn't work.
2. **Render thread**: no error dialogs with Multi-Frame Rendering on (Preferences > Memory & Performance).
3. **Cache**: RAM-preview, then trim the out point. The ramp out must move with it, with no stale frames.
4. **Bit depth**: switch the project to 16 and 32 bpc; the image must stay correct.
5. **Footage**: repeat with a long video clip (5 min), trimmed to 7 s in the middle.
