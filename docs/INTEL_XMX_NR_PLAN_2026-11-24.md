# Intel XMX DLSS-NR 集成方案（调研 2026-11-24）

上游：`uzbekunknown/dlss-nr-on-intel` v0.0.7（Apache-2.0，commit 921d9754）。
定位：I 卡实验性 NR（`NrRuntime::IntelXmx`，能力位 nr5），仅 vendor 0x8086 + XMX。

## 上游是什么

- DLSS5 NR 推理的**重实现**：Vulkan coopmat（XMX）跑恢复出的网络（A=71块/145.8M 参数，B=31块/113.3M），不是 ZLUDA。
- 输出 = RGB 残差 + 时序 gate；输入输出同尺寸；16 通道特征（当前帧 + 重投影历史 + 风格标量），噪声由 extent+帧号决定。
- **不带 NVIDIA 二进制/权重**：用户自备 `nvngx_dlssnr.dll`（310.8 / 310.9.16），`src/tools/unpack_preset.py`（驱动 MLX-DLSS，固定 commit 06a3e11a）提取到 `work/mlxw/dlssnr-{,b-}logical.safetensors`。A/B 由 `NR_PRESET` 选择，无 manifest。
- 仅 XMX：B570/B580/140V 已验证；A 系列实验（A770 网络B 720p@0.4≈19ms）；无 XMX 核显不行。`nr_gpu_probe.exe` 一分钟自检。

## 发行包实测（v0.0.7 zip，47.4MB 压缩 / 117.8MiB 解压）

| 组件 | 体积 | 集成是否需要 |
|---|---|---|
| python/（CPython 3.14.10 + numpy 2.5.4 + safetensors 0.8.0） | ~62MiB（可裁 tests/tk 等 6.7MiB） | 路线A需要 |
| dxvk/x32+x64 | 43.6MiB | **不需要**（游戏注入用） |
| nr_layer/nr_xess/nr_ffx/nr_vulkan_proxy DLL | ~4MiB | **不需要**（游戏代理） |
| src/layer+gpu+ref、work/mlx-dlss、libxmx/libnr_image/libnr_alloc.dll | ~4MiB | 需要（daemon 本体） |
| work/*.spv（25 个） | 0.7MiB | 需要 |
| NR-Setup/wizard/探测 exe | ~1MiB | probe 可选随包 |

**daemon 子集合计 ≈68MiB 解压（压缩后约 25–30MB）**——这是路线A的真实包增量，不是 47MB。

## 运行时契约（protocol 3，D3D12 客户端）

现成 C 头：`src/layer/nr_gpu_wire.h` / `nr_gpu_rpc.h` / `nr_gpu_d3d12.h`（可直接引入 Veyra worker）。

- 客户端在**自己的 D3D12 device** 上建两个 `HEAP_FLAG_SHARED` committed buffer（input=帧 RGBA8 + 可选逐像素 motion 2×f32；output=答案+stamp）和一个 `FENCE_FLAG_SHARED` fence，`CreateSharedHandle` 出 NT 句柄。
- 命名管道（`\\.\pipe\...`，byte mode，`GetNamedPipeClientProcessId` 认证）发 16B header+128B packet：HELLO/FRAME/RELEASE；句柄值放 packet，daemon `OpenProcess(DUP_HANDLE)` 后 duplicate。
- 帧同步：客户端队列写 input → signal V → 发 FRAME(done_sem=V) → daemon 等 V、跑网络、写 output+stamp、signal V+1 → 客户端队列 wait V+1 → COMPOSE。V 每帧 +2。stamp 命名 V 才算有效答案（失败=保留原图，与 Veyra RDNA3 的 validity 语义一致）。
- reset：`driver_uuid[8]=2|reset`（客户端告知历史重置，对应 Veyra 的 reset/scene cut）；motion follow：`driver_uuid` 前 8 字节=上一答案帧的 V。
- daemon 死亡 fence 可能留 UINT64_MAX（Intel 101.9150 驱动）→ 每会话 `renew_fence`。
- 颜色：网络吃 display-referred sRGB。线性 HDR 输入走 `nr_display.py` 软膝代理（white 参数），ENCODE/COMPOSE 着色器做往返；8bit display 输入直接字节进出。

## daemon 启动/生命周期

```
python\python.exe src\layer\nr_daemon.py
  --socket \\.\pipe\veyra-nr-<sessionId>
  --root <runtime_local\intel\nr>
  --preset B --render-scale 0.5 --settings <knobs.json>
env: NR_GPU_IMAGE=1 NR_GPU_TRANSPORT=1 NR_GPU_ONLY=1
     NR_LAYER_SPAWNED=<Veyra pid>   (daemon 随主程序退出)
     NR_PRESET=B
```
- `EndpointOwner` 防重复启动；`--settings` JSON 热重载（强度/render-scale/网络切换不重启进程）。
- 权重缺失时 daemon 明确报"要哪个文件、怎么生成"，不静默回退。

## Veyra 侧接线（镜像 RDNA3 worker 架构）

现有参照：`Rdna3NrBackend`/`Rdna3NrWorker` + `Rdna3NrProtocol.h`（共享纹理/fence + 管道 + validity buffer + 5s 看门狗 + 无答案保留源图）。Intel 路线**复用同一主图侧模式**，worker 换成 protocol-3 客户端：

1. `NrRuntime::IntelXmx=5`；`validNrRuntime`/`nrRuntimeName`/`currentNrRuntime` 扩展；能力位 `nr5`：`vendor==0x8086 && XMX 探测通过 && daemon 子集在位 && 权重已提取`（XMX 判定用上游 probe 逻辑或 device 特性，A 系列标实验）。
2. `EnhanceGraph`：`intelNr_` 分支镜像 `lmxxfNr_`（needNgxCore 排除、createNrFeature 跳过、layer->amd 式 backend 槽）。NR 尺寸策略沿用现有预算（I 卡建议内部 ≤720p + render-scale）。
3. 新 `IntelNrBackend`（主图侧）：ENCODE（linear→display 代理，复用 RDNA3 的 parity/proxy 思路）→ 共享 buffer → FRAME → wait → COMPOSE（残差交回 Veyra 既有 residual/protection/temporal 合成）。motion 用现有 flowTex_（current→previous 像素，NaN=出屏）喂 protocol-3 motion 区。
4. daemon 进程管理：主程序拉起/看门狗/随会话终止（`NR_LAYER_SPAWNED`）；管道名含 sessionId 防串。
5. 权重工作流（UI）：组件页"Intel NR"条目 → 选择本地 `nvngx_dlssnr.dll` → 随包 python 跑 `unpack_preset.py`（A/B 各一次，进度/校验 649 张量）→ 写 `runtime_local/intel/nr/work/mlxw/`。**权重/DLL 绝不随包**（与 AMD RDNA3 同规矩）。
6. UI：NR 版本下拉增"Intel XMX · 实验"（nr5 支持时可见）；强度/profile 映射 daemon knobs；render-scale 滑条（0.25–1，默认 0.5）；网络 A/B 切换走 settings 热重载。

## 分期

- **P0 打通**（可先不做 UI）：vendored daemon 子集 + protocol-3 客户端 backend + nr5 能力位 + 命令行权重提取；契约测试（协议往返/无答案保留源图/reset/看门狗），本机 N 卡只能测协议层（copy worker 模式，同 RDNA3 测试做法）。
- **P1 产品化**：UI 提取向导 + NR 卡接入 + knobs + probe 门禁 + Intel 精简包增量（约 +25–30MB 压缩）。
- **P2 可选去 Python**：libxmx.c + 20 着色器 + nr_image.c 编 native worker（上游已有 MSVC 构建脚本），包增量降到 ~5MB；工作量大，P0/P1 验证后再议。

## 风险

- 性能：1080p 全尺寸网络 A 在 B580 ≈88ms/帧——必须默认 render-scale 0.5 + 网络 B + 内部 ≤720p，UI 明示耗时；隔帧模式（上游 cache=N）可后续接。
- 驱动坑：B580 `packHalf2x16` 往返、间歇 fence -4、daemon 死亡 fence UINT64_MAX——probe 门禁 + renew_fence + 无答案保留原图兜底。
- 合规：Apache-2.0 代码可随包（NOTICE 署名上游 + MLX-DLSS + OpenDLSS-NR）；权重用户自备提取，不随包不下载。
- 上游全 AI 生成代码，以它的 70 个测试程序为验收基准；vendored 子集固定 commit，不追 main。
- 本机无 I 卡：XMX 路径真机画质/性能只能用户验证。