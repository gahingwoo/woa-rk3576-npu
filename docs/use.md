# Using the NPU under Windows

How to turn the NPU on, install the driver and run a model with charsiu on
an LCKFB TaishanPi 3M under Windows 11 23H2. [简体中文](zh-CN/use.md)

New to these boards? Start at
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576#from-a-new-board-to-windows-or-linux).
Windows itself is installed with
[woa-rk3576](https://github.com/gahingwoo/woa-rk3576/blob/main/docs/INSTALL.md).

Run on one TaishanPi 3M, 2026-11-22 to 2026-11-24. The CM5-IO firmware does
not offer the NPU option. There is no release: the driver and the tools are
CI artifacts.

## What you need

- edk2-rk3576 firmware newer than 0.4.1 (its
  [`ci` build](https://edk2.gahingwoo.com/flash.html#the-rolling-ci-build)
  until the next release).
- From one run of this repository's CI: the `rknpu-m7-arm64` artifact (the
  driver, `rknputest.exe`, `signer.cer`, `rknpu-reinstall.ps1`) and the
  `charsiu-m7-arm64` artifact (`charsiu_run.exe` and the benchmark).
- A model. The one that has been run is Llama-3.2-1B-Instruct Q8_0 from
  unsloth (`Llama-3.2-1B-Instruct-Q8_0.gguf`, sha256 `3f87a880...`, about
  1.3 GB). The TaishanPi's C: has little room; a USB stick or a second
  partition works.

## Steps

1. Turn on "NPU (experimental)" in the firmware setup (ACPI / Device Tree
   page), save and restart. From the UEFI Shell the same is
   `setvar AcpiNpu -guid 8b4f2e3d-ac5e-4f9b-c2d6-3e7e9f1b4d8c -bs -rt -nv =01`
   followed by `reset`. The firmware then powers the NPU (rail 800 mV, clock
   594 MHz) and declares it as `ACPI\RKCP3576`.
2. In Windows, sync the clock: `w32tm /resync /force`.
3. Unpack both artifacts into one folder, for example `C:\rknpu`.
4. Trust the signer and stage the driver, from an administrator prompt:

   ```
   certutil -addstore -f Root C:\rknpu\signer.cer
   certutil -addstore -f TrustedPublisher C:\rknpu\signer.cer
   pnputil /add-driver C:\rknpu\rknpu\rknpu.inf
   ```

5. Mark the device for reinstall as SYSTEM, then restart (the general rule is
   in woa-rk3576's [drivers](https://github.com/gahingwoo/woa-rk3576/blob/main/docs/drivers.md)):

   ```
   schtasks /create /tn rknpu-reinstall /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\rknpu-reinstall.ps1"
   schtasks /run /tn rknpu-reinstall
   schtasks /delete /tn rknpu-reinstall /f
   shutdown /r /t 0
   ```

6. Check it:

   ```
   C:\rknpu\rknputest.exe info
   ```

   It ends with `RKNPU INFO: OK`.

7. Run a prompt on the NPU:

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

   These are the settings the benchmark uses. `--cache` keeps the quantised
   weights in a file beside the model: the first run writes it (about 90 s
   of staging), later runs read it (about 10 s).

## What to expect

Llama-3.2-1B decodes at about 14 tokens a second on the NPU with the M7
defaults (14.23 tok/s in the 2026-11-24 benchmark; 15.5 to 16.6 in its best
arms), against 18.15 on Linux on the same board. The text is the same as on
the CPU arm and as on Linux. Numbers are from single runs.

## Back out

Turn "NPU (experimental)" off in setup and restart: the NPU stays powered
off and Windows no longer sees the device.

## More

- How it works: [architecture](dev/architecture.md)
- The benchmark and the host checks: [testing](dev/testing.md)
- Where the time goes against Linux: [performance](dev/performance.md)
