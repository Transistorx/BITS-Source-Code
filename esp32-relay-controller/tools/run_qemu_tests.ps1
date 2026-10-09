# Builds tests/qemu and runs it under QEMU, then evaluates the serial output.
# Exits 0 only when the suite reports zero failures.
#
# QEMU is located by IDF via shutil.which('qemu-system-xtensa'), so it only
# needs to be on PATH.

$ErrorActionPreference = 'Stop'

$qemuBin = 'C:\Espressif\tools\qemu-xtensa\bin'
if (-not (Test-Path "$qemuBin\qemu-system-xtensa.exe")) {
    throw "QEMU not found at $qemuBin."
}

$testDir = (Resolve-Path (Join-Path $PSScriptRoot '..\tests\qemu')).Path

$job = Start-Job -ScriptBlock {
    param($dir, $bin)
    . 'C:\Espressif\tools\Microsoft.v6.1.PowerShell_profile.ps1' 6>&1 | Out-Null
    # Prepend after the profile: the job subprocess otherwise lacks the QEMU dir.
    if (Test-Path $bin) { $env:PATH = "$bin;$env:PATH" }
    Set-Location $dir
    idf.py qemu 2>&1
} -ArgumentList $testDir, $qemuBin

$deadline = (Get-Date).AddSeconds(600)
$text = ''
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1000
    # Receive-Job drains what has arrived since the last call, so accumulate.
    # Overwriting here loses any line that landed in an earlier window.
    $chunk = (Receive-Job $job -ErrorAction SilentlyContinue | Out-String)
    if ($chunk) { $text += $chunk }
    if ($text -match 'TEST_SUITE_END') { break }
    if ($job.State -ne 'Running') {
        $text += (Receive-Job $job -ErrorAction SilentlyContinue | Out-String)
        break
    }
}

Stop-Job $job -ErrorAction SilentlyContinue
Remove-Job $job -Force -ErrorAction SilentlyContinue
Get-Process -Name 'qemu-system-xtensa' -ErrorAction SilentlyContinue | Stop-Process -Force

$logDir = Join-Path $testDir 'build'
if (Test-Path $logDir) { $text | Set-Content (Join-Path $logDir 'qemu_serial.txt') }

$lines = $text -split "`r?`n" | Where-Object { $_ -match 'TEST_' }
$lines | ForEach-Object { Write-Host $_.Trim() }

if ($text -notmatch 'TEST_SUITE_END') {
    Write-Host '--- diagnostics ---'
    $text -split "`r?`n" |
        Where-Object { $_ -match 'could not be found|No such file|undefined reference|error:|CMake Error|ninja: error|fatal error' } |
        Select-Object -First 12 |
        ForEach-Object {
            $line = $_.Trim()
            if ($line.Length -gt 300) { $line = $line.Substring(0, 300) + ' ...[truncated]' }
            Write-Host $line
        }
    throw 'suite did not complete'
}
if ($text -match 'TEST_FAIL') { throw 'one or more tests failed' }

Write-Host 'ALL TESTS PASSED'