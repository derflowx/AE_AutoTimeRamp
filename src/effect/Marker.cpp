#include "Marker.h"
#include "Guard.h"

#include <cmath>
#include <cstring>
#include <cwchar>
#include <map>
#include <string>

#ifdef AE_OS_WIN
    #include <Windows.h> // GetTickCount64
#endif

namespace
{
constexpr A_u_long kMarkerTimeScale = 120000; // exact for 23.976/24/25/29.97/30/48/50/59.94/60 fps

A_Time ToATime(double seconds)
{
    A_Time t;
    t.scale = kMarkerTimeScale;
    t.value = static_cast<A_long>(std::llround(seconds * kMarkerTimeScale));
    return t;
}

struct FoundMarker
{
    bool exists = false;
    bool legacy = false; // still carries the old visible comment: rewrite it
    AEGP_KeyframeIndex index = 0;
    double start = 0.0;
    double duration = 0.0;
};

// Scan the layer's marker stream for our marker. `otherAt` (optional) reports whether some other
// marker sits exactly at `probeTime`, so we never overwrite a user's marker.
A_Err FindOurMarker(AEGP_StreamRefH streamH, FoundMarker& found, double probeTime = -1.0e9, bool* otherAt = nullptr)
{
    A_Err err = A_Err_NONE;
    AEFX_SuiteScoper<AEGP_KeyframeSuite5> kf(g_pica, kAEGPKeyframeSuite, kAEGPKeyframeSuiteVersion5);
    AEFX_SuiteScoper<AEGP_MarkerSuite3> mk(g_pica, kAEGPMarkerSuite, kAEGPMarkerSuiteVersion3);
    AEFX_SuiteScoper<AEGP_MemorySuite1> mem(g_pica, kAEGPMemorySuite, kAEGPMemorySuiteVersion1);
    AEFX_SuiteScoper<AEGP_StreamSuite6> streams(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);

    A_long n = 0;
    err = kf->AEGP_GetStreamNumKFs(streamH, &n);
    for (A_long i = 0; !err && i < n; ++i)
    {
        A_Time t = {0, 1};
        if (kf->AEGP_GetKeyframeTime(streamH, i, AEGP_LTimeMode_LayerTime, &t) != A_Err_NONE)
            continue;
        AEGP_StreamValue2 val;
        AEFX_CLR_STRUCT(val);
        if (kf->AEGP_GetNewKeyframeValue(g_pluginId, streamH, i, &val) != A_Err_NONE)
            continue;
        // UTF-16 string out of an AEGP mem handle (freed here).
        auto take = [&](AEGP_MemHandle h) {
            std::wstring out;
            if (!h)
                return out;
            void* p = nullptr;
            if (mem->AEGP_LockMemHandle(h, &p) == A_Err_NONE && p)
            {
                out = reinterpret_cast<const wchar_t*>(p);
                mem->AEGP_UnlockMemHandle(h);
            }
            mem->AEGP_FreeMemHandle(h);
            return out;
        };
        bool ours = false, legacy = false;
        if (val.val.markerP)
        {
            AEGP_MemHandle commentH = nullptr;
            if (mk->AEGP_GetMarkerString(g_pluginId, val.val.markerP, AEGP_MarkerString_COMMENT, &commentH) ==
                A_Err_NONE)
                legacy = take(commentH) == ATR_MARKER_LEGACY_COMMENT;
            A_long params = 0;
            mk->AEGP_CountCuePointParams(val.val.markerP, &params);
            for (A_long k = 0; k < params && !ours; ++k)
            {
                AEGP_MemHandle keyH = nullptr, valueH = nullptr;
                if (mk->AEGP_GetIndCuePointParam(g_pluginId, val.val.markerP, k, &keyH, &valueH) == A_Err_NONE)
                {
                    const std::wstring key = take(keyH), value = take(valueH);
                    ours = key == ATR_MARKER_KEY && value == ATR_MARKER_VALUE;
                }
            }
            ours = ours || legacy;
        }
        if (ours && !found.exists)
        {
            A_Time d = {0, 1};
            mk->AEGP_GetMarkerDuration(val.val.markerP, &d);
            found.exists = true;
            found.legacy = legacy;
            found.index = i;
            found.start = ToSeconds(t);
            found.duration = ToSeconds(d);
        }
        else if (!ours && otherAt && std::fabs(ToSeconds(t) - probeTime) < 1.0e-6)
        {
            *otherAt = true;
        }
        streams->AEGP_DisposeStreamValue(&val);
    }
    return err;
}

// ---- Idle hook -----------------------------------------------------------------------------------

struct SeenState
{
    double inPoint, outPoint, start, duration;
    bool want;
    bool operator!=(const SeenState& o) const
    {
        auto ne = [](double a, double b) { return std::fabs(a - b) > 1.0e-6; };
        return ne(inPoint, o.inPoint) || ne(outPoint, o.outPoint) || ne(start, o.start) ||
               ne(duration, o.duration) || want != o.want;
    }
};

std::map<unsigned long long, SeenState> g_seen;
AEGP_InstalledEffectKey g_ourKey = AEGP_InstalledEffectKey_NONE;
unsigned long long g_lastRunMs = 0;

bool IsOurEffect(AEGP_EffectRefH effH)
{
    AEFX_SuiteScoper<AEGP_EffectSuite5> fx(g_pica, kAEGPEffectSuite, kAEGPEffectSuiteVersion5);
    AEGP_InstalledEffectKey key = AEGP_InstalledEffectKey_NONE;
    if (fx->AEGP_GetInstalledKeyFromLayerEffect(effH, &key) != A_Err_NONE)
        return false;
    if (g_ourKey != AEGP_InstalledEffectKey_NONE)
        return key == g_ourKey;
    A_char name[AEGP_MAX_EFFECT_MATCH_NAME_SIZE + 1] = {};
    if (fx->AEGP_GetEffectMatchName(key, name) == A_Err_NONE && std::strcmp(name, ATR_MATCH_NAME) == 0)
    {
        g_ourKey = key;
        return true;
    }
    return false;
}

void RefreshWindows()
{
    try
    {
        AEFX_SuiteScoper<PF_AdvAppSuite2> app(g_pica, kPFAdvAppSuite, kPFAdvAppSuiteVersion2);
        app->PF_RefreshAllWindows();
    }
    catch (...)
    {
    }
}

void ProcessLayer(AEGP_LayerH layerH, AEGP_EffectRefH effH, A_long compId, double frameSeconds)
{
    AEFX_SuiteScoper<AEGP_LayerSuite9> layers(g_pica, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
    AEFX_SuiteScoper<AEGP_UtilitySuite6> util(g_pica, kAEGPUtilitySuite, kAEGPUtilitySuiteVersion6);

    ATR_STEP("idle:read_timing");
    LayerTiming timing;
    if (ReadLayerTimingAEGP(g_pica, layerH, timing) != A_Err_NONE)
        return;
    ATR_STEP("idle:read_settings");
    Settings s;
    A_Time at = ToATime(timing.inPoint);
    if (ReadSettingsAEGP(g_pluginId, effH, at, s) != A_Err_NONE)
        return;
    const atr::Solved solved = BuildRamp(s, timing, frameSeconds);
    const MarkerZone zone = ComputeMarkerZone(s, solved);

    AEGP_LayerIDVal layerId = 0;
    if (layers->AEGP_GetLayerID(layerH, &layerId) != A_Err_NONE)
        return;
    const unsigned long long key = (static_cast<unsigned long long>(static_cast<A_u_long>(compId)) << 32) |
                                   static_cast<A_u_long>(layerId);
    const SeenState now = {timing.inPoint, timing.outPoint, zone.start, zone.duration, zone.want};

    auto it = g_seen.find(key);
    if (it == g_seen.end())
    {
        // First look at this layer (project just opened, or effect just applied): only create the
        // marker if ours is missing entirely; don't rewrite an existing one the user may have kept.
        g_seen[key] = now;
        AEFX_SuiteScoper<AEGP_StreamSuite6> streams(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
        AEGP_StreamRefH streamH = nullptr;
        if (streams->AEGP_GetNewLayerStream(g_pluginId, layerH, AEGP_LayerStream_MARKER, &streamH) != A_Err_NONE)
            return;
        ATR_STEP("idle:first_sight_find");
        FoundMarker found;
        FindOurMarker(streamH, found);
        streams->AEGP_DisposeStream(streamH);
        if ((found.exists && !found.legacy) || !zone.want)
            return;
    }
    else
    {
        if (!(it->second != now))
            return; // nothing changed since last look (also: a just-undone marker stays undone)
        it->second = now;
        RefreshWindows(); // graph depends on the trim too
    }

    ATR_STEP("idle:sync_dryrun");
    if (!SyncMarker(layerH, zone, frameSeconds, /*dryRun*/ true))
        return;
    ATR_STEP("idle:sync");
    util->AEGP_StartUndoGroup("Auto Time Ramp marker");
    SyncMarker(layerH, zone, frameSeconds);
    util->AEGP_EndUndoGroup();
}

A_Err IdleHookImpl(A_long* max_sleepPL)
{
    ATR_STEP("idle:start");
    try
    {
#ifdef AE_OS_WIN
        const unsigned long long nowMs = GetTickCount64();
        if (nowMs - g_lastRunMs < 250)
            return A_Err_NONE;
        g_lastRunMs = nowMs;
#endif
        AEFX_SuiteScoper<AEGP_ItemSuite9> items(g_pica, kAEGPItemSuite, kAEGPItemSuiteVersion9);
        AEFX_SuiteScoper<AEGP_CompSuite12> comps(g_pica, kAEGPCompSuite, kAEGPCompSuiteVersion12);
        AEFX_SuiteScoper<AEGP_LayerSuite9> layers(g_pica, kAEGPLayerSuite, kAEGPLayerSuiteVersion9);
        AEFX_SuiteScoper<AEGP_EffectSuite5> fx(g_pica, kAEGPEffectSuite, kAEGPEffectSuiteVersion5);

        AEGP_ItemH itemH = nullptr;
        if (items->AEGP_GetActiveItem(&itemH) != A_Err_NONE || !itemH)
            return A_Err_NONE;
        AEGP_ItemType type = AEGP_ItemType_NONE;
        if (items->AEGP_GetItemType(itemH, &type) != A_Err_NONE || type != AEGP_ItemType_COMP)
            return A_Err_NONE;
        AEGP_CompH compH = nullptr;
        A_long compId = 0;
        if (comps->AEGP_GetCompFromItem(itemH, &compH) != A_Err_NONE || !compH ||
            items->AEGP_GetItemID(itemH, &compId) != A_Err_NONE)
            return A_Err_NONE;
        A_Time frameT = {1, 25};
        comps->AEGP_GetCompFrameDuration(compH, &frameT);
        const double frameSeconds = ToSeconds(frameT);

        A_long numLayers = 0;
        layers->AEGP_GetCompNumLayers(compH, &numLayers);
        for (A_long li = 0; li < numLayers; ++li)
        {
            AEGP_LayerH layerH = nullptr;
            if (layers->AEGP_GetCompLayerByIndex(compH, li, &layerH) != A_Err_NONE || !layerH)
                continue;
            A_long numFx = 0;
            if (fx->AEGP_GetLayerNumEffects(layerH, &numFx) != A_Err_NONE)
                continue;
            for (A_long ei = 0; ei < numFx; ++ei)
            {
                AEGP_EffectRefH effH = nullptr;
                if (fx->AEGP_GetLayerEffectByIndex(g_pluginId, layerH, ei, &effH) != A_Err_NONE || !effH)
                    continue;
                ATR_STEP("idle:is_ours");
                const bool ours = IsOurEffect(effH);
                if (ours)
                    ProcessLayer(layerH, effH, compId, frameSeconds);
                fx->AEGP_DisposeEffect(effH);
                if (ours)
                    break; // one instance per layer drives the marker
            }
        }
    }
    catch (...)
    {
        // Never let exceptions reach AE.
    }
    if (max_sleepPL && *max_sleepPL > 15)
        *max_sleepPL = 15; // ~1/4 s
    return A_Err_NONE;
}

} // namespace

MarkerZone ComputeMarkerZone(const Settings& s, const atr::Solved& solved)
{
    MarkerZone z;
    z.start = solved.RealTimeStart();
    z.duration = std::max(0.0, solved.RealTimeEnd() - z.start);
    z.want = s.marker && z.duration > 1.0e-6;
    return z;
}

bool SyncMarker(AEGP_LayerH layerH, const MarkerZone& zone, double frameSeconds, bool dryRun)
{
    if (!g_pica || !g_pluginId)
        return false;
    bool changed = false;
    try
    {
        AEFX_SuiteScoper<AEGP_StreamSuite6> streams(g_pica, kAEGPStreamSuite, kAEGPStreamSuiteVersion6);
        AEFX_SuiteScoper<AEGP_KeyframeSuite5> kf(g_pica, kAEGPKeyframeSuite, kAEGPKeyframeSuiteVersion5);
        AEFX_SuiteScoper<AEGP_MarkerSuite3> mk(g_pica, kAEGPMarkerSuite, kAEGPMarkerSuiteVersion3);

        AEGP_StreamRefH streamH = nullptr;
        if (streams->AEGP_GetNewLayerStream(g_pluginId, layerH, AEGP_LayerStream_MARKER, &streamH) != A_Err_NONE ||
            !streamH)
            return false;

        ATR_STEP("sync:find");
        FoundMarker found;
        bool otherAtStart = false;
        FindOurMarker(streamH, found, zone.start, &otherAtStart);

        const double tol = std::max(1.0e-4, 0.25 * frameSeconds);
        const bool matches = found.exists && !found.legacy && zone.want && std::fabs(found.start - zone.start) < tol &&
                             std::fabs(found.duration - zone.duration) < tol;
        const bool needed = zone.want ? !matches : found.exists;

        if (needed && !dryRun)
        {
            ATR_STEP("sync:write");
            if (found.exists)
                kf->AEGP_DeleteKeyframe(streamH, found.index);
            if (zone.want && !otherAtStart)
            {
                const A_Time startT = ToATime(zone.start);
                const A_Time durT = ToATime(zone.duration);
                AEGP_KeyframeIndex idx = 0;
                if (kf->AEGP_InsertKeyframe(streamH, AEGP_LTimeMode_LayerTime, &startT, &idx) == A_Err_NONE)
                {
                    AEGP_MarkerValP markerP = nullptr;
                    if (mk->AEGP_NewMarker(&markerP) == A_Err_NONE && markerP)
                    {
                        // No visible text: identify the marker by a hidden parameter instead.
                        const wchar_t* key = ATR_MARKER_KEY;
                        const wchar_t* value = ATR_MARKER_VALUE;
                        if (mk->AEGP_InsertCuePointParam(markerP, 0) == A_Err_NONE)
                            mk->AEGP_SetIndCuePointParam(markerP,
                                                         0,
                                                         reinterpret_cast<const A_u_short*>(key),
                                                         static_cast<A_long>(std::wcslen(key)),
                                                         reinterpret_cast<const A_u_short*>(value),
                                                         static_cast<A_long>(std::wcslen(value)));
                        mk->AEGP_SetMarkerDuration(markerP, &durT);
                        mk->AEGP_SetMarkerLabel(markerP, ATR_MARKER_LABEL);
                        AEGP_StreamValue2 v;
                        AEFX_CLR_STRUCT(v);
                        v.streamH = streamH;
                        v.val.markerP = markerP;
                        kf->AEGP_SetKeyframeValue(streamH, idx, &v);
                        mk->AEGP_DisposeMarker(markerP);
                    }
                }
            }
        }
        changed = needed;
        streams->AEGP_DisposeStream(streamH);
    }
    catch (...)
    {
        return false;
    }
    return changed;
}

namespace
{
A_Err IdleHook(AEGP_GlobalRefcon, AEGP_IdleRefcon, A_long* max_sleepPL)
{
    __try
    {
        return IdleHookImpl(max_sleepPL);
    }
    __except (AtrCrashFilter(GetExceptionInformation(), "IdleHook"))
    {
        return A_Err_NONE;
    }
}
} // namespace

void RegisterMarkerIdleHook()
{
    if (!g_pica || !g_pluginId)
        return;
    try
    {
        AEFX_SuiteScoper<AEGP_RegisterSuite5> reg(g_pica, kAEGPRegisterSuite, kAEGPRegisterSuiteVersion5);
        reg->AEGP_RegisterIdleHook(g_pluginId, IdleHook, nullptr);
    }
    catch (...)
    {
    }
}
