<div align="center">

# Veyra Xbox Streaming Enhanced

**基于 [Veyra](https://github.com/Likely7/Veyra-NRVideo) 2.2.1 的 Xbox 串流增强版**

[下载 Releases](https://github.com/RyanLookchard/Veyra-Xbox-Streaming-enhanced/releases) · [上游项目](https://github.com/Likely7/Veyra-NRVideo)

</div>

> 本仓库是 fork，全部版权归原作者 [Likely7](https://github.com/Likely7) 所有。
> 原项目说明、教程、赞助信息请见[上游 README](https://github.com/Likely7/Veyra-NRVideo#readme)。

---

## 与上游的差异

### Xbox 串流增强（Intel Arc / AMD / NVIDIA 三个厂商包）

- 1080p HQ 串流配置 + 完整 Teredo 隧道展开，提升连接成功率与画质
- 可选 V2 传输引擎（Chromium 参考播放器，WebView2），与原生 V1 可切换
- Xbox 手柄震动/扳机反馈
- D3D11VA 硬解偏好，提升兼容性
- NACK 丢包恢复 + 串流状态 HUD（码率/丢包/延迟实时可见）

### Intel Arc 专用精简版

在增强版基础上：

- 删掉 Arc 用不到的 NVIDIA NGX/DLSS 运行库等，体积大幅缩小
- **XeSS 超分**（XMX 加速，档位按缩放比自动选择）
- **FSR 4.1 ML 超分**（INT8 Lite 量化模型，Arc 实验性，1080p→4K 已实机验证；驱动 DLL 随包附带）
- **XeSS 补帧**（原生 2X–4X，无需补丁）
- FSR 超分锐化滑杆（0–100%，默认 20%）

已在 **Intel Arc B390** 实机验证 Xbox 串流 + 超分 + 补帧。

### 上游能力提交

Intel 相关能力（XeSS SR / FSR4 ML / XeSS 原生 MFG）已整理成 [PR #360](https://github.com/Likely7/Veyra-NRVideo/pull/360) 提交上游。

---

## Differences from upstream

### Xbox streaming enhancements (Intel Arc / AMD / NVIDIA vendor packages)

- 1080p HQ streaming profile + full Teredo tunnel expansion
- Selectable V2 transport (Chromium reference player via WebView2), switchable with native V1
- Xbox controller rumble/trigger feedback
- D3D11VA hardware decode preference
- NACK loss recovery + streaming stats HUD

### Intel Arc slim edition

- Removed NVIDIA NGX/DLSS runtimes unused on Arc; much smaller size
- **XeSS SR** (XMX accelerated, auto quality by scale ratio)
- **FSR 4.1 ML SR** (INT8 Lite quantized model, experimental on Arc, 1080p→4K verified; driver DLL bundled)
- **XeSS FG** (native 2X–4X, no patch needed)
- FSR SR sharpness slider (0–100%, default 20%)

Verified on **Intel Arc B390**. Intel capabilities submitted upstream as [PR #360](https://github.com/Likely7/Veyra-NRVideo/pull/360).

---

## ⚠️ 防冒充 / Beware of impersonation

本人只通过 GitHub 账号 **[RyanLookchard](https://github.com/RyanLookchard)** 发布，唯一下载地址为本仓库 [Releases](https://github.com/RyanLookchard/Veyra-Xbox-Streaming-enhanced/releases)。任何冒充网站、网盘、聊天群提供的"修改版"均与本人无关，请勿下载。

I only publish via GitHub account **[RyanLookchard](https://github.com/RyanLookchard)**. The only download source is this repo's [Releases](https://github.com/RyanLookchard/Veyra-Xbox-Streaming-enhanced/releases). "Modded builds" from impersonating websites, cloud drives, or chat groups are NOT mine.

---

## 许可 / License

与上游一致：GPL-3.0。详见 [LICENSE](LICENSE)。