# charsiu on Windows

The same charsiu as on Linux, with the same commands: `charsiu` for a
conversation, `charsiu run`, `charsiu pull`, `charsiu serve` and the rest.
[简体中文](zh-CN/charsiu.md)

Status: code-complete on branch `charsiu-windows`, checked in CI (the
Windows build, and the front door and installer under pwsh on Linux with
charsiu's own Linux programs standing in). Not run on a board yet.

## Install

From PowerShell (no administrator needed):

```
irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1 | iex
```

With options:

```
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1))) --dry-run
```

`--dry-run` prints every action as `would ...` and changes nothing; the
checks still run, so it shows what the installer sees on this machine. The
other options are at the top of `win/charsiu-install.ps1`.

What it does, in order:

1. Checks the machine (ARM64) and the NPU: whether `ACPI\RKCP3576` is there
   (it is only when "NPU (experimental)" is on in the firmware setup) and
   whether rknpu.sys is bound to it.
2. With `--driver` (administrator): stages rknpu.sys from the release's
   `rknpu-windows-arm64.zip`, or a zip or folder given after `--driver`,
   and marks the device for reinstall at the next restart. It never installs
   a driver on the running device: `pnputil /add-driver` only stages, and
   ConfigFlags 0x20 is set as SYSTEM through a one-shot scheduled task, the
   same steps as [use.md](use.md) 4 and 5.
3. From an administrator PowerShell, lets ordinary users open the NPU
   (below; `--admin-only` does not). Takes a restart.
4. Downloads `charsiu-windows-arm64.zip` from the latest release (or
   `--zip FILE`), checks it against its `.sha256`, and puts it in
   `%LOCALAPPDATA%\Programs\charsiu`.
5. Adds `%LOCALAPPDATA%\Programs\charsiu\bin` to the user PATH.
6. Writes `%LOCALAPPDATA%\charsiu\config.ini` and fetches a model with
   `charsiu pull`: Llama-3.2-1B Q8_0 when the disk has room for it and its
   weight cache (2.6 GB), Qwen3-0.6B otherwise, `--model NAME` for another.
7. Asks it one sentence, so you see it work.

`charsiu update` runs the installer again for the programs only;
`--uninstall` removes them (models stay; `--purge` removes those too).

There is no release yet. Until there is, give the installer the CI
artifact: `--zip charsiu-windows-arm64.zip` and `--driver rknpu-windows-arm64.zip`.

## Where things live

| | Windows | Linux |
|---|---|---|
| programs | `%LOCALAPPDATA%\Programs\charsiu` | `/usr/bin`, `/opt/charsiu` |
| config | `%LOCALAPPDATA%\charsiu\config.ini` | `~/.charsiu/config.ini` |
| models | `%LOCALAPPDATA%\charsiu\models` | `~/.charsiu/models` |

`CHARSIU_HOME`, `CHARSIU_CONFIG` and `CHARSIU_MODELS` move them on both.
The config has the same keys. Two defaults differ, and the template says
why: the weight cache is on (`[run] cache = yes`; the first run writes it,
about 130 s on the board, and later runs read it in seconds) and `[run] cpus` is unset (the port pins the decode
thread to the fast cores itself).

## The commands

`charsiu --help` lists them. As on Linux, `charsiu pull` fetches from
Hugging Face, checks the file's sha256 against the one Hugging Face
publishes, then gates it with `charsiu_check` (the runtime's own parser) and
deletes it if it would not run; an interrupted download resumes. The list of
models is charsiu's own (`scripts/charsiu-get`), copied into the package at
build time.

`charsiu serve` is charsiu_serve over Winsock: `POST /v1/chat/completions`
(streaming with `stream: true`), `POST /v1/messages`, `GET /v1/models`,
`GET /health`, one request at a time, the model staged once. It listens on
127.0.0.1:11434; `--host 0.0.0.0` offers it to the network and `--port 0`
lets Windows choose a port, which the ready line names. A client that
disconnects stops the answer within a token or two.

What is not the same: there is no full-screen setup (the guided setup is a
numbered list), and the staging progress bar is charsiu's own heartbeat
lines.

## The NPU is the default

`charsiu_run.exe` on its own runs on the CPU unless seven `CHARSIU_*`
variables are set (about 3 tok/s instead of 13 to 14). The front door sets
them from `[npu]` in the config, as the Linux one does, so `charsiu`,
`charsiu serve` and the desktop app use the NPU without anybody knowing the
names; `charsiu --show` prints them. When the NPU cannot be used it says why
before the run (no device: the firmware setting; no driver: `--driver`; no
access: the installer once from an administrator PowerShell, or an
administrator terminal) and charsiu then runs
on the CPU.

## Who may use the NPU

The device's security is stated in rknpu.inf (`Security` on the device,
which PnP applies to the whole device stack, and the same SDDL in the
driver): SYSTEM, administrators and interactive users may ask to open
`\\.\RKNPU`, and the driver's create callback decides. Before this the
INF set none, and when the device moved to the ComputeAccelerator class an
elevated administrator got ERROR_ACCESS_DENIED while SYSTEM still opened
it. `charsiu doctor` prints the security the device has.

rknpu.sys creates `\\.\RKNPU` for administrators and SYSTEM only, so by
default charsiu reaches the NPU only from an administrator terminal; from an
ordinary one it says so and runs on the CPU. The installer, run from an
administrator PowerShell, sets `Device Parameters\UserAccess = 1` on the
device by default (`--admin-only` leaves it at 0, the driver's own default);
the driver then also
admits interactive users, which is what the desktop app needs and what
Linux does (the accel node belongs to the video group).

The driver grants it only in IOMMU mode. There every address a job can
program is one of five registers, each checked against the caller's own
buffers, and the IOMMU confines the NPU to the buffers mapped for it. In
bypass mode (`M6Iommu = 0`, or `M4AllowUnlisted = 1`) a job's unchecked
sizes reach physical memory, so the device stays administrators only, and
if the IOMMU could not be set up when the device started, opening it is
refused to anyone who is not an administrator.

**The caveat, for a machine with more than one user:** the IOMMU is one
address space for all handles, so one user's job could read or overwrite
another user's NPU buffers (their model weights, activations and text while
a run is in progress). On a one-user machine, which is what this board is,
that is the same user. Use `--admin-only` on a shared machine.

## Board test

On the TaishanPi 3M, Windows 11 23H2, with "NPU (experimental)" on. From the
CI run of this branch, the `charsiu-windows-arm64` and `rknpu-windows-arm64`
artifacts, unpacked once each so that `charsiu-windows-arm64.zip` and
`rknpu-windows-arm64.zip` are in, say, `R:\ci`.

1. Dry run, ordinary PowerShell:
   `powershell -NoProfile -ExecutionPolicy Bypass -File R:\ci\charsiu-install.ps1 --dry-run --zip R:\ci\charsiu-windows-arm64.zip --driver R:\ci\rknpu-windows-arm64.zip`
   (extract `charsiu-install.ps1` from the zip first). Every step says
   `would`; nothing under `%LOCALAPPDATA%` changes.
2. The driver with user access, administrator PowerShell:
   `... -File R:\ci\charsiu-install.ps1 --zip R:\ci\charsiu-windows-arm64.zip --driver R:\ci\rknpu-windows-arm64.zip --no-model`
   Expect `rknpu.inf staged`, `ConfigFlags 0x...: PnP binds the new package
   at the next restart`, `UserAccess = 1`. Restart.
3. After the restart, an administrator PowerShell: run the same installer
   with `--no-model` once more; it registers the counters. `rknputest info`
   ends with `RKNPU INFO: OK`.
4. An ORDINARY PowerShell, new window: `charsiu doctor`. Expect `npu driver
   started`, `npu access UserAccess = 1`, the three programs with one
   version.
5. `charsiu pull llama3.2-1b-q8` (or `qwen3-0.6b-q4` if C: is short; the
   model directory can be moved with `CHARSIU_MODELS`). Expect `sha256
   matches` and `VERDICT  charsiu can run this file.`
6. `charsiu "Name three colors."`: the first run writes the weight cache
   (about 130 s of staging measured with charsiu_run on 2026-11-24), the
   answer follows. Again: staging a few seconds. `charsiu bench`: about 14 tok/s on Llama-3.2-1B (`charsiu ps` in a
   second window meanwhile shows both cores busy).
7. `charsiu` alone: a conversation; type in Chinese too (the console is set
   to UTF-8 both ways). Ctrl+C ends it.
8. `charsiu serve`, and from a second window:
   `curl.exe -N http://127.0.0.1:11434/v1/chat/completions -d "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":true}"`.
   Then the desktop app (charsiu-desktop) against it.
9. With `--admin-only` (administrator PowerShell, then restart; or a machine
   without it): `charsiu` from an ordinary terminal warns that the NPU is
   administrators only and runs on the CPU.

Report: the doctor output, the bench line, the staging lines of the first
and second run, and anything a step printed that this page does not say.
