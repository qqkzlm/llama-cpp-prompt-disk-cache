# llama.cpp + Persistent Prompt Disk Cache

[![Release](https://img.shields.io/github/v/release/qqkzlm/llama-cpp-prompt-disk-cache?color=brightgreen)](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Patch](https://img.shields.io/badge/patch-v0.1.0-orange)](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/download/v0.1.0/0001-feat-server-add-persistent-prompt-disk-cache.patch)
[![Backend](https://img.shields.io/badge/backend-CUDA%20%7C%20source-blue)](tools/server/README.md)

> **一句话：关机、重启、换模型之后，4 万 token 的提示词不用重新预填充，首字时间从 155 秒降到 6 秒。**
>
> **In one line:** kill the server, reboot the box, reload the model — a 40K-token prompt
> no longer re-runs prefill. **Time-to-first-token: ~155 s cold → ~6 s warm.**
>
> 原理是把 KV 缓存的前缀检查点（`.centry` 快照）持久化到磁盘，服务启动时自动恢复最长
> 匹配的前缀。客户端不用改、协议不用变，服务自己记得上次算过什么。
> The patch persists KV-cache prefix checkpoints to disk (`.centry` snapshots) and restores
> the longest matching prefix on boot. No client changes — the server just remembers.

## 三步跑起来

这是给第一次尝试的用户准备的最短路径。完整参数说明见
[server 文档](tools/server/README.md)。

> **适用范围：**下面的直接下载包是 NVIDIA CUDA 版本，适用于 Windows 和 Linux。
> 它不包含模型，也不适用于 AMD/Intel/Apple GPU。Windows 用户可以双击压缩包里的
> `start-llama-server.bat` 自动检查显卡、询问模型路径并启动；需要自定义参数时再用下方命令。

### 1. 下载带缓存功能的 server

- **Windows CUDA（NVIDIA）**：从 [v0.2.3 Release](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/tag/v0.2.3) 下载
  `llama-server-win-cuda-v0.2.3.zip`，解压整个目录，不要只拿走 `.exe`。
- **Linux CUDA（NVIDIA）**：同一个 Release 里下载 `llama-server-linux-cuda-v0.2.3.tar.gz`，
  该包为 `sm_61` / Pascal 编译，因此 GTX 10 系也能跑。
- **AMD / Intel / Apple GPU**：本项目只提供 CUDA 预编译包，其他后端请下载源码按
  [构建说明](docs/build.md) 编译。注意本项目是
  [PrismML/llama.cpp](https://github.com/PrismML-Eng/llama.cpp) 的 `prism` 分支补丁，
  不是 stock `ggml-org/llama.cpp` 二进制——直接用官方二进制**不会**有缓存功能。
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
  --prompt-cache-disk-budget 20 `
  --checkpoint-min-step 2048 `
  --ctx-checkpoints 64
```

Linux 示例：

```bash
# 确认 NVIDIA 驱动可用
nvidia-smi

tar xzf llama-server-linux-cuda-v0.2.3.tar.gz
./llama-server \
  -m /models/model.gguf \
  --host 0.0.0.0 --port 8080 \
  -c 32768 -np 1 -ngl 99 -fa on \
  --fit \
  --slot-save-path /var/tmp/kvstore/model \
  --prompt-cache-disk-budget 20 \
  --checkpoint-min-step 2048 \
  --ctx-checkpoints 64
```

如果不想输入命令，使用压缩包里的 `start-llama-server.bat`。它会询问模型文件路径，
自动使用压缩包目录旁的 `cache` 文件夹保存缓存。也可以继续手动使用上面的 PowerShell 命令。

### 到底需要哪些参数

**唯一必须手写的是 `--slot-save-path`**（缓存保存到哪）。其余都有合理默认值：

| 参数 | 必须？ | 说明 |
|---|---|---|
| `--slot-save-path` | **是** | 缓存根目录。`--prompt-cache-disk-budget`、`--prompt-cache-disk-path` 都挂在它下面，不给这个参数其它磁盘缓存参数一律无效。条目实际落在 `<该路径>/pdcache` |
| `--prompt-cache-disk` | 否 | 磁盘缓存开关，**默认已开启**，写不写都行。想临时关掉用 `--no-prompt-cache-disk` |
| `--prompt-cache-disk-budget` | 否 | 磁盘占用上限（GiB），默认 20，`0` 表示不限。超出后自动淘汰旧条目 |
| `--prompt-cache-disk-path` | 否 | 想把条目放到别的目录时才用，默认 `<slot-save-path>/pdcache` |
| `--prompt-cache-disk-namespace` | 否 | 在同一个根目录下按项目隔离子目录，多个项目共用一份缓存时用 |
| `--checkpoint-min-step` | 否 | 每增长多少 token 存一次快照，默认 8192。对话分支多就调小（2048），更在意磁盘和内存开销就调大（4096） |
| `--ctx-checkpoints` | 否 | 内存里最多保留多少个检查点，默认 32 |

上面的示例命令把可调项都写出来了，方便你照抄后逐项调整；只看原理的话，记住 `--slot-save-path` 一个就够。

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

## 缓存什么时候会命中，什么时候不会

这块最容易产生误解，先讲清楚：

**会命中：**

- 同一个 prompt **原样重发**，哪怕中间关了服务、重启了机器
- 长对话持续增长：每次请求的 prompt = 旧对话 + 新增几条，历史部分直接复用，
  只算新增的。上面表格里 40K 提示词只重算 32 个 token 就是这个场景
- 服务重启后换了进程，但用同一个 `--slot-save-path`

**不会命中：**

- **prompt 前缀变了**。缓存只认「从第一个 token 开始的公共前缀」。如果你在最前面插了
  一句 system prompt，后面全部错位，缓存等于没有。所以别把时间戳、随机 ID 之类
  会变化的内容放在 prompt 最前面
- **换了模型、上下文长度或 KV 类型**。检查点带配置签名，不匹配的条目会被安全忽略
  （直接当没缓存，不会算错），换回来才会重新命中
- **`cache_prompt: false`**，或者客户端自己在改写 prompt
- 缓存目录被删了，或者 `--slot-save-path` 指向了别的位置

**前缀一致性的实际做法：**把稳定内容（system prompt、工具定义、长文档）放在最前面，
把每轮变化的内容（用户新消息、时间戳）放最后。opencode、Claude Code 这类 agent 客户端
天然就是这个结构，所以开箱即用。

**分支回退时更快：**对话分支（同一段历史换个方向继续）本来要从分叉点重算，
有了磁盘检查点可以直接回退到内存里已有的检查点，不用从磁盘重读。

## 实现位置

核心代码在 `tools/server/server-prompt-disk.cpp`（约 900 行）加上 slot/task 的接入，
完整参数语义见 [tools/server/README.md](tools/server/README.md)。
补丁是相对 PrismML `prism` 分支的单个 commit：
- **只想用现成的**：下 [v0.2.3 Release](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/tag/v0.2.3) 的预编译包
- **要改代码**：直接 clone 本仓库（`main` 是当前开发线），或在 `prism` 分支上 `git am`
  [v0.1.0 补丁](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/download/v0.1.0/0001-feat-server-add-persistent-prompt-disk-cache.patch)

<details>
<summary><b>English summary (what you get)</b></summary>

> This repo is an experimental persistent prompt-prefix cache for the PrismML llama.cpp fork.
> Long agentic sessions re-send the same growing conversation on every request. This patch
> makes the server **persist the KV-cache prefix to disk and restore it automatically**, so a
> restart (or a new process on the same prompt) skips the prefill it has already paid for.
>
> ```bash
> llama-server -m model.gguf -c 32768 \
>   --slot-save-path D:/kvstore/mymodel \    # the only REQUIRED flag
>   --checkpoint-min-step 4096 \
>   --ctx-checkpoints 64 \
>   --prompt-cache-disk-budget 20           # --prompt-cache-disk defaults to ON
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
> reference. Prebuilt CUDA binaries (Windows + Linux) are on the
> [v0.2.3 Release](https://github.com/qqkzlm/llama-cpp-prompt-disk-cache/releases/tag/v0.2.3);
> `main` is the active development line.

</details>

### NVIDIA 驱动要求

The v0.2.3 CUDA packages require an NVIDIA driver that supports CUDA 12.0 or newer.
Before starting, run `nvidia-smi` and update the driver if the command fails or reports
an older driver. The Windows package needs a Windows R525-class-or-newer driver; the
Linux package was built for CUDA `sm_61` / Pascal and also requires a compatible Linux
NVIDIA driver. A driver error during startup usually appears as `CUDA driver version is
insufficient for CUDA runtime version`; this means the driver must be updated, not that
the model or cache is broken.

### 实测数据

在 GTX 1080 8 GB + Qwen3.6-35B-A3B-NVFP4-Q4_K_M、`-c 92160`、4 万 token 智能体提示词下测得：

| | 冷算（无缓存） | 命中 |
|---|---|---|
| 预填充 40K | ~151 s（265 tok/s） | 1.35 s（仅重算 32 个 token） |
| 解码 | 24–28 tok/s | 22–28 tok/s（265 个 token 全程稳定） |
| 端到端 | ~155 s | ~6 s |
| 磁盘开销 | 1.5 s 写入 | 2.1 s 读取+解析+恢复（仅重启后首次） |

英文对照 / in English:

| | Cold (no cache) | Full prefix hit |
|---|---|---|
| Prefill 40K | ~151 s (265 tok/s) | 1.35 s (32 tokens recomputed) |
| Decode | 24–28 tok/s | 22–28 tok/s (sustained over 265 tokens) |
| End-to-end | ~155 s | ~6 s |
| Disk cost | 1.5 s write | 2.1 s read + parse + restore (first request after restart only) |

**MoE 分层经验**：专家留 GPU，KV 跟层走。保持 `-ncmoe 0` + `--fit`，hybrid 模型不要加
`--no-kv-offload`（40 层里只有 10 层带 KV）。把 20 层 MoE 搬去 CPU 会直接崩：
预填充掉到 30 tok/s，解码掉到 4 tok/s。混合模型保持 prefix-only 关闭。

**MoE layering lesson** (same box): keep experts on GPU (`-ncmoe 0` + `--fit`), let KV
follow the layers (hybrid models must not add `--no-kv-offload` — only 10 of 40 layers
carry KV). Pushing 20 MoE layers to CPU collapsed prefill to 30 tok/s and decode to
4 tok/s. Keep `--prompt-cache-disk-prefix-only` off for hybrid models.

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
> **This is the PrismML fork of llama.cpp** (branch `prism`), the main line behind the
> [Bonsai](https://huggingface.co/collections/prism-ml/bonsai) models. It tracks current
> mainline llama.cpp and adds the fork's low-bit formats and runtime features on top.
> **New here? Start with [Bonsai-demo](https://github.com/PrismML-Eng/Bonsai-demo)** — it
> picks the right models and prebuilt binaries for your hardware/backend automatically.
>
> <details>
> <summary><b>三元模型文件怎么选 / which model file to use（仅本 fork 相关）</b></summary>
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
>
> </details>

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
