    // ---- NR master switch: toggle on with zero layers must create a layer and run NR ----
    Timer { interval: 150; running: true; repeat: true; onTriggered: veyra.logUi("uitest-tick", "") }
    Timer {
        interval: 1000; running: true; repeat: true
        property int ticks: 0
        onTriggered: {
            ++ticks
            if (ticks === 3) {
                veyra.logUi("uitest", "UITEST_NOTE before layers=" + veyra.nrLayers.length + " anyEnabled=" + veyra.nrAnyEnabled)
                veyra.openDroppedUrls(["file:///C:/veyra-xbox-enhanced/veyra-nrvideo/assets/veyra-2.0.0-promo.mp4"])
            }
            if (ticks === 6) {
                const ok = veyra.setAllNrEnabled(true)
                veyra.logUi("uitest", "UITEST_NOTE toggle ok=" + ok + " layers=" + veyra.nrLayers.length + " anyEnabled=" + veyra.nrAnyEnabled)
                if (!ok || veyra.nrLayers.length === 0) { veyra.logUi("uitest", "UITEST_FAIL master switch did not create a layer"); running = false; return }
            }
            if (ticks === 12) {
                veyra.logUi("uitest", "UITEST_NOTE final layers=" + veyra.nrLayers.length + " anyEnabled=" + veyra.nrAnyEnabled)
                veyra.logUi("uitest", "UITEST_DONE")
                running = false
            }
        }
    }