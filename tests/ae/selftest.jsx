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
        fill.property("ADBE Fill-0002").expression = "[time / " + SRC_DUR + ", 0, 0, 1]";

        var main = proj.items.addComp("ATR_main", 64, 64, 1, 30, FPS);
        main.parentFolder = folder;
        var L = main.layers.add(src);
        L.name = "src";

        var fx = L.Effects.addProperty("DFLW AutoTimeRamp");
        var P = leafParams(fx, []);
        for (var k = 0; k < P.length; k++) log("#param," + k + "," + P[k].name + "," + P[k].matchName);
        // Order: in dur, in src, in ease, out dur, out src, out ease, anchor
        var inDur = P[0], inSrc = P[1], inEase = P[2], outDur = P[3], outSrc = P[4], outEase = P[5], anchor = P[6];

        var nul = main.layers.addNull();
        var slider = nul.Effects.addProperty("ADBE Slider Control").property(1);
        slider.expression = 'thisComp.layer("src").sampleImage([32,32],[0.5,0.5],true,time)[0] * ' + SRC_DUR;

        function setup(startTime, inPt, outPt, iD, iS, iE, oD, oS, oE, an) {
            L.startTime = startTime;
            L.inPoint = inPt;
            L.outPoint = outPt;
            inDur.setValue(iD); inSrc.setValue(iS); inEase.setValue(iE);
            outDur.setValue(oD); outSrc.setValue(oS); outEase.setValue(oE);
            anchor.setValue(an);
        }
        // Header per case: everything the C++ comparer needs (layer times = comp time - startTime).
        function run(name, startTime, inPt, outPt, iD, iS, iE, oD, oS, oE, an) {
            setup(startTime, inPt, outPt, iD, iS, iE, oD, oS, oE, an);
            log("#case," + name + "," + (inPt - startTime) + "," + (outPt - startTime) + "," +
                iD + "," + iS + "," + iE + "," + oD + "," + oS + "," + oE + "," + an + "," + proj.bitsPerChannel);
            var f0 = Math.round(inPt * FPS), f1 = Math.round(outPt * FPS);
            for (var f = f0; f < f1; f++) {
                var c = f / FPS;
                var v = slider.valueAtTime(c, false);
                log(name + "," + (c - startTime) + "," + v);
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
