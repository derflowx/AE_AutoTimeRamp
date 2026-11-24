// Real-time marker: one layer duration marker spanning the real-time zone, kept in sync with the
// ramp settings and the layer's trim.
//
// - SyncMarker() is called when an effect param changes (inside that change's undo step).
// - An idle hook watches layers with Auto Time Ramp in the active comp and re-syncs after trims.
//   It only acts when the layer's timing or settings changed since it last looked, so undoing a
//   marker update doesn't get re-applied straight away.
#pragma once

#include "Settings.h"

// Comment that identifies our marker. Other markers on the layer are never touched.
#define ATR_MARKER_COMMENT L"Real-time (Auto Time Ramp)"
#define ATR_MARKER_LABEL 9 // AE label colour index: Green

struct MarkerZone
{
    bool want = false;    // marker should exist
    double start = 0.0;   // layer time, seconds
    double duration = 0.0;
};

MarkerZone ComputeMarkerZone(const Settings& s, const atr::Solved& solved);

// Returns true if the marker was changed. frameSeconds = comp frame duration (match tolerance).
bool SyncMarker(AEGP_LayerH layerH, const MarkerZone& zone, double frameSeconds, bool dryRun = false);

void RegisterMarkerIdleHook();
