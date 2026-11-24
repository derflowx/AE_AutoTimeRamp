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
- Speed-based motion blur: above a threshold speed, average multiple source samples per frame
  (shutter angle, sample count). Optional frame blending for slow parts.
- Effect Controls custom UI: speed graph with zones, peak speeds, footage used, warnings.
- One layer duration marker spanning the real-time zone, kept updated by the effect (display only).

## Technical risks (prototype first)

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
