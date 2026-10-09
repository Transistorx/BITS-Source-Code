# No-secret log check. Fails when an ESP_LOGx statement in main/ or components/
# could print a credential:
#   - the format string mentions password/passwd/secret/token/psk AND takes a
#     value (%s, %d, ...), or
#   - an argument expression names such an identifier.
# A literal message that merely says "password" with no value is allowed
# (e.g. "password too short"). Exit 0 = clean.

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$words = '(?i)(password|passwd|secret|token|psk)'
$bad = @()

Get-ChildItem -Path (Join-Path $root 'main'), (Join-Path $root 'components') -Recurse -Include *.c, *.h |
    Where-Object { $_.FullName -notmatch '\\(build|managed_components)\\' } |
    ForEach-Object {
        $text = [IO.File]::ReadAllText($_.FullName)
        foreach ($m in [regex]::Matches($text, 'ESP_LOG[EWIDV]\((?:[^;]|"[^"]*;[^"]*")*?\);', 'Singleline')) {
            $stmt = $m.Value
            # Split literal string pieces from the rest (argument expressions).
            $literals = ([regex]::Matches($stmt, '"(?:[^"\\]|\\.)*"') | ForEach-Object { $_.Value }) -join ' '
            $args = [regex]::Replace($stmt, '"(?:[^"\\]|\\.)*"', '""')
            $args = $args -replace '^ESP_LOG[EWIDV]\(\s*[A-Za-z_]+\s*,', ''
            $hit = ($literals -match $words -and $literals -match '%[-+ #0-9.lhzjt]*[sdiuxXcfp]') -or ($args -match $words)
            if ($hit) {
                $line = ($text.Substring(0, $m.Index) -split "`n").Count
                $bad += ('{0}:{1}: {2}' -f $_.FullName.Substring($root.Length + 1), $line, ($stmt -replace '\s+', ' '))
            }
        }
    }

if ($bad.Count -gt 0) {
    $bad | ForEach-Object { Write-Host "SECRET_LOG_RISK $_" }
    Write-Host "check_log_secrets: FAIL ($($bad.Count))"
    exit 1
}
Write-Host 'check_log_secrets: OK'
exit 0
