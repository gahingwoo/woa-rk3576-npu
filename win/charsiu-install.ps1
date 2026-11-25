# charsiu-install.ps1 - set up charsiu on Windows on Arm (RK3576, rknpu.sys)
#
#   irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1 | iex
#
# or, to pass options, either of
#
#   & ([scriptblock]::Create((irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1))) --dry-run
#   powershell -NoProfile -ExecutionPolicy Bypass -File charsiu-install.ps1 --dry-run
#
# Options (each also as -PascalCase: -DryRun, -Zip, ...):
#   --dry-run          say what it would do and change nothing; the checks still run
#   --yes, -y          no questions: the default model, no demo question
#   --zip FILE|URL     install this charsiu-windows-arm64.zip instead of the release's
#   --version TAG      a particular release instead of the latest
#   --dir DIR          install into DIR (default %LOCALAPPDATA%\Programs\charsiu)
#   --model NAME       the model to fetch (a `charsiu pull` name)
#   --no-model         fetch no model
#   --no-demo          do not ask it a question at the end
#   --driver [ZIP]     administrator: stage rknpu.sys from the release's
#                      rknpu-windows-arm64.zip (or ZIP / a folder) and mark the
#                      device for reinstall at the next restart
#   --admin-only       keep the NPU for administrators. By default an install
#                      from an administrator PowerShell lets ordinary users
#                      open it (Device Parameters\UserAccess = 1, IOMMU mode
#                      only; takes a restart), as Linux lets the video group
#   --update           what `charsiu update` runs: replace the programs only
#   --uninstall        remove the programs and the PATH entry (models stay;
#                      --purge removes %LOCALAPPDATA%\charsiu as well)
#
# The same steps as charsiu's Linux installer, in Windows' terms: check that
# the NPU is there and has its driver, put the programs in place, put them on
# PATH, fetch a model, and ask it something so you can see it work.
#
# A DRIVER IS NEVER INSTALLED ON A RUNNING DEVICE. `pnputil /install` on a
# live device has bugchecked this board with other drivers, so --driver only
# stages the package (pnputil /add-driver) and sets CONFIGFLAG_REINSTALL
# (ConfigFlags 0x20) as SYSTEM; PnP binds it at the next restart.
#
# Written for Windows PowerShell 5.1 (Windows 11 23H2).
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

function Install-Charsiu {
    $ErrorActionPreference = 'Stop'
    $ProgressPreference = 'SilentlyContinue'   # Invoke-WebRequest is ten times slower drawing it
    $Repo = 'gahingwoo/woa-rk3576-npu'
    $PkgName = 'charsiu-windows-arm64.zip'
    $DrvName = 'rknpu-windows-arm64.zip'
    $NpuKey = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0'

    # ---- options ---------------------------------------------------------
    $o = @{ DryRun = $false; Yes = $false; Zip = ''; Version = ''; Dir = ''; Model = ''; NoModel = $false
            NoDemo = $false; Driver = $false; DriverZip = ''; AdminOnly = $false; Update = $false
            Uninstall = $false; Purge = $false }
    $list = @($args)
    if ($env:CHARSIU_INSTALL_ARGS) { $list += ($env:CHARSIU_INSTALL_ARGS -split '\s+' | Where-Object { $_ }) }
    for ($i = 0; $i -lt $list.Count; $i++) {
        $a = [string]$list[$i]
        $next = ''
        if ($i + 1 -lt $list.Count) { $next = [string]$list[$i + 1] }
        switch -Regex ($a) {
            '^(--dry-run|-n|-DryRun)$' { $o.DryRun = $true; continue }
            '^(--yes|-y|-Yes)$' { $o.Yes = $true; continue }
            '^(--zip|-Zip)$' { $o.Zip = $next; $i++; continue }
            '^(--version|-Version)$' { $o.Version = $next; $i++; continue }
            '^(--dir|-Dir)$' { $o.Dir = $next; $i++; continue }
            '^(--model|-Model)$' { $o.Model = $next; $i++; continue }
            '^(--no-model|-NoModel)$' { $o.NoModel = $true; continue }
            '^(--no-demo|-NoDemo)$' { $o.NoDemo = $true; continue }
            '^(--driver|-Driver)$' {
                $o.Driver = $true
                if ($next -and -not $next.StartsWith('-')) { $o.DriverZip = $next; $i++ }
                continue
            }
            # the default now; accepted so the commands already written down still work
            '^(--user-access|-UserAccess)$' { $o.AdminOnly = $false; continue }
            '^(--admin-only|-AdminOnly)$' { $o.AdminOnly = $true; continue }
            '^(--update|-Update)$' { $o.Update = $true; $o.NoModel = $true; $o.NoDemo = $true; continue }
            '^(--uninstall|-Uninstall)$' { $o.Uninstall = $true; continue }
            '^(--purge|-Purge)$' { $o.Purge = $true; continue }
            '^(--help|-h|-Help|/\?)$' {
                Write-Host 'charsiu-install.ps1 [--dry-run] [--yes] [--zip FILE] [--version TAG] [--dir DIR] [--model NAME]'
                Write-Host '                    [--no-model] [--no-demo] [--driver [ZIP]] [--admin-only] [--update] [--uninstall [--purge]]'
                return
            }
            default { throw "charsiu-install: unknown option $a (--help lists them)" }
        }
    }

    $script:Planned = New-Object System.Collections.Generic.List[string]
    function Step($m) { Write-Host ''; Write-Host "== $m" }
    function Ok($m) { Write-Host "  ok    $m" }
    function Note($m) { Write-Host "  note  $m" }
    function Bad($m) { Write-Host "  FAIL  $m" -ForegroundColor Red }
    function Would($m) { $script:Planned.Add($m); Write-Host "  would $m" -ForegroundColor Cyan }
    function Ask($q, $default) {
        if ($o.Yes -or $o.DryRun) { return $default }
        $d = 'y/N'; if ($default) { $d = 'Y/n' }
        $r = Read-Host "$q [$d]"
        if (-not $r) { return $default }
        return ($r -match '^(y|yes)$')
    }
    function IsAdmin {
        try {
            $id = [Security.Principal.WindowsIdentity]::GetCurrent()
            return (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
        } catch { return $false }
    }
    function Download($url, $out) {
        $curl = Join-Path $env:SystemRoot 'System32\curl.exe'
        if (Test-Path -LiteralPath $curl) {
            & $curl -fL --retry 3 --connect-timeout 15 -sS -o $out $url
            if ($LASTEXITCODE -ne 0) { throw "download failed ($LASTEXITCODE): $url" }
        } else {
            Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $out
        }
    }
    function ReleaseUrl($name) {
        if ($o.Version) { return "https://github.com/$Repo/releases/download/$($o.Version)/$name" }
        return "https://github.com/$Repo/releases/latest/download/$name"
    }

    # The device's Enum key, Device Parameters included, is SYSTEM's: an
    # administrator cannot write it. Run a few lines as SYSTEM through a
    # one-shot scheduled task, wait for them, and hand back their log.
    function Invoke-AsSystem([string]$Name, [string]$Body) {
        $f = Join-Path $env:SystemRoot "Temp\$Name.ps1"
        $log = Join-Path $env:SystemRoot "Temp\$Name.log"
        Remove-Item -LiteralPath $log -Force -ErrorAction SilentlyContinue
        [IO.File]::WriteAllText($f, "`$ErrorActionPreference = 'Stop'`r`ntry {`r`n$Body`r`n'done' | Out-File -Encoding ascii -Append '$log'`r`n} catch { `$_ | Out-File -Encoding ascii -Append '$log' }`r`n")
        $tn = "charsiu-$Name"
        & schtasks /create /tn $tn /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File $f" | Out-Null
        & schtasks /run /tn $tn | Out-Null
        for ($w = 0; $w -lt 30; $w++) {
            if ((Test-Path -LiteralPath $log) -and (Select-String -Quiet -LiteralPath $log -Pattern '^done$|Exception|Error')) { break }
            Start-Sleep -Seconds 1
        }
        & schtasks /delete /tn $tn /f | Out-Null
        if (Test-Path -LiteralPath $log) { return (Get-Content -LiteralPath $log -Raw) }
        return ''
    }

    $LocalApp = $env:LOCALAPPDATA
    $Dir = $o.Dir
    if (-not $Dir) { $Dir = Join-Path $LocalApp 'Programs\charsiu' }
    $BinDir = Join-Path $Dir 'bin'
    $CharsiuHome = if ($env:CHARSIU_HOME) { $env:CHARSIU_HOME } else { Join-Path $LocalApp 'charsiu' }
    $Conf = Join-Path $CharsiuHome 'config.ini'
    $Models = Join-Path $CharsiuHome 'models'

    Write-Host ''
    Write-Host 'charsiu for Windows on Arm (RK3576 NPU)'
    if ($o.DryRun) { Write-Host 'DRY RUN: the checks run, nothing is changed.' -ForegroundColor Cyan }

    # ---- PATH ------------------------------------------------------------
    # The user's Path, read and written as REG_EXPAND_SZ so entries such as
    # %USERPROFILE%\... stay unexpanded ([Environment]::SetEnvironmentVariable
    # writes REG_SZ and expands nothing, which breaks them).
    function Get-UserPath {
        $k = $null
        try { $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment') } catch { }
        if (-not $k) { return '' }
        $v = $k.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
        $k.Close()
        return [string]$v
    }
    function Set-UserPath($v) {
        $k = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
        $k.SetValue('Path', $v, [Microsoft.Win32.RegistryValueKind]::ExpandString)
        $k.Close()
        # tell running programs (Explorer, new terminals) the environment changed
        [Environment]::SetEnvironmentVariable('CHARSIU_PATH_STAMP', [string](Get-Date -Format o), 'User')
        [Environment]::SetEnvironmentVariable('CHARSIU_PATH_STAMP', $null, 'User')
    }

    # ---- uninstall -------------------------------------------------------
    if ($o.Uninstall) {
        Step 'removing charsiu'
        $p = Get-UserPath
        $parts = @($p -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $BinDir.TrimEnd('\')) })
        if ($o.DryRun) {
            Would "remove $Dir"
            Would "take $BinDir off the user PATH"
            if ($o.Purge) { Would "remove $CharsiuHome (config, models and weight caches)" }
            return
        }
        if (Test-Path -LiteralPath $Dir) { Remove-Item -LiteralPath $Dir -Recurse -Force; Ok "removed $Dir" }
        Set-UserPath ($parts -join ';'); Ok 'PATH entry removed'
        if ($o.Purge -and (Test-Path -LiteralPath $CharsiuHome)) {
            Remove-Item -LiteralPath $CharsiuHome -Recurse -Force; Ok "removed $CharsiuHome"
        } else { Note "models and config stay in $CharsiuHome (--purge removes them)" }
        return
    }

    # ---- 1. this machine -------------------------------------------------
    Step 'this machine'
    $arch = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $arch = $env:PROCESSOR_ARCHITEW6432 }
    $build = [Environment]::OSVersion.Version.Build
    if ($arch -ne 'ARM64') { Bad "this is $arch; charsiu's Windows build is ARM64 only"; if (-not $o.DryRun) { return } }
    else { Ok "ARM64, Windows build $build" }
    $admin = IsAdmin
    if ($admin) { Ok 'administrator session' } else { Note 'not an administrator session (the driver and NPU access for ordinary users need one; the rest does not)' }

    # ---- 2. the NPU and its driver ---------------------------------------
    Step 'the NPU'
    $dev = $null
    try { $dev = Get-CimInstance Win32_PnPEntity -ErrorAction Stop | Where-Object { $_.DeviceID -like 'ACPI\RKCP3576\*' } | Select-Object -First 1 } catch { }
    # for CI only: a dry run that pretends the NPU and an administrator are
    # here, so the steps that need both can be checked off the board
    if ($o.DryRun -and $env:CHARSIU_INSTALL_FAKE_NPU -eq '1') {
        $dev = [pscustomobject]@{ ConfigManagerErrorCode = 0; Status = 'OK' }
        $admin = $true
        Note 'CHARSIU_INSTALL_FAKE_NPU=1: pretending the NPU and an administrator are here'
    }
    $npuStarted = $false
    if (-not $dev) {
        Bad 'no NPU device (ACPI\RKCP3576). Turn on "NPU (experimental)" in the firmware setup (ACPI / Device Tree page), save and restart.'
        Note 'charsiu still installs and runs on the CPU, at a fraction of the speed.'
    } elseif ([int]$dev.ConfigManagerErrorCode -eq 0 -and $dev.Status -eq 'OK') {
        $npuStarted = $true
        $drv = $null
        try { $drv = Get-CimInstance Win32_PnPSignedDriver -ErrorAction Stop | Where-Object { $_.DeviceID -like 'ACPI\RKCP3576\*' } | Select-Object -First 1 } catch { }
        $ver = ''; if ($drv) { $ver = " $($drv.DriverVersion)" }
        Ok "ACPI\RKCP3576 is started (rknpu.sys$ver)"
    } else {
        Bad "ACPI\RKCP3576 is there with problem code $($dev.ConfigManagerErrorCode): rknpu.sys is not bound."
        if (-not $o.Driver) {
            Note 'From an administrator PowerShell, the same command with --driver stages it for the next restart:'
            Note "  & ([scriptblock]::Create((irm https://raw.githubusercontent.com/$Repo/main/win/charsiu-install.ps1))) --driver"
            Note 'or by hand: docs/use.md, steps 3 to 6.'
        }
    }

    $reboot = $false
    if ($o.Driver) {
        Step 'staging rknpu.sys'
        if (-not $admin) { Bad '--driver needs an administrator PowerShell'; if (-not $o.DryRun) { return } }
        if (-not $dev) { Bad 'there is no NPU device to bind it to; turn the NPU on in the firmware setup first'; if (-not $o.DryRun) { return } }
        $src = $o.DriverZip
        $tmp = Join-Path $env:TEMP ("rknpu-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
        if ($o.DryRun) {
            if ($src) { Would "unpack $src" } else { Would "download $(ReleaseUrl $DrvName) and check its sha256" }
            Would 'w32tm /resync /force (a test signature is checked against the clock)'
            Would 'certutil -addstore Root and TrustedPublisher signer.cer'
            Would 'pnputil /add-driver rknpu\rknpu.inf   (stage only; nothing is installed on the running device)'
            Would "set ConfigFlags |= 0x20 on $NpuKey, as SYSTEM, through a one-shot scheduled task"
        } else {
            New-Item -ItemType Directory -Force -Path $tmp | Out-Null
            if (-not $src) {
                $src = Join-Path $tmp $DrvName
                Download (ReleaseUrl $DrvName) $src
                Download ((ReleaseUrl $DrvName) + '.sha256') "$src.sha256"
                $want = ((Get-Content "$src.sha256" -TotalCount 1) -split '\s+')[0].ToLowerInvariant()
                $got = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($got -ne $want) { throw "sha256 of $DrvName is $got, the release says $want" }
                Ok "$DrvName sha256 $($got.Substring(0, 12))..."
            }
            $root = $src
            if (Test-Path -LiteralPath $src -PathType Leaf) {
                $root = Join-Path $tmp 'drv'
                Expand-Archive -LiteralPath $src -DestinationPath $root -Force
            }
            $inf = Get-ChildItem -LiteralPath $root -Recurse -Filter 'rknpu.inf' | Select-Object -First 1
            $cer = Get-ChildItem -LiteralPath $root -Recurse -Filter 'signer.cer' | Select-Object -First 1
            $re = Get-ChildItem -LiteralPath $root -Recurse -Filter 'rknpu-reinstall.ps1' | Select-Object -First 1
            if (-not $inf -or -not $re) { throw "no rknpu.inf or rknpu-reinstall.ps1 in $src" }
            & w32tm /resync /force | Out-Null
            if ($cer) {
                & certutil -addstore -f Root $cer.FullName | Out-Null
                & certutil -addstore -f TrustedPublisher $cer.FullName | Out-Null
                Ok 'signer.cer trusted (Root, TrustedPublisher)'
            }
            & pnputil /add-driver $inf.FullName
            if ($LASTEXITCODE -ne 0) { throw "pnputil /add-driver failed ($LASTEXITCODE)" }
            Ok 'rknpu.inf staged'
            # the Enum key is SYSTEM's: run the reinstall mark as SYSTEM
            $body = (Get-Content -LiteralPath $re.FullName -Raw) -replace '(?m)^\$ErrorActionPreference.*$', ''
            [void](Invoke-AsSystem 'rknpu-reinstall' $body)
            $cf = (Get-ItemProperty -Path $NpuKey -Name ConfigFlags).ConfigFlags
            if ($cf -band 0x20) { Ok ('ConfigFlags 0x{0:x8}: PnP binds the new package at the next restart' -f $cf) }
            else { Bad ('ConfigFlags is 0x{0:x8}; the SYSTEM task did not set 0x20 (see %SystemRoot%\Temp\rknpu-reinstall.log)' -f $cf) }
            $reboot = $true
        }
    }
    if ($npuStarted -and $admin -and -not $o.Driver) {
        # the counters the desktop app and `charsiu ps` read; registered from
        # the driver store folder the running rknpu.sys came from
        $have = $true
        try { Get-Counter -Counter '\RK3576 NPU(*)\% Busy Time' -ErrorAction Stop | Out-Null } catch { $have = $false }
        if (-not $have) {
            $pc = $null
            if (Test-Path -LiteralPath (Join-Path $Dir 'rknpu-perfcounters.ps1')) { $pc = Join-Path $Dir 'rknpu-perfcounters.ps1' }
            if ($o.DryRun) { Would 'register the "RK3576 NPU" performance counters (rknpu-perfcounters.ps1)' }
            elseif ($pc) { & powershell -NoProfile -ExecutionPolicy Bypass -File $pc | Out-Null; Ok 'performance counters registered' }
            else { Note 'the performance counters are not registered; this runs rknpu-perfcounters.ps1 once the package is in place (run the installer again)' }
        }
    }

    # ---- NPU access for ordinary users (the default) -----------------------
    if ($dev) {
        $dpKey = "$NpuKey\Device Parameters"
        $ua = 0
        try { $v = (Get-ItemProperty -Path $dpKey -ErrorAction Stop).UserAccess; if ($null -ne $v) { $ua = [int]$v } } catch { }
        $want = 1; if ($o.AdminOnly) { $want = 0 }
        Step 'who may use the NPU'
        if ($ua -eq $want) {
            if ($want) { Ok 'ordinary users may use the NPU (UserAccess = 1)' } else { Ok 'administrators only (UserAccess = 0)' }
        } elseif (-not $admin) {
            if ($want) {
                Note 'rknpu.sys lets only administrators use the NPU. Run this installer once from an'
                Note 'administrator PowerShell to let ordinary users use it (then restart), or run charsiu'
                Note 'from an administrator terminal. --admin-only keeps it as it is.'
            } else { Note 'UserAccess is 1; --admin-only needs an administrator PowerShell to change it' }
        } else {
            if ($want) {
                Note 'Any interactive user of this machine will be able to open \\.\RKNPU. The driver grants'
                Note 'it only in IOMMU mode, where every address a job can use is checked against that'
                Note "user's own buffers and the NPU reaches only buffers mapped for it; in bypass mode it"
                Note 'stays administrators only. All users share one NPU address space, so on a machine'
                Note "with several users one user's job could reach another's NPU buffers. --admin-only"
                Note 'keeps the NPU for administrators.'
            }
            $body = "if (-not (Test-Path '$dpKey')) { New-Item -Path '$dpKey' | Out-Null }`r`nNew-ItemProperty -Path '$dpKey' -Name UserAccess -Value $want -PropertyType DWord -Force | Out-Null"
            if ($o.DryRun) { Would "set UserAccess = $want in $dpKey, as SYSTEM (read when the device starts: takes a restart)" }
            else {
                $log = Invoke-AsSystem 'rknpu-useraccess' $body
                $now = -1
                try { $now = [int](Get-ItemProperty -Path $dpKey -ErrorAction Stop).UserAccess } catch { }
                if ($now -eq $want) { Ok "UserAccess = $want; it takes effect at the next restart"; $reboot = $true }
                else { Bad "UserAccess is still $ua ($log)" }
            }
        }
    }

    # ---- 3. the programs -------------------------------------------------
    Step "the programs, into $Dir"
    $running = @(Get-Process -Name 'charsiu_run', 'charsiu_serve' -ErrorAction SilentlyContinue)
    if ($running.Count) {
        Bad "charsiu is running ($(($running | ForEach-Object { "$($_.ProcessName) $($_.Id)" }) -join ', ')); its files cannot be replaced. Close it and run this again."
        if (-not $o.DryRun) { return }
    }
    $pkg = $o.Zip
    if ($o.DryRun) {
        if ($pkg) { Would "unpack $pkg" } else { Would "download $(ReleaseUrl $PkgName) and check it against $PkgName.sha256" }
        Would "copy charsiu.ps1, charsiu_run.exe, charsiu_serve.exe, charsiu_check.exe, zoo.txt and bin\charsiu.cmd into $Dir"
    } else {
        $tmp = Join-Path $env:TEMP ("charsiu-" + [guid]::NewGuid().ToString('N').Substring(0, 8))
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        if (-not $pkg -or $pkg -match '^https?://') {
            $url = $pkg; if (-not $url) { $url = ReleaseUrl $PkgName }
            $pkg = Join-Path $tmp $PkgName
            Write-Host "  fetching $url"
            Download $url $pkg
            try {
                Download "$url.sha256" "$pkg.sha256"
                $want = ((Get-Content "$pkg.sha256" -TotalCount 1) -split '\s+')[0].ToLowerInvariant()
                $got = (Get-FileHash -LiteralPath $pkg -Algorithm SHA256).Hash.ToLowerInvariant()
                if ($got -ne $want) { throw "sha256 of the package is $got, and $url.sha256 says $want" }
                Ok "package sha256 $($got.Substring(0, 12))..."
            } catch {
                if ($_.Exception.Message -like 'sha256 of the package*') { throw }
                Note "no $PkgName.sha256 beside it; the package is not checked"
            }
        }
        $x = Join-Path $tmp 'pkg'
        Expand-Archive -LiteralPath $pkg -DestinationPath $x -Force
        # the zip may hold the files at its top or inside one folder
        $top = $x
        if (-not (Test-Path (Join-Path $x 'charsiu.ps1'))) {
            $sub = Get-ChildItem -LiteralPath $x -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'charsiu.ps1') } | Select-Object -First 1
            if ($sub) { $top = $sub.FullName }
        }
        foreach ($f in 'charsiu.ps1', 'charsiu_run.exe', 'charsiu_serve.exe', 'charsiu_check.exe', 'zoo.txt', 'bin\charsiu.cmd') {
            if (-not (Test-Path (Join-Path $top $f))) { throw "the package has no $f" }
        }
        New-Item -ItemType Directory -Force -Path $Dir | Out-Null
        Copy-Item -Path (Join-Path $top '*') -Destination $Dir -Recurse -Force
        Get-ChildItem -LiteralPath $Dir -Recurse -File | Unblock-File -ErrorAction SilentlyContinue
        Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue
        $v = ''; if (Test-Path (Join-Path $Dir 'VERSION')) { $v = (Get-Content (Join-Path $Dir 'VERSION') -TotalCount 1).Trim() }
        Ok "charsiu $v in $Dir"
    }

    # ---- 4. PATH -----------------------------------------------------------
    $up = Get-UserPath
    $onPath = @($up -split ';' | Where-Object { $_.TrimEnd('\') -eq $BinDir.TrimEnd('\') }).Count -gt 0
    if ($onPath) { Ok "$BinDir is on your PATH" }
    elseif ($o.DryRun) { Would "add $BinDir to your user PATH" }
    else {
        $new = $BinDir; if ($up) { $new = $up.TrimEnd(';') + ';' + $BinDir }
        Set-UserPath $new
        Ok "added $BinDir to your user PATH (new terminals see it)"
    }
    if (-not $o.DryRun -and $env:Path -and -not (($env:Path -split ';') -contains $BinDir)) { $env:Path = $env:Path.TrimEnd(';') + ';' + $BinDir }

    if ($o.Update) {
        Step 'done'
        Write-Host '  charsiu was updated. Models and config were left as they were.'
        return
    }

    # ---- 5. config and a model -------------------------------------------
    Step 'a model'
    $ps = Join-Path $Dir 'charsiu.ps1'
    # its output goes to the screen, and only the exit code comes back
    $runCharsiu = {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $ps @args | Out-Host
        return $LASTEXITCODE
    }
    if (-not (Test-Path -LiteralPath $Conf)) {
        if ($o.DryRun) { Would "write $Conf from the template (weight cache on, [serve] context 2048)" }
        else {
            New-Item -ItemType Directory -Force -Path $CharsiuHome | Out-Null
            Copy-Item -LiteralPath (Join-Path $Dir 'config.ini') -Destination $Conf
            Ok "config $Conf"
        }
    } else { Ok "config $Conf (kept)" }

    $have = @()
    if (Test-Path -LiteralPath $Models) { $have = @(Get-ChildItem -LiteralPath $Models -Filter '*.gguf' -File | Where-Object { $_.Name -notlike 'mmproj-*' }) }
    if ($have.Count) {
        Ok "$($have.Count) model(s) in $Models"
    } elseif ($o.NoModel) {
        Note 'no model fetched (--no-model); charsiu pull lists them'
    } else {
        # The default: the 1B Llama every board measurement uses when the disk
        # has room for it and its weight cache, else the 0.6B Qwen.
        $free = -1
        try { $free = [int64]((New-Object IO.DriveInfo([IO.Path]::GetPathRoot($Models))).AvailableFreeSpace / 1MB) } catch { }
        $pick = $o.Model
        if (-not $pick) {
            if ($free -lt 0 -or $free -ge 2600) { $pick = 'llama3.2-1b-q8' } else { $pick = 'qwen3-0.6b-q4' }
        }
        Note "$free MB free for models; the model to fetch: $pick (another: --model NAME, the list: charsiu pull)"
        if ($free -ge 0 -and $free -lt 900) {
            Bad "$free MB is not enough for a model and its weight cache. Free some space, or set CHARSIU_MODELS to a larger drive, then run charsiu pull."
        } elseif ($o.DryRun) {
            Would "charsiu pull $pick   (sha256 against Hugging Face, then the gate)"
        } elseif ($o.Yes -or (Ask "Fetch $pick now?" $true)) {
            $rc = & $runCharsiu pull $pick
            if ($rc -ne 0) { Bad "charsiu pull $pick failed ($rc); running it again resumes the download" }
        }
    }

    # ---- 6. ask it something ---------------------------------------------
    if ($o.DryRun) {
        Would 'charsiu -p "The capital of France is" -n 32 -q   (one sentence, to see it work)'
    } elseif (-not $o.NoDemo -and -not $o.Yes) {
        $have = @()
        if (Test-Path -LiteralPath $Models) { $have = @(Get-ChildItem -LiteralPath $Models -Filter '*.gguf' -File | Where-Object { $_.Name -notlike 'mmproj-*' }) }
        if ($have.Count -and -not $reboot -and (Ask 'Ask it something, to see it work? (the first run stages the NPU weights, about two minutes; later runs take seconds)' $true)) {
            Step 'asking: the capital of France is'
            [void](& $runCharsiu -p 'The capital of France is' -n 32 -q)
        }
    }

    Step 'done'
    if ($o.DryRun) {
        Write-Host ''
        Write-Host 'DRY RUN. Nothing above was done. In order, it would have:' -ForegroundColor Cyan
        foreach ($p in $script:Planned) { Write-Host "  - $p" }
        return
    }
    @"

  charsiu             a conversation
  charsiu "..."       ask once and exit
  charsiu list        what is here
  charsiu pull        fetch another model
  charsiu doctor      what works and what does not
  charsiu serve       an OpenAI compatible endpoint on http://127.0.0.1:11434

  Open a NEW terminal for the charsiu command (PATH changed).

  programs  $Dir
  config    $Conf
  models    $Models
"@ | Write-Host
    if ($reboot) {
        Write-Host '  RESTART to finish: the driver change takes effect at the next boot.' -ForegroundColor Yellow
        Write-Host '  Then run this installer again to check it and register the counters.' -ForegroundColor Yellow
    }
}

Install-Charsiu @args
