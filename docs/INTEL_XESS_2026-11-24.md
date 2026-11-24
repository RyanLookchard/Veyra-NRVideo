# Intel XeSS 满血：FG 原生 MFG + SR 后端（2026-11-24）

## 范围
- FG：Intel（vendor 0x8086）走原生 MFG——不跳补丁、不 pacing；请求帧数按运行时上限钳制（原生 4X），非 Intel 保持原有 unlock/pacing 路径逐字节不变。
- FG 默认：I 卡新会话默认 XeSS；Automatic 光流对除 VFG/AMD FRC 外所有后端生效；上下文以 MAX 初始化、SetNumInterpolatedFrames 执行用户请求。
- SR：新增 quality 6 = Intel XeSS（`kVideoSrXess`），`XessSrBackend` 动态加载 `runtime_local/intel/experimental/libxess.dll`（xessD3D12CreateContext/Init/Execute），图接线镜像 FSR（单pass/双pass、history 复位、teardown 释放），UI sr6 选项与跨厂商能力位、BackendRecovery 跨厂商例外。
- MV 契约：默认低分辨率 MV（flowTex_ 为 render/input extent）+ 常量远深度；**不**设 HIGH_RES_MV（该 flag 期望 output-extent MV）。

## 运行时来源
- libxess.dll / libxell.dll / libxess_fg.dll 取自官方 release `intel/xess v3.0.2` 资产 XeSS_SDK_3.0.2.zip（sha256 88b8a373…1990，逐文件哈希一致）。
- 注意：该官方 3.0.2 二进制的 `xessGetVersion`  baked SR API 版本为 2.0.2（隔离加载实测），日志 provider=2.0.2 属正常，非装错包。

## 验证（NVIDIA RTX 5880 Ada 开发机）
- 离线：xbox 119、web gamepad 30、repair contract 371、UI contract 全过。
- 选择 smoke（scripts/ui-check/xess-sr-select.qml）：sr6 available=true，commit=true，quality 保持 6，RESULT PASS。
- GPU 探针（临时，已删）：720p→4K，`xess-sr selected`、srEvaluateCount=6、输出 3840x2160 非零，exit 0。
- 真机 Arc（XMX 路径、画质/性能）未验证——开发机无 Intel GPU；I 卡包 Veyra-Xbox-Enhanced-Intel-slim-portable.zip 已含 libxess.dll 待用户验证。

## 工程教训
- `EnhancementSettings::validate()` 为头文件内联（每 TU 一份 COMDAT）。改 `kVideoSrXess` 边界后增量编译只重编部分 obj，链接器选旧 obj 的旧边界 → 产品拒 q6 而探针通过。**改内联常量/谓词后必须 clean 重编**（本次 --clean-first 全量后 q6 生效）。
- clean 全量会暴露 legacy Win32 `veyra` target 的上游断裂（RemotePlayPanel.cpp 引用已删除的 PsnAuth.h）；产品构建只编 veyra_qml_ui + 测试 target。
- ui-check.py 传给子进程的环境路径必须绝对（子进程 cwd=app，相对路径会写错位置导致 runner 看空日志误判 FAIL）。
- veyra_xess_motion_contract_tests 需要 D3D12 调试层（ID3D12InfoQueue）；无 Graphics Tools 的机器 exit 2 属环境限制。