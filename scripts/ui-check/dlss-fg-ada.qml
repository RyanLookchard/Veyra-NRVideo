    // ---- DLSS FG on Ada: select dlss 4X, open a local file, watch fg-compat/admission ----
    Timer { interval: 150; running: true; repeat: true; onTriggered: veyra.logUi("uitest-tick", "") }
    Timer {
        interval: 1000; running: true; repeat: true
        property int ticks: 0
        onTriggered: {
            ++ticks
            if (ticks === 3) {
                const cap = veyra.effectAvailability("dlss")
                veyra.logUi("uitest", "UITEST_NOTE dlss available=" + cap.available + " reason=" + cap.reason)
                veyra.fgBackendName = "dlss"
                veyra.fgEnabled = true
                veyra.fgMultiplier = 4
                veyra.logUi("uitest", "UITEST_NOTE set backend=" + veyra.fgBackendName + " mult=" + veyra.fgMultiplier)
            }
            if (ticks === 5) {
                veyra.openDroppedUrls(["file:///C:/veyra-xbox-enhanced/veyra-nrvideo/assets/veyra-2.0.0-promo.mp4"])
            }
            if (ticks === 12) {
                veyra.logUi("uitest", "UITEST_NOTE fgActive=" + veyra.fgActive + " backend=" + veyra.fgBackendName + " mult=" + veyra.fgMultiplier)
            }
            if (ticks === 20) {
                veyra.logUi("uitest", "UITEST_NOTE final fgActive=" + veyra.fgActive + " backend=" + veyra.fgBackendName)
                veyra.logUi("uitest", "UITEST_SHOT dlss-fg-ada")
                veyra.logUi("uitest", "UITEST_DONE")
                running = false
            }
        }
    }