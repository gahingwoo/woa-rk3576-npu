# 在 Windows 下使用 NPU

怎样在立创泰山派 3M 的 Windows 11 23H2 上打开 NPU、安装驱动，并用 charsiu 跑模型。
[English](../use.md)（本页对应 2026-11-24 的英文版）

第一次接触这些板子？请从 [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/blob/main/README.zh-CN.md) 开始。Windows 本身用
[woa-rk3576](https://github.com/gahingwoo/woa-rk3576/blob/main/docs/zh-CN/install.md) 安装。

2026-11-22 到 2026-11-24 在一块泰山派 3M 上跑过。CM5-IO 的固件不提供 NPU 选项。还没有正式版：驱动和工具都是 CI 产物。

## 需要准备

- 0.4.1 之后的 edk2-rk3576 固件（下一个正式版发布前用它的 [`ci` 构建](https://edk2.gahingwoo.com/zh-CN/flash.html#滚动的-ci-构建)）。
- 本仓库同一次 CI 运行的两个产物：`rknpu-m7-arm64`（驱动、`rknputest.exe`、`signer.cer`、`rknpu-reinstall.ps1`）和 `charsiu-m7-arm64`（`charsiu_run.exe` 和基准测试脚本）。
- 一个模型。跑过的是 unsloth 的 Llama-3.2-1B-Instruct Q8_0（`Llama-3.2-1B-Instruct-Q8_0.gguf`，sha256 `3f87a880...`，约 1.3 GB）。泰山派的 C 盘空间不多，可以放在 U 盘或另一个分区上。

## 步骤

1. 在固件设置（ACPI / Device Tree 页）里打开 "NPU (experimental)"，保存并重启。在 UEFI Shell 里也可以用
   `setvar AcpiNpu -guid 8b4f2e3d-ac5e-4f9b-c2d6-3e7e9f1b4d8c -bs -rt -nv =01`，然后 `reset`。固件随后给 NPU 上电（电压 800 mV，时钟 594 MHz），并把它声明为 `ACPI\RKCP3576`。
2. 在 Windows 里同步时间：`w32tm /resync /force`。
3. 把两个产物解压到同一个文件夹，比如 `C:\rknpu`。
4. 用管理员命令提示符信任签名证书，并暂存驱动：

   ```
   certutil -addstore -f Root C:\rknpu\signer.cer
   certutil -addstore -f TrustedPublisher C:\rknpu\signer.cer
   pnputil /add-driver C:\rknpu\rknpu\rknpu.inf
   ```

5. 以 SYSTEM 身份把设备标记为需要重装，然后重启（一般规则见 woa-rk3576 的[驱动](https://github.com/gahingwoo/woa-rk3576/blob/main/docs/zh-CN/drivers.md)）：

   ```
   schtasks /create /tn rknpu-reinstall /sc once /st 23:59 /ru SYSTEM /f /tr "powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\rknpu-reinstall.ps1"
   schtasks /run /tn rknpu-reinstall
   schtasks /delete /tn rknpu-reinstall /f
   shutdown /r /t 0
   ```

6. 检查：

   ```
   C:\rknpu\rknputest.exe info
   ```

   最后一行是 `RKNPU INFO: OK`。

7. 在 NPU 上跑一段提示词：

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

   这些是基准测试用的设置。`--cache` 会把量化后的权重存在模型旁边的一个文件里：第一次运行时写入（准备约 90 秒），之后读取（约 10 秒）。

## 查看 NPU 的负载

Windows 11 23H2 的任务管理器没有 NPU 页面。驱动改为提供 Windows 性能计数器：`RK3576 NPU`，每个核心一个实例（`core0`、`core1`），有 `% Busy Time`、`Jobs/sec` 和平均作业时间；`RK3576 NPU Memory` 是 NPU 缓冲区占用的字节数。每次装完驱动后，在管理员命令行里运行一次，让它们可见：

```
powershell -NoProfile -ExecutionPolicy Bypass -File C:\rknpu\rknpu-perfcounters.ps1
```

之后可以在 perfmon 里看，或者：

```
Get-Counter "\RK3576 NPU(*)\% Busy Time", "\RK3576 NPU(*)\Jobs/sec" -Continuous
```

woa-rk3576 的 RK3576 工具包（CI 产物 `rk3576-tools-arm64`）会在每次驱动更新后自动完成这一步，其中的 rk-monitor 在托盘和桌面上显示 NPU、GPU、CPU 的负载和温度。全部计数器及其测量方式（英文）：[性能计数器](../dev/perfcounters.md)。

## 预期结果

使用 M7 的默认设置，Llama-3.2-1B 在 NPU 上解码约每秒 14 个 token（2026-11-24 基准测试 14.23 tok/s；最好的几组 15.5 到 16.6），同一块板子在 Linux 下是 18.15。输出文字和 CPU 以及 Linux 上的一样。数据都是单次运行的结果。

## 撤销

在设置里关闭 "NPU (experimental)" 并重启：NPU 保持断电，Windows 也不再看到这个设备。

## 更多（英文）

- 工作原理：[架构](../dev/architecture.md)
- 基准测试和主机端检查：[测试](../dev/testing.md)
- 和 Linux 的时间差在哪里：[性能](../dev/performance.md)
- NPU 的性能计数器：[perfcounters](../dev/perfcounters.md)
