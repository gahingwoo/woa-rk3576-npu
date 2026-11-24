# rknpu-reinstall.ps1 - mark ACPI\RKCP3576\0 for reinstall at the next boot
#
# Sets bit 0x20 (CONFIGFLAG_REINSTALL) in the device's ConfigFlags, so that
# after a restart PnP binds the device to the newest staged rknpu package.
# This replaces `pnputil /install` on a running device, which has bugchecked
# this board with other drivers. The Enum key is writable by SYSTEM only, so
# run it as SYSTEM (docs/use.md, step 5, does it through a scheduled task).
# It changes nothing else, and prints the value before and after.
#
# Copyright (c) 2026 Ga Hing Woo <gahing@gahingwoo.com>
# SPDX-License-Identifier: GPL-2.0-only

$ErrorActionPreference = 'Stop'
$key = 'HKLM:\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0'
$log = Join-Path $env:SystemRoot 'Temp\rknpu-reinstall.log'

$old = (Get-ItemProperty -Path $key -Name ConfigFlags).ConfigFlags
Set-ItemProperty -Path $key -Name ConfigFlags -Value ($old -bor 0x20) -Type DWord
$new = (Get-ItemProperty -Path $key -Name ConfigFlags).ConfigFlags
"ConfigFlags 0x{0:x8} -> 0x{1:x8}" -f $old, $new | Tee-Object -FilePath $log
