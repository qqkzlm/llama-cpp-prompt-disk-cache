# llama.cpp + Persistent Prompt Disk Cache

## 三步跑起来

这是给第一次尝试的用户准备的最短路径。完整参数说明见
[server 文档](tools/server/README.md)。

> **适用范围：**下面的直接下载包是 NVIDIA CUDA 版本，适用于 Windows 和 Linux。
> 它不包含模型，也不适用于 AMD/Intel/Apple GPU。第一次使用时不要双击
> `llama-server.exe`；请在解压后的目录打开 PowerShell，让错误信息留在窗口里。

### 1. 下载带缓存功能的 server

- **Windows CUDA（NVIDIA）**：从 [v0.2.2 Release](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/tag/v0.2.2) 下载
  `llama-server-win-cuda-v0.2.2.zip`，解压整个目录，不要只拿走 `.exe`。
- **Linux / 其他后端**：下载源码后按 [构建说明](docs/build.md) 编译；本项目是
  [PrismML/llama.cpp](https://github.com/PrismML-Eng/llama.cpp) 的 `prism` 分支补丁，
  不是 stock `ggml-org/llama.cpp` 二进制。
- 只想使用 PrismML 官方预编译程序时，可从
  [Bonsai-demo](https://github.com/PrismML-Eng/Bonsai-demo) 下载对应硬件的包；
  但请确认包内 server 已包含本项目的 persistent prompt disk cache 功能。

### 2. 下载模型

准备一个与 server 兼容的 `.gguf` 模型。Bonsai 系列模型和文件格式说明见
[PrismML Bonsai collection](https://huggingface.co/collections/prism-ml/bonsai)。
模型不在压缩包里，需要单独下载；把真实的模型文件路径替换到下面命令的 `-m`
参数。不要把模型文件提交到 Git 仓库。

### 3. 启动 server

Windows PowerShell 示例（路径按本机修改）：

```powershell
# 先确认 NVIDIA 驱动可用；至少应能正常显示显卡信息
nvidia-smi

# 在解压后的 llama-server.exe 所在目录打开 PowerShell
.\llama-server.exe `
  -m D:\models\model.gguf `
  --host 127.0.0.1 --port 8080 `
  -c 32768 -np 1 -ngl 99 -fa on `
  --fit `
  --slot-save-path D:\kvstore\model `
  --prompt-cache-disk `
  --prompt-cache-disk-budget 20 `
  --checkpoint-min-step 2048 `
  --ctx-checkpoints 64
```

这几个参数已经足够启用持久化缓存：`--slot-save-path` 指定缓存位置，
`--prompt-cache-disk` 启用磁盘快照；其余参数只是预算和 checkpoint 颗粒度调优，
不改也能使用默认值。先按这条命令跑通，其他 `--prompt-cache-disk-*`、
`--ctx-checkpoints` 和 checkpoint range 参数都可以以后再调。

看到下面的结果后，服务已经可以接受请求：

```text
HTTP GET /health -> {"status":"ok"}
```

若 `nvidia-smi` 失败，先修复 NVIDIA 驱动。若 server 报显存不足，先降低 `-c`；
仍然不足时再逐步增加 `-ncmoe 1`、`2`、`4`。
不要直接套用 GTX 1080 的 `-ncmoe 23`。RTX 30/40/50 系列的完整调参方法见下方指南。

### 请求端：`cache_prompt` 默认就是开启

客户端不需要额外改协议。`/completion` 的 `cache_prompt` 默认值是 `true`，
但建议显式写出，排障时一眼能确认：

```powershell
$body = @{
    prompt = "Explain persistent KV cache in one paragraph."
    cache_prompt = $true
    timings_per_token = $true
    n_predict = 128
    temperature = 0.2
} | ConvertTo-Json
Invoke-RestMethod http://127.0.0.1:8080/completion `
  -Method Post -ContentType "application/json" -Body $body
```

把这条请求原样再发一次，第二次才有机会看到缓存复用。服务日志中出现
`prompt disk: restored N prompt tokens to device` 表示从磁盘恢复；
`prompt cache = total / reused / recomputed` 中的 `reused` 就是本次跳过的 token 数。

连续请求时，必须保持从开头开始的 prompt 前缀完全一致；只有公共前缀能复用，
不同的后缀仍然需要重新计算。响应中的关键字段：

```json
{
  "tokens_cached": 22528,
  "timings": {
    "prompt_n": 4,
    "prompt_per_second": 250.0,
    "predicted_n": 128,
    "predicted_per_second": 24.5
  }
}
```

`tokens_cached` 表示本次请求复用的 prompt token 数；`prompt_n` 是本次重新处理的
prompt token 数；`predicted_per_second` 是吐字速度。磁盘缓存只负责在重启或分支切换
后恢复状态，仍然需要 `cache_prompt: true` 才会进入正常的前缀匹配流程。

> [!NOTE]
> **This repo's purpose: an experimental persistent prompt-prefix cache for the PrismML llama.cpp fork.**
>
> Long agentic sessions re-send the same growing conversation on every request. This patch
> makes the server **persist the KV-cache prefix to disk and restore it automatically**, so a
> restart (or a new process on the same prompt) skips the prefill it has already paid for.
>
> ```bash
> llama-server -m model.gguf -c 32768 \
>   --slot-save-path D:/kvstore/mymodel \    # checkpoint directory (pdcache)
>   --checkpoint-min-step 4096 \              # save granularity: every 4096 tokens of growth
>   --ctx-checkpoints 64 \                    # max checkpoints kept in RAM
>   --prompt-cache-disk \                     # enable persistent disk snapshots
>   --prompt-cache-disk-budget 20             # disk budget in GB
> ```
>
> What you get:
> - **Session restore across restarts** — on boot the newest checkpoint for the matching
>   prompt is loaded back into the KV cache automatically.
> - **Incremental prefix saves** — as a long conversation grows past `--checkpoint-min-step`,
>   a new `.centry` snapshot is written; the next request reuses the longest cached prefix.
> - **Config-signature safety** — checkpoints are keyed by model + context + KV quant; entries
>   from a different configuration are ignored instead of corrupted.
> - **Disk budget** — old entries are evicted once `--prompt-cache-disk-budget` is exceeded.
>
> Implementation lives in `tools/server/server-prompt-disk.cpp` (~900 lines) plus slot/task
> integration; see [tools/server/README.md](tools/server/README.md) for the full flag
> reference. The source is maintained on top of the
> [PrismML `prism` branch](https://github.com/PrismML-Eng/llama.cpp/tree/prism); grab the
> current source or binaries from the [v0.2.2 Release](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/tag/v0.2.2).
> Developers applying the patch can use the direct
> [`v0.1.0` patch download](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/download/v0.1.0/0001-feat-server-add-persistent-prompt-disk-cache.patch)
> on top of the linked PrismML `prism` branch. `main` is the active development line.

### NVIDIA 驱动要求

The v0.2.2 CUDA packages require an NVIDIA driver that supports CUDA 12.0 or newer.
Before starting, run `nvidia-smi` and update the driver if the command fails or reports
an older driver. The Windows package needs a Windows R525-class-or-newer driver; the
Linux package was built for CUDA `sm_61` / Pascal and also requires a compatible Linux
NVIDIA driver. A driver error during startup usually appears as `CUDA driver version is
insufficient for CUDA runtime version`; this means the driver must be updated, not that
the model or cache is broken.
>
> 实测 / Measured on GTX 1080 8 GB, Qwen3.6-35B-A3B-NVFP4-Q4_K_M, `-c 92160`, 40K-token agentic prompt（4万token智能体提示词）:
>
> | | 冷算 Cold (no cache) | 命中 Full prefix hit |
> |---|---|---|
> | 预填充 Prefill 40K | ~151 s (265 tok/s) | 1.35 s (仅重算32个token / 32 tokens recomputed) |
> | 解码 Decode | 24–28 tok/s | 22–28 tok/s（265个token全程稳定 / sustained over 265 tokens） |
> | 端到端 End-to-end | ~155 s | ~6 s |
> | 磁盘开销 Disk cost | 1.5 s 写入/write | 2.1 s 读取+解析+恢复（仅重启后首次 / first request after restart only）|
>
> MoE分层经验 / MoE layering lesson（同一台机器 / same box）: 专家留GPU，KV跟层走。
> Keep experts on GPU (`-ncmoe 0` + `--fit`), let KV follow the layers (hybrid模型不要加
> `--no-kv-offload` — 40层里只有10层带KV / only 10 of 40 layers carry KV)。
> 把20层MoE搬去CPU会直接崩：预填充掉到30 tok/s，解码掉到4 tok/s。
> Pushing 20 MoE layers to CPU collapsed prefill to 30 tok/s and decode to 4 tok/s.
> 混合模型禁用prefix-only / `--prompt-cache-disk-prefix-only` stays off for hybrid models.

## NVIDIA RTX 30/40/50 系列配置指南

下面的配置适用于 CUDA 构建和混合 MoE 模型。`-ncmoe` 没有跨显卡的固定最佳值：它表示放到 CPU 的 MoE 专家层数，数值越大通常越省显存，但会降低速度。先用 GPU KV 跑通，再根据显存余量调整。

### 推荐起点

RTX 30/40/50 系列优先从下面的配置开始：

```text
-ngl 99
-ncmoe 0
-c 32768                 # 按需要改成 90112 或其他上下文长度
-np 1
-fa on
-ctk q8_0
-ctv q8_0
--fit
--ctx-checkpoints 64
--prompt-cache-disk
--slot-save-path D:\ai\kvstore\apex-ranges
```

混合模型默认不要加 `--no-kv-offload`。GPU KV 通常比 CPU KV 快很多；只有显存确实不够启动，或长上下文导致显存不足时，才考虑 CPU KV。

### 按显存调整 `-ncmoe`

推荐用阶梯方式测试，而不是直接套用另一台机器的值：

```text
-ncmoe 0  # 首选：专家和 KV 尽量留在 GPU
-ncmoe 1
-ncmoe 2
-ncmoe 4
-ncmoe 8
```

每次只增加一个档位，并记录启动后的显存和生成速度。出现 OOM、启动失败或 decode 明显下降时，退回上一个档位。当前 GTX 1080 上使用的 `-ncmoe 23` 是 8 GB 显存的特殊折中值，不应直接用于 RTX 30/40/50 系列。

经验上，显存更大的卡应先尝试 `-ncmoe 0`；同一显存下，RTX 40/50 系列通常比 RTX 30 系列有更大的速度余量，但最终结果仍取决于模型量化、上下文长度、KV 类型和 batch 参数。

### 一套可复现的速度测试

保持模型、prompt、`-c`、`-b`、`-ub` 和生成长度不变，只修改 `-ncmoe` 或是否使用 `--no-kv-offload`：

```powershell
$body = @{ prompt = ("Benchmark text: " + (("The quick brown fox tests MoE inference speed. ") * 180)); n_predict = 128; temperature = 0.2; ignore_eos = $true } | ConvertTo-Json -Compress
Invoke-RestMethod http://127.0.0.1:8000/completion -Method Post `
  -Headers @{ Authorization = "Bearer YOUR_KEY"; "Content-Type" = "application/json" } `
  -Body $body
```

比较日志中的：

```text
prompt eval time ... tokens per second
eval time ... tokens per second
prompt cache = ... reused ... recomputed
```

选配置时优先看 `eval time` 的持续 token/s，其次看显存余量和服务稳定性。第一次请求可能是冷算；速度比较至少应使用相同 prompt 重复一次，并区分 prompt prefill 和 decode。

### 常见问题

| 现象 | 处理 |
|---|---|
| 启动 OOM | 增大 `-ncmoe`，或降低 `-c`；不要先关闭 GPU KV |
| 吐字突然变慢 | 检查是否加了 `--no-kv-offload`，并比较 GPU 利用率和 `eval time` |
| 显存还有余量但速度低 | 从更小的 `-ncmoe` 重新测试，专家留 CPU 会增加 PCIe/CPU 路径开销 |
| 长 prompt 每次都冷算 | 检查 prompt 前缀是否一致、`--slot-save-path` 是否相同、缓存目录是否可写 |
| 缓存目录被删除 | 当前版本会在后台写入前自动重建目录；被删除的旧快照无法恢复，需要重新写入 |
| 缓存显示配置不匹配 | 模型、上下文长度或 KV 类型变化会使旧条目被安全忽略 |

完整的 90K 上下文示例：

```text
-ngl 99 -ncmoe 0 -c 90112 -np 1 -fa on -ctk q8_0 -ctv q8_0
--fit --ctx-checkpoints 64
--checkpoint-range1-end 30000 --checkpoint-range1-step 1000
--checkpoint-range2-end 40000 --checkpoint-range2-step 256
--checkpoint-range3-step 4096
--slot-save-path D:\ai\kvstore\apex-ranges --prompt-cache-disk
```

如果该配置显存不足，依次尝试 `-ncmoe 1`、`2`、`4`，直到启动稳定；不要把 GTX 1080 的 `-ncmoe 23` 作为新显卡默认参数。

> [!IMPORTANT]
> **This is the PrismML fork of llama.cpp**, the main line behind the [Bonsai](https://huggingface.co/collections/prism-ml/bonsai) models (branch `prism`, developed as `prism-v7`). It tracks current mainline llama.cpp and adds the fork's low-bit formats and runtime features on top.
>
> **New here? Start with the [Bonsai-demo](https://github.com/PrismML-Eng/Bonsai-demo) repo.** It downloads the right models and the correct prebuilt binaries for your hardware/backend automatically.
>
> **Which ternary model file to use:**
>
> - `*-PQ2_0.gguf` (fork group-128, ggml id 142): preferred on Metal, CUDA, HIP and CPU. About 6% smaller than group-64.
> - `*-Q2_0_g64.gguf` / 27B `*-Q2_g64.gguf` (official group-64, ggml id 42): runs on every backend here AND on mainline llama.cpp. If unsure, use this. Newer model releases name this file plain `*-Q2_0.gguf`.
> - `*-Q2_0.gguf` on OLDER model repos is the **deprecated legacy format** (group 128 stored as id 42). It does not load on these builds; the error tells you which file to get instead. If you must run it, use the frozen [`prism-v5`](https://github.com/PrismML-Eng/llama.cpp/tree/prism-v5) line and its final release [`prism-b9601`](https://github.com/PrismML-Eng/llama.cpp/releases/tag/prism-b9601-68faa14).
>
> **Speculative decoding (dspark)** is supported via mainline's draft-dspark plus fork patches. Drafters published for older model releases need a one-time conversion with `gguf-dspark-to-dflash` (see [SPECULATIVE.md](https://github.com/PrismML-Eng/Bonsai-demo/blob/main/SPECULATIVE.md) in Bonsai-demo); newer releases ship ready-to-use drafters.
>
> Do NOT build from `prism-v6` (stale mid-migration snapshot) and do NOT mix this fork's `ggml-*` libraries with a stock llama.cpp build.

---

![llama](https://raw.githubusercontent.com/ggml-org/llama.brand/refs/heads/master/cover/llama-cpp/cover-llama-cpp-dark.svg)

<div align="center">

<b>LLM inference in C/C++</b>

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](https://opensource.org/licenses/MIT)
[![Release](https://img.shields.io/github/v/release/ggml-org/llama.cpp?filter=v*&color=brightgreen)](https://github.com/ggml-org/llama.cpp/releases?q=tag:v0)
[![Nightly](https://img.shields.io/github/v/release/ggml-org/llama.cpp?label=nightly&filter=b*&color=orange)](https://github.com/ggml-org/llama.cpp/releases?q=b)
[![Server](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/server.yml?label=Server)](https://github.com/ggml-org/llama.cpp/actions/workflows/server.yml)
[![Docker](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/docker.yml?label=Docker)](https://github.com/ggml-org/llama.cpp/actions/workflows/docker.yml)
[![Winget](https://img.shields.io/github/actions/workflow/status/ggml-org/llama.cpp/winget.yml?label=Winget)](https://github.com/ggml-org/llama.cpp/actions/workflows/winget.yml)

[ggml](https://github.com/ggml-org/ggml) / [ops](https://github.com/ggml-org/llama.cpp/blob/master/docs/ops.md) / [maintainer PRs](https://github.com/ggml-org/llama.cpp/issues?q=is%3Apr%20is%3Aopen%20draft%3AFalse%20(author%3Argerganov%20OR%20author%3AKitaitiMakoto%20OR%20author%3Adanbev%20OR%20author%3Aaldehir%20OR%20author%3Amax-krasnyansky%20OR%20author%3ACISC%20OR%20author%3Aggerganov%20OR%20author%3Aam17an%20OR%20author%3Abartowski1182%20OR%20author%3Anikwen%20OR%20author%3Ahipudding%20OR%20author%3AServeurpersoCom%20OR%20author%3Apwilkin%20OR%20author%3Areeselevine%20OR%20author%3Angxson%20OR%20author%3Ajeffbolznv%20OR%20author%3Amarty1885%20OR%20author%3A0cc4m%20OR%20author%3ATitaniumtown%20OR%20author%3Aangt%20OR%20author%3AIMbackK%20OR%20author%3Aarthw%20OR%20author%3AJohannesGaessler%20OR%20author%3AORippler%20OR%20author%3Aruixiang63%20OR%20author%3Axctan%20OR%20author%3Aallozaur%20OR%20author%3Ayomaytk%20OR%20author%3Aaendk%20OR%20author%3Agaugarg-nv%20OR%20author%3Ataronaeo%20OR%20author%3Aforforever73%20OR%20author%3Alhez%20OR%20author%3Anetrunnereve%20OR%20author%3Afairydreaming)%20sort%3Aupdated-desc) / [dev stats](https://github.com/ggml-org/llama.cpp-dev) / [lib llama API](https://github.com/ggml-org/llama.cpp/issues/9289) / [llama-server REST API](https://github.com/ggml-org/llama.cpp/issues/9291)

</div>

## Quick start

A few options to get `llama.cpp` installed on your machine:

- Visit https://llama.app and follow the instructions
- Run with Docker - see our [Docker documentation](docs/docker.md)
- Download pre-built binaries from the [releases page](https://github.com/ggml-org/llama.cpp/releases)
- Build from source by cloning this repository - check out [our build guide](docs/build.md)

Once installed:

```sh
# Download and run a model directly from Hugging Face
llama cli -hf ggml-org/Qwen3.5-0.8B-GGUF

# Launch OpenAI-compatible API server
llama serve -hf ggml-org/Qwen3.5-0.8B-GGUF
```

<table align="center">
    <tr>
        <td align="center" width=50%>
            <img width="1310" height="888" alt="VLM session with `llama cli`" src="https://github.com/user-attachments/assets/88726b48-1713-48aa-a525-95a02e78afc4" />
            <i>VLM session with <b>llama cli</b></i>
        </td>
        <td align="center">
            <img width="1392" height="958" alt="Built-in web UI against `llama serve` running Qwen 3.6" src="https://github.com/user-attachments/assets/b402f972-2e32-4def-8771-8d849f08cf2e" />
            <i>Built-in web UI against <b>llama serve</b></i>
        </td>
    </tr>
<table>

## Description

The main goal of `llama.cpp` is to enable LLM (and VLM) inference with minimal setup and state-of-the-art performance on
a wide range of hardware - locally and in the cloud.

- Plain C/C++ implementation without any dependencies
- Apple silicon is a first-class citizen - optimized via ARM NEON, Accelerate and Metal frameworks
- AVX, AVX2, AVX512 and AMX support for x86 architectures
- RVV, ZVFH, ZFH, ZICBOP and ZIHINTPAUSE support for RISC-V architectures
- 1.5-bit, 2-bit, 3-bit, 4-bit, 5-bit, 6-bit, and 8-bit integer quantization for faster inference and reduced memory use
- Custom CUDA kernels for running LLMs on NVIDIA GPUs (support for AMD GPUs via HIP and Moore Threads GPUs via MUSA)
- Vulkan and SYCL backend support
- CPU+GPU hybrid inference to partially accelerate models larger than the total VRAM capacity

The `llama.cpp` project is build on top of the [ggml](https://github.com/ggml-org/ggml) library.

## Supported backends

| Backend | Target devices |
| --- | --- |
| [BLAS](docs/build.md#blas-build) | All |
| [BLIS](docs/backend/BLIS.md) | All |
| [CANN](docs/build.md#cann) | Ascend NPU |
| [CUDA](docs/build.md#cuda) | Nvidia GPU |
| [HIP](docs/build.md#hip) | AMD GPU |
| [Hexagon [In Progress]](docs/backend/snapdragon/README.md) | Snapdragon |
| [IBM zDNN](docs/backend/zDNN.md) | IBM Z & LinuxONE |
| [MUSA](docs/build.md#musa) | Moore Threads GPU |
| [Metal](docs/build.md#metal-build) | Apple Silicon |
| [OpenCL](docs/backend/OPENCL.md) | Adreno GPU |
| [OpenVINO [In Progress]](docs/backend/OPENVINO.md) | Intel CPUs, GPUs, and NPUs |
| [RPC](https://github.com/ggml-org/llama.cpp/tree/master/tools/rpc) | All |
| [SYCL](docs/backend/SYCL.md) | Intel GPU |
| [VirtGPU](docs/backend/VirtGPU.md) | VirtGPU APIR |
| [Vulkan](docs/build.md#vulkan) | GPU |
| [WebGPU](docs/build.md#webgpu) | All |
| [ZenDNN](docs/build.md#zendnn) | AMD CPU |

## Documentation

#### Tools

- [cli](tools/cli/README.md)
- [completion](tools/completion/README.md)
- [server](tools/server/README.md)
- [GBNF grammars](grammars/README.md)

#### Development

- [How to build](docs/build.md)
- [Running on Docker](docs/docker.md)
- [Build on Android](docs/android.md)
- [Multi-GPU usage](docs/multi-gpu.md)
- [Performance troubleshooting](docs/development/token_generation_performance_tips.md)
- [GGML tips & tricks](https://github.com/ggml-org/llama.cpp/wiki/GGML-Tips-&-Tricks)
- [XCFramework](docs/xcframework.md)
- [Completions](docs/completions.md)
- [Models](docs/models.md)
- [Release process](docs/release.md)

## Contributing

- Contributors can open PRs
- Collaborators will be invited based on contributions
- Maintainers can push to branches in the `llama.cpp` repo and merge PRs into the `master` branch
- Any help with managing issues, PRs and projects is very appreciated!
- Read the [CONTRIBUTING.md](CONTRIBUTING.md) for more information

## Acknowledgements

- [yhirose/cpp-httplib](https://github.com/yhirose/cpp-httplib) - Single-header HTTP server, used by `llama-server` - MIT license
- [nothings/stb](https://github.com/nothings/stb) - Single-header image format decoder, used by multimodal subsystem - Public domain
- [nlohmann/json](https://github.com/nlohmann/json) - Single-header JSON library, used by various tools/examples - MIT License
- [mackron/miniaudio](https://github.com/mackron/miniaudio) - Single-header audio format decoder, used by multimodal subsystem - Public domain
- [sheredom/subprocess.h](https://github.com/sheredom/subprocess.h) - Single-header process launching solution for C and C++ - Public domain
