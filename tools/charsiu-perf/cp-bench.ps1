# cp-bench.ps1 - charsiu 0.2 on the board against the build it replaces
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File cp-bench.ps1 -Model R:\npu\Llama-3.2-1B-Instruct-Q8_0.gguf -Old C:\path\to\old\charsiu_run.exe [-Out R:\npu\cp-bench] [-Reps 3] [-NoSettle] [-Only new|old]
#
# From the unpacked charsiu-m7-arm64 artifact (this script beside its
# charsiu_run.exe). -Old is the charsiu_run.exe in use now (charsiu doctor
# names the folder). The NPU variables are the front door's; the weight
# cache beside the model is used, never deleted (both builds read the same
# file, the format did not change). Arms, in this order:
#
#   new-1 .. new-N   the new build, cached start, the chat question: the
#                    staging of every run after the first is the number
#   old-1 .. old-N   the old build, the same
#   new-fread        the new build, CHARSIU_WCACHE_MAP=0 (the cache read
#                    as before: what the mapping buys)
#   new-pack         the new build, CHARSIU_NPU_PACK_DIRECT=0 and
#                    CHARSIU_NPU_COPY_POOL=0 (what the direct pack buys)
#   dec-new / dec-old  64 tokens of "The capital of France is", the
#                    bench prompt with --ignore-eos, -Reps each, interleaved
#   dec-spec         the new build, CHARSIU_SPEC=auto (speculation is
#                    off by default on Windows; this is the control)
#
# -Only new or -Only old runs that build's chat arms and nothing else: the
# way to time a cold start of each, one per reboot.
#
# summary.txt (and summary.csv, every column) has one line an arm: staging
# (charsiu's own clock, the only whole number), the time inside
# charsiu_npu_add, what the new build read of the cache from disk (MB, ms, records already
# in memory), the driver's CREATE_BO and
# MAP_BO, prompt and decode rates, free memory before the arm; then whether
# the arms of one build printed the same text. The heartbeat's "quantising"
# is not a column: staging is lazy, so that wall clock also holds the
# prompt's own work between tensors.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [Parameter(Mandatory = $true)][string]$Model,
    [Parameter(Mandatory = $true)][string]$Old,
    [string]$Out = '.\cp-bench',
    [int]$Reps = 3,
    [switch]$NoSettle,
    [ValidateSet('', 'new', 'old')][string]$Only = ''
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$new = Join-Path $here 'charsiu_run.exe'
foreach ($f in $Model, $Old, $new) { if (-not (Test-Path -LiteralPath $f)) { throw "missing $f" } }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

Get-ChildItem Env: | Where-Object { $_.Name -like 'CHARSIU_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$base = @{
    CHARSIU_NPU = '1'; CHARSIU_NPU_QUANT = '1'; CHARSIU_NPU_W4V = '1'
    CHARSIU_NPU_KMAX = '1024'; CHARSIU_NPU_W4_GROUP = '1024'
    CHARSIU_NPU_MAXN = '262144'; CHARSIU_COEF_ELEMS = '65536'
    CHARSIU_RKNPU_STATS = '1'
}

$bat = Join-Path $Out 'run.cmd'
function Run-Arm([string]$Name, [string]$Exe, [string]$ArgLine, [hashtable]$Extra) {
    $vars = $base.Clone(); foreach ($k in $Extra.Keys) { $vars[$k] = $Extra[$k] }
    $set = ($vars.Keys | Sort-Object | ForEach-Object { "set $_=$($vars[$_])" }) -join "`r`n"
    $o = Join-Path $Out "$Name.txt"; $e = Join-Path $Out "$Name.err"
    $line = "`"$Exe`" `"$Model`" -c 512 -t 4 --cache $ArgLine > `"$o`" 2> `"$e`""
    [IO.File]::WriteAllText($bat, "@echo off`r`n$set`r`n$line`r`nexit /b %errorlevel%`r`n", [Text.Encoding]::ASCII)
    $free = [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)
    Write-Host "cp-bench: $Name ($free MB free)"
    & cmd.exe /d /c "`"$bat`"" | Out-Host
    return [pscustomobject]@{ Name = $Name; Code = $LASTEXITCODE; FreeMB = $free }
}

# A server or the desktop app that just exited gives its memory back over
# some seconds; an arm started inside that window measures the board, not
# charsiu (2026-11-28: 128 MB free, 41.9 s staging). So wait, up to two
# minutes, for 1200 MB free first. -NoSettle starts at once, which is the
# way to test a start under memory pressure on purpose.
if (-not $NoSettle) {
    for ($w = 0; $w -lt 120; $w += 5) {
        $f = [int]((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory / 1024)
        if ($f -ge 1200) { break }
        Write-Host "cp-bench: $f MB free, waiting"
        Start-Sleep -Seconds 5
    }
}

$chat = '-p "Name three colors." --chat -n 64'
$dec = '-p "The capital of France is" -n 64 --ignore-eos'
$arms = @()
if ($Only -ne 'old') { for ($i = 1; $i -le $Reps; $i++) { $arms += Run-Arm "new-$i" $new $chat @{} } }
if ($Only -ne 'new') { for ($i = 1; $i -le $Reps; $i++) { $arms += Run-Arm "old-$i" $Old $chat @{} } }
if (-not $Only) {
    $arms += Run-Arm 'new-fread' $new $chat @{ CHARSIU_WCACHE_MAP = '0' }
    $arms += Run-Arm 'new-pack' $new $chat @{ CHARSIU_NPU_PACK_DIRECT = '0'; CHARSIU_NPU_COPY_POOL = '0' }
    for ($i = 1; $i -le $Reps; $i++) {
        $arms += Run-Arm "dec-new-$i" $new $dec @{}
        $arms += Run-Arm "dec-old-$i" $Old $dec @{}
    }
    $arms += Run-Arm 'dec-spec' $new $dec @{ CHARSIU_SPEC = 'auto' }
}

function Num($m, $i) { if ($m.Success) { $m.Groups[$i].Value } else { '' } }
$rows = foreach ($a in $arms) {
    $t = Get-Content -Raw (Join-Path $Out "$($a.Name).txt") -ErrorAction SilentlyContinue
    $e = Get-Content -Raw (Join-Path $Out "$($a.Name).err") -ErrorAction SilentlyContinue
    $all = "$t`n$e"
    $st = [regex]::Matches($all, 'tensors staged, (\d+) ms of which (\d+) ms adding and (\d+) ms quantising')
    $last = if ($st.Count) { $st[$st.Count - 1] } else { $null }
    $al = [regex]::Matches($all, 'buffers: (\d+) allocated, CREATE_BO ([0-9.]+) ms .*MAP_BO ([0-9.]+) ms')
    $create = 0.0; $mapbo = 0.0
    foreach ($m in $al) { $create += [double]$m.Groups[2].Value; $mapbo += [double]$m.Groups[3].Value }
    $body = if ($t) { ($t -split "`n" | Where-Object { $_ -notmatch '^\[' }) -join "`n" } else { '' }
    $md5 = [BitConverter]::ToString([Security.Cryptography.MD5]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($body))).Replace('-', '').Substring(0, 8)
    [pscustomobject]@{
        arm = $a.Name; rc = $a.Code; free_mb = $a.FreeMB
        staging_ms = Num ([regex]::Match($all, 'staging (\d+) ms')) 1
        adding_ms = if ($last) { $last.Groups[2].Value } else { '' }
        readin_mb = Num ([regex]::Match($all, 'weight cache read in: (\d+) MB from disk')) 1
        readin_ms = Num ([regex]::Match($all, 'weight cache read in: \d+ MB from disk in (\d+) ms')) 1
        resident = Num ([regex]::Match($all, '(\d+ of \d+) records already in memory')) 1
        create_bo_ms = [int]$create
        map_bo_ms = [int]$mapbo
        prompt_tok_s = Num ([regex]::Match($all, 'prompt \d+ tok in \d+ ms, ([0-9.]+) tok/s')) 1
        gen_tok_s = Num ([regex]::Match($all, 'gen \d+ tok in \d+ ms, ([0-9.]+) tok/s')) 1
        mapped = [bool]($all -match 'weight cache .*mapped')
        errors = ([regex]::Matches($all, 'TIMED OUT|IOMMU FAULT|DMA ERROR|SUBMIT of .* failed| [1-9][0-9]* refused')).Count
        md5 = $md5
    }
}
$sum = Join-Path $Out 'summary.txt'
$rows | Export-Csv -NoTypeInformation -Encoding ASCII (Join-Path $Out 'summary.csv')
$rows | Format-Table -AutoSize | Out-String -Width 400 | Set-Content -Encoding ASCII $sum
foreach ($g in 'new-[0-9]', 'old-[0-9]', 'dec-new', 'dec-old') {
    $h = @($rows | Where-Object { $_.arm -match "^$g" } | ForEach-Object md5 | Sort-Object -Unique)
    Add-Content -Encoding ASCII $sum ("{0,-8} arms print {1} text(s): {2}" -f $g, $h.Count, ($h -join ' '))
}
Get-Content $sum
