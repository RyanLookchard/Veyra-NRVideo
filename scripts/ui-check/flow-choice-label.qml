    // ---- flow choice label resolves by id after vendor filtering ----
    Timer { interval: 150; running: true; repeat: true; onTriggered: veyra.logUi("uitest-tick", "") }
    Timer {
        interval: 1000; running: true; repeat: true
        property int ticks: 0
        function filtered(caps) {
            // Mirror Support.option's hidden rule against an injected capability map.
            return [{id:"0",label:"NVIDIA NVOF",key:"flow0"},{id:"1",label:"AMD FidelityFX",key:"flow1"}]
                .filter(o => (caps[o.key] || {}).supported !== false)
        }
        onTriggered: {
            ++ticks
            if (ticks === 3) {
                const real = veyra.effectCapabilities
                const intel = {flow0:{available:false,supported:false},flow1:{available:true,supported:true}}
                const realList = filtered(real), intelList = filtered(intel)
                const realIds = realList.map(o => o.id).join(",")
                const intelIds = intelList.map(o => o.id).join(",")
                // choice=1 (AMD) must resolve by id even when NVOF is filtered out.
                const intelLabel1 = intelList.find(o => o.id === String(1))?.label ?? "未知配置"
                const intelLabel0 = intelList.find(o => o.id === String(0))?.label ?? "未知配置"
                const realLabel1 = realList.find(o => o.id === String(1))?.label ?? "未知配置"
                veyra.logUi("uitest", "UITEST_NOTE real=[" + realIds + "] intel=[" + intelIds + "] intelLabel1=" + intelLabel1 + " intelLabel0=" + intelLabel0)
                if (intelIds !== "1") { veyra.logUi("uitest", "UITEST_FAIL Intel filter should leave only AMD"); running = false; return }
                if (intelLabel1 !== "AMD FidelityFX") { veyra.logUi("uitest", "UITEST_FAIL AMD label not resolved by id after filter"); running = false; return }
                if (intelLabel0 !== "未知配置") { veyra.logUi("uitest", "UITEST_FAIL filtered NVOF must not resolve"); running = false; return }
                if (realLabel1 !== "AMD FidelityFX") { veyra.logUi("uitest", "UITEST_FAIL real AMD label not resolved"); running = false; return }
            }
            if (ticks === 5) { veyra.logUi("uitest", "UITEST_DONE"); running = false }
        }
    }