"""Run a sandboxed Windows installer with the real binary and mocked service APIs.

Only a temporary script copy omits elevation. ProgramFiles/ProgramData point into
the test directory; service control and ACL cmdlets cannot access the real system.
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile
import time
import unittest

PROJECT = Path(__file__).resolve().parents[1]

HARNESS = r'''
param([string]$Installer, [string]$Binary)
$ErrorActionPreference = 'Stop'
$env:ProgramFiles = Join-Path $PSScriptRoot 'ProgramFiles'
$env:ProgramData = Join-Path $PSScriptRoot 'ProgramData'
$global:TestService = $null
$global:StoredImagePath = ''
$global:Stopped = 0
function global:Set-Acl { [CmdletBinding()] param([string]$LiteralPath,$AclObject) }
function global:Get-Service { [CmdletBinding()] param([string]$Name) return $global:TestService }
function global:Stop-Service { [CmdletBinding()] param([string]$Name,[switch]$Force) $global:Stopped++ }
function global:Start-Service { [CmdletBinding()] param([string]$Name) }
function global:New-Service {
    [CmdletBinding()] param([string]$Name,[string]$BinaryPathName,[string]$DisplayName,[string]$StartupType)
    $global:StoredImagePath = $BinaryPathName
    $global:TestService = New-Object PSObject
    $global:TestService | Add-Member ScriptMethod WaitForStatus { param($status,$timeout) }
}
function global:Get-CimInstance { [CmdletBinding()] param([string]$ClassName,[string]$Filter) return (New-Object PSObject) }
function global:Invoke-CimMethod {
    [CmdletBinding()] param($InputObject,[string]$MethodName,[hashtable]$Arguments)
    $global:StoredImagePath = $Arguments.PathName
    return @{ ReturnValue = 0 }
}
function global:Get-ItemProperty { [CmdletBinding()] param([string]$LiteralPath,[string]$Name) return @{ ImagePath = $global:StoredImagePath } }
function Assert([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
function Read-Config { Get-Content -LiteralPath (Join-Path $env:ProgramData 'IpReporter\config.json') -Raw | ConvertFrom-Json }
$tokenFile = Join-Path $PSScriptRoot 'token.txt'
[IO.File]::WriteAllText($tokenFile, 'installer-test-secret')
& $Installer -BinaryPath $Binary -Endpoint 'https://first.invalid/report' -DeviceId 'first-device' -TokenFile $tokenFile
$first = Read-Config
Assert ($first.device_id -eq 'first-device') 'Initial explicit ID was not stored'
Assert ($first.bearer_token -eq 'installer-test-secret') 'Initial test token was not stored'
$configPath = Join-Path $env:ProgramData 'IpReporter\config.json'
$before = [IO.File]::ReadAllText($configPath)
# 首次安装后使用已复制的同版本二进制；身份测试不反复覆盖 EXE，避免文件扫描的短暂锁。
$installedBinary = Join-Path $env:ProgramFiles 'IpReporter\ip-reporter.exe'
& $Installer -BinaryPath $installedBinary
Assert ([IO.File]::ReadAllText($configPath) -ceq $before) 'Upgrade changed configuration'
& $Installer -BinaryPath $installedBinary -Reconfigure -Endpoint 'https://retained.invalid/report'
$retained = Read-Config
Assert ($retained.device_id -eq 'first-device') 'Reconfigure changed device identity'
Assert ($retained.endpoint -eq 'https://retained.invalid/report') 'New endpoint was not applied'
Assert ($retained.bearer_token -eq '') 'Reconfigure copied old credentials'
[IO.File]::WriteAllText($configPath, '{"device_id":"first-device"}')
$corrupt = [IO.File]::ReadAllText($configPath)
$stopped = $global:Stopped
$failed = $false
try { & $Installer -BinaryPath $installedBinary -Reconfigure -Endpoint 'https://recovery.invalid/report' }
catch { $failed = $true }
Assert $failed 'Corrupt old configuration changed identity silently'
Assert ([IO.File]::ReadAllText($configPath) -ceq $corrupt) 'Failed reconfiguration changed stored file'
Assert ($global:Stopped -eq $stopped) 'Failed reconfiguration stopped the running service'
& $Installer -BinaryPath $installedBinary -Reconfigure -Endpoint 'https://recovery.invalid/report' -DeviceId 'explicit-replacement'
$recovered = Read-Config
Assert ($recovered.device_id -eq 'explicit-replacement') 'Explicit identity did not override corrupt configuration'
Assert ($recovered.bearer_token -eq '') 'Recovery copied old credentials'
Write-Host 'Windows installer preserved identity, replaced settings and rejected implicit recovery.'
'''


@unittest.skipUnless(os.name == 'nt', 'Windows installer requires Windows PowerShell')
class WindowsInstallerIdentityTest(unittest.TestCase):
    binary: str

    def test_reconfigure_identity_and_recovery_with_real_binary(self):
        directory = tempfile.TemporaryDirectory(prefix='ip-reporter-windows-installer-')
        try:
            temporary = directory.name
            root = Path(temporary).resolve()
            source = (PROJECT / 'scripts/install.ps1').read_text(encoding='utf-8-sig')
            self.assertIn('#Requires -RunAsAdministrator', source)
            source = source.replace('#Requires -RunAsAdministrator', '# Test copy: mocked service and ACL APIs')
            assignment = "$scExe = Join-Path $env:SystemRoot 'System32\\sc.exe'"
            self.assertIn(assignment, source)
            source = source.replace(assignment, "$scExe = Join-Path $PSScriptRoot 'sc-mock.ps1'")
            installer = root / 'install.ps1'
            installer.write_text(source, encoding='utf-8-sig')
            (root / 'sc-mock.ps1').write_text('$global:LASTEXITCODE = 0\n', encoding='utf-8')
            harness = root / 'test.ps1'
            harness.write_text(HARNESS, encoding='utf-8-sig')
            powershell = Path(os.environ['SystemRoot']) / 'System32/WindowsPowerShell/v1.0/powershell.exe'
            result = subprocess.run([str(powershell), '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                str(harness), '-Installer', str(installer), '-Binary', self.binary],
                capture_output=True, encoding='utf-8', errors='replace', timeout=45)
            if result.returncode:
                print(result.stdout + result.stderr)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertNotIn('installer-test-secret', result.stdout + result.stderr)
        finally:
            # Windows 的可执行文件扫描可能短暂持有刚复制的文件，稍后重试清理。
            for attempt in range(20):
                try:
                    directory.cleanup()
                    break
                except PermissionError:
                    if attempt == 19:
                        raise
                    time.sleep(0.1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('binary')
    args, remaining = parser.parse_known_args()
    WindowsInstallerIdentityTest.binary = str(Path(args.binary).resolve())
    unittest.main(argv=[__file__, *remaining], verbosity=2)
