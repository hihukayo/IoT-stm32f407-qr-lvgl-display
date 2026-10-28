<#
.SYNOPSIS
    把本仓库推送到 GitHub，并自动处理常见的网络/代理问题。

.DESCRIPTION
    本机直连 github.com:443 经常被重置，但 127.0.0.1:7890 上的本地代理可用。
    本脚本会：
      1. 先试直连，失败则自动改走本地代理（可用 -ProxyUrl 指定）；
      2. 用 Git Credential Manager 读取 Windows 凭据管理器里已保存的 GitHub 账号；
      3. 推送 main 分支并设置 upstream。

    若提示 403（Permission denied to <账号>），说明当前登录的 GitHub 账号
    **对该仓库没有写权限**，需要二选一：
      a) 用仓库所有者账号登录：`git credential-manager erase` 清掉旧凭据后重新登录；
      b) 让仓库所有者在 GitHub 上把该账号加为 Collaborator（Settings → Collaborators）。

.PARAMETER RemoteUrl
    目标仓库地址，默认本项目的 GitHub 地址。

.PARAMETER Branch
    要推送的本地分支，默认 main。

.PARAMETER ProxyUrl
    本地代理地址，默认 http://127.0.0.1:7890（Clash/V2RayN 常见端口）。

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/push_to_github.ps1
#>
param(
    [string]$RemoteUrl = "https://github.com/hihukayo/IoT-stm32f407-qr-lvgl-display.git",
    [string]$Branch = "main",
    [string]$ProxyUrl = "http://127.0.0.1:7890"
)

$ErrorActionPreference = "Stop"
try {
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
} catch { }

$repo = Resolve-Path (Join-Path $PSScriptRoot "..")
Set-Location $repo

$env:GIT_TERMINAL_PROMPT = "0"
$env:GCM_INTERACTIVE = "never"

# --- 1. 确保 remote 存在 ---
$existing = git remote 2>$null
if ($existing -notcontains "origin") {
    git remote add origin $RemoteUrl
    Write-Host "已添加 remote origin -> $RemoteUrl" -ForegroundColor Green
} else {
    git remote set-url origin $RemoteUrl
}

# --- 2. 确认本地已提交 ---
$dirty = git status --porcelain
if ($dirty) {
    Write-Host "工作区有未提交的改动，请先 git add/commit：" -ForegroundColor Yellow
    $dirty | Select-Object -First 10
    exit 1
}

# --- 3. 选择连接方式：直连优先，失败改用代理 ---
$commonArgs = @(
    "-c", "credential.helper=manager",
    "-c", "credential.interactive=false",
    "-c", "http.postBuffer=524288000"
)

function Test-GithubDirect {
    $code = & curl.exe -s -o NUL -w "%{http_code}" --max-time 15 "https://github.com" 2>$null
    return ($code -eq "200")
}

$useProxy = $false
if (-not (Test-GithubDirect)) {
    $useProxy = $true
    Write-Host "直连 github.com 失败，改用代理 $ProxyUrl" -ForegroundColor Yellow
}

$pushArgs = @()
if ($useProxy) {
    $pushArgs += @("-c", "http.proxy=$ProxyUrl", "-c", "https.proxy=$ProxyUrl")
}
$pushArgs += $commonArgs
$pushArgs += @("push", "-u", "origin", $Branch)

Write-Host "执行：git $($pushArgs -join ' ')" -ForegroundColor Cyan
& git @pushArgs
$rc = $LASTEXITCODE

if ($rc -ne 0) {
    Write-Host ""
    Write-Host "推送失败 (exit=$rc)。常见原因：" -ForegroundColor Red
    Write-Host "  1) 403 / Permission denied：当前 GitHub 账号对该仓库无写权限"
    Write-Host "     处理：让仓库所有者把你加为 Collaborator，或改用所有者账号登录。"
    Write-Host "     清理已保存凭据： git credential-manager erase"
    Write-Host "                   协议 https / 主机 github.com / 用户名 <旧账号>"
    Write-Host "  2) 连接被重置：确认本地代理端口（默认 7890）是否在运行，可用 -ProxyUrl 指定。"
    Write-Host "  3) 文件过大：检查是否误提交了 Objects/ Listings/ 等编译产物。"
    exit $rc
}

Write-Host ""
Write-Host "推送成功：$RemoteUrl (分支 $Branch)" -ForegroundColor Green
& git log --oneline -1
& git remote -v
exit 0
