# Demonstrates the production guard: SCALE_CMD_TEST_HOOKS must be rejected
# unless BITS_QEMU_TEST_PROJECT is also set, and the production ELF (if built)
# must not contain the test seam. Run from the ESP-IDF PowerShell profile.
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$guard = Join-Path $root 'components\scale_cmd\include\scale_cmd_guard.h'
$cc = (Get-Command xtensa-esp32-elf-gcc -ErrorAction Stop).Source
$fail = 0

function Try-Pre([string[]]$defs) {
    $ErrorActionPreference = 'Continue'  # gcc writes the #error to stderr
    & $cc -E -x c @defs $guard 2>$null | Out-Null
    return ($LASTEXITCODE -eq 0)
}
if (-not (Try-Pre @())) { Write-Host 'FAIL: clean production preprocess rejected'; $fail++ }
else { Write-Host 'PASS: production (no hooks) accepted' }
if (Try-Pre @('-DSCALE_CMD_TEST_HOOKS=1')) { Write-Host 'FAIL: TEST_HOOKS accepted in production'; $fail++ }
else { Write-Host 'PASS: TEST_HOOKS without BITS_QEMU_TEST_PROJECT rejected (#error)' }
if (-not (Try-Pre @('-DSCALE_CMD_TEST_HOOKS=1', '-DBITS_QEMU_TEST_PROJECT=1'))) { Write-Host 'FAIL: QEMU test project rejected'; $fail++ }
else { Write-Host 'PASS: TEST_HOOKS with BITS_QEMU_TEST_PROJECT accepted' }

$elf = Join-Path $root 'build\weight-sender.elf'
if (-not (Test-Path $elf)) { $elf = (Get-ChildItem (Join-Path $root 'build') -Filter *.elf -ErrorAction SilentlyContinue | Where-Object { $_.Name -notmatch 'bootloader' } | Select-Object -First 1).FullName }
if ($elf -and (Test-Path $elf)) {
    $nm = (Get-Command xtensa-esp32-elf-nm -ErrorAction Stop).Source
    if (& $nm $elf | Select-String 'scale_cmd_test_allow_post_encode') { Write-Host 'FAIL: test seam symbol in production ELF'; $fail++ }
    else { Write-Host 'PASS: production ELF has no test seam symbol' }
}
if ($fail -ne 0) { throw "$fail guard check(s) failed" }
Write-Host 'PROD GUARD OK'

