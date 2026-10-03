llama.cpp Persistent Prompt Disk Cache - Windows CUDA v0.2.2

1. 解压整个压缩包，不要单独移动 EXE 或 DLL。
2. 把启动命令里的 model.gguf 改成你的 GGUF 模型路径。
3. 在本目录打开 PowerShell，先运行 nvidia-smi，再运行 llama-server.exe；无需安装 CUDA Toolkit。

示例：
llama-server.exe -m model.gguf -c 32768 --port 8000 --slot-save-path D:/kvstore/mymodel --prompt-cache-disk --checkpoint-min-step 4096 --ctx-checkpoints 64 --prompt-cache-disk-budget 20

需要 NVIDIA 驱动支持 CUDA 12.0 或更高版本。如果 nvidia-smi 失败，或提示
CUDA driver version is insufficient for CUDA runtime version，请更新 NVIDIA 驱动。

请求端无需修改，cache_prompt 默认是 true。缓存命中可在服务日志中看到：
prompt disk: restored N prompt tokens to device
