#requires -RunAsAdministrator
<#
.SYNOPSIS
    WLAN_Monitor 附带工具：诊断并尝试修复「移动热点一开就关 / 无法开启」。

.DESCRIPTION
    症状链：SoftAP 起来后约 2 秒被系统关掉，同时 ICS(SharedAccess) 服务崩溃
    （svchost.exe_SharedAccess / ntdll.dll / 0xc0000008）。
    常见原因是长时间开着 5GHz 热点跨过睡眠/休眠后，内核侧 NAT/WFP 句柄状态损坏。

    脚本按「能救就救，救不了就明确告诉你需要重启」的顺序处理：
      0. 先排除 DFS 干扰（DFS 不是 ICS 崩溃，但同样导致热点起不来）
      1. 重启 ICS 服务栈（停 icssvc + 杀掉 SharedAccess 宿主进程 + 重启）
      2. 重置无线适配器
      3. 重启 WinNAT / HNS
      4. 每档之后都真实尝试启动热点并观察是否稳住

.PARAMETER DwellSeconds
    启动热点后观察多少秒判定“稳住”，默认 8 秒。

.EXAMPLE
    .\heal-ics.ps1
    .\heal-ics.ps1 -DwellSeconds 12
#>
[CmdletBinding()]
param(
    [int]$DwellSeconds = 8
)

# WinRT 反射在 PowerShell 7 (Core) 上不可用，自动切回 Windows PowerShell 5.1 执行
if ($PSVersionTable.PSEdition -eq 'Core' -and -not $env:WLAN_MONITOR_RELAUNCH) {
    $env:WLAN_MONITOR_RELAUNCH = '1'
    $ps51 = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    & $ps51 -NoProfile -ExecutionPolicy Bypass -File $PSCommandPath -DwellSeconds $DwellSeconds
    exit $LASTEXITCODE
}

$ErrorActionPreference = 'Stop'
$script:fixed = $false

function Write-Step($text) { Write-Host ""; Write-Host "== $text" -ForegroundColor Cyan }
function Write-Ok($text)   { Write-Host "   [OK] $text" -ForegroundColor Green }
function Write-Bad($text)  { Write-Host "   [!!] $text" -ForegroundColor Yellow }
function Write-Info($text) { Write-Host "   $text" }

# ---------------------------------------------------------------- WinRT helpers
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$asTaskGeneric = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
    $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]

function AwaitOp($op, $type) {
    $m = $asTaskGeneric.MakeGenericMethod($type); $t = $m.Invoke($null, @($op)); $t.Wait(-1) | Out-Null; $t.Result
}
function AwaitAction($op) {
    $t = $asTaskAction.Invoke($null, @($op)); $t.Wait(-1) | Out-Null
}

[Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime] | Out-Null
[Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime] | Out-Null
[Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime] | Out-Null

function Get-TetheringManager {
    $p = [Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile()
    if (-not $p) { throw '当前没有可用的 Internet 连接，热点无法启动（检查上游网络）' }
    return [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($p)
}

function Get-IcsCrashEvents([int]$hours = 24) {
    Get-WinEvent -FilterHashtable @{ LogName = 'System'; Id = 7031, 7034; StartTime = (Get-Date).AddHours(-$hours) } -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'Connection Sharing' }
}

# 真实尝试启动热点：返回 $true 表示成功保持 DwellSeconds 秒
function Test-HotspotStart {
    $mgr = Get-TetheringManager
    if ($mgr.TetheringOperationalState -ne 'Off') {
        AwaitOp ($mgr.StopTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult]) | Out-Null
        Start-Sleep -Seconds 2
    }
    $before = (Get-IcsCrashEvents 1 | Measure-Object).Count
    $r = AwaitOp ($mgr.StartTetheringAsync()) ([Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult])
    Start-Sleep -Seconds 2
    $s1 = $mgr.TetheringOperationalState
    Start-Sleep -Seconds $DwellSeconds
    $s2 = $mgr.TetheringOperationalState
    $after = (Get-IcsCrashEvents 1 | Measure-Object).Count

    Write-Info ("启动结果=" + $r.Status + "  2s=" + $s1 + "  " + $DwellSeconds + "s=" + $s2 + "  ICS 新增崩溃=" + ($after - $before))

    if ($s2 -eq 'On') {
        try { Write-Info ("客户端数=" + $mgr.ClientCount) } catch {}
        return $true
    }
    if ($r.Status -eq 'WiFiDeviceOff') { Write-Bad '返回 WiFiDeviceOff：多为 DFS 信道导致，请用 WLAN_Monitor 切到非 DFS 信道' }
    if (($after - $before) -gt 0)      { Write-Bad '伴随 ICS 崩溃事件（7031/7034）：这是本次要修的目标' }
    return $false
}

function Restart-IcsStack {
    Write-Info '停止 icssvc …'
    & net stop icssvc 2>&1 | Out-Null
    Start-Sleep -Seconds 1

    $q = & sc.exe queryex SharedAccess | Out-String
    $state = ([regex]::Match($q, 'STATE\s*:\s*\d+\s+(\w+)')).Groups[1].Value
    $pidLine = ([regex]::Match($q, 'PID\s*:\s*(\d+)')).Groups[1].Value

    if ($state -eq 'STOP_PENDING' -and $pidLine) {
        Write-Info "SharedAccess 卡在 STOP_PENDING，强制结束宿主进程 PID=$pidLine"
        & taskkill /f /pid $pidLine 2>&1 | Out-Null
        Start-Sleep -Seconds 2
    } elseif ($pidLine -and $pidLine -ne '0') {
        Write-Info "结束 SharedAccess 宿主进程 PID=$pidLine"
        & taskkill /f /pid $pidLine 2>&1 | Out-Null
        Start-Sleep -Seconds 2
    }

    Write-Info '启动 SharedAccess / icssvc …'
    & sc.exe start SharedAccess 2>&1 | Out-Null
    Start-Sleep -Seconds 2
    Start-Service icssvc -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 3
    Write-Info ((Get-Service SharedAccess, icssvc | ForEach-Object { "$($_.Name)=$($_.Status)" }) -join '  ')
}

# ---------------------------------------------------------------- 0. 环境检查
Write-Step '0. 环境检查'

if (-not ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Write-Bad '需要管理员权限运行（非提权进程查询/控制 WLAN 会被拒绝）'
    exit 2
}

$crashEvents = Get-IcsCrashEvents 24
if ($crashEvents) {
    Write-Bad ("近 24 小时 ICS 崩溃 " + ($crashEvents | Measure-Object).Count + " 次，最近一次 " + (($crashEvents | Sort-Object TimeCreated | Select-Object -Last 1).TimeCreated))
} else {
    Write-Ok '近 24 小时没有 ICS 崩溃记录'
}

$wlanInfo = (& netsh wlan show interfaces | Out-String)
$ch = ([regex]::Match($wlanInfo, '通道\s*:\s*(\d+)')).Groups[1].Value
$band = ([regex]::Match($wlanInfo, '波段\s*:\s*(.+)')).Groups[1].Value.Trim()
if ($ch) {
    Write-Info ("上游信道=$ch  频段=$band")
    if ([int]$ch -in 52..64 -or [int]$ch -in 100..144) {
        Write-Bad '上游在 DFS 信道：5GHz 热点必然会启动失败，先用 WLAN_Monitor 切到非 DFS 信道'
    }
} else {
    Write-Bad '未读取到无线连接信息'
}

Write-Step '基线测试：先看现在能不能开热点'
if (Test-HotspotStart) {
    Write-Ok '热点正常，无需修复'
    exit 0
}

# ---------------------------------------------------------------- 1~3. 逐档修复
$tiers = @(
    @{ Name = '第 1 档：重启 ICS 服务栈';          Action = { Restart-IcsStack } },
    @{ Name = '第 2 档：重置无线适配器';            Action = {
            & net stop icssvc 2>&1 | Out-Null
            Disable-NetAdapter -Name 'WLAN' -Confirm:$false -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 4
            Enable-NetAdapter -Name 'WLAN' -Confirm:$false -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 8
            Start-Service icssvc -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 3
        } },
    @{ Name = '第 3 档：重启 WinNAT / HNS';         Action = {
            & net stop icssvc 2>&1 | Out-Null
            foreach ($s in @('winnat', 'hns')) {
                try { Restart-Service $s -Force -ErrorAction Stop } catch { Write-Info "跳过 $s : $($_.Exception.Message)" }
                Start-Sleep -Seconds 2
            }
            Start-Service icssvc -ErrorAction SilentlyContinue
            Start-Sleep -Seconds 3
        } }
)

foreach ($t in $tiers) {
    Write-Step $t.Name
    & $t.Action
    Start-Sleep -Seconds 2
    if (Test-HotspotStart) {
        Write-Ok ("修复成功（" + $t.Name + "）")
        $script:fixed = $true
        break
    }
    Write-Bad '本档无效'
}

# ---------------------------------------------------------------- 结论
Write-Step '结论'
if ($script:fixed) {
    Write-Ok '热点已可正常启动。建议：串流结束后先关热点再让机器睡眠，可显著降低复发概率。'
    exit 0
}

Write-Bad '三档在线修复全部无效：损坏状态在内核层（NAT/WFP 句柄），服务重启清不掉。'
Write-Info '需要重启：'
Write-Info '    Restart-Computer            （或 shutdown /r /t 0）'
Write-Info '重启后再跑一次本脚本确认，正常输出应为「热点正常，无需修复」。'
exit 1
