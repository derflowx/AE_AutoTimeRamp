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

        // ---- Ramp transform: geometry + opacity + edges ------------------------------------------
        // Source: a static horizontal gradient (value = x / XW), so a sampled pixel tells which source
        // x it came from. Blur off, so each frame is exactly one transformed sample.
        var XW = 200, XH = 100;
        var xsrc = proj.items.addComp("ATR_xf_src", XW, XH, 1, SRC_DUR, FPS);
        xsrc.parentFolder = folder;
        var xsolid = xsrc.layers.addSolid([0, 0, 0], "ATR_xf_grad", XW, XH, 1, SRC_DUR);
        var ramp = xsolid.Effects.addProperty("ADBE Ramp");
        ramp.property("ADBE Ramp-0001").setValue([0, XH / 2]);
        ramp.property("ADBE Ramp-0002").setValue([0, 0, 0]);
        ramp.property("ADBE Ramp-0003").setValue([XW, XH / 2]);
        ramp.property("ADBE Ramp-0004").setValue([1, 1, 1]);
        var xmain = proj.items.addComp("ATR_xf_main", XW, XH, 1, 30, FPS);
        xmain.parentFolder = folder;
        var XL = xmain.layers.add(xsrc);
        XL.name = "xf";
        XL.Effects.addProperty("DFLW AutoTimeRamp");
        function XP(disk) {
            var id = "000" + disk;
            return XL.property("ADBE Effect Parade").property("DFLW AutoTimeRamp")
                .property("DFLW AutoTimeRamp-" + id.substr(id.length - 4));
        }
        // One slider pair per sample point, expressions set once: AE evaluates a freshly changed expression
        // at the comp's current time rather than the time passed to valueAtTime.
        var PTS = [[20.5, 20.5], [100.5, 50.5], [180.5, 80.5], [60.5, 70.5], [150.5, 30.5], [10.5, 90.5], [190.5, 10.5]];
        var xnul = xmain.layers.addNull();
        for (var s0 = 0; s0 < 2 * PTS.length; s0++) xnul.Effects.addProperty("ADBE Slider Control");
        var XR = [], XA = [];
        for (var s1 = 0; s1 < PTS.length; s1++) {
            var at = '[' + PTS[s1][0] + ',' + PTS[s1][1] + ']';
            XR.push(xnul.Effects.property(2 * s1 + 1).property(1));
            XA.push(xnul.Effects.property(2 * s1 + 2).property(1));
            XR[s1].expression = 'thisComp.layer("xf").sampleImage(' + at + ',[0.5,0.5],true,time)[0]';
            XA[s1].expression = 'thisComp.layer("xf").sampleImage(' + at + ',[0.5,0.5],true,time)[3]';
        }
        // xin/xout: [scale, rotation, posX, posY, opacity]; link 0/1; dir 1=continue 2=mirror; edges 1..3
        // pivot: [x, y] in layer px (omit for the default, layer centre)
        function runXf(name, xin, xout, link, dir, edges, pivot) {
            XL.startTime = -10; XL.inPoint = 0; XL.outPoint = 4;
            XP(D.inDur).setValue(1); XP(D.inSrc).setValue(5); XP(D.inEase).setValue(1);
            XP(D.outDur).setValue(1); XP(D.outSrc).setValue(5); XP(D.outEase).setValue(4);
            XP(D.blurOn).setValue(0); XP(D.link).setValue(link); XP(42).setValue(dir); XP(43).setValue(edges);
            for (var k = 0; k < 5; k++) { XP(29 + k).setValue(xin[k]); XP(36 + k).setValue(xout[k]); }
            XP(45).setValue(pivot ? pivot : [XW / 2, XH / 2]);
            log("#xfcase," + name + ",10,14,1,5,1,1,5,4," + link + "," + dir + "," + edges + "," + XW + "," + XH + "," +
                xin.join(",") + "," + xout.join(",") + (pivot ? "," + pivot.join(",") : ""));
            for (var f = 0; f < 4 * FPS; f += 2) {
                var c = f / FPS;
                for (var q = 0; q < PTS.length; q++) {
                    log("xf," + name + "," + (c + 10) + "," + PTS[q][0] + "," + PTS[q][1] + "," +
                        XR[q].valueAtTime(c, false) + "," + XA[q].valueAtTime(c, false));
                }
            }
        }
        runXf("xf_scale_rot_mirror", [150, 30, 40, -20, 100], [100, 0, 0, 0, 100], 1, 1, 1);
        runXf("xf_shrink_transparent", [60, -20, 0, 0, 40], [100, 0, 0, 0, 100], 1, 2, 2);
        runXf("xf_unlinked_repeat", [100, 0, 60, 0, 100], [70, 45, 0, 25, 70], 0, 1, 3);
        runXf("xf_pivot_corner", [200, 25, 0, 0, 100], [100, 0, 0, 0, 100], 1, 1, 2, [30, 20]);
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
