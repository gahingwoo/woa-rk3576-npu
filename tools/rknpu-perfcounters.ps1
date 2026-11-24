# rknpu-perfcounters.ps1 - register (or remove) rknpu.sys's performance counters
#
# rknpu.sys is a kernel-mode PCW provider ("RK3576 NPU", "RK3576 NPU Memory").
# An INF cannot register a counter provider, so this does what the WDK
# documents for one: lodctr /m with the manifest the INF copied into the
# driver store, and the driver store folder as the install path, so the
# names are read from the rknpu.sys there. The folder changes with every
# reinstall of a new package, so run this again after each one (after the
# reboot that binds it). -Uninstall runs unlodctr.
#
# Run as administrator. It changes nothing but the counter registration and
# writes what it did to %SystemRoot%\Temp\rknpu-perfcounters.log.
#
#   powershell -ExecutionPolicy Bypass -File rknpu-perfcounters.ps1
#   powershell -ExecutionPolicy Bypass -File rknpu-perfcounters.ps1 -Uninstall
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param([switch]$Uninstall)

$ErrorActionPreference = 'Stop'
$provider = '{6f33a325-aa80-4286-b76c-0b11e14f9f2b}'    # rknpu.man providerGuid
$log = Join-Path $env:SystemRoot 'Temp\rknpu-perfcounters.log'

function Say([string]$m) { $m | Tee-Object -FilePath $log -Append }

"--- $(Get-Date -Format s)" | Out-File -FilePath $log -Append

# Whatever was registered before (an older driver store folder): remove it.
# unlodctr fails when nothing is registered; that is fine here.
$o = & unlodctr.exe "/g:$provider" 2>&1
Say "unlodctr /g:$provider -> exit $LASTEXITCODE $o"
if ($Uninstall) {
    Say 'removed'
    exit 0
}

# The folder the running rknpu.sys came from.
$svc = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Services\rknpu' -Name ImagePath
$img = $svc.ImagePath -replace '^\\SystemRoot', $env:SystemRoot -replace '^\\\?\?\\', ''
if ($img -notmatch '^[A-Za-z]:') { $img = Join-Path $env:SystemRoot $img }
$dir = Split-Path -Parent $img
$man = Join-Path $dir 'rknpu.man'
Say "driver: $img"
if (-not (Test-Path $man)) {
    Say "no $man : this rknpu package has no performance counters"
    exit 1
}

$o = & lodctr.exe "/m:$man" "$dir" 2>&1
$rc = $LASTEXITCODE
Say "lodctr /m:$man $dir -> exit $rc $o"
if ($rc -ne 0) { exit 1 }

# What a consumer now sees.
foreach ($set in 'RK3576 NPU', 'RK3576 NPU Memory') {
    try {
        $s = Get-Counter -ListSet $set
        Say ("{0}: {1} counters, instances {2}" -f $set, $s.Counter.Count, (($s.PathsWithInstances |
             ForEach-Object { ($_ -split '[()]')[1] } | Sort-Object -Unique) -join ', '))
    } catch {
        Say "${set}: not visible yet ($($_.Exception.Message))"
    }
}
exit 0
