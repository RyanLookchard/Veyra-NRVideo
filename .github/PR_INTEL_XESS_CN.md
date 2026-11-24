# PR 3: xess — Intel 原生多帧生成 + XeSS 超分后端（quality 6）

## 为什么

两个 Intel 平台缺陷/缺口，与 Xbox 传输无关：

1. **I 卡补帧靠字节补丁解锁。** XeSS FG 在 Intel 上原生支持 >2X，但现有路径对
   所有厂商一视同仁地跳补丁/pacing，既多余又掩盖运行时真实上限；I 卡新会话还
   默认落在 FSR 3.1，Automatic 运动源对 XeSS 例外地用零运动。
2. **I 卡没有可用的时序超分。** DLSS SR/RTX Video SR 是 NVIDIA 专属，FSR SR 无
   时序历史；Intel 的 XeSS 超分（libxess.dll，XMX 路径）从未接入，I 卡用户只能
   用空域放大。

## 改了什么

- **FG（`XessPresenter`/`PresentSink`/`VideoPresenter`）**：适配器 vendor
  （`GetAdapterLuid` → vendorId）传入 presenter；0x8086 走原生 MFG——不跳补丁、
  不 pacing，请求帧数按运行时上限钳制（原生 4X，钳制不失败）；非 Intel 路径逐字节
  不变。UI 倍率上限按厂商显示；I 卡新会话默认 XeSS；Automatic 光流对除 VFG/AMD
  FRC 外所有后端生效；上下文以 MAX 初始化、`SetNumInterpolatedFrames` 执行用户请求。
- **SR（新增 `XessSrBackend`）**：quality 6 = `kVideoSrXess`。动态加载
  `runtime_local/intel/experimental/libxess.dll`
  （`xessD3D12CreateContext/Init/Execute/Destroy`，官方 v3.0.2 SDK 二进制，
  逐文件哈希校验）。图接线镜像 FSR：单/双 pass、history 复位、teardown 释放、
  temporal 内存记账；UI sr6 选项 + 跨厂商能力位；`BackendRecovery` 跨厂商例外
  含 XeSS。
- **MV 契约**：默认低分辨率 MV（flowTex_ 为 render/input extent）+ 常量远深度；
  **不**设 HIGH_RES_MV（该 flag 期望 output-extent MV）。
- ui-check 片段：`xess-sr-select`（sr6 选择回归）、`dlss-fg-ada`（NVIDIA 对照）。

## 范围 / 依赖

- 不改 `vcpkg.json`/`.gitignore`/`VEYRA_*_ROOT`。libxess.dll 与现有 FSR/XeSS FG
  运行库同样走 `runtime_local` 随包分发（gitignore，不提交）；来源为官方
  `intel/xess` release v3.0.2 资产，哈希见 docs/INTEL_XESS_2026-11-24.md。
- 行为变化作用于所有源，故与 Xbox 传输分开成独立 PR。与 PR 2 共享
  `Hardware.cpp`/`EnhancementSettings.h` 小片上下文，建议 PR 2 先合。

## 验证

- 离线：xbox 119、web gamepad 30、repair contract 371、UI contract 全过。
- 选择 smoke：sr6 available=true、commit=true、quality 保持 6，RESULT PASS。
- GPU 探针（NVIDIA RTX 5880 Ada，跨厂商路径）：720p→4K，`xess-sr selected`、
  srEvaluateCount=6、真实 3840×2160 输出。
- **限制**：开发机无 Intel GPU；Arc XMX 画质/性能未验证，随附 Intel 精简包待真机。
- 工程注意：`validate()` 为头文件内联（COMDAT），改内联边界后须 clean 重编，
  否则链接器可能选旧 obj 的旧边界（本次 q6 被拒的根因）。