# charsiu on Windows

The same charsiu as on Linux, with the same commands: `charsiu` for a
conversation, `charsiu run`, `charsiu pull`, `charsiu serve` and the rest.
[简体中文](zh-CN/charsiu.md)

Status: on main, checked in CI only (the Windows build, and the front door
and installer under pwsh on Linux). Not run on a board yet. The NPU itself
is board-tested through `charsiu_run` ([use.md](use.md)).

## Install

From PowerShell, no administrator needed:

```
irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1 | iex
```

With options:

```
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1))) --dry-run
```

There is no release yet, so give it the CI artifacts:
`--zip charsiu-windows-arm64.zip --driver rknpu-windows-arm64.zip`.

| Option | What it does |
|---|---|
| `--dry-run` | prints every action as `would ...`, changes nothing; the checks still run |
| `--driver [ZIP or folder]` | (administrator) stages rknpu.sys and marks the device for reinstall at the next restart, as [use.md](use.md) steps 4 and 5. Never installs on the running device |
| `--zip FILE` | the charsiu package to install instead of the release's |
| `--model NAME` | the model to pull; default Llama-3.2-1B Q8_0 when the disk has room for it and its weight cache (2.6 GB), else Qwen3-0.6B |
| `--no-model` | pull no model |
| `--admin-only` | do not let ordinary users open the NPU |
| `--uninstall`, `--purge` | remove the programs and the PATH entry; with `--purge` also `%LOCALAPPDATA%\charsiu` (config and models) |

The rest are at the top of `win/charsiu-install.ps1`. In order, it checks the
machine and the NPU (`ACPI\RKCP3576` present, rknpu.sys bound), optionally
stages the driver, sets `UserAccess` when run from an administrator
PowerShell (takes a restart), installs the package checked against its
`.sha256`, adds `bin` to the user PATH, writes the config, pulls a model and
asks it one sentence. `charsiu update` reruns it for the programs only.

## Where things live

| | Windows | Linux |
|---|---|---|
| programs | `%LOCALAPPDATA%\Programs\charsiu` | `/usr/bin`, `/opt/charsiu` |
| config | `%LOCALAPPDATA%\charsiu\config.ini` | `~/.charsiu/config.ini` |
| models | `%LOCALAPPDATA%\charsiu\models` | `~/.charsiu/models` |

`CHARSIU_HOME`, `CHARSIU_CONFIG` and `CHARSIU_MODELS` move them. The config
keys are the same as on Linux. Two defaults differ: `[run] cache = yes` (the
weight cache; the first run writes it, about 130 s measured on 2026-11-24,
later runs read it in seconds) and `[run] cpus` is unset (the port pins the
decode thread itself).

## The commands

`charsiu --help` lists them.

- `charsiu pull` fetches from Hugging Face, checks the sha256 Hugging Face
  publishes, then gates the file with `charsiu_check` and deletes it if it
  would not run. An interrupted download resumes.
- `charsiu serve`: `POST /v1/chat/completions` (streaming with
  `stream: true`), `POST /v1/messages`, `GET /v1/models`, `GET /health`, one
  request at a time. Listens on 127.0.0.1:11434; `--host 0.0.0.0` for the
  network, `--port 0` lets Windows pick (the ready line names it).
- `charsiu doctor` prints the NPU, driver, access and program versions.
- `charsiu --show` prints the `CHARSIU_*` variables it would set.

The NPU is the default: `charsiu`, `charsiu serve` and the desktop app set
the NPU variables from `[npu]` in the config. When the NPU cannot be used,
charsiu says why (no device: the firmware option; no driver: `--driver`; no
access: see below) and runs on the CPU.

Not the same as Linux: no full-screen setup (a numbered list instead), and
staging progress is heartbeat lines.

## Who may use the NPU

By default rknpu.sys lets only administrators and SYSTEM open `\\.\RKNPU`,
so charsiu uses the NPU only from an administrator terminal. With
`Device Parameters\UserAccess = 1` (the installer sets it unless
`--admin-only`) the driver also admits interactive users, which the desktop
app needs.

The driver grants this only in IOMMU mode, where every address a job can
program is checked against the caller's own buffers and the IOMMU confines
the NPU to mapped buffers. In bypass mode (`M6Iommu = 0` or
`M4AllowUnlisted = 1`), or if the IOMMU failed to start, the device stays
administrators only.

**Shared machines:** the IOMMU is one address space for all handles, so one
user's job could read or overwrite another user's NPU buffers while a run is
in progress. Use `--admin-only` on a machine with more than one user.

## Board test

TaishanPi 3M, Windows 11 23H2, "NPU (experimental)" on, the
`charsiu-windows-arm64.zip` and `rknpu-windows-arm64.zip` from one CI run in
`R:\ci`, and `charsiu-install.ps1` extracted from the first.

1. Ordinary PowerShell: `powershell -NoProfile -ExecutionPolicy Bypass -File R:\ci\charsiu-install.ps1 --dry-run --zip R:\ci\charsiu-windows-arm64.zip --driver R:\ci\rknpu-windows-arm64.zip`.
   Every step says `would`.
2. Administrator PowerShell: the same without `--dry-run`, with
   `--no-model`. Expect `rknpu.inf staged`, `ConfigFlags 0x...`,
   `UserAccess = 1`. Restart.
3. Administrator PowerShell: the same once more (registers the counters).
   `rknputest info` ends with `RKNPU INFO: OK`.
4. Ordinary PowerShell, new window: `charsiu doctor` shows `npu driver
   started`, `npu access UserAccess = 1`, the three programs at one version.
5. `charsiu pull llama3.2-1b-q8` (or `qwen3-0.6b-q4`): `sha256 matches` and
   `VERDICT  charsiu can run this file.`
6. `charsiu "Name three colors."` twice (the first writes the weight cache),
   then `charsiu bench` (`charsiu ps` in a second window shows both cores
   busy).
7. `charsiu` alone, including Chinese input; Ctrl+C ends it.
8. `charsiu serve`, then from a second window:
   `curl.exe -N http://127.0.0.1:11434/v1/chat/completions -d "{\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}],\"stream\":true}"`.
9. With `--admin-only`: `charsiu` from an ordinary terminal warns and runs on
   the CPU.

Report the doctor output, the bench line, the staging lines of the first and
second run, and anything unexpected.
