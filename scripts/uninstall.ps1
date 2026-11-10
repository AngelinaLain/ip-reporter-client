#Requires -Version 5.1
#Requires -RunAsAdministrator
<# .SYNOPSIS 删除 IP Reporter 服务和程序；默认保留配置。 #>
[CmdletBinding()]
param([switch]$PurgeConfig)
$ErrorActionPreference = 'Stop'
$serviceName = 'IpReporter'
$binaryPath = Join-Path $env:ProgramFiles 'IpReporter\ip-reporter.exe'
$configPath = Join-Path $env:ProgramData 'IpReporter\config.json'
$service = Get-Service -Name $serviceName -ErrorAction SilentlyContinue
if ($service) {
    Stop-Service -Name $serviceName -Force
    (Get-Service -Name $serviceName).WaitForStatus('Stopped', [TimeSpan]::FromSeconds(60))
    & (Join-Path $env:SystemRoot 'System32\sc.exe') delete $serviceName | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Service deletion failed: $LASTEXITCODE" }
}
# 只删除明确属于本程序的文件，避免递归删除用户目录。
if (Test-Path -LiteralPath $binaryPath) { Remove-Item -LiteralPath $binaryPath -Force }
if ($PurgeConfig -and (Test-Path -LiteralPath $configPath)) { Remove-Item -LiteralPath $configPath -Force }
Write-Host 'IP Reporter uninstalled.'
if (-not $PurgeConfig) { Write-Host "Configuration retained at $configPath" }
