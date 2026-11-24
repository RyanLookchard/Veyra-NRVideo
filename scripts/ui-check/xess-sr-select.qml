    // ---- XeSS SR on this GPU: select quality 6, start a source, confirm SR evaluates ----
    Timer { interval: 150; running: true; repeat: true; onTriggered: veyra.logUi("uitest-tick", "") }
    Timer {
        interval: 1000; running: true; repeat: true
        property int ticks: 0
        property bool started: false
        onTriggered: {
            ++ticks
            if (ticks < 4) return
            if (!started) {
                const cap = veyra.effectAvailability("sr6")
                veyra.logUi("uitest", "UITEST_NOTE sr6 available=" + cap.available + " reason=" + cap.reason)
                veyra.videoSrQuality = 6
                veyra.srEnabled = true
                started = true
                veyra.logUi("uitest", "UITEST_NOTE set quality=" + veyra.videoSrQuality + " srEnabled=" + veyra.srEnabled)
                return
            }
            if (ticks === 8) {
                veyra.logUi("uitest", "UITEST_NOTE after srEnabled=" + veyra.srEnabled + " quality=" + veyra.videoSrQuality)
                veyra.logUi("uitest", "UITEST_SHOT xess-sr-selected")
                veyra.logUi("uitest", "UITEST_DONE")
                running = false
            }
        }
    }