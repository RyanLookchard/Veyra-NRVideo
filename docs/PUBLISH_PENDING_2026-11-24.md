# 发布待办（等 I 卡真机验证后执行）

状态：用户先真机测 I 卡；GitHub 发布暂停。本地一切就绪。

## 包（覆盖旧资产，同名）
- Veyra-Xbox-Enhanced-NVIDIA-full-portable.zip ← nvidia-app（含 runtime/config/ngx-local.json + clean exe）
- Veyra-Xbox-Enhanced-Intel-slim-portable.zip ← intel-app（含 libxess.dll，212.6MB，已 CRC）
- Veyra-Xbox-Enhanced-AMD-full-portable.zip ← full-app（exe/qml/shaders 已更新；重组装中）
- 目标 release：fork `v2.2.0-xbox-enhanced`（现有 AMD/NVIDIA 两资产同名覆盖；Intel 新增资产）。
- 上传需 PAT（repo scope）或用户自行上传；本机 GCM 无缓存凭据。

## 分支 / PR
- `intel-xess`（105a5d13 + bffcf7e5）：独立 Intel PR，基于 main；正文 .github/PR_INTEL_XESS_{CN,EN}.md。
- `intel-xess-full`（9ea33a7c）：集成分支 = main + Intel + DLSS 探针；包由此构建。
- `xbox-v2-transport` / `ui-sr-amdof`：落后本地 main（0175c430，含 PR 准备：V1 测试恢复、归属、双语正文）；发布时快进到 main 即更新 #289/#290。
- Intel PR base 待用户定：main（惯例，注明先合 #290）或叠 #290。

## 验证摘要
- 离线 119/30/371/UI 全过；q6 smoke PASS；GPU 探针 720p→4K evaluations=6。
- DLSS Ada：transfusion applied、fgActive=true、2X admitted=true；4X 需 240Hz（60fps 源）。
- 待用户：I 卡 XMX 画质/性能、FG 3X/4X 真机。