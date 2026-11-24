// Auto Time Ramp — marker self-test, run in stages so AE's idle loop can run in between:
//   stage "setup"  : build a test comp, apply the effect, open the comp in the viewer
//   stage "check1" : marker exists for the defaults; change ramp in duration (param path); trim out point
//   stage "check2" : marker followed the trim (idle path); turn the marker off
//   stage "check3" : marker removed; clean up
// The stage name is read from marker_stage.txt next to this script; results are appended to
// marker_result.txt as "PASS/FAIL,<check>,<details>".

(function () {
    var dir = File($.fileName).parent.fsName;
    var stageFile = new File(dir + "/marker_stage.txt");
    var outFile = new File(dir + "/marker_result.txt");
    stageFile.open("r");
    var stage = stageFile.readln();
    stageFile.close();

    var COMMENT = "Real-time (Auto Time Ramp)";
    var FPS = 25;
    var lines = [];
    function check(name, ok, details) { lines.push((ok ? "PASS," : "FAIL,") + name + "," + details); }
    function near(a, b) { return Math.abs(a - b) < 0.5 / FPS; }

    function findTest() {
        for (var i = 1; i <= app.project.numItems; i++) {
            var it = app.project.item(i);
            if (it instanceof CompItem && it.name === "ATR_marker_main") return it;
        }
        return null;
    }
    function P(L, disk) {
        var id = "000" + disk;
        return L.property("ADBE Effect Parade").property("DFLW AutoTimeRamp")
            .property("DFLW AutoTimeRamp-" + id.substr(id.length - 4));
    }
    // Our marker: [start (layer time), duration] or null.
    function ourMarker(L) {
        var m = L.property("ADBE Marker");
        for (var k = 1; k <= m.numKeys; k++) {
            var v = m.keyValue(k);
            if (v.comment === COMMENT) return [m.keyTime(k) - L.startTime, v.duration];
        }
        return null;
    }
    function expect(name, L, inDur, outDur) {
        var inLayer = L.inPoint - L.startTime, outLayer = L.outPoint - L.startTime;
        var wantStart = inLayer + inDur, wantDur = (outLayer - outDur) - wantStart;
        var got = ourMarker(L);
        check(name, got !== null && near(got[0], wantStart) && near(got[1], wantDur),
              "want " + wantStart.toFixed(3) + "/" + wantDur.toFixed(3) +
              " got " + (got ? got[0].toFixed(3) + "/" + got[1].toFixed(3) : "none"));
    }

    try {
        if (stage === "setup") {
            app.beginUndoGroup("ATR marker test setup");
            var src = app.project.items.addComp("ATR_marker_src", 64, 64, 1, 60, FPS);
            src.layers.addSolid([0.5, 0.5, 0.5], "ATR_marker_solid", 64, 64, 1, 60);
            var main = app.project.items.addComp("ATR_marker_main", 64, 64, 1, 30, FPS);
            var L = main.layers.add(src);
            L.startTime = -10;
            L.inPoint = 0;
            L.outPoint = 8;
            L.Effects.addProperty("DFLW AutoTimeRamp");
            app.endUndoGroup();
            main.openInViewer(); // the idle hook watches the active comp
            check("setup", true, "comp created");
        } else if (stage === "check1") {
            var L1 = findTest().layer(1);
            expect("marker_on_apply", L1, 1, 1);              // created by the idle hook after apply
            P(L1, 2).setValue(2);                             // ramp in duration -> 2 s (param path)
            expect("marker_after_param_change", L1, 2, 1);
            L1.outPoint = 7;                                  // trim: idle path, checked next stage
        } else if (stage === "check2") {
            var L2 = findTest().layer(1);
            expect("marker_after_trim", L2, 2, 1);
            P(L2, 25).setValue(1);                            // link: out mirrors in (2 s)
            expect("marker_after_link", L2, 2, 2);
            P(L2, 27).setValue(0);                            // marker off
            check("marker_removed_when_off", ourMarker(L2) === null, ourMarker(L2) ? "still there" : "gone");
        } else if (stage === "check3") {
            var main3 = findTest();
            var srcItem = main3.layer(1).source;
            main3.remove();
            for (var i = app.project.numItems; i >= 1; i--) {
                var it = app.project.item(i);
                if (it === srcItem || (it instanceof FootageItem && it.name === "ATR_marker_solid")) it.remove();
            }
            srcItem = null;
            for (var j = app.project.numItems; j >= 1; j--) {
                var it2 = app.project.item(j);
                if (it2.name === "ATR_marker_src") it2.remove();
            }
            check("cleanup", true, "removed");
        }
    } catch (e) {
        check("exception_" + stage, false, e.toString() + " line " + e.line);
    }

    if (outFile.open("a")) {
        for (var n = 0; n < lines.length; n++) outFile.writeln(lines[n]);
        outFile.writeln("#stage_done," + stage);
        outFile.close();
    }
})();
