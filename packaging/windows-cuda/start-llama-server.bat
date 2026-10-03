@echo off
setlocal
cd /d "%~dp0"

echo ===============================================
echo  Persistent Prompt Disk Cache - Windows CUDA
echo ===============================================
echo.

where nvidia-smi >nul 2>&1
if errorlevel 1 (
    echo [ERROR] nvidia-smi was not found. Install/update the NVIDIA driver first.
    pause
    exit /b 1
)

echo [GPU check]
nvidia-smi
echo.

if not exist "%~dp0llama-server.exe" (
    echo [ERROR] llama-server.exe is missing. Keep this script beside the DLL files.
    pause
    exit /b 1
)

set "MODEL="
set /p "MODEL=Enter the full path to your GGUF model: "
if "%MODEL%"=="" (
    echo [ERROR] No model path was entered.
    pause
    exit /b 1
)

if not exist "%MODEL%" (
    echo [ERROR] Model file not found:
    echo %MODEL%
    pause
    exit /b 1
)

if not exist "%~dp0cache" mkdir "%~dp0cache"

echo.
echo [Starting server]
echo Open http://127.0.0.1:8080 in your client.
echo Keep this window open while using the model.
echo.

"%~dp0llama-server.exe" ^
  -m "%MODEL%" ^
  --host 127.0.0.1 --port 8080 ^
  -c 32768 -np 1 -ngl 99 -fa on --fit ^
  --slot-save-path "%~dp0cache" ^
  --prompt-cache-disk ^
  --prompt-cache-disk-budget 20 ^
  --checkpoint-min-step 2048 ^
  --ctx-checkpoints 64

echo.
echo Server stopped. Press any key to close this window.
pause >nul
