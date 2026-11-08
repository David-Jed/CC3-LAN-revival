# Runs the lanfix harness under several lanfix.ini settings and prints the key results.
# Usage: powershell -NoProfile -File test\run-tests.ps1 [-StaleIp 192.168.0.48] [-OtherIp 10.5.0.2]
#   -OtherIp: an address of another local adapter (e.g. a connected VPN) to test bind redirection
param([string]$StaleIp = '192.168.0.48', [string]$OtherIp = '')

$test = Join-Path $PSScriptRoot '..\build\test' | Resolve-Path
$ini = Join-Path $test 'lanfix.ini'
$log = Join-Path $test 'lanfix.log'
$template = Get-Content (Join-Path $PSScriptRoot '..\lanfix.ini')

function Invoke-Case([string]$label, [hashtable]$settings) {
    $lines = $template
    foreach ($k in $settings.Keys) { $lines = $lines -replace "^$k=.*$", "$k=$($settings[$k])" }
    $lines | Set-Content -Encoding ascii $ini
    Remove-Item $log -ErrorAction SilentlyContinue
    "===== $label"
    if ($OtherIp) { & (Join-Path $test 'harness.exe') $StaleIp $OtherIp }
    else { & (Join-Path $test 'harness.exe') $StaleIp }
    if (Test-Path $log) {
        Get-Content $log | Where-Object { $_ -notmatch '  adapter ' } | ForEach-Object { "  log: " + $_.Substring(21) }
    }
}

Invoke-Case 'baseline (Enabled=0)'          @{ Enabled = 0 }
Invoke-Case 'auto'                          @{}
Invoke-Case 'Adapter=ZeroTier (no match)'   @{ Adapter = 'ZeroTier' }
Invoke-Case 'Adapter=VirtualBox (by name)'  @{ Adapter = 'VirtualBox' }
Invoke-Case 'FixBroadcast=0 FixBind=0'      @{ FixBroadcast = 0; FixBind = 0 }

$template | Set-Content -Encoding ascii $ini
