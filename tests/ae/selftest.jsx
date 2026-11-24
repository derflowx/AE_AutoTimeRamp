// Auto Time Ramp — in-AE self-test.
//
// Builds a throwaway precomp whose pixel colour encodes its own time (red = time / 120), puts it in a
// test comp with Auto Time Ramp applied, and reads the post-effect pixel back on every frame via
// sampleImage(). That yields the source time the effect actually rendered, frame by frame.
//
// Everything is created inside new project items that are removed again at the end; nothing is saved.
// Run in a running AE:  AfterFX.exe -r selftest.jsx
// Output: CSV written next to this script (selftest_result.csv).

(function () {
    var FPS = 25;
    var SRC_DUR = 120; // seconds; red channel = time / SRC_DUR
    var outFile = new File(File($.fileName).parent.fsName + "/selftest_result.csv");
    var lines = [];
    var log = function (s) { lines.push(s); };

    var proj = app.project;
    var oldBpc = proj.bitsPerChannel;
    var preIds = {};
    for (var i = 1; i <= proj.numItems; i++) preIds[proj.item(i).id] = true;

    function leafParams(group, out) {
        for (var i = 1; i <= group.numProperties; i++) {
            var p = group.property(i);
            if (p.propertyType === PropertyType.PROPERTY) {
                if (p.propertyValueType !== PropertyValueType.NO_VALUE) out.push(p);
            } else {
                leafParams(p, out);
            }
        }
        return out;
    }

    function cleanup() {
        // Remove every item created during the test (newest first), then restore bit depth.
        for (var i = proj.numItems; i >= 1; i--) {
            var it = proj.item(i);
            if (!preIds[it.id]) { try { it.remove(); } catch (e) { log("#cleanup error," + e); } }
        }
        proj.bitsPerChannel = oldBpc;
    }

    app.beginUndoGroup("Auto Time Ramp self-test");
    try {
        proj.bitsPerChannel = 32;
        var folder = proj.items.addFolder("_ATR selftest");

        var src = proj.items.addComp("ATR_src", 64, 64, 1, SRC_DUR, FPS);
        src.parentFolder = folder;
        var solid = src.layers.addSolid([0, 0, 0], "ATR_time", 64, 64, 1, SRC_DUR);
        var fill = solid.Effects.addProperty("ADBE Fill");
        // red: linear in time (timing); green: sine of time (non-linear, so averaging = visible blur)
        fill.property("ADBE Fill-0002").expression = "[time / " + SRC_DUR + ", 0.5 + 0.5 * Math.sin(2 * Math.PI * time), 0, 1]";

        var main = proj.items.addComp("ATR_main", 64, 64, 1, 30, FPS);
        main.parentFolder = folder;
        var L = main.layers.add(src);
        L.name = "src";

        L.Effects.addProperty("DFLW AutoTimeRamp");
        // Params are looked up by disk ID (match name suffix) every time: changing the ease to Custom
        // re-lays out the effect UI, which can invalidate property references held across calls.
        function P(disk) {
            var id = "000" + disk;
            return L.property("ADBE Effect Parade").property("DFLW AutoTimeRamp")
                .property("DFLW AutoTimeRamp-" + id.substr(id.length - 4));
        }
        var D = { inDur: 2, inSrc: 3, inEase: 4, outDur: 7, outSrc: 8, outEase: 9, anchor: 11,
                  blurOn: 13, shutter: 14, maxSamples: 15, inBz: [17, 18, 19, 20], outBz: [21, 22, 23, 24],
                  link: 25, marker: 27 };
        var BLUR = [0, 180, 16];                 // enable, shutter angle, max samples
        var LINK = 0;                            // ramp out "Same as ramp in"
        var BZ_IN = [0, 0.8, 0.2, 1], BZ_OUT = [0, 0.8, 0.2, 1];

        var nul = main.layers.addNull();
        // Adding an effect invalidates earlier property references, so add both before grabbing them.
        nul.Effects.addProperty("ADBE Slider Control");
        nul.Effects.addProperty("ADBE Slider Control");
        var slider = nul.Effects.property(1).property(1);
        var slider2 = nul.Effects.property(2).property(1);
        slider.expression = 'thisComp.layer("src").sampleImage([32,32],[0.5,0.5],true,time)[0] * ' + SRC_DUR;
        slider2.expression = 'thisComp.layer("src").sampleImage([32,32],[0.5,0.5],true,time)[1]';

        function setup(startTime, inPt, outPt, iD, iS, iE, oD, oS, oE, an) {
            L.startTime = startTime;
            L.inPoint = inPt;
            L.outPoint = outPt;
            P(D.inDur).setValue(iD); P(D.inSrc).setValue(iS); P(D.inEase).setValue(iE);
            P(D.outDur).setValue(oD); P(D.outSrc).setValue(oS); P(D.outEase).setValue(oE);
            P(D.anchor).setValue(an);
            P(D.blurOn).setValue(BLUR[0]); P(D.shutter).setValue(BLUR[1]); P(D.maxSamples).setValue(BLUR[2]);
            P(D.link).setValue(LINK);
            for (var b = 0; b < 4; b++) { P(D.inBz[b]).setValue(BZ_IN[b]); P(D.outBz[b]).setValue(BZ_OUT[b]); }
        }
        // Header per case: everything the C++ comparer needs (layer times = comp time - startTime).
        function run(name, startTime, inPt, outPt, iD, iS, iE, oD, oS, oE, an) {
            setup(startTime, inPt, outPt, iD, iS, iE, oD, oS, oE, an);
            log("#case," + name + "," + (inPt - startTime) + "," + (outPt - startTime) + "," +
                iD + "," + iS + "," + iE + "," + oD + "," + oS + "," + oE + "," + an + "," + proj.bitsPerChannel + "," +
                BLUR[0] + "," + BLUR[1] + "," + BLUR[2] + "," + LINK + "," + BZ_IN.join(",") + "," + BZ_OUT.join(","));
            var f0 = Math.round(inPt * FPS), f1 = Math.round(outPt * FPS);
            for (var f = f0; f < f1; f++) {
                var c = f / FPS;
                var v = slider.valueAtTime(c, false);
                var g = slider2.valueAtTime(c, false);
                log(name + "," + (c - startTime) + "," + v + "," + g);
            }
        }

        // Popups are 1-based: ease 1=Snap ... 6=Circ snap, 7=Settle, 5=Expo snap; anchor 1=Sync 2=Head 3=Tail.
        run("sync_default", -10, 0, 7, 1, 5, 1, 1, 5, 1, 1);
        run("trimmed_out", -10, 0, 6, 1, 5, 1, 1, 5, 1, 1);      // same layer, out point trimmed by 1 s
        run("trimmed_in", -10, 0.5, 7, 1, 5, 1, 1, 5, 1, 1);     // in point trimmed by 0.5 s
        run("head_anchor", -10, 0, 7, 1, 5, 1, 1, 5, 1, 2);
        run("tail_anchor", -10, 0, 7, 1, 5, 1, 1, 5, 1, 3);
        run("settle_slowout", -10, 0, 7, 0.5, 3, 7, 1.5, 0.6, 6, 1); // fast settle in, slow circ out
        run("expo_whip", -30, 0, 4, 1.2, 12, 5, 0.8, 9, 4, 1);
        run("clamp_head", -2, 0, 7, 1, 5, 1, 1, 5, 1, 1);         // ramp in needs source before 0

        // Custom bezier eases (popup 10) and linked ramp out.
        BZ_IN = [0, 0.8, 0.2, 1]; BZ_OUT = [0.42, 0, 0.58, 1];
        run("custom_bezier", -10, 0, 7, 1, 5, 10, 1.5, 4, 10, 1);
        BZ_IN = [0.1, 0.9, 0.3, 1]; BZ_OUT = [0, 0.8, 0.2, 1];
        LINK = 1;
        run("linked_custom", -10, 0, 7, 1.2, 6, 10, 0.3, 0.5, 1, 1); // out values must be ignored
        run("linked_whip", -10, 0, 7, 0.8, 4, 4, 2, 9, 7, 1);
        LINK = 0; BZ_IN = [0, 0.8, 0.2, 1]; BZ_OUT = [0, 0.8, 0.2, 1];

        // Motion blur: each frame must equal the average of the shutter samples.
        BLUR = [1, 180, 16];
        run("blur_default", -10, 0, 7, 1, 5, 1, 1, 5, 1, 1);
        BLUR = [1, 360, 4];
        run("blur_360_cap4", -10, 0, 7, 1, 5, 1, 1, 5, 1, 1);
        BLUR = [1, 270, 32];
        run("blur_expo_whip", -30, 0, 4, 1.2, 12, 5, 0.8, 9, 4, 1);
        run("blur_clamp", -2, 0, 7, 1, 5, 1, 1, 5, 1, 1);
        BLUR = [1, 180, 16];
        // OCIO-managed projects only allow 32 bpc; switch to Adobe colour management for the 8/16 bpc runs.
        // colorManagementSystem is numeric (0 = Adobe, 1 = OCIO); the enum object isn't exposed in AE 26.
        var ADOBE_CMS = (typeof ColorManagementSystem !== "undefined") ? ColorManagementSystem.ADOBE : 0;
        var hasCms = proj.colorManagementSystem !== undefined;
        var oldCms = hasCms ? proj.colorManagementSystem : null;
        try {
            if (hasCms && proj.colorManagementSystem !== ADOBE_CMS) {
                proj.colorManagementSystem = ADOBE_CMS;
                log("#info,switched colour management " + oldCms + " -> " + proj.colorManagementSystem);
            }
            proj.bitsPerChannel = 16;
            run("bpc16", -10, 0, 7, 1, 5, 1, 1, 5, 1, 1);
            proj.bitsPerChannel = 8;
            run("bpc8", -10, 0, 7, 1, 5, 1, 1, 5, 1, 1);
        } catch (e2) {
            log("#error,bpc cases: " + e2.toString());
        }
        proj.bitsPerChannel = 32;
        if (hasCms && oldCms !== null && proj.colorManagementSystem !== oldCms) proj.colorManagementSystem = oldCms;
        log("#done");
    } catch (e) {
        log("#error," + e.toString() + " line " + e.line);
    }
    cleanup();
    app.endUndoGroup();

    if (outFile.open("w")) {
        outFile.write(lines.join("\n") + "\n");
        outFile.close();
    }
})();
