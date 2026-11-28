# 在 Windows 下使用 NPU

在 Windows 11 23H2 上打开 NPU、安装驱动，并用 charsiu 跑模型。这些步骤是在立创泰山派 3M 上跑的；在 Radxa ROCK 4D 上驱动也能同样绑定（下面的速度是泰山派的）。[English](../use.md)

Windows 本身用 [woa-rk3576](https://github.com/gahingwoo/woa-rk3576/blob/main/docs/INSTALL.zh-CN.md) 安装。`charsiu` 安装脚本（[charsiu.md](charsiu.md)）可以代做第 3 到 6 步，但只在 CI 里跑过；下面的步骤是在板子上实际跑过的。

## 需要准备

- 0.4.1 之后的 edk2-rk3576 固件（下一个正式版发布前用它的 [`ci` 构建](https://edk2.gahingwoo.com/zh-CN/flash.html#滚动的-ci-构建)）。
- 本仓库同一次 CI 运行的两个产物：`rknpu-m7-arm64`（驱动、`rknputest.exe`、`signer.cer`、`rknpu-reinstall.ps1`、`rknpu-perfcounters.ps1`）和 `charsiu-m7-arm64`（`charsiu_run.exe` 和基准测试脚本）。
- 一个模型。跑过的是 unsloth 的 `Llama-3.2-1B-Instruct-Q8_0.gguf`（sha256 `3f87a880...`，约 1.3 GB）。C 盘空间不多，可以放在 U 盘或另一个分区上。
- SoC 上装风扇（见"已知问题"）。

## 步骤

1. 在固件设置的 ACPI / Device Tree 页打开 "NPU (experimental)"，保存并重启。在 UEFI Shell 里：`setvar AcpiNpu -guid 8b4f2e3d-ac5e-4f9b-c2d6-3e7e9f1b4d8c -bs -rt -nv =01`，然后 `reset`。
2. 在 Windows 里：`w32tm /resync /force`。
3. 把两个产物解压到同一个文件夹，比如 `C:\rknpu`。
4. 用管理员身份信任签名证书，并暂存驱动：

   ```
   certutil -addstore -f Root C:\rknpu\signer.cer
   certutil -addstore -f TrustedPublisher C:\rknpu\signer.cer
   pnputil /add-driver C:\rknpu\rknpu\rknpu.inf
   ```

5. 以 SYSTEM 身份把设备标记为需要重装，然后重启：

   ```
   schtasks /create /tn rknpu-reinstall /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\rknpu-reinstall.ps1"
   schtasks /run /tn rknpu-reinstall
   schtasks /delete /tn rknpu-reinstall /f
   shutdown /r /t 0
   ```

6. 检查：`C:\rknpu\rknputest.exe info` 最后一行是 `RKNPU INFO: OK`，设备管理器在 *Neural processors* 下列出 "Rockchip RK3576 NPU"。
7. 在管理员命令提示符里，在 NPU 上跑一段提示词：

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

   `--cache` 把量化后的权重存在模型旁边的一个文件里：第一次运行时写入（准备约 100 秒），之后读取（约 2 秒，刚重启后约 14 秒）。

## 查看 NPU 负载

23H2 的任务管理器没有 NPU 页面，驱动改为提供性能计数器。每次装完驱动后用管理员身份注册一次：

```
powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\rknpu-perfcounters.ps1
Get-Counter "\RK3576 NPU(*)\% Busy Time", "\RK3576 NPU(*)\Jobs/sec" -Continuous
```

woa-rk3576 的 `rk3576-tools-arm64` 工具包会自动注册，其中的 rk-monitor 在托盘里显示 NPU、GPU、CPU 负载。详见[性能计数器](../dev/perfcounters.md)（英文）。

## 预期结果

默认设置，2026-11-26，风扇开启：解码 16.12 tok/s，是同一块板子在 Linux 下 18.15 tok/s 的 89%。输出文字和 CPU 以及 Linux 上的一样。更多见[性能](../dev/performance.md)（英文）。

## 设置

驱动的设置在 `HKLM\SYSTEM\CurrentControlSet\Enum\ACPI\RKCP3576\0\Device Parameters`（只有 SYSTEM 能写，改完后重启设备）。charsiu 的设置是环境变量。完整的表见[架构](../dev/architecture.md#settings)（英文）。

## 已知问题

| 现象 | 原因和处理 |
|---|---|
| 约 3 tok/s | 没有设第 7 步的七个 `CHARSIU_*` 变量，`charsiu_run.exe` 就在 CPU 上跑。`charsiu` 命令会自动设置 |
| 12.91 tok/s 而不是 16 | 没装风扇，SoC 降频（2026-11-25 那次没开风扇） |
| 普通命令提示符打不开 `\\.\RKNPU` | 默认只有管理员能用，除非设了 `UserAccess = 1`（[charsiu.md](charsiu.md#谁能用-npu)） |
| 没有 `ACPI\RKCP3576` 设备 | 设置里的 "NPU (experimental)" 没打开，它默认关闭 |
| ArmSoM CM5-IO | 它的固件没有 NPU 选项（ROCK 4D 的固件有，在 edk2-rk3576 `main` 上） |

## 撤销

在设置里关闭 "NPU (experimental)" 并重启：NPU 保持断电，Windows 也不再看到这个设备。
