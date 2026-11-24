.pragma library

function status(api, id) {
    // Reading the notifying property also invalidates bindings after settings,
    // adapter/runtime status or a rejected backend request changes.
    var caps = api.effectCapabilities
    // The "sr" group resolves to the currently selected algorithm; depend on
    // videoSrQuality explicitly so picking FSR re-evaluates the card switch
    // even if the invokable's internal read is not tracked by the binding.
    if (id === "sr") { var q = api.videoSrQuality }
    return typeof api.effectAvailability === "function"
            ? api.effectAvailability(id) : {available: true, reason: ""}
}
function available(api, id) { return status(api, id).available }
function reason(api, id) { return status(api, id).reason }
// Hardware gate alone: false means this GPU can never run the effect, so the
// UI hides it instead of greying. Missing flag (test mocks) reads as supported.
function supported(api, id) { var s = status(api, id).supported; return s === undefined ? true : s }
function option(api, id, label, key) {
    var cap = (api.effectCapabilities || {})[key] || {available: true, reason: ""}
    return {id: id, label: label, disabled: !cap.available, hidden: cap.supported === false, note: cap.reason}
}

function fgBackendShortLabel(option) {
    if (!option) return ""
    if (option.id === "vfg") return "NVIDIA VFG 👑"
    if (option.id === "anr") return "ANR FG"
    return option.label || ""
}
