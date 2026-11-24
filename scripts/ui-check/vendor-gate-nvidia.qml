    // ---- vendor-gated UI on this NVIDIA Ada: NVIDIA options present, flow NVOF present ----
    Timer { interval: 150; running: true; repeat: true; onTriggered: veyra.logUi("uitest-tick", "") }
    Timer {
        interval: 1000; running: true; repeat: true
        property int ticks: 0
        onTriggered: {
            ++ticks
            if (ticks === 3) {
                const fg = veyra.fgBackendChoices.map(o => o.id).join(",")
                const sr = veyra.srBackendChoices.map(o => o.id).join(",")
                const nr = veyra.nrRuntimeChoices.map(o => o.id).join(",")
                const caps = veyra.effectCapabilities
                veyra.logUi("uitest", "UITEST_NOTE fg=[" + fg + "] sr=[" + sr + "] nr=[" + nr + "]")
                veyra.logUi("uitest", "UITEST_NOTE dlss.supported=" + caps.dlss.supported + " vfg.supported=" + caps.vfg.supported + " flow0.supported=" + caps.flow0.supported + " xess.supported=" + caps.xess.supported)
                const hasDlss = veyra.fgBackendChoices.some(o => o.id === "dlss")
                const hasVfg = veyra.fgBackendChoices.some(o => o.id === "vfg")
                const hasNvof = caps.flow0.supported === true
                if (!hasDlss || !hasVfg || !hasNvof) { veyra.logUi("uitest", "UITEST_FAIL NVIDIA options missing on NVIDIA GPU"); running = false; return }
            }
            if (ticks === 6) {
                veyra.logUi("uitest", "UITEST_SHOT vendor-gate-nvidia")
                veyra.logUi("uitest", "UITEST_DONE")
                running = false
            }
        }
    }