# m7-bench.ps1 - M7 on the board: where a decode token goes, and what each M7 change buys
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File m7-bench.ps1 -Model R:\npu\Llama-3.2-1B-Instruct-Q8_0.gguf [-Out R:\npu\m7-windows] [-Quick] [-SkipDriverTests] [-IdleDisable] [-KeepCache]
#   powershell -NoProfile -ExecutionPolicy Bypass -File m7-bench.ps1 -Report R:\npu\m7-windows
#
# As administrator, from the unpacked charsiu-m7-arm64 artifact with
# rknputest.exe from rknpu-m7-arm64 beside it (docs/dev/testing.md). Each step writes
# its own files under -Out, so a run that dies part way still says how far
# it got. -Report only rebuilds the tables from a finished -Out.
#
# Steps:
#   1. rknputest info, m3 and m6 (unless -SkipDriverTests): the M6 checks,
#      unchanged, against the M7 driver.
#   2. charsiu arms, all the same prompt, 128 tokens, -t 4. Every M7 change
#      is numerics-neutral, so EVERY NPU arm must print the same text; the
#      table says whether it did. Each arm turns ONE change off against the
#      M7 defaults, except short-m6, which turns all of them off:
#
#        short-m6      M6's port: CHARSIU_RKNPU_FUSE=0 CHARSIU_RKNPU_UCACHE=0
#                      CHARSIU_WIN_SYNC=pthread CHARSIU_WIN_SYSFS=0
#                      CHARSIU_RKNPU_QOS=0, no weight cache
#        short-npu     M7 defaults; writes the weight cache (--cache)
#        short-npu2    M7 defaults; reads it (the staging fix)
#        short-noucache CHARSIU_RKNPU_UCACHE=0    PREP / FINI by the driver, fused
#                      into SUBMIT and WAIT (the default before 2026-11-24)
#        short-nofuse  CHARSIU_RKNPU_UCACHE=0 CHARSIU_RKNPU_FUSE=0
#                      one IOCTL per PREP / FINI
#        short-nopin   CHARSIU_WIN_SYSFS=0        decode thread not pinned
#        short-pthread CHARSIU_WIN_SYNC=pthread   winpthreads' barrier
#        short-spin    CHARSIU_WIN_SPIN_US=50     SRW barrier spins 50 us first
#                      (the default before 2026-11-24)
#        short-noqos   CHARSIU_NPU_IDLE=1         no CPU hold (charsiu's control)
#        short-async   CHARSIU_RKNPU_ASYNC=1      a call's two SUBMITs overlap
#        short-idle    the processor idle-disable power setting for this arm
#                      only (restored after): the /dev/cpu_dma_latency control
#        short-npu1    CHARSIU_NPU_ONEDEV=1       one core
#        short-m7submit CHARSIU_RKNPU_M7SUBMIT=1  M8 driver, M7's SUBMIT checks (the
#                      full clock read, streams checked in the arena): the
#                      control for what M8 changed inside SUBMIT
#        stages        CHARSIU_STAGES=1, 32 tokens, M7 defaults
#        stages-m6     CHARSIU_STAGES=1, 32 tokens, as short-m6 (with the cache)
#        long-npu / long-m6   the 515-token prompt, 64 tokens (long-m6 with the cache)
#
#      -Quick runs short-m6, short-npu, short-npu2, stages and stages-m6.
#      -IdleDisable sets the idle-disable setting for every arm instead.
#   3. bench.txt (one line an arm), decode.txt (per generated token: the
#      NPU's busy time, the SUBMIT and WAIT IOCTLs split into the driver's
#      own time and the rest, the interrupt-to-waiter path, the PREP / FINI
#      IOCTLs that are left, charsiu's own decode split, and the two stage
#      tables side by side), bench.csv, text.md5.
#
# Every program runs through cmd.exe so its stdout reaches the file as bytes.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

param(
    [string]$Model = '',
    [string]$Out = '.\m7-windows',
    [string]$Report = '',
    [switch]$Quick,
    [switch]$SkipDriverTests,
    [switch]$IdleDisable,
    [switch]$KeepCache
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$sha = '3f87a880027e7b9ea8e0da9e4009584336f352af444a0e6e5c20721ac4c7ffd1'
$prompt = 'Write a short story about a lighthouse keeper who finds a message in a bottle.'
$crumbs = 'HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0\Device Parameters'
# Linux, charsiu on rocket, same board class, NPU 594 MHz, CPU pinned at
# maximum and held out of deep idle (charsiu docs/paper-evidence.md 1.)
$linuxTokS = 18.15

function Die($m) { Write-Host "m7-bench: $m" -ForegroundColor Red; exit 1 }

# ---- the arms ------------------------------------------------------------------

$m6env = @{ CHARSIU_RKNPU_FUSE = '0'; CHARSIU_RKNPU_UCACHE = '0'; CHARSIU_WIN_SYNC = 'pthread'; CHARSIU_WIN_SYSFS = '0'; CHARSIU_RKNPU_QOS = '0' }
$short = "-p `"$prompt`" -n 128 -c 512"
$armList = @(
    @{ Name = 'short-m6';      Args = $short; Env = $m6env; Cache = $false },
    @{ Name = 'short-npu';     Args = $short; Env = @{}; Cache = $true },
    @{ Name = 'short-npu2';    Args = $short; Env = @{}; Cache = $true },
    @{ Name = 'short-noucache'; Args = $short; Env = @{ CHARSIU_RKNPU_UCACHE = '0' }; Cache = $true },
    @{ Name = 'short-nofuse';  Args = $short; Env = @{ CHARSIU_RKNPU_UCACHE = '0'; CHARSIU_RKNPU_FUSE = '0' }; Cache = $true },
    @{ Name = 'short-nopin';   Args = $short; Env = @{ CHARSIU_WIN_SYSFS = '0' }; Cache = $true },
    @{ Name = 'short-pthread'; Args = $short; Env = @{ CHARSIU_WIN_SYNC = 'pthread' }; Cache = $true },
    @{ Name = 'short-spin';    Args = $short; Env = @{ CHARSIU_WIN_SPIN_US = '50' }; Cache = $true },
    @{ Name = 'short-noqos';   Args = $short; Env = @{ CHARSIU_NPU_IDLE = '1' }; Cache = $true },
    @{ Name = 'short-async';   Args = $short; Env = @{ CHARSIU_RKNPU_ASYNC = '1' }; Cache = $true },
    @{ Name = 'short-idle';    Args = $short; Env = @{}; Cache = $true; Idle = $true },
    @{ Name = 'short-npu1';    Args = $short; Env = @{ CHARSIU_NPU_ONEDEV = '1' }; Cache = $true },
    @{ Name = 'short-m7submit'; Args = $short; Env = @{ CHARSIU_RKNPU_M7SUBMIT = '1' }; Cache = $true },
    @{ Name = 'stages';        Args = "-p `"$prompt`" -n 32 -c 512"; Env = @{ CHARSIU_STAGES = '1' }; Cache = $true },
    @{ Name = 'stages-m6';     Args = "-p `"$prompt`" -n 32 -c 512"; Env = ($m6env + @{ CHARSIU_STAGES = '1' }); Cache = $true },
    @{ Name = 'long-npu';      Args = 'LONG'; Env = @{}; Cache = $true },
    @{ Name = 'long-m6';       Args = 'LONG'; Env = $m6env; Cache = $true }
)
$quickArms = 'short-m6', 'short-npu', 'short-npu2', 'short-m7submit', 'stages', 'stages-m6'

# ---- the tables ------------------------------------------------------------------

function Num($m, $i) { if ($m -and $m.Success) { [double]$m.Groups[$i].Value } else { $null } }
function R1($x) { if ($null -eq $x) { '' } else { '{0:N1}' -f $x } }
function R2($x) { if ($null -eq $x) { '' } else { '{0:N2}' -f $x } }

# One arm's numbers from its .txt and .err.
function Read-Arm($dir, $name) {
    $txt = Get-Content -Raw (Join-Path $dir "$name.txt") -ErrorAction SilentlyContinue
    $err = Get-Content (Join-Path $dir "$name.err") -ErrorAction SilentlyContinue
    if (-not $txt) { $txt = '' }
    if (-not $err) { $err = @() }
    $a = [ordered]@{ arm = $name }
    $a.staging_ms = Num ([regex]::Match($txt, 'staging (\d+) ms')) 1
    $mp = [regex]::Match($txt, 'prompt (\d+) tok in (\d+) ms, ([0-9.]+) tok/s')
    $mg = [regex]::Match($txt, 'gen (\d+) tok in (\d+) ms, ([0-9.]+) tok/s')
    $a.prompt_tok_s = Num $mp 3
    $a.gen_tok = Num $mg 1
    $a.gen_ms = Num $mg 2
    $a.gen_tok_s = Num $mg 3
    $a.ms_token = if ($a.gen_tok) { $a.gen_ms / $a.gen_tok } else { $null }
    $devs = 0; $hw = 0.0; $bad = 0; $syncs = 0; $syncms = 0.0
    $subN = 0; $subUs = 0.0; $drvUs = 0.0; $waitN = 0; $waitUs = 0.0
    $dpcUs = 0.0; $wakeUs = 0.0; $slept = 0; $timedN = 0; $byWait = 0; $bySub = 0
    foreach ($l in $err) {
        $d = [regex]::Match($l, '^charsiu rknpu dev \d+: (\d+) submits.*, (\d+) timeouts, (\d+) dma errors, (\d+) refused;.*NPU ([0-9.]+) ms, waited [0-9.]+ ms; .*?, .*?, (\d+) preps \+ (\d+) finis in ([0-9.]+) ms; (\d+) page faults, peak working set (\d+) MiB')
        if ($d.Success) {
            $devs++
            $bad += [int]$d.Groups[2].Value + [int]$d.Groups[3].Value + [int]$d.Groups[4].Value
            $hw += [double]$d.Groups[5].Value
            $syncs += [int]$d.Groups[6].Value + [int]$d.Groups[7].Value
            $syncms += [double]$d.Groups[8].Value
            $a.page_faults = [double]$d.Groups[9].Value
            $a.peak_ws_mib = [double]$d.Groups[10].Value
        }
        $t = [regex]::Match($l, '^charsiu rknpu dev \d+ timing: (\d+) SUBMITs ([0-9.]+) us avg \(driver ([0-9.]+)\), (\d+) WAITs ([0-9.]+) us avg, irq to dpc ([0-9.]+) us, dpc to waiter ([0-9.]+) us over (\d+) that slept, (\d+) came after.*?; \d+ FINIs deferred \((\d+) by SUBMIT, \d+ list full\), (\d+) PREPs by WAIT')
        if ($t.Success) {
            $n = [double]$t.Groups[1].Value; $w = [double]$t.Groups[4].Value
            $subN += $n; $subUs += $n * [double]$t.Groups[2].Value; $drvUs += $n * [double]$t.Groups[3].Value
            $waitN += $w; $waitUs += $w * [double]$t.Groups[5].Value
            $tw = [double]$t.Groups[8].Value + [double]$t.Groups[9].Value
            $timedN += $tw; $dpcUs += $tw * [double]$t.Groups[6].Value
            $slept += [double]$t.Groups[8].Value; $wakeUs += [double]$t.Groups[8].Value * [double]$t.Groups[7].Value
            $bySub += [double]$t.Groups[10].Value; $byWait += [double]$t.Groups[11].Value
        }
    }
    # M8: the driver's split of a SUBMIT, us, the mean of the devices
    $phN = 0; $ph = @{}
    foreach ($l in $err) {
        $q = [regex]::Match($l, '^charsiu rknpu dev \d+ submit phases: us a SUBMIT, (.*?)( \(|$)')
        if ($q.Success) {
            $phN++
            foreach ($kv in ($q.Groups[1].Value -split ', ')) {
                $kp = $kv -split ' '
                if ($kp.Count -eq 2) { $ph[$kp[0]] = [double]$ph[$kp[0]] + [double]$kp[1] }
            }
        }
    }
    foreach ($k in 'entry', 'safe', 'idle', 'arena', 'mmu', 'clean', 'copy', 'check', 'cbuf', 'book', 'start', 'total') {
        $a["sp_$k"] = if ($phN) { [double]$ph[$k] / $phN } else { $null }
    }
    $a.devices = $devs
    $a.errors = $bad
    $a.npu_busy_ms = $hw
    $a.fallbacks = @($err | Where-Object { $_ -match 'NOT on the NPU|rknpu buffer of' }).Count
    $a.pinned = [bool]($err | Where-Object { $_ -match 'this thread on the fastest cluster' })
    $a.hold = (($err | Where-Object { $_ -match '^charsiu \(Windows\): the CPU hold' }) | Select-Object -First 1)
    $a.cache = (($err | Where-Object { $_ -match '^charsiu: weight cache' }) | Select-Object -First 1)
    $g = $a.gen_tok
    if ($g -and $devs) {
        # per generated token; the run's totals include the one batched prompt
        # chunk and the staging probe, so these are a few percent high
        $a.tok_npu_core_ms = $hw / $devs / $g
        $a.tok_ioctls = ($subN + $waitN + $syncs) / $g
        $a.tok_submit_ms = $subUs / 1000 / $g
        $a.tok_submit_drv_ms = $drvUs / 1000 / $g
        $a.tok_wait_ms = $waitUs / 1000 / $g
        $a.tok_sync_ms = $syncms / $g
        $a.tok_syncs = $syncs / $g
        $a.submit_us = if ($subN) { $subUs / $subN } else { $null }
        $a.submit_drv_us = if ($subN) { $drvUs / $subN } else { $null }
        $a.irq_dpc_us = if ($timedN) { $dpcUs / $timedN } else { $null }
        $a.dpc_wake_us = if ($slept) { $wakeUs / $slept } else { $null }
        $a.by_submit = $bySub; $a.by_wait = $byWait
    }
    # charsiu's own split of the decode matmul calls (process totals, ms)
    $s = $err | Where-Object { $_ -match '^charsiu NPU: of that, ([0-9.]+) ms submitting, ([0-9.]+) ms waiting for the fence.*, ([0-9.]+) ms summing' } | Select-Object -First 1
    if ($s -and $g) {
        $m = [regex]::Match($s, 'of that, ([0-9.]+) ms submitting, ([0-9.]+) ms waiting for the fence.*, ([0-9.]+) ms summing')
        $a.tok_c_submit_ms = [double]$m.Groups[1].Value / $g
        $a.tok_c_fence_ms = [double]$m.Groups[2].Value / $g
        $a.tok_c_read_ms = [double]$m.Groups[3].Value / $g
    }
    $p = $err | Where-Object { $_ -match '^charsiu NPU: and ([0-9.]+) ms packing the activation' } | Select-Object -First 1
    if ($p -and $g) { $a.tok_c_pack_ms = [double]([regex]::Match($p, 'and ([0-9.]+) ms packing').Groups[1].Value) / $g }
    $f = $err | Where-Object { $_ -match '^charsiu NPU: us a call = ' } | Select-Object -First 1
    $a.fit = if ($f) { ($f -replace '^charsiu NPU: ', '') -replace ', so of.*$', '' } else { '' }
    $a.md5 = ''
    $tf = Join-Path $dir "$name.text"
    if (Test-Path $tf) { $a.md5 = (Get-FileHash -Algorithm MD5 $tf).Hash.ToLower() }
    [pscustomobject]$a
}

# The "charsiu stages:" tables of an arm, last one first (the run's second half).
function Read-Stages($dir, $name) {
    $err = Get-Content (Join-Path $dir "$name.err") -ErrorAction SilentlyContinue
    $tabs = @(); $cur = $null
    foreach ($l in $err) {
        $h = [regex]::Match($l, '^charsiu stages: (\d+) tokens, ([0-9.]+) ms a token')
        if ($h.Success) { $cur = [ordered]@{ '(total)' = [double]$h.Groups[2].Value }; $tabs += , $cur; continue }
        if ($null -ne $cur) {
            $r = [regex]::Match($l, '^  ([a-z][a-z +*]+?)\s+([0-9.]+) ms a token')
            if ($r.Success) {
                # the table has two "residual" rows: the second is " 2"
                $k = $r.Groups[1].Value.Trim()
                if ($cur.Contains($k)) { $k = "$k 2" }
                $cur[$k] = [double]$r.Groups[2].Value
            } elseif ($l -notmatch '^\s') { $cur = $null }
        }
    }
    $tabs
}

function Write-Tables($dir) {
    $names = Get-ChildItem (Join-Path $dir '*.err') | ForEach-Object { $_.BaseName } |
        Where-Object { $_ -notmatch '^(info|m3|m6|version)' }
    $order = @($armList | ForEach-Object { $_.Name })
    $names = @($names | Sort-Object { $i = [array]::IndexOf($order, $_); if ($i -lt 0) { 99 } else { $i } })
    $rows = @(foreach ($n in $names) { Read-Arm $dir $n })
    $rows | Export-Csv -NoTypeInformation -Path (Join-Path $dir 'bench.csv')

    $L = New-Object System.Collections.ArrayList
    [void]$L.Add('M7 bench, ' + (Get-Date -Format o) + ", $dir")
    [void]$L.Add(($rows | Format-Table -AutoSize arm, staging_ms, prompt_tok_s, gen_tok_s,
        @{ n = 'ms_tok'; e = { R1 $_.ms_token } }, devices, pinned, errors, fallbacks, page_faults, peak_ws_mib, md5 |
        Out-String -Width 240))
    # the text: every NPU arm must agree with the others of its prompt
    foreach ($grp in @(@{ P = '^short-'; R = 'short-npu' }, @{ P = '^stages'; R = 'stages' }, @{ P = '^long-'; R = 'long-npu' })) {
        $g = @($rows | Where-Object { $_.arm -match $grp.P -and $_.md5 -and $_.devices -gt 0 })
        if ($g.Count -lt 2) { continue }
        $ref = ($g | Where-Object { $_.arm -eq $grp.R } | Select-Object -First 1)
        if (-not $ref) { $ref = $g[0] }
        $diff = @($g | Where-Object { $_.md5 -ne $ref.md5 } | ForEach-Object { $_.arm })
        if ($diff.Count) { [void]$L.Add("TEXT: $($grp.R) group DIFFERS in: $($diff -join ', ') (an M7 change is not numerics-neutral: stop and send this)") }
        else { [void]$L.Add("TEXT: all $($g.Count) $($grp.R) arms print the same text") }
    }
    $base = $rows | Where-Object { $_.arm -eq 'short-npu2' } | Select-Object -First 1
    if (-not $base) { $base = $rows | Where-Object { $_.arm -eq 'short-npu' } | Select-Object -First 1 }
    $m6 = $rows | Where-Object { $_.arm -eq 'short-m6' } | Select-Object -First 1
    if ($base -and $base.gen_tok_s) {
        [void]$L.Add(('decode: M7 {0} tok/s ({1:N1} ms a token), Linux {2} tok/s ({3:N1} ms): {4:N0}% of Linux' -f
            $base.gen_tok_s, $base.ms_token, $linuxTokS, (1000 / $linuxTokS), (100 * $base.gen_tok_s / $linuxTokS)))
        if ($m6 -and $m6.gen_tok_s) {
            [void]$L.Add(('decode: M6 port {0} tok/s ({1:N1} ms): M7 saves {2:N1} ms a token' -f $m6.gen_tok_s, $m6.ms_token, ($m6.ms_token - $base.ms_token)))
        }
        [void]$L.Add('')
        [void]$L.Add('each arm against short-npu2 (the M7 defaults). For an arm that turns a default OFF, + is what')
        [void]$L.Add('that default saves; for short-spin, short-async and short-idle, which turn an option ON, - is a gain:')
        foreach ($x in 'short-m7submit', 'short-noucache', 'short-nofuse', 'short-nopin', 'short-pthread', 'short-spin', 'short-noqos', 'short-async', 'short-idle', 'short-npu1') {
            $r = $rows | Where-Object { $_.arm -eq $x } | Select-Object -First 1
            if ($r -and $r.ms_token) { [void]$L.Add(('  {0,-14} {1,6:N2} tok/s  {2,6:N1} ms a token  {3,7:+0.0;-0.0} ms' -f $x, $r.gen_tok_s, $r.ms_token, ($r.ms_token - $base.ms_token))) }
        }
        $c1 = $rows | Where-Object { $_.arm -eq 'short-npu' } | Select-Object -First 1
        if ($c1 -and $m6 -and $base.staging_ms) {
            [void]$L.Add(('staging: {0:N1} s without the weight cache (short-m6), {1:N1} s writing it (short-npu), {2:N1} s reading it (short-npu2)' -f
                ($m6.staging_ms / 1000), ($c1.staging_ms / 1000), ($base.staging_ms / 1000)))
        }
    }
    [IO.File]::WriteAllText((Join-Path $dir 'bench.txt'), (($L -join "`r`n") + "`r`n"))
    $L | ForEach-Object { Write-Host $_ }

    # ---- decode.txt: one generated token, arm by arm ----
    $D = New-Object System.Collections.ArrayList
    [void]$D.Add('Per generated token (run totals over the generated tokens; the one batched prompt chunk and')
    [void]$D.Add('the staging probe are in the totals, so the IOCTL rows are a few percent high).')
    [void]$D.Add("Linux reference: $linuxTokS tok/s = $('{0:N1}' -f (1000 / $linuxTokS)) ms a token.")
    [void]$D.Add('')
    $cols = @($rows | Where-Object { $_.devices -gt 0 -and $_.gen_tok })
    $fmt = '{0,-34}' + (($cols | ForEach-Object { '{' + ([array]::IndexOf($cols, $_) + 1) + ',16}' }) -join '')
    function Row($label, $sel) {
        $vals = @($label) + @($cols | ForEach-Object { & $sel $_ })
        [void]$D.Add(($fmt -f $vals))
    }
    Row 'arm' { param($r) $r.arm }
    Row 'ms a token' { param($r) R1 $r.ms_token }
    Row '  NPU busy, per core' { param($r) R1 $r.tok_npu_core_ms }
    Row '  charsiu: submitting' { param($r) R1 $r.tok_c_submit_ms }
    Row '  charsiu: fence (wait + prep)' { param($r) R1 $r.tok_c_fence_ms }
    Row '  charsiu: reading the output' { param($r) R1 $r.tok_c_read_ms }
    Row '  charsiu: packing the input' { param($r) R1 $r.tok_c_pack_ms }
    Row 'IOCTLs a token' { param($r) R1 $r.tok_ioctls }
    Row '  SUBMIT, wall' { param($r) R1 $r.tok_submit_ms }
    Row '    of which in the driver' { param($r) R1 $r.tok_submit_drv_ms }
    Row '  WAIT, wall' { param($r) R1 $r.tok_wait_ms }
    Row '  PREP/FINI IOCTLs left' { param($r) R1 $r.tok_syncs }
    Row '    their time' { param($r) R1 $r.tok_sync_ms }
    Row 'one SUBMIT, us' { param($r) R1 $r.submit_us }
    Row '  of which the driver, us' { param($r) R1 $r.submit_drv_us }
    Row 'interrupt to DPC, us' { param($r) R1 $r.irq_dpc_us }
    Row 'DPC to the waiter running, us' { param($r) R1 $r.dpc_wake_us }
    Row 'decode thread pinned' { param($r) $r.pinned }
    Row 'inside SUBMIT (M8 driver), us:' { param($r) '' }
    foreach ($k in 'entry', 'safe', 'idle', 'arena', 'mmu', 'clean', 'copy', 'check', 'cbuf', 'book', 'start', 'total') {
        $kk = "sp_$k"
        Row "  $k" ([scriptblock]::Create("param(`$r) R2 `$r.$kk"))
    }
    [void]$D.Add('')
    foreach ($r in $cols) { if ($r.fit) { [void]$D.Add(('{0,-14} {1}' -f $r.arm, $r.fit)) } }
    foreach ($r in $cols) { if ($r.hold) { [void]$D.Add(('{0,-14} {1}' -f $r.arm, $r.hold)); break } }
    foreach ($r in $cols) { if ($r.cache) { [void]$D.Add(('{0,-14} {1}' -f $r.arm, $r.cache)) } }

    # the two stage tables, second half of each run (the first half carries the warm-up)
    $st = @{}
    foreach ($n in 'stages', 'stages-m6') { $t = @(Read-Stages $dir $n); if ($t.Count) { $st[$n] = $t } }
    if ($st.Count) {
        [void]$D.Add('')
        [void]$D.Add('CHARSIU_STAGES=1, ms a token (first half / second half of 32 tokens):')
        $keys = New-Object System.Collections.ArrayList
        foreach ($n in $st.Keys) { foreach ($t in $st[$n]) { foreach ($k in $t.Keys) { if (-not $keys.Contains($k)) { [void]$keys.Add($k) } } } }
        $hdr = '{0,-20}' -f 'stage'
        foreach ($n in 'stages', 'stages-m6') { if ($st[$n]) { $hdr += ('{0,22}' -f $n) } }
        [void]$D.Add($hdr)
        foreach ($k in $keys) {
            $line = '{0,-20}' -f $k
            foreach ($n in 'stages', 'stages-m6') {
                if (-not $st[$n]) { continue }
                $v = @($st[$n] | ForEach-Object { if ($_.Contains($k)) { '{0:N2}' -f $_[$k] } else { '-' } })
                $line += ('{0,22}' -f ($v -join ' / '))
            }
            [void]$D.Add($line)
        }
    }
    [IO.File]::WriteAllText((Join-Path $dir 'decode.txt'), (($D -join "`r`n") + "`r`n"))
    $D | ForEach-Object { Write-Host $_ }
}

if ($Report) {
    Write-Tables (Resolve-Path $Report).Path
    exit 0
}

# ---- the run ---------------------------------------------------------------------

if (-not $Model) { Die 'give -Model (or -Report on a finished run)' }
$run = Join-Path $here 'charsiu_run.exe'
$long = Join-Path $here 'long.txt'
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
if (-not (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Die 'run as administrator (\\.\RKNPU is administrators-only)'
}
foreach ($f in $run, $long) { if (-not (Test-Path $f)) { Die "missing ${f}: run from the unpacked artifact" } }
$rkt = Join-Path $here 'rknputest.exe'
if (-not (Test-Path $rkt)) { $rkt = (Get-Command rknputest.exe -ErrorAction SilentlyContinue).Source }
if (-not $rkt) { Die 'rknputest.exe not found (copy it from rknpu-m7-arm64 beside this script)' }
New-Item -ItemType Directory -Force -Path $Out | Out-Null
$Out = (Resolve-Path $Out).Path

Get-ChildItem Env: | Where-Object { $_.Name -like 'CHARSIU_*' } | ForEach-Object { Remove-Item "Env:$($_.Name)" }

Write-Host 'm7-bench: checking the model (1.3 GB)'
$got = (Get-FileHash -Algorithm SHA256 $Model).Hash.ToLower()
if ($got -ne $sha) { Die "$Model is not Llama-3.2-1B-Instruct-Q8_0.gguf (sha256 $got)" }
$wcache = "$Model.wcache"
if (-not $KeepCache -and (Test-Path $wcache)) { Remove-Item -Force $wcache }

$bat = Join-Path $Out 'run.cmd'
function Cmd($line, $stdout, $stderr) {
    $body = "@echo off`r`n$line > `"$stdout`" 2> `"$stderr`"`r`nexit /b %errorlevel%`r`n"
    [IO.File]::WriteAllText($bat, $body, [Text.Encoding]::ASCII)
    & cmd.exe /d /c "`"$bat`"" | Out-Host
    return $LASTEXITCODE
}

# stdout without the lines that start with '[', as grep -v '^\[' writes it
function Write-Text($src, $dst) {
    $b = [IO.File]::ReadAllBytes($src)
    $o = New-Object IO.MemoryStream
    $start = 0
    for ($i = 0; $i -le $b.Length; $i++) {
        if ($i -eq $b.Length -or $b[$i] -eq 10) {
            $len = $i - $start
            if ($len -gt 0 -or $i -lt $b.Length) {
                if (-not ($len -gt 0 -and $b[$start] -eq 91)) {
                    $o.Write($b, $start, $len)
                    $o.WriteByte(10)
                }
            }
            $start = $i + 1
        }
    }
    [IO.File]::WriteAllBytes($dst, $o.ToArray())
}

function Idle-Set($on) {
    $q = powercfg /query SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE | Select-String 'Current AC Power Setting Index: (0x[0-9a-f]+)'
    $old = if ($q) { $q.Matches[0].Groups[1].Value } else { $null }
    powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE $(if ($on) { 1 } else { 0 }) | Out-Null
    powercfg /setactive SCHEME_CURRENT | Out-Null
    return $old
}
function Idle-Restore($old) {
    if ($null -eq $old) { return }
    powercfg /setacvalueindex SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE $old | Out-Null
    powercfg /setactive SCHEME_CURRENT | Out-Null
}

& {
    Get-CimInstance Win32_OperatingSystem | Format-List Caption, Version, BuildNumber, TotalVisibleMemorySize, FreePhysicalMemory
    Get-CimInstance Win32_Processor | Format-List DeviceID, Name, CurrentClockSpeed, MaxClockSpeed, NumberOfLogicalProcessors
    Get-ChildItem 'HKLM:\HARDWARE\DESCRIPTION\System\CentralProcessor' | ForEach-Object {
        $p = Get-ItemProperty $_.PSPath
        '{0}: {1} ~MHz {2} MIDR 0x{3:x}' -f $_.PSChildName, $p.Identifier, $p.'~MHz', $p.'CP 4000'
    }
    powercfg /query SCHEME_CURRENT SUB_PROCESSOR IDLEDISABLE
    Get-Date -Format o
    Get-FileHash -Algorithm SHA256 $run, $rkt | Format-Table -AutoSize
    pnputil /enum-devices /instanceid 'ACPI\RKCP3576\0' /drivers
    reg query $crumbs
} *>&1 | Out-File -Encoding ascii (Join-Path $Out 'system.txt')
Cmd "`"$run`" --version" (Join-Path $Out 'version.txt') (Join-Path $Out 'version.err') | Out-Null

# ---- 1. the driver -----------------------------------------------------------

Write-Host 'm7-bench: rknputest info'
Cmd "`"$rkt`" info" (Join-Path $Out 'info-before.txt') (Join-Path $Out 'info-before.err') | Out-Null
if (-not (Select-String -Quiet -Path (Join-Path $Out 'info-before.txt') -Pattern '^m6: version .*memory IOMMU')) {
    Die "the driver is not in IOMMU mode (see $Out\info-before.txt)"
}
if (-not $SkipDriverTests) {
    Write-Host 'm7-bench: rknputest m3'
    $rc = Cmd "`"$rkt`" m3" (Join-Path $Out 'm3.txt') (Join-Path $Out 'm3.err')
    if ($rc -ne 0) { Die "rknputest m3 failed ($rc): stop here and send $Out" }
    Write-Host 'm7-bench: rknputest m6'
    $rc = Cmd "`"$rkt`" m6" (Join-Path $Out 'm6.txt') (Join-Path $Out 'm6.err')
    if ($rc -ne 0) { Die "rknputest m6 failed ($rc): stop here and send $Out" }
}

# ---- 2. charsiu ----------------------------------------------------------------

$npu = @{ CHARSIU_NPU = '1'; CHARSIU_NPU_QUANT = '1'; CHARSIU_NPU_W4V = '1'; CHARSIU_NPU_KMAX = '1024';
          CHARSIU_NPU_W4_GROUP = '1024'; CHARSIU_NPU_MAXN = '262144'; CHARSIU_COEF_ELEMS = '65536';
          CHARSIU_RKNPU_STATS = '1' }

$idleAll = $null
if ($IdleDisable) {
    $idleAll = Idle-Set $true
    Write-Host "m7-bench: processor idle disabled for every arm (was $idleAll)"
}
try {
    foreach ($a in $armList) {
        if ($Quick -and $quickArms -notcontains $a.Name) { continue }
        Write-Host "m7-bench: $($a.Name)"
        $set = @{}
        foreach ($k in $npu.Keys) { $set[$k] = $npu[$k] }
        foreach ($k in $a.Env.Keys) { $set[$k] = $a.Env[$k] }
        foreach ($k in $set.Keys) { Set-Item "Env:$k" $set[$k] }
        $argline = if ($a.Args -eq 'LONG') { "-f `"$long`" -n 64 -c 1024" } else { $a.Args }
        if ($a.Cache) { $argline += ' --cache' }
        $old = $null
        if ($a.Idle -and -not $IdleDisable) { $old = Idle-Set $true }
        try {
            $t0 = Get-Date
            $rc = Cmd "`"$run`" `"$Model`" $argline -t 4" (Join-Path $Out "$($a.Name).txt") (Join-Path $Out "$($a.Name).err")
            $s = [int]((Get-Date) - $t0).TotalSeconds
        } finally {
            if ($a.Idle -and -not $IdleDisable) { Idle-Restore $old }
            foreach ($k in $set.Keys) { Remove-Item "Env:$k" }
        }
        if ($rc -ne 0) { Add-Content (Join-Path $Out "$($a.Name).err") "exit $rc" }
        Write-Text (Join-Path $Out "$($a.Name).txt") (Join-Path $Out "$($a.Name).text")
        Write-Host "  exit $rc, $s s"
        reg query $crumbs | Out-File -Encoding ascii (Join-Path $Out "$($a.Name).crumbs.txt")
    }
} finally {
    if ($IdleDisable) {
        Idle-Restore $idleAll
        Write-Host "m7-bench: processor idle setting restored ($idleAll)"
    }
}

Cmd "`"$rkt`" info" (Join-Path $Out 'info-after.txt') (Join-Path $Out 'info-after.err') | Out-Null

# ---- 3. the tables -------------------------------------------------------------

$lines = Get-ChildItem (Join-Path $Out '*.text') | Sort-Object Name | ForEach-Object {
    '{0}  ./{1}' -f (Get-FileHash -Algorithm MD5 $_.FullName).Hash.ToLower(), $_.Name
}
[IO.File]::WriteAllText((Join-Path $Out 'text.md5'), (($lines -join "`n") + "`n"))
Write-Tables $Out
Compress-Archive -Force -Path $Out -DestinationPath "$Out.zip"
Write-Host "m7-bench: done, send $Out.zip"
