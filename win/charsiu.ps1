# charsiu.ps1 - the charsiu front door on Windows on Arm
#
# The same commands as charsiu's front door on Linux (scripts/charsiu and the
# charsiu-* scripts it dispatches to), for the Windows build in this
# repository. bin\charsiu.cmd runs it, so `charsiu` works from cmd and from
# PowerShell whatever the execution policy says.
#
#   charsiu                      a conversation, or the guided setup when no model is here
#   charsiu "what is a bandgap"  ask once and exit
#   charsiu -p "..."             print mode (a completion, no chat template)
#   charsiu run MODEL [PROMPT]   use a particular model
#   charsiu pull [NAME]          fetch one (sha256 and the gate checked); no NAME lists them
#   charsiu list | ls            what is here
#   charsiu rm [-f] NAME         delete one, and its weight cache
#   charsiu show [MODEL]         what it is, and whether it can run here
#   charsiu ps                   is anything running, and what the NPU is doing
#   charsiu bench                tokens per second on this board
#   charsiu doctor               what works and what does not
#   charsiu serve [--port N]     the OpenAI compatible endpoint, on 127.0.0.1
#   charsiu update               fetch the latest release and reinstall
#   charsiu setup                the guided screen, on purpose
#
# Where things live (Linux in brackets):
#   config   %LOCALAPPDATA%\charsiu\config.ini   [~/.charsiu/config.ini]
#   models   %LOCALAPPDATA%\charsiu\models       [~/.charsiu/models]
#   programs this script's folder, %LOCALAPPDATA%\Programs\charsiu by default
# CHARSIU_HOME, CHARSIU_CONFIG and CHARSIU_MODELS move them, as on Linux.
#
# Written for Windows PowerShell 5.1, which is what Windows 11 23H2 has.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Self = 'charsiu'

# ---- where things live -------------------------------------------------------

function Get-LocalAppData {
    if ($env:LOCALAPPDATA) { return $env:LOCALAPPDATA }
    return [Environment]::GetFolderPath('LocalApplicationData')
}
$CharsiuHome = if ($env:CHARSIU_HOME) { $env:CHARSIU_HOME } else { Join-Path (Get-LocalAppData) 'charsiu' }
$Conf = if ($env:CHARSIU_CONFIG) { $env:CHARSIU_CONFIG } else { Join-Path $CharsiuHome 'config.ini' }
$Models = if ($env:CHARSIU_MODELS) { $env:CHARSIU_MODELS } else { Join-Path $CharsiuHome 'models' }
$BinDir = if ($env:CHARSIU_BIN_DIR) { $env:CHARSIU_BIN_DIR } else { $Here }
$HF = if ($env:HF_ENDPOINT) { $env:HF_ENDPOINT.TrimEnd('/') } else { 'https://huggingface.co' }
$NpuInstance = 'ACPI\RKCP3576\0'
$Version = 'unknown'
$vf = Join-Path $Here 'VERSION'
if (Test-Path $vf) { $Version = (Get-Content $vf -TotalCount 1).Trim() }

function Say($m)  { [Console]::Error.WriteLine("[charsiu] $m") }
function Warn($m) { [Console]::Error.WriteLine("[charsiu] WARN: $m") }
function Die($m, $code = 1) { [Console]::Error.WriteLine("${Self}: $m"); exit $code }

# ---- the config --------------------------------------------------------------
#
# The same ini as on Linux, read and written the same way: a value is replaced
# where it is, a missing key goes at the end of its own section, comments
# survive.

function Get-Ini([string]$Section, [string]$Key, [string]$File = $Conf) {
    if (-not (Test-Path -LiteralPath $File)) { return '' }
    $cur = ''
    foreach ($line in [IO.File]::ReadAllLines($File)) {
        if ($line -match '^\s*\[(.*)\]\s*$') { $cur = $Matches[1].Trim(); continue }
        if ($cur -ne $Section) { continue }
        if ($line -match '^\s*([^=#;]+?)\s*=(.*)$') {
            if ($Matches[1] -eq $Key) {
                $v = $Matches[2] -replace '\s#.*$', ''
                return $v.Trim()
            }
        }
    }
    return ''
}

function Set-Ini([string]$Section, [string]$Key, [string]$Value, [string]$File = $Conf) {
    $dir = Split-Path -Parent $File
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $lines = @()
    if (Test-Path -LiteralPath $File) { $lines = @([IO.File]::ReadAllLines($File)) }
    $out = New-Object System.Collections.Generic.List[string]
    $cur = ''; $done = $false; $seen = $false
    foreach ($line in $lines) {
        if ($line -match '^\s*\[(.*)\]\s*$') {
            if ($cur -eq $Section -and -not $done) { $out.Add("$Key = $Value"); $done = $true }
            $cur = $Matches[1].Trim()
            if ($cur -eq $Section) { $seen = $true }
            $out.Add($line); continue
        }
        if ($cur -eq $Section -and -not $done -and $line -match '^\s*([^=#;]+?)\s*=') {
            if ($Matches[1] -eq $Key) { $out.Add("$Key = $Value"); $done = $true; continue }
        }
        $out.Add($line)
    }
    if (-not $done) {
        if ($cur -eq $Section) { $out.Add("$Key = $Value") }
        elseif (-not $seen) { $out.Add(''); $out.Add("[$Section]"); $out.Add("$Key = $Value") }
    }
    # no BOM: the ini is shared with tools that read it as UTF-8 or ASCII
    [IO.File]::WriteAllLines($File, $out.ToArray(), (New-Object System.Text.UTF8Encoding($false)))
}

function Test-Yes([string]$v) { return @('yes', '1', 'true', 'on') -contains $v.ToLowerInvariant() }

# ---- programs ----------------------------------------------------------------

function Find-Exe([string]$Name) {
    foreach ($d in @($BinDir, (Join-Path $Here '..\port\build\win'))) {
        $p = Join-Path $d "$Name.exe"
        if (Test-Path -LiteralPath $p) { return (Resolve-Path -LiteralPath $p).Path }
    }
    $c = Get-Command "$Name.exe" -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    return ''
}

function Need-Exe([string]$Name) {
    $p = Find-Exe $Name
    if (-not $p) {
        Die "cannot find '$Name.exe'. Looked in $BinDir and on PATH; reinstall with charsiu-install.ps1, or set CHARSIU_BIN_DIR." 127
    }
    return $p
}

function Get-Curl {
    if ($env:SystemRoot) {
        $w = Join-Path $env:SystemRoot 'System32\curl.exe'
        if (Test-Path -LiteralPath $w) { return $w }
    }
    $c = Get-Command curl -CommandType Application -ErrorAction SilentlyContinue
    if ($c) { return ($c | Select-Object -First 1).Source }
    return ''
}

# Run a program and hand back what it printed, both streams, as text. NOT
# `& exe 2>&1`: Windows PowerShell turns every stderr line into an error
# record, and under ErrorActionPreference Stop the first one ends the script.
function Invoke-Captured([string]$Exe, [string[]]$ArgList) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (($ArgList | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [Text.Encoding]::UTF8
    $p = [Diagnostics.Process]::Start($psi)
    $err = $p.StandardError.ReadToEndAsync()
    $out = $p.StandardOutput.ReadToEnd()
    $p.WaitForExit()
    return [pscustomobject]@{ Code = $p.ExitCode; Out = $out; Err = $err.Result }
}

# The quoting CommandLineToArgvW undoes: what the CRT's argv parser reads back.
function Quote-Arg([string]$a) {
    if ($a -ne '' -and $a -notmatch '[\s"]') { return $a }
    $b = $a -replace '(\\*)"', '$1$1\"'
    $b = $b -replace '(\\+)$', '$1$1'
    return '"' + $b + '"'
}

# Start a program in THIS console and wait for it, with its exit code.
# Through the Process class rather than `& exe @args`, because Windows
# PowerShell 5.1 passes an argument holding a double quote to a native
# program wrongly, and a prompt may hold one.
function Invoke-Here([string]$Exe, [string[]]$ArgList) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = (($ArgList | ForEach-Object { Quote-Arg $_ }) -join ' ')
    $psi.UseShellExecute = $false
    $p = [Diagnostics.Process]::Start($psi)
    $p.WaitForExit()
    return $p.ExitCode
}

# ---- models ------------------------------------------------------------------

function Get-ModelDirs {
    $d = @()
    if (Test-Path -LiteralPath $Models) { $d += (Resolve-Path -LiteralPath $Models).Path }
    return $d
}

function Get-AllModels {
    $r = @()
    foreach ($d in Get-ModelDirs) {
        $r += Get-ChildItem -LiteralPath $d -Filter '*.gguf' -File -ErrorAction SilentlyContinue |
              Where-Object { $_.Name -notlike 'mmproj-*' } | ForEach-Object { $_.FullName }
    }
    return , $r
}

function Resolve-Model([string]$Name) {
    if (-not $Name) { return '' }
    if (Test-Path -LiteralPath $Name -PathType Leaf) { return (Resolve-Path -LiteralPath $Name).Path }
    foreach ($d in Get-ModelDirs) {
        foreach ($c in @((Join-Path $d $Name), (Join-Path $d "$Name.gguf"))) {
            if (Test-Path -LiteralPath $c -PathType Leaf) { return $c }
        }
    }
    # a zoo name, so `charsiu run qwen3-0.6b-q4` means what `charsiu pull` called it
    $z = Get-Zoo | Where-Object { $_.Name -eq $Name } | Select-Object -First 1
    if ($z) {
        $c = Join-Path $Models $z.File
        if (Test-Path -LiteralPath $c -PathType Leaf) { return $c }
    }
    return ''
}

function Get-CurrentModel { return (Get-Ini 'model' 'path') }

# The model a run uses: the one named, else the config's, else the only one
# here. The same order and the same messages as the Linux runner.
function Select-Model([string]$Named, [switch]$Quiet) {
    $m = $Named
    if (-not $m) { $m = Get-CurrentModel }
    $r = Resolve-Model $m
    if ($r) { return $r }
    $all = Get-AllModels
    if ($all.Count -eq 1) {
        if ($m -and -not $Quiet) { Say "$m is not here; using the only model here, $(Split-Path -Leaf $all[0])" }
        elseif (-not $Quiet) { Say "using the only model here, $(Split-Path -Leaf $all[0])" }
        return $all[0]
    }
    if ($all.Count -gt 1) {
        [Console]::Error.WriteLine("${Self}: the configured model is not here. $($all.Count) models are:")
        foreach ($f in $all) { [Console]::Error.WriteLine("  $(Split-Path -Leaf $f)") }
        Die "pick one with:  charsiu run NAME"
    }
    if ($m) { Die "$m does not exist (charsiu pull fetches one)" }
    Die "no model set and none here. Try: charsiu pull"
}

# ---- the zoo -----------------------------------------------------------------
#
# zoo.txt is charsiu's own list (scripts/charsiu-get), extracted when the
# package is built, so Windows and Linux offer the same models.

function Get-Zoo {
    $f = Join-Path $Here 'zoo.txt'
    if (-not (Test-Path -LiteralPath $f)) { return @() }
    $rows = @()
    foreach ($l in [IO.File]::ReadAllLines($f)) {
        if (-not $l -or $l.StartsWith('#')) { continue }
        $p = $l.Split('|')
        while ($p.Count -lt 7) { $p += '' }
        $rows += [pscustomobject]@{
            Name = $p[0]; Repo = $p[1]; File = $p[2]; MB = [int]$p[3]; Note = $p[4]
            Mmproj = $p[5]; MmprojMB = $(if ($p[6]) { [int]$p[6] } else { 0 })
        }
    }
    return $rows
}

# ---- the NPU -----------------------------------------------------------------

function Test-Admin {
    try {
        $id = [Security.Principal.WindowsIdentity]::GetCurrent()
        return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole(
            [Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch { return $false }
}

# What PnP says about ACPI\RKCP3576: absent (the firmware does not publish it:
# "NPU (experimental)" is off in setup), present with a problem (no driver, or
# the driver failed), or started.
function Get-NpuDevice {
    $r = [pscustomobject]@{ Present = $false; Started = $false; Problem = 0; Status = ''; Driver = ''; UserAccess = $false; Iommu = $true }
    try {
        $d = Get-CimInstance Win32_PnPEntity -Filter "DeviceID LIKE 'ACPI\\\\RKCP3576%'" -ErrorAction Stop | Select-Object -First 1
    } catch { return $r }
    if (-not $d) { return $r }
    $r.Present = $true
    $r.Problem = [int]$d.ConfigManagerErrorCode
    $r.Started = ($r.Problem -eq 0 -and $d.Status -eq 'OK')
    $r.Status = $d.Status
    try {
        $s = Get-CimInstance Win32_PnPSignedDriver -Filter "DeviceID LIKE 'ACPI\\\\RKCP3576%'" -ErrorAction Stop | Select-Object -First 1
        if ($s) { $r.Driver = "$($s.DriverVersion)  $($s.InfName)" }
    } catch { }
    $k = "HKLM:\SYSTEM\CurrentControlSet\Enum\$NpuInstance\Device Parameters"
    try {
        $p = Get-ItemProperty -Path $k -ErrorAction Stop
        if ($p.PSObject.Properties['UserAccess'] -and $p.UserAccess -eq 1) { $r.UserAccess = $true }
        if ($p.PSObject.Properties['M6Iommu'] -and $p.M6Iommu -eq 0) { $r.Iommu = $false }
    } catch { }
    return $r
}

# Say, before a run, the one thing that makes the NPU quietly not happen.
# charsiu itself falls back to the CPU and says why on its own; this adds what
# to do about it, once, in words.
function Write-NpuHint {
    if ($env:CHARSIU_NO_NPU_HINT) { return }
    if ((Get-Ini 'npu' 'enable') -and -not (Test-Yes (Get-Ini 'npu' 'enable'))) { return }
    $d = Get-NpuDevice
    if (-not $d.Present) {
        Warn "no NPU device: enable 'NPU (experimental)' in the firmware setup. This run uses the CPU."
    } elseif (-not $d.Started) {
        Warn "the NPU has no working driver (problem $($d.Problem)). charsiu-install.ps1 --driver, from an administrator PowerShell, stages rknpu.sys. This run uses the CPU."
    } elseif (-not (Test-Admin) -and -not $d.UserAccess) {
        Warn "rknpu.sys lets only administrators use the NPU here. Run charsiu-install.ps1 once from an administrator PowerShell (it lets ordinary users use the NPU, then restart), or run this from an administrator terminal. This run uses the CPU."
    }
}

# The six variables that decide whether a decode reaches the NPU, from the
# config. An exported CHARSIU_* wins, as on Linux (env_default).
$script:Inherited = @()
function Set-EnvDefault([string]$Name, [string]$Value) {
    if (Test-Path "Env:$Name") { $script:Inherited += $Name; return }
    Set-Item -Path "Env:$Name" -Value $Value
}

function Set-RunEnvironment {
    $npu = Get-Ini 'npu' 'enable'; if (-not $npu) { $npu = 'yes' }
    $ks = Get-Ini 'npu' 'kslice'; if (-not $ks) { $ks = '1024' }
    $mx = Get-Ini 'npu' 'maxn'; if (-not $mx) { $mx = '262144' }
    $co = Get-Ini 'npu' 'coef_elems'; if (-not $co) { $co = '65536' }
    if (Test-Yes $npu) {
        Set-EnvDefault CHARSIU_NPU 1
        Set-EnvDefault CHARSIU_NPU_QUANT 1
        Set-EnvDefault CHARSIU_NPU_W4V 1
        # KMAX and GROUP MUST BE EQUAL (npudev.c gates int4 on it)
        Set-EnvDefault CHARSIU_NPU_KMAX $ks
        Set-EnvDefault CHARSIU_NPU_W4_GROUP $ks
        Set-EnvDefault CHARSIU_NPU_MAXN $mx
        Set-EnvDefault CHARSIU_COEF_ELEMS $co
        if (Test-Yes (Get-Ini 'npu' 'group_fit')) { Set-EnvDefault CHARSIU_NPU_W4_GROUP_FIT 1 }
    }
    $cpus = Get-Ini 'run' 'cpus'
    if ($cpus) { Set-EnvDefault CHARSIU_CPUS $cpus }
    $pool = Get-Ini 'run' 'pool_cpus'
    if ($pool) { Set-EnvDefault CHARSIU_POOL_CPUS $pool }
    if (Test-Yes (Get-Ini 'debug' 'enable')) { Set-EnvDefault CHARSIU_STAGES 1 }
    # [extra]: CHARSIU_NAME=VALUE lines, passed through
    if (Test-Path -LiteralPath $Conf) {
        $in = $false
        foreach ($l in [IO.File]::ReadAllLines($Conf)) {
            if ($l -match '^\s*\[(.*)\]\s*$') { $in = ($Matches[1].Trim() -eq 'extra'); continue }
            if (-not $in) { continue }
            $t = ($l -replace '\s+#.*$', '').Trim()
            if ($t -match '^(CHARSIU_[A-Za-z0-9_]+)=(.*)$') { Set-EnvDefault $Matches[1] $Matches[2] }
        }
    }
}

# The weight cache is on unless [run] cache says otherwise (on Linux it is
# off unless it says so; see config.ini).
function Use-Cache {
    $c = Get-Ini 'run' 'cache'
    if (-not $c) { return $true }
    return (Test-Yes $c)
}

# ---- first run ---------------------------------------------------------------

function Initialize-Config {
    if (Test-Path -LiteralPath $Conf) { return }
    $t = Join-Path $Here 'config.ini'
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Conf) | Out-Null
    if (Test-Path -LiteralPath $t) { Copy-Item -LiteralPath $t -Destination $Conf }
    else { Set-Ini 'model' 'path' '' }
}

# ---- pull --------------------------------------------------------------------

function Get-FreeMB([string]$Dir) {
    try {
        $root = [IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $Dir).Path)
        $di = New-Object IO.DriveInfo($root)
        return [int64]($di.AvailableFreeSpace / 1MB)
    } catch { return -1 }
}

function Get-HfSha256([string]$Repo, [string]$File) {
    try {
        $tree = Invoke-RestMethod -UseBasicParsing -TimeoutSec 30 -Uri "$HF/api/models/$Repo/tree/main"
        foreach ($e in $tree) {
            if ($e.path -eq $File -and $e.lfs -and $e.lfs.oid) { return [string]$e.lfs.oid }
        }
    } catch { }
    return ''
}

function Save-Url([string]$Url, [string]$Part) {
    $curl = Get-Curl
    if (-not $curl) { Die 'curl.exe is missing (it ships with Windows 10 1803 and later)' }
    $quiet = $env:CHARSIU_NO_PROGRESS -or [Console]::IsErrorRedirected
    $a = @('-fL', '--retry', '3', '--connect-timeout', '15', '-C', '-', '-o', $Part)
    if ($quiet) { $a += '-sS' } else { $a += '--progress-bar' }
    $a += $Url
    return (Invoke-Here $curl $a)
}

# The config names what was fetched when what it names is not here. A model
# somebody chose and still has is left alone (charsiu-get's claim_config).
function Set-ConfigModel([string]$Path) {
    Initialize-Config
    $cur = Get-CurrentModel
    if ($cur -and (Test-Path -LiteralPath $cur -PathType Leaf)) { return }
    Set-Ini 'model' 'path' $Path
    Say "config now runs $(Split-Path -Leaf $Path)"
}

function Invoke-Gate([string]$File) {
    $ck = Need-Exe 'charsiu_check'
    return (Invoke-Here $ck @($File))
}

function Get-Model([string]$Url, [string]$Base, [int]$WantMB, [string]$Repo) {
    New-Item -ItemType Directory -Force -Path $Models | Out-Null
    $out = Join-Path $Models $Base
    $part = "$out.part"
    $ck = Need-Exe 'charsiu_check'

    if (Test-Path -LiteralPath $out) {
        Say "already here: $out"
        $rc = (Invoke-Captured $ck @('-q', $out)).Code
        if ($rc -eq 0) { Set-ConfigModel $out; return }
        if ($rc -eq 3) { return }
        Warn '...but it does not pass the gate; fetching it again'
        Remove-Item -LiteralPath $out -Force
    }
    if ($WantMB -gt 0) {
        $free = Get-FreeMB $Models
        # the model, and the weight cache it will write beside itself
        $need = $WantMB + 128
        if ($free -ge 0 -and $free -lt $need) {
            Die "$Models has ${free} MB free and this needs about ${WantMB} MB (and its weight cache about as much again on the first run)"
        }
    }
    Say "fetching $Base$(if ($WantMB) { ", about $WantMB MB" })"
    $rc = Save-Url $Url $part
    if ($rc -ne 0) {
        $got = 0
        if (Test-Path -LiteralPath $part) { $got = [int]((Get-Item -LiteralPath $part).Length / 1MB) }
        Die "the download stopped after $got MB of about $WantMB MB (curl $rc).`n  It does not start over: the part that arrived is kept in`n    $part`n  and running the same command again picks up where it stopped."
    }
    if ($Repo) {
        $want = Get-HfSha256 $Repo $Base
        if (-not $want) {
            Warn "could not read the published sha256 for $Base; the gate alone decides"
        } else {
            $got = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
            if ($got -ne $want.ToLowerInvariant()) {
                Remove-Item -LiteralPath $part -Force
                Die "sha256 of $Base is $got, and Hugging Face publishes $want.`n  The download was deleted; running the same command again fetches it fresh."
            }
            Say "sha256 matches what Hugging Face publishes ($($want.Substring(0, 12))...)"
        }
    } else {
        Say 'no published hash for a plain URL; the gate alone decides'
    }
    Say 'checking what actually came down'
    $rc = Invoke-Gate $part
    if ($rc -eq 0 -or $rc -eq 3) {
        Move-Item -LiteralPath $part -Destination $out -Force
        Say "kept $out"
        if ($rc -eq 0) { Set-ConfigModel $out }
        else { Say '  ...it is not a chat model, so the config still points at the one it did.' }
        return
    }
    Remove-Item -LiteralPath $part -Force
    Die "charsiu cannot run that file, so it was deleted rather than left in $Models"
}

function Show-Zoo {
    Write-Output ''
    Write-Output "Models verified to load on charsiu   (into $Models)"
    Write-Output ''
    Write-Output ('  {0,-22} {1,7}  {2}' -f 'NAME', 'SIZE', 'WHERE')
    foreach ($z in Get-Zoo) {
        $mb = $z.MB + $z.MmprojMB
        $have = ''
        if (Test-Path -LiteralPath (Join-Path $Models $z.File)) { $have = ' [have]' }
        Write-Output ('  {0,-22} {1,6}M  {2}{3}' -f $z.Name, $mb, $z.Repo, $have)
        if ($z.Note) { Write-Output ('  {0,-19} {1,7}  {2}' -f '', '', $z.Note) }
    }
    Write-Output ''
    Write-Output '  charsiu pull <name>              fetch one'
    Write-Output '  charsiu pull <user/repo> <file>  fetch a file from any repo'
    Write-Output '  charsiu pull --check FILE        gate a file you already have'
    Write-Output ''
}

function Invoke-Pull([string[]]$A) {
    if ($A.Count -eq 0 -or $A[0] -eq '--list' -or $A[0] -eq 'list') { Show-Zoo; return }
    switch ($A[0]) {
        '--zoo' {
            # the zoo as data, for the desktop app; the same records as
            # charsiu-get --zoo on Linux
            Write-Output "dir|$Models"
            $cur = Get-CurrentModel
            if ($cur) { Write-Output "current|$cur" }
            foreach ($z in Get-Zoo) {
                Write-Output ("zoo|{0}|{1}|{2}|{3}|{4}|{5}|{6}" -f $z.Name, $z.Repo, $z.File, $z.MB, $z.Note, $z.Mmproj, $(if ($z.MmprojMB) { $z.MmprojMB } else { '' }))
            }
            return
        }
        '--wizard' { Invoke-Setup; return }
        '--check' {
            if ($A.Count -lt 2) { Die '--check needs a file' 2 }
            exit (Invoke-Gate $A[1])
        }
    }
    $arg = $A[0]
    $z = Get-Zoo | Where-Object { $_.Name -eq $arg } | Select-Object -First 1
    if ($z) {
        Get-Model "$HF/$($z.Repo)/resolve/main/$($z.File)" $z.File $z.MB $z.Repo
        if ($z.Mmproj) {
            Write-Output ''
            Write-Output '  and its vision tower, which is the other half of this model:'
            Get-Model "$HF/$($z.Repo)/resolve/main/$($z.Mmproj)" $z.Mmproj $z.MmprojMB $z.Repo
            Write-Output '  Both halves are here. To ask it about a picture:'
            Write-Output '    charsiu --image photo.jpg "what is in this picture?"'
        }
        return
    }
    if ($arg -match '^https?://') { Get-Model $arg ([IO.Path]::GetFileName(([Uri]$arg).AbsolutePath)) 0 ''; return }
    if ($arg -match '^[^/\\]+/[^/\\]+$') {
        if ($A.Count -ge 2) { Get-Model "$HF/$arg/resolve/main/$($A[1])" $A[1] 0 $arg; return }
        Say "listing $arg"
        try {
            $tree = Invoke-RestMethod -UseBasicParsing -TimeoutSec 30 -Uri "$HF/api/models/$arg/tree/main"
            $tree | Where-Object { $_.path -like '*.gguf' } | ForEach-Object { Write-Output "  $($_.path)" }
        } catch { Die "could not list $arg ($($_.Exception.Message))" }
        Write-Output ''
        Write-Output "  charsiu pull $arg <file>   downloads, checks, and keeps it only if it runs."
        return
    }
    Die "no zoo entry called '$arg'. Run charsiu pull with no arguments for the list."
}

# ---- setup -------------------------------------------------------------------

function Invoke-Setup {
    Initialize-Config
    $zoo = @(Get-Zoo | Where-Object { $_.File -like '*.gguf' -and $_.Name -notlike 'clip-*' })
    Write-Output ''
    Write-Output 'charsiu: which model?'
    Write-Output ''
    Write-Output '  charsiu re-quantises whatever it loads, so a smaller file is a faster first'
    Write-Output '  run rather than a worse one. The 1B Llama is what the board measurements use.'
    Write-Output ''
    for ($i = 0; $i -lt $zoo.Count; $i++) {
        $z = $zoo[$i]
        $have = ''
        if (Test-Path -LiteralPath (Join-Path $Models $z.File)) { $have = '  (already here)' }
        Write-Output ('  {0,2}  {1,-22} {2,6}M  {3}{4}' -f ($i + 1), $z.Name, ($z.MB + $z.MmprojMB), $z.Note, $have)
    }
    Write-Output ''
    $sel = Read-Host 'Number or name (Enter to decide later)'
    if (-not $sel) { Write-Output '  later, then: charsiu pull lists them all.'; return }
    $pick = $null
    $n = 0
    if ([int]::TryParse($sel, [ref]$n) -and $n -ge 1 -and $n -le $zoo.Count) { $pick = $zoo[$n - 1] }
    else { $pick = $zoo | Where-Object { $_.Name -eq $sel } | Select-Object -First 1 }
    if (-not $pick) { Die "no model called '$sel'" 2 }
    Invoke-Pull @($pick.Name)
}

# ---- run / chat --------------------------------------------------------------

# The flags of charsiu_run that take a value, which the bare-word scan steps
# over (the Linux runner's list).
$ValueFlags = @('--sys', '-n', '-c', '-t', '--temp', '--top-p', '--seed', '--hold-secs', '--cache-at', '--image', '--mmproj')

function Invoke-Run([string]$Named, [string[]]$A, [switch]$Bench) {
    $show = $false
    $rest = New-Object System.Collections.Generic.List[string]
    foreach ($x in $A) { if ($x -eq '--show') { $show = $true } else { $rest.Add($x) } }
    $A = $rest.ToArray()

    # A BARE WORD IS THE QUESTION (the runner's rule, word for word): it
    # becomes -p with the chat template and -q, and flags keep their values.
    $hasP = $false; $bare = $false; $skip = $false
    foreach ($x in $A) {
        if ($skip) { $skip = $false; continue }
        if ($x -eq '-p' -or $x -eq '-f') { $hasP = $true; $skip = $true }
        elseif ($ValueFlags -contains $x) { $skip = $true }
        elseif ($x.StartsWith('-')) { }
        else { $bare = $true }
    }
    if ($bare -and -not $hasP) {
        $out = New-Object System.Collections.Generic.List[string]
        $q = $null; $skip = $false
        foreach ($x in $A) {
            if ($skip) { $out.Add($x); $skip = $false; continue }
            if ($ValueFlags -contains $x) { $out.Add($x); $skip = $true }
            elseif ($x.StartsWith('-')) { $out.Add($x) }
            elseif ($null -eq $q) { $q = $x }
            else { $out.Add($x) }
        }
        $out.Add('-p'); $out.Add($q)
        if ($q -match '^[A-Za-z0-9_-]+$') {
            [Console]::Error.WriteLine("${Self}: reading `"$q`" as a question, not a command. ``charsiu --help`` lists the commands.")
        }
        if (-not ($out -contains '--chat' -or $out -contains '-i' -or $out -contains '--interactive')) { $out.Add('--chat') }
        if (-not ($out -contains '-q')) { $out.Add('-q') }
        $A = $out.ToArray()
    }
    $ask = ($A -contains '--info') -or ($A -contains '--tokens') -or ($A -contains '--logits')
    $interactive = (-not $bare) -and (-not $hasP) -and (-not $ask) -and (-not $Bench) -and (-not $show)

    $model = Select-Model $Named -Quiet:$show
    $run = Need-Exe 'charsiu_run'
    $threads = Get-Ini 'run' 'threads'; if (-not $threads) { $threads = '4' }
    $ctx = Get-Ini 'run' 'context'; if (-not $ctx) { $ctx = '512' }
    Set-RunEnvironment

    $argv = New-Object System.Collections.Generic.List[string]
    $argv.Add($model)
    $argv.Add('-c'); $argv.Add($ctx)
    $argv.Add('-t'); $argv.Add($threads)
    if ((Use-Cache) -and -not ($A -contains '--cache' -or $A -contains '--cache-at')) { $argv.Add('--cache') }

    if ($Bench) {
        Write-Output ''
        Write-Output "  charsiu bench: $(Split-Path -Leaf $model)"
        $mode = 'CPU'; if ($env:CHARSIU_NPU -eq '1') { $mode = 'NPU int4' }
        Write-Output "  $mode, $threads threads, context $ctx"
        Write-Output ''
        Write-NpuHint
        $argv.AddRange([string[]]@('-p', 'The capital of France is', '-n', '64', '--ignore-eos', '-q'))
        $r = Invoke-Captured $run $argv.ToArray()
        $all = $r.Out + "`n" + $r.Err
        $m = [regex]::Match($all, 'gen 64 tok in [0-9]+ ms, [0-9.]+ tok/s')
        if ($m.Success) { Write-Output "  $($m.Value)" } else { Write-Output '  (no rate reported)'; [Console]::Error.WriteLine($r.Err) }
        $m = [regex]::Match($all, '[0-9.]+ GB/s of weights')
        if ($m.Success) { Write-Output "  $($m.Value)" }
        Write-Output ''
        exit $r.Code
    }

    if ($interactive) { $argv.Add('-i') }
    if ($A.Count) { $argv.AddRange([string[]]$A) }
    if ($show) {
        $envs = Get-ChildItem Env: | Where-Object { $_.Name -like 'CHARSIU_*' -and $script:Inherited -notcontains $_.Name } |
                ForEach-Object { "$($_.Name)=$($_.Value)" }
        Write-Output ("env " + ($envs -join ' ') + " ``")
        Write-Output ("  " + (Quote-Arg $run) + ' ' + (($argv | ForEach-Object { Quote-Arg $_ }) -join ' '))
        if ($script:Inherited.Count) { Write-Output "# from the environment, not the config: $($script:Inherited -join ' ')" }
        exit 0
    }
    Write-NpuHint
    exit (Invoke-Here $run $argv.ToArray())
}

# ---- serve -------------------------------------------------------------------

function Invoke-Serve([string[]]$A) {
    $model = ''
    if ($env:CHARSIU_MODEL) { $model = $env:CHARSIU_MODEL }
    $model = Select-Model $model
    $bin = Need-Exe 'charsiu_serve'
    $ctx = Get-Ini 'serve' 'context'
    if (-not $ctx) { $ctx = Get-Ini 'run' 'context' }
    if (-not $ctx) { $ctx = '2048' }
    $threads = Get-Ini 'run' 'threads'; if (-not $threads) { $threads = '4' }
    Set-RunEnvironment
    $argv = New-Object System.Collections.Generic.List[string]
    $argv.AddRange([string[]]@($model, '-c', $ctx, '-t', $threads))
    if ((Use-Cache) -and -not ($A -contains '--cache' -or $A -contains '--cache-at')) { $argv.Add('--cache') }
    if ($A.Count) { $argv.AddRange([string[]]$A) }

    $port = '11434'; $hostA = ''
    for ($i = 0; $i -lt $A.Count - 1; $i++) {
        if ($A[$i] -eq '--port') { $port = $A[$i + 1] }
        if ($A[$i] -eq '--host') { $hostA = $A[$i + 1] }
    }
    # this machine only unless told otherwise (charsiu_serve.exe's own default)
    if (-not $hostA) { $hostA = '127.0.0.1'; $argv.AddRange([string[]]@('--host', $hostA)) }
    if ($port -ne '0') {
        $shown = $hostA; if ($hostA -eq '0.0.0.0') { $shown = '127.0.0.1' }
        [Console]::Error.WriteLine('')
        [Console]::Error.WriteLine("  OpenAI     http://${shown}:$port/v1/chat/completions")
        [Console]::Error.WriteLine("  Anthropic  http://${shown}:$port/v1/messages")
        if ($hostA -eq '127.0.0.1') { [Console]::Error.WriteLine('  (this machine only; --host 0.0.0.0 offers it to the network)') }
    }
    Write-NpuHint
    exit (Invoke-Here $bin $argv.ToArray())
}

# ---- list / rm / show / ps ---------------------------------------------------

function Invoke-List {
    $cur = Get-CurrentModel
    $ck = Find-Exe 'charsiu_check'
    Write-Output ('{0,-44} {1,8}  {2,-5} {3}' -f 'NAME', 'SIZE', 'RUNS', 'WHERE')
    $all = Get-AllModels
    foreach ($f in $all) {
        $it = Get-Item -LiteralPath $f
        $runs = '?'
        if ($ck) { if ((Invoke-Captured $ck @('-q', $f)).Code -eq 0) { $runs = 'yes' } else { $runs = 'NO' } }
        $mark = ''; if ($f -eq $cur) { $mark = ' *' }
        Write-Output ('{0,-44} {1,7}M  {2,-5} {3}{4}' -f $it.Name, [int]($it.Length / 1MB), $runs, $it.DirectoryName, $mark)
    }
    if ($all.Count -eq 0) { Write-Output ''; Write-Output '  nothing yet. `charsiu pull` fetches one.' }
}

function Invoke-Rm([string[]]$A) {
    $force = $false
    $names = @()
    foreach ($x in $A) { if ($x -eq '-f') { $force = $true } else { $names += $x } }
    if ($names.Count -eq 0) { Die 'charsiu rm: which model? (charsiu list)' 2 }
    $cur = Get-CurrentModel
    $rc = 0
    foreach ($n in $names) {
        $t = Resolve-Model $n
        if (-not $t) { [Console]::Error.WriteLine("charsiu rm: no such model: $n"); $rc = 1; continue }
        if ($t -eq $cur -and -not $force) {
            [Console]::Error.WriteLine("charsiu rm: $n is the model in use. -f to remove it anyway."); $rc = 1; continue
        }
        Remove-Item -LiteralPath $t -Force
        Write-Output "removed $t"
        if (Test-Path -LiteralPath "$t.wcache") { Remove-Item -LiteralPath "$t.wcache" -Force; Write-Output "removed $t.wcache" }
    }
    exit $rc
}

function Invoke-Show([string[]]$A) {
    $n = ''; if ($A.Count) { $n = $A[0] } else { $n = Get-CurrentModel }
    if (-not $n) { Die 'charsiu show: no model set and none named' 2 }
    $m = Resolve-Model $n
    if (-not $m) { Die "charsiu show: no such model: $n" 2 }
    $run = Find-Exe 'charsiu_run'
    if ($run) { Write-Output (Invoke-Captured $run @($m, '--info')).Out }
    $ck = Find-Exe 'charsiu_check'
    if ($ck) { exit (Invoke-Here $ck @($m)) }
}

function Get-NpuCounters {
    try {
        $s = Get-Counter -Counter @('\RK3576 NPU(*)\% Busy Time', '\RK3576 NPU(*)\Jobs/sec', '\RK3576 NPU Memory\Buffer Bytes') -ErrorAction Stop
        return $s.CounterSamples
    } catch { return $null }
}

function Invoke-Ps {
    Write-Output ('{0,-8} {1,-14} {2}' -f 'PID', 'NAME', 'MEMORY')
    $p = @(Get-Process -Name 'charsiu_run', 'charsiu_serve' -ErrorAction SilentlyContinue)
    foreach ($x in $p) { Write-Output ('{0,-8} {1,-14} {2} MB' -f $x.Id, $x.ProcessName, [int]($x.WorkingSet64 / 1MB)) }
    if ($p.Count -eq 0) { Write-Output '(nothing running)' }
    Write-Output ''
    $d = Get-NpuDevice
    if (-not $d.Present) { Write-Output '  npu              not present (firmware setup: NPU (experimental))' }
    elseif ($d.Started) { Write-Output "  npu              started   $($d.Driver)" }
    else { Write-Output "  npu              problem $($d.Problem)" }
    $c = Get-NpuCounters
    if ($c) {
        foreach ($s in $c) {
            $name = ($s.Path -split '\\')[-1]
            $inst = $s.InstanceName
            Write-Output ('  {0,-16} {1,-28} {2:N1}' -f $inst, $name, $s.CookedValue)
        }
    } elseif ($d.Started) {
        Write-Output '  counters         not registered (tools\rknpu-perfcounters.ps1 registers them)'
    }
}

# ---- doctor ------------------------------------------------------------------

function Invoke-Doctor {
    $bad = 0
    function Row($ok, $what, $how) {
        $w = 'ok  '; if ($ok -eq $false) { $w = 'FAIL'; $script:docBad++ } elseif ($ok -eq $null) { $w = 'note' }
        Write-Output ('  {0}  {1,-14} {2}' -f $w, $what, $how)
    }
    $script:docBad = 0
    Write-Output ''
    Write-Output "  charsiu $Version on Windows"
    Write-Output ''
    $os = [Environment]::OSVersion.Version
    $arch = $env:PROCESSOR_ARCHITECTURE
    Row ($arch -eq 'ARM64') 'machine' "$arch, Windows $($os.Major).$($os.Minor).$($os.Build)"
    $d = Get-NpuDevice
    if (-not $d.Present) { Row $false 'npu device' 'ACPI\RKCP3576 is not there: enable "NPU (experimental)" in the firmware setup' }
    elseif (-not $d.Started) { Row $false 'npu driver' "problem code $($d.Problem): charsiu-install.ps1 -Driver stages rknpu.sys (then restart)" }
    else { Row $true 'npu driver' "started, $($d.Driver)" }
    if ($d.Started) {
        if (Test-Admin) { Row $true 'npu access' 'this is an administrator session' }
        elseif ($d.UserAccess) { Row $true 'npu access' 'UserAccess = 1: this user may open the NPU' }
        else { Row $false 'npu access' 'administrators only: run charsiu-install.ps1 once from an administrator PowerShell and restart, or use an administrator terminal' }
        if (-not $d.Iommu) { Row $null 'npu iommu' 'M6Iommu = 0 (bypass): the NPU stays administrators only' }
        if (Get-NpuCounters) { Row $true 'counters' '"RK3576 NPU" performance counters are registered' }
        else { Row $null 'counters' 'not registered; tools\rknpu-perfcounters.ps1 registers them (charsiu ps, the desktop app)' }
    }
    foreach ($e in 'charsiu_run', 'charsiu_serve', 'charsiu_check') {
        $p = Find-Exe $e
        if (-not $p) { Row $false $e 'missing'; continue }
        $v = (Invoke-Captured $p @('--version')).Out.Trim()
        Row $true $e "$v"
    }
    if (Test-Path -LiteralPath $Conf) { Row $true 'config' $Conf } else { Row $null 'config' "$Conf (not yet; charsiu setup writes it)" }
    $m = Get-CurrentModel
    $all = Get-AllModels
    if ($m -and (Test-Path -LiteralPath $m)) { Row $true 'model' $m }
    elseif ($all.Count) { Row $null 'model' "the config names none; $($all.Count) here, charsiu uses the only one or asks" }
    else { Row $false 'model' 'none here: charsiu pull' }
    if (Test-Path -LiteralPath $Models) {
        $free = Get-FreeMB $Models
        Row ($free -lt 0 -or $free -gt 1024) 'disk' "$free MB free where the models are ($Models)"
    }
    $curl = Get-Curl
    Row ([bool]$curl) 'curl' $(if ($curl) { $curl } else { 'missing: charsiu pull needs it' })
    Write-Output ''
    if ($script:docBad) { exit 1 }
}

# ---- update ------------------------------------------------------------------

function Invoke-Update([string[]]$A) {
    $inst = Join-Path $Here 'charsiu-install.ps1'
    if (-not (Test-Path -LiteralPath $inst)) { Die "charsiu-install.ps1 is not beside this script ($Here)" }
    $ps = Join-Path $PSHOME 'powershell.exe'
    if (-not (Test-Path -LiteralPath $ps)) { $ps = (Get-Process -Id $PID).Path }
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $inst, '-Update') + $A
    exit (Invoke-Here $ps $argv)
}

function Show-Usage {
@"
charsiu $Version, an open LLM runtime for the RK3576 NPU, on Windows on Arm

  charsiu                       a conversation, or the guided setup on a new machine
  charsiu "what is a bandgap"   ask once and exit
  charsiu -p "..."              print mode (a completion, no chat template)
  charsiu --image FILE "..."    ask about a picture, with a model that sees

  charsiu run MODEL [PROMPT]    use a particular model
  charsiu pull [NAME]           fetch one; no NAME lists them
  charsiu list                  what is here                        (alias: ls)
  charsiu rm [-f] NAME          delete one, and its weight cache
  charsiu show [MODEL]          what it is, and whether it can run here
  charsiu ps                    is anything running, and what the NPU is doing
  charsiu bench                 tokens per second on this board
  charsiu doctor                what works and what does not
  charsiu serve [--port N]      an OpenAI and Anthropic compatible endpoint,
                                on 127.0.0.1:11434 (--host 0.0.0.0 for the network)
  charsiu update                fetch the latest release and reinstall
  charsiu setup                 the guided screen, on purpose

  models  $Models
  config  $Conf
"@
}

# ---- the front door ----------------------------------------------------------

$A = @($args | ForEach-Object { [string]$_ })
$cmd = ''
if ($A.Count) { $cmd = $A[0] }
$rest = @()
if ($A.Count -gt 1) { $rest = $A[1..($A.Count - 1)] }

switch -CaseSensitive ($cmd) {
    { $_ -in '-h', '--help', 'help' } { Show-Usage; exit 0 }
    { $_ -in '-V', '--version' } { Write-Output "charsiu $Version"; exit 0 }
    'setup' { Invoke-Setup; exit 0 }
    'update' { Invoke-Update $rest }
    'doctor' { Invoke-Doctor; exit 0 }
    'pull' { Invoke-Pull $rest; exit 0 }
    { $_ -in 'list', 'ls' } { Invoke-List; exit 0 }
    'rm' { Invoke-Rm $rest }
    'show' { Invoke-Show $rest; exit 0 }
    'ps' { Invoke-Ps; exit 0 }
    'bench' { Invoke-Run '' $rest -Bench }
    'serve' { Invoke-Serve $rest }
    'run' {
        if ($rest.Count -lt 1) { Die 'charsiu run: which model?' 2 }
        $more = @(); if ($rest.Count -gt 1) { $more = $rest[1..($rest.Count - 1)] }
        Invoke-Run $rest[0] $more
    }
}

# No subcommand: a conversation, or the guided screen on a machine with
# nothing set up yet (the Linux front door's one rule).
$cfgModel = Get-CurrentModel
if ($A.Count -eq 0 -and -not ($cfgModel -and (Test-Path -LiteralPath $cfgModel)) -and (Get-AllModels).Count -eq 0) {
    Invoke-Setup
    if ((Get-AllModels).Count -eq 0) { exit 0 }
}
Invoke-Run '' $A
