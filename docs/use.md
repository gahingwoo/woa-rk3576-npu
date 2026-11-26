# Using the NPU under Windows

Turn the NPU on, install the driver and run a model with charsiu under
Windows 11 23H2. The steps were run on an LCKFB TaishanPi 3M; on a Radxa
ROCK 4D the driver binds the same way (the speeds below are the
TaishanPi's). [简体中文](zh-CN/use.md)

Windows itself is installed with
[woa-rk3576](https://github.com/gahingwoo/woa-rk3576/blob/main/docs/INSTALL.md).
The `charsiu` installer ([charsiu.md](charsiu.md)) does steps 3 to 6 for you,
but has only run in CI; the steps below are the ones run on the board.

## What you need

- edk2-rk3576 firmware newer than 0.4.1 (its
  [`ci` build](https://edk2.gahingwoo.com/flash.html#the-rolling-ci-build)
  until the next release).
- From one CI run of this repository: the `rknpu-m7-arm64` artifact (driver,
  `rknputest.exe`, `signer.cer`, `rknpu-reinstall.ps1`,
  `rknpu-perfcounters.ps1`) and `charsiu-m7-arm64` (`charsiu_run.exe` and
  the benchmark).
- A model. The one that has been run is unsloth's
  `Llama-3.2-1B-Instruct-Q8_0.gguf` (sha256 `3f87a880...`, about 1.3 GB).
  C: has little room; a USB stick or a second partition works.
- A fan on the SoC (see Known issues).

## Steps

1. In firmware setup, ACPI / Device Tree page, turn on "NPU (experimental)",
   save and restart. From the UEFI Shell:
   `setvar AcpiNpu -guid 8b4f2e3d-ac5e-4f9b-c2d6-3e7e9f1b4d8c -bs -rt -nv =01`,
   then `reset`.
2. In Windows: `w32tm /resync /force`.
3. Unpack both artifacts into one folder, for example `C:\rknpu`.
4. As administrator, trust the signer and stage the driver:

   ```
   certutil -addstore -f Root C:\rknpu\signer.cer
   certutil -addstore -f TrustedPublisher C:\rknpu\signer.cer
   pnputil /add-driver C:\rknpu\rknpu\rknpu.inf
   ```

5. Mark the device for reinstall as SYSTEM, then restart:

   ```
   schtasks /create /tn rknpu-reinstall /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\rknpu-reinstall.ps1"
   schtasks /run /tn rknpu-reinstall
   schtasks /delete /tn rknpu-reinstall /f
   shutdown /r /t 0
   ```

6. Check: `C:\rknpu\rknputest.exe info` ends with `RKNPU INFO: OK`, and
   Device Manager lists "Rockchip RK3576 NPU" under *Neural processors*.
7. Run a prompt on the NPU, from an administrator prompt:

   ```
   set CHARSIU_NPU=1
   set CHARSIU_NPU_QUANT=1
   set CHARSIU_NPU_W4V=1
   set CHARSIU_NPU_KMAX=1024
   set CHARSIU_NPU_W4_GROUP=1024
   set CHARSIU_NPU_MAXN=262144
   set CHARSIU_COEF_ELEMS=65536
   C:\rknpu\charsiu_run.exe R:\npu\Llama-3.2-1B-Instruct-Q8_0.gguf -p "Hello" -n 128 -c 512 -t 4 --cache
   ```

   `--cache` keeps the quantised weights in a file beside the model: the
   first run writes it (about 100 s of staging), later runs read it (13 to
   21 s).

## Watch the NPU

Task Manager on 23H2 has no NPU page. The driver publishes performance
counters instead. Register them once after each driver install, as
administrator:

```
powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\rknpu-perfcounters.ps1
Get-Counter "\RK3576 NPU(*)\% Busy Time", "\RK3576 NPU(*)\Jobs/sec" -Continuous
```

The `rk3576-tools-arm64` bundle from woa-rk3576 registers them for you, and
its rk-monitor shows NPU, GPU and CPU load in the tray. Details:
[perfcounters](dev/perfcounters.md).

## What to expect

With the defaults, 2026-11-26, fan on: 16.12 tok/s decode, 89% of Linux's
18.15 tok/s on the same board. The text is the same as on the CPU and on
Linux. More: [performance](dev/performance.md).

## Settings

Driver values live under
`HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0\Device Parameters`
(writable by SYSTEM only; restart the device after a change). charsiu's are
environment variables. The table is in
[architecture](dev/architecture.md#settings).

## Known issues

| Symptom | Cause, and what to do |
|---|---|
| about 3 tok/s | `charsiu_run.exe` runs on the CPU unless the seven `CHARSIU_*` variables in step 7 are set. The `charsiu` command sets them for you |
| 12.91 tok/s, not 16 | no fan: the SoC throttles (2026-11-25 run, fan off) |
| `\\.\RKNPU` refused from an ordinary prompt | the device is administrators only unless `UserAccess = 1` is set ([charsiu.md](charsiu.md#who-may-use-the-npu)) |
| no `ACPI\RKCP3576` device | "NPU (experimental)" is off in setup; it is off by default |
| ArmSoM CM5-IO | its firmware has no NPU option (the ROCK 4D's has, on edk2-rk3576 `main`) |

## Back out

Turn "NPU (experimental)" off in setup and restart: the NPU stays powered
off and Windows no longer sees the device.
