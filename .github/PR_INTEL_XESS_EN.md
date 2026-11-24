# PR 3: xess — Intel native multi-frame generation + XeSS upscaling backend (quality 6)

## Why

Two Intel-platform gaps, independent of the Xbox transport:

1. **I-card FG relied on byte-patching.** XeSS FG natively supports >2X on Intel,
   but the existing path applied the unlock patch/pacing to every vendor alike —
   redundant on Intel and hiding the runtime's real ceiling. Fresh Intel sessions
   also defaulted to FSR 3.1, and Automatic motion special-cased XeSS to zero motion.
2. **No temporal upscaler for I-cards.** DLSS SR / RTX Video SR are NVIDIA-only and
   FSR SR has no temporal history; Intel's XeSS upscaler (libxess.dll, XMX path) was
   never wired, leaving Intel users with spatial scaling only.

## What

- **FG (`XessPresenter`/`PresentSink`/`VideoPresenter`)**: adapter vendor
  (`GetAdapterLuid` → vendorId) reaches the presenter; 0x8086 takes the native MFG
  path — no byte patch, no pacing, requested frames clamped to the runtime ceiling
  (native 4X, clamp never fails); the non-Intel path is byte-for-byte unchanged.
  UI multiplier ceiling is vendor-aware; fresh Intel sessions default to XeSS;
  Automatic flow applies to every backend except VFG/AMD FRC; contexts init at MAX
  and `SetNumInterpolatedFrames` enforces the user request.
- **SR (new `XessSrBackend`)**: quality 6 = `kVideoSrXess`. Dynamically loads
  `runtime_local/intel/experimental/libxess.dll`
  (`xessD3D12CreateContext/Init/Execute/Destroy`; official v3.0.2 SDK binaries,
  per-file hash-verified). Graph wiring mirrors FSR: single/dual pass, history
  reset, teardown release, temporal memory accounting; UI sr6 choice + cross-vendor
  capability bit; `BackendRecovery` cross-vendor exception includes XeSS.
- **MV contract**: default low-res MV (flowTex_ is render/input extent) plus
  constant-far depth; HIGH_RES_MV is NOT set (that flag expects output-extent MV).
- ui-check snippets: `xess-sr-select` (sr6 selection regression), `dlss-fg-ada`
  (NVIDIA control).

## Scope / dependencies

- No `vcpkg.json`/`.gitignore`/`VEYRA_*_ROOT` changes. libxess.dll ships via
  `runtime_local` like the existing FSR/XeSS FG runtimes (gitignored, not committed);
  provenance: official `intel/xess` release v3.0.2 asset, hashes in
  docs/INTEL_XESS_2026-11-24.md.
- Behavior changes apply to every source, hence a separate PR from the Xbox
  transport. Shares small context hunks in `Hardware.cpp`/`EnhancementSettings.h`
  with PR 2; merge PR 2 first.

## Verification

- Offline: xbox 119, web gamepad 30, repair contract 371, UI contract — all green.
- Selection smoke: sr6 available=true, commit=true, quality stays 6, RESULT PASS.
- GPU probe (NVIDIA RTX 5880 Ada, cross-vendor path): 720p→4K, `xess-sr selected`,
  srEvaluateCount=6, real 3840×2160 output.
- **Limit**: no Intel GPU on the dev machine; Arc XMX quality/performance unverified,
  the Intel slim package awaits real-hardware validation.
- Engineering note: `validate()` is header-inline (COMDAT); after changing an inline
  bound a clean rebuild is required or the linker may pick a stale object's old
  bound (the root cause of q6 rejections seen during development).