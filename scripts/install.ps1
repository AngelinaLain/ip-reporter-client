#Requires -Version 5.1
#Requires -RunAsAdministrator
<#
.SYNOPSIS
安装或升级 IP Reporter Windows 服务；已有配置默认保留。
#>
[CmdletBinding()]
param(
    [string]$Endpoint,
    [string]$BinaryPath,
    [string]$TokenFile,
    [string]$DeviceId,
    [int]$PollSeconds = 60,
    [int]$HeartbeatSeconds = 300,
    [int]$TimeoutSeconds = 10,
    [switch]$AllowHttp,
    [switch]$Reconfigure
)

$ErrorActionPreference = 'Stop'
$serviceName = 'IpReporter'
$installDir = Join-Path $env:ProgramFiles 'IpReporter'
$configDir = Join-Path $env:ProgramData 'IpReporter'
$targetBinary = Join-Path $installDir 'ip-reporter.exe'
$configPath = Join-Path $configDir 'config.json'
$newBinary = Join-Path $installDir ('ip-reporter.new.' + $PID + '.exe')
$newConfig = Join-Path $configDir ('config.new.' + $PID + '.json')
$scExe = Join-Path $env:SystemRoot 'System32\sc.exe'

function Invoke-ServiceControl {
    param([string[]]$Arguments)
    & $scExe @Arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "sc.exe failed with exit code $LASTEXITCODE" }
}

# 禁止普通用户读取令牌，只给管理员、SYSTEM 和服务账户必要权限。
function Set-PrivateAcl {
    param([string]$Path, [bool]$Directory)
    if ($Directory) { $acl = New-Object System.Security.AccessControl.DirectorySecurity }
    else { $acl = New-Object System.Security.AccessControl.FileSecurity }
    $acl.SetAccessRuleProtection($true, $false)
    $inheritance = [System.Security.AccessControl.InheritanceFlags]::None
    if ($Directory) { $inheritance = [System.Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit' }
    foreach ($sid in @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-19')) {
        $identity = New-Object System.Security.Principal.SecurityIdentifier($sid)
        $rights = [System.Security.AccessControl.FileSystemRights]::FullControl
        if ($sid -eq 'S-1-5-19') { $rights = [System.Security.AccessControl.FileSystemRights]::ReadAndExecute }
        $rule = New-Object System.Security.AccessControl.FileSystemAccessRule(
            $identity, $rights, $inheritance,
            [System.Security.AccessControl.PropagationFlags]::None,
            [System.Security.AccessControl.AccessControlType]::Allow)
        $acl.AddAccessRule($rule)
    }
    Set-Acl -LiteralPath $Path -AclObject $acl
}

if (-not $BinaryPath) {
    $candidates = @(
        (Join-Path $PSScriptRoot '..\bin\ip-reporter.exe'),
        (Join-Path $PSScriptRoot '..\..\..\bin\ip-reporter.exe'),
        (Join-Path $PSScriptRoot '..\build\Release\ip-reporter.exe'),
        (Join-Path $PSScriptRoot '..\build\ip-reporter.exe')
    )
    $BinaryPath = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if (-not $BinaryPath -or -not (Test-Path -LiteralPath $BinaryPath -PathType Leaf)) {
    throw 'Cannot find ip-reporter.exe. Supply -BinaryPath with a built release binary.'
}
$BinaryPath = (Resolve-Path -LiteralPath $BinaryPath).Path
$needsConfig = $Reconfigure -or -not (Test-Path -LiteralPath $configPath -PathType Leaf)
if ($needsConfig -and -not $Endpoint) { throw 'Supply -Endpoint for first installation or -Reconfigure.' }
if (-not $needsConfig -and ($Endpoint -or $TokenFile -or $DeviceId -or $AllowHttp)) {
    Write-Warning 'Existing configuration is preserved. Use -Reconfigure to change initialization settings.'
}
# 重新配置默认继承已验证配置的设备 ID，避免同一主机在服务端成为新机器。
# 只读取 ID，不把旧令牌或接收地址合并到新配置。显式 ID 可用于损坏配置恢复。
if ($needsConfig -and -not $DeviceId -and (Test-Path -LiteralPath $configPath -PathType Leaf)) {
    $existingDeviceId = & $BinaryPath --print-device-id --config $configPath
    if ($LASTEXITCODE -ne 0 -or -not $existingDeviceId -or
        $existingDeviceId -is [array] -or $existingDeviceId -cnotmatch '^[A-Za-z0-9._-]{1,128}\z') {
        throw 'Cannot preserve device_id from existing configuration. Repair it or supply -DeviceId explicitly.'
    }
    $DeviceId = $existingDeviceId
}
if ($TokenFile) { $TokenFile = (Resolve-Path -LiteralPath $TokenFile).Path }

New-Item -ItemType Directory -Force -Path $installDir, $configDir | Out-Null
Set-PrivateAcl -Path $installDir -Directory $true
Set-PrivateAcl -Path $configDir -Directory $true

try {
    # 使用待安装程序生成临时配置，检查成功后才停止现有服务。
    if ($needsConfig) {
        $initArgs = @('--init', '--config', $newConfig, '--endpoint', $Endpoint,
            '--poll-seconds', "$PollSeconds", '--heartbeat-seconds', "$HeartbeatSeconds",
            '--timeout-seconds', "$TimeoutSeconds")
        if ($TokenFile) { $initArgs += @('--token-file', $TokenFile) }
        if ($DeviceId) { $initArgs += @('--device-id', $DeviceId) }
        if ($AllowHttp) { $initArgs += '--allow-http' }
        & $BinaryPath @initArgs
        if ($LASTEXITCODE -ne 0) { throw 'Configuration initialization failed.' }
        Set-PrivateAcl -Path $newConfig -Directory $false
        $checkPath = $newConfig
    } else { $checkPath = $configPath }
    & $BinaryPath --check-config --config $checkPath
    if ($LASTEXITCODE -ne 0) { throw 'Configuration validation failed.' }
    if ($BinaryPath -ne $targetBinary) { Copy-Item -LiteralPath $BinaryPath -Destination $newBinary -Force }
    $existingService = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
    if ($existingService) {
        Stop-Service -Name $serviceName -Force
        (Get-Service -Name $serviceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60))
    }
    if (Test-Path -LiteralPath $newBinary) { Move-Item -LiteralPath $newBinary -Destination $targetBinary -Force }
    if ($needsConfig) { Move-Item -LiteralPath $newConfig -Destination $configPath -Force }
    Set-PrivateAcl -Path $targetBinary -Directory $false
    Set-PrivateAcl -Path $configPath -Directory $false
    $commandLine = '"' + $targetBinary + '" --service --config "' + $configPath + '"'
    # 通过服务 API 写入命令行，避免 PowerShell 5.1 调用 sc.exe 时丢失嵌套引号。
    if ($existingService) {
        $serviceInstance = Get-CimInstance -ClassName Win32_Service -Filter "Name='IpReporter'"
        $changeResult = Invoke-CimMethod -InputObject $serviceInstance -MethodName Change -Arguments @{ PathName = $commandLine }
        if ($changeResult.ReturnValue -ne 0) { throw "Service update failed: $($changeResult.ReturnValue)" }
    } else {
        New-Service -Name $serviceName -BinaryPathName $commandLine -DisplayName 'IP Reporter' -StartupType Automatic | Out-Null
    }
    Invoke-ServiceControl -Arguments @('config', $serviceName, 'start=', 'delayed-auto', 'obj=', 'NT AUTHORITY\LocalService')
    $storedImagePath = (Get-ItemProperty -LiteralPath 'HKLM:\SYSTEM\CurrentControlSet\Services\IpReporter' -Name ImagePath).ImagePath
    if ($storedImagePath -cne $commandLine) { throw 'Service executable path verification failed.' }
    Invoke-ServiceControl -Arguments @('description', $serviceName, 'Reports local IPv4/IPv6 addresses and service heartbeats to the configured endpoint.')
    Invoke-ServiceControl -Arguments @('failure', $serviceName, 'reset=', '86400',
        'actions=', 'restart/60000/restart/120000/restart/300000')
    Invoke-ServiceControl -Arguments @('failureflag', $serviceName, '1')
    Start-Service -Name $serviceName
    (Get-Service -Name $serviceName).WaitForStatus('Running', [TimeSpan]::FromSeconds(30))
    Write-Host "Installed $serviceName. Config: $configPath"
    Write-Host 'Manage with Get-Service IpReporter / Stop-Service IpReporter.'
} finally {
    foreach ($temporaryPath in @($newBinary, $newConfig)) {
        if (Test-Path -LiteralPath $temporaryPath) { Remove-Item -LiteralPath $temporaryPath -Force }
    }
}
