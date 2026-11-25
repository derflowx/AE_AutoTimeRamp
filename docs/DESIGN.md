# Auto Time Ramp — design

Native C++ After Effects effect (AE SDK, `.aex`) that speed-ramps a layer in and out at its
in/out points, derived entirely from the layer's trimmed length. No Time Remap property, no keyframes.
Works on footage and precomp layers. Primary use: whoosh/snap cuts between shots.

## Zones

```
| ramp in | ........ real-time (100%) ........ | ramp out |
in-point                                            out-point
```

- **Ramp in / ramp out**: fixed timeline durations (seconds), set in the effect.
- **Real-time**: whatever remains = layer length − ramp in − ramp out. Trimming the layer only
  changes the real-time length.

## Source budget

Each ramp has two inputs:

- `duration` R — timeline seconds
- `source` S — seconds of footage the ramp consumes

Speed within a ramp: `v(u) = v_peak + (1 − v_peak) · e(u)`, `u ∈ [0,1]` running from the cut
toward the real-time zone, `e` = ease preset with `e(0)=0, e(1)=1`.
Footage used: `S = R · (v_peak + (1 − v_peak) · Ē)` where `Ē = ∫₀¹ e(u) du`, so

```
v_peak = (S/R − Ē) / (1 − Ē)        clamped to ≥ 0
```

- S > R → fast ramp (whoosh). Example: cubic snap, R = 1 s, S = 5 s → ~1700 % at the cut.
- S < R → slow ramp; minimum S = R·Ē gives a freeze frame at the cut.
- Speed lands on exactly 100 % at the zone boundary (no speed jump).
- Source time is the integral of speed; all presets need closed-form (or precomputed) integrals.

## Anchor

- **Sync (default)**: real-time zone plays at the layer's natural source time. Choose the content
  by slip-editing the footage inside the layer. Ramp in eats footage before it, ramp out after it.
- **Head**: first frame = source frame at the in-point.
- Out-of-footage: hold the last available frame and flag it red in the UI graph.

## Presets

Snap family (fastest change at the cut, gentle landing into 100 %):
Soft snap (quad out), Snap (cubic out), Hard snap (quart out), Whip (quint out), Expo snap,
Circ snap, Settle (slight dip under 100 %, limited so speed never goes negative).

Others: Smooth (sine in-out), Linear, Custom (cubic-bezier x1 y1 x2 y2).
In and out have separate presets with a link checkbox.

## Effect features

- Render: check out own layer at the remapped source time.
- Speed-based motion blur (v0.2, done): centred shutter of `angle/360` frames; samples spread evenly
  across it and mapped through the ramp. Sample count = source frames swept (rounded up), capped by
  Max samples. Under one swept frame, a single sample at t keeps the image untouched. Box filter over
  premultiplied pixels (8/16/32 bpc). Source times go to AE on a grid of at least 1/6000 s, so precomps
  get sub-frame samples. Frame blending for slow parts is still open.
  Revisit later, once the rest is done: footage at ~2–4× can show stepping (only 2–4 distinct frames
  averaged). Options are fractional sample weights to fade the blur in smoothly (cheap) or optional
  optical-flow in-betweens for footage (big).
- Effect Controls custom UI (v0.3): speed graph with zones, peak speeds, footage used, warnings.
  Drawn with Drawbot on a colour param with `PF_PUI_CONTROL` (its value is unused).
- Real-time marker (v0.3, display only): one layer duration marker spanning the real-time zone,
  identified by a hidden cue-point parameter `AutoTimeRamp=real-time` (no visible text, since v0.5; older
  markers identified by the comment "Real-time (Auto Time Ramp)" are migrated). Synced in `USER_CHANGED_PARAM` (inside the param change's undo step) and
  by an idle hook that scans the active comp's layers with the effect and re-syncs after trims (its own
  undo step). The hook only acts when a layer's timing or settings changed since it last looked, so an
  undone marker update isn't re-applied in a loop. On first sight (project open) it only creates a
  missing marker; it doesn't rewrite an existing one.
- Custom bezier (v0.3): CSS-style cubic bezier for the ease, tabulated (512 points) with an exact
  piecewise integral so the footage budget stays exact; clamped to [0,1] so speed never goes negative.
- Ramp out "Same as ramp in" (v0.3): mirrors duration, footage and ease. Bezier sliders are hidden
  unless the ease is Custom (dynamic stream flags); linked ramp-out controls are greyed out.
- Params are matched by disk ID, so new params were inserted into their groups. Saved v0.1/v0.2
  projects should load with the new params at their defaults (not yet verified with a saved project).

## Ramp transform (v0.4)

Scale/rotation/position/opacity at each cut with weight `w = 1 - e(u)` (the ramp's ease; equals
`(speed - 1)/(peak - 1)`), scale in log space. A linked ramp out either continues through the cut
(inverse scale, negated rotation/position, same opacity dip) or mirrors ramp in. The transform is
evaluated per shutter sample at its timeline time, so the transform motion is blurred too. Rendering
requests the whole layer, maps each output pixel through the sample's inverse affine (pivot = the
"Transform pivot" point param, default layer centre, v0.6; square-pixel space via the pixel aspect ratio), samples bilinearly with Mirror/Transparent/
Repeat edges, multiplies by opacity and averages.

Since v0.6 the per-pixel accumulation (blur average and transform) runs in 16-row bands through
`PF_Iterate8Suite2::iterate_generic`, so AE spreads one frame across its render threads. Each band
writes only its own rows. If the suite is unavailable it falls back to one thread.

Lesson: the effect must declare `PF_OutFlag_NON_PARAM_VARY`. Its output depends on the current time,
not only on params and input, and without the flag AE rendered a static input (still, solid, static
precomp) once and reused that frame for every time.

## Crash during v0.3 testing (2026-11-24)

AE crashed once, about a second after the marker test applied the effect, and once more 8 s into a
launch whose project never contained the effect. The crash report was uploaded and deleted, so the
cause is unknown. Repeated `AfterFX -r` probes had started AE several times in a row around then. Since
then the plug-in has a crash guard (Guard.h) and three full test runs plus a manual demo ran clean
with an empty crash log. Watch `%TEMP%\AutoTimeRamp_crash.log` if AE crashes again.

## Technical risks (prototype first)

Status 2026-11-24 (v0.1, AE 26.5): risks 1–3 verified. Frames beyond the trim render correctly;
no errors with Multi-Frame Rendering on; trimming re-renders correctly. The automated self-test
matches RampMath to within 0.001 frames at 32 bpc. Risk 4 is still open.


1. Can the effect check out its own layer at times outside the layer's in/out (handles)?
2. Reading layer in/out + markers safely under Multi-Frame Rendering. (Likely fine: the SDK's
   SmartyPants sample calls AEGP layer suites in PreRender with threaded rendering enabled.)
3. Cache invalidation when the layer is trimmed (mix in/out into the cache GUID in PreRender, implemented in v0.1).

Build gotcha: the SDK's `PiPLTool.exe` silently writes an empty resource when its file arguments
contain spaces; the project runs it from the intermediate folder with relative names.
4. Updating the marker on trim via an idle hook registered from the effect.

## Build phases

1. Toolchain: AE SDK + Visual Studio 2022; build and load an SDK sample.
2. Spike for risks 1–3.
3. Ramp math as a pure C++ library with standalone unit tests.
4. Effect render + params.
5. Effect Controls graph + marker sync.
6. Motion blur / frame blending.
7. macOS build (optional).
