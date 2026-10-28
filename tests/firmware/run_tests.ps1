<#
.SYNOPSIS
    在 PC 上编译并运行固件"纯逻辑模块"的单元测试（无需 STM32 硬件）。

.DESCRIPTION
    用主机 gcc 编译以下固件源码（原样编译，不改一行）：
      Core/Src/uart_protocol.c   帧协议 / CRC16 / CRC32 / 解析状态机
      Core/Src/qr_service.c      qrcodegen 封装
      Core/Src/qr_bitmap.c       240x240 1bpp 位图渲染
      Core/Src/bmp_stream.c      BMP 1bpp 文件头与流式读取
      Middlewares/qrcodegen/qrcodegen.c
    并链接 tests/firmware/stub/stm32f4xx_hal.h 提供的 HAL 桩。

    运行后自动调用 validate_bmp.py 校验导出 BMP 的结构与二维码内容。

.PARAMETER Gcc
    gcc 可执行文件路径。默认在 PATH 中查找 gcc。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tests/firmware/run_tests.ps1
#>
param(
    [string]$Gcc = "gcc"
)

$ErrorActionPreference = "Stop"

# 让中文输出（本脚本与 Python 子进程）在控制台和管道里都不乱码
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    $env:PYTHONIOENCODING = "utf-8"
} catch {
    # 某些受限环境不允许改控制台编码，忽略即可
}

$repo = Resolve-Path (Join-Path $PSScriptRoot "..\..")
$outDir = Join-Path $PSScriptRoot "out"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

Write-Host "=== 1/4 编译主机端测试程序 ===" -ForegroundColor Cyan
$gccArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-O2", "-g",
    "-I$PSScriptRoot/stub",
    "-I$repo/firmware/Core/Inc",
    "-I$repo/firmware/Middlewares/qrcodegen",
    "-o", "$outDir/test_fw.exe",
    "$PSScriptRoot/test_main.c",
    "$repo/firmware/Core/Src/uart_protocol.c",
    "$repo/firmware/Core/Src/qr_service.c",
    "$repo/firmware/Core/Src/qr_bitmap.c",
    "$repo/firmware/Core/Src/bmp_stream.c",
    "$repo/firmware/Middlewares/qrcodegen/qrcodegen.c"
)
& $Gcc @gccArgs
if ($LASTEXITCODE -ne 0) {
    Write-Host "编译失败 (exit=$LASTEXITCODE)" -ForegroundColor Red
    exit 1
}

Write-Host "=== 2/4 运行单元测试 ===" -ForegroundColor Cyan
Push-Location $PSScriptRoot
try {
    & "$outDir/test_fw.exe"
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}
if ($rc -ne 0) {
    Write-Host "单元测试失败 (exit=$rc)" -ForegroundColor Red
    exit $rc
}

$py = Get-Command python -ErrorAction SilentlyContinue
if (-not $py) {
    Write-Host "未找到 python，跳过 BMP 校验与跨实现比对" -ForegroundColor Yellow
} else {
    Write-Host "=== 3/4 校验导出的 BMP 与二维码内容 ===" -ForegroundColor Cyan
    & $py.Source (Join-Path $PSScriptRoot "validate_bmp.py") $outDir
    if ($LASTEXITCODE -ne 0) {
        Write-Host "BMP 校验失败 (exit=$LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }

    Write-Host "=== 4/4 MCU(C) <-> PC(Python) 协议一致性 ===" -ForegroundColor Cyan
    & $py.Source (Join-Path $PSScriptRoot "check_interop.py")
    if ($LASTEXITCODE -ne 0) {
        Write-Host "协议一致性比对失败 (exit=$LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
}

Write-Host ""
Write-Host "全部软件测试通过。" -ForegroundColor Green
Write-Host "注意：LCD 显示、扫码器识别、串口实物通信仍需上板验证。" -ForegroundColor Yellow
exit 0
