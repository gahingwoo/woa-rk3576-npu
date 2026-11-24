# woa-rk3576-npu

[English](README.md)

在 Windows on Arm 上使用 Rockchip RK3576 的 NPU：一个 KMDF 驱动，加上
[charsiu](https://github.com/gahingwoo/charsiu) 大模型运行时的移植，让在
Linux 上能用 NPU 跑的模型，在 Windows 上也能跑。

现状：NPU 已经在泰山派 3M 的 Windows 下上电，驱动也能启动（M1 和 M2，2026 年 11 月 22 日）；还没有在它上面跑过任务。可行性研究见
[docs/FEASIBILITY.md](docs/FEASIBILITY.md)（英文）。在板子上的 Linux 下运行的 M0 测量工具在
[tools/m0](tools/m0/README.md)（英文）。M2 的驱动框架（[drivers/rknpu](drivers/rknpu)）、
测试工具（[tools/rknputest](tools/rknputest)）和 IOCTL 头文件
（[include/rknpu_ioctl.h](include/rknpu_ioctl.h)）已能在 CI 里编译；板上测试步骤见
[docs/M2.md](docs/M2.md)（英文）。

## 做法

- **驱动。** KMDF 驱动，任务模型和主线 Linux 的 `rocket` 驱动以及
  [linux-rk3576-npu](https://github.com/gahingwoo/linux-rk3576-npu)
  里的 RK3576 支持一致：分配缓冲区、缓存维护、提交任务、完成中断。
  框架沿用 [woa-rk3576](https://github.com/gahingwoo/woa-rk3576) 里
  VDPU383 测试驱动的做法：DMA 缓冲区放在 4 GiB 以下，NPU 的 IOMMU
  先用旁路模式，设备接口只给管理员，配一个测试工具。
- **固件。** 由 [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576)
  给 NPU 上电、设电压和时钟，并在 ACPI 里描述它。这些放在一个默认关闭的
  设置选项后面。
- **运行时。** charsiu 只通过一个文件（`src/device.c`）和内核打交道。
  移植时替换这个文件和少量只在 POSIX 下有的部分，用 clang 编译。
- **不打算做：** 给 DirectML 或 Windows ML 用的 MCDM（只做计算的 WDDM）
  驱动。RK3576 是 ARMv8.0，最高只能跑 Windows 11 23H2；在 23H2 上 MCDM
  需要一个能编译着色器的 D3D12 计算驱动，而这颗 NPU 是固定功能的。详见
  研究报告第 6 节。

## 里程碑

| # | 内容 | 完成标准 | 状态 |
|---|---|---|---|
| M0 | 在 Linux 上测量 | 关掉 IOMMU 分页后，一个 NPU 任务的结果和 CPU 完全一致 | 已在泰山派 3M 上完成（2026-11-22）：`m0_bypass` 3 次全部一致，哨兵数据没被改动，IOMMU 分页关闭（Armbian 7.3.5 edge，rocket） |
| M1 | 固件：上电、时钟、ACPI | 设备出现在 ACPI 里，能读到版本寄存器 | 已在泰山派 3M 上完成（2026-11-22）：电压 750 调到 800 mV，clk_rknn_dsu0 594 MHz，两个核都读到版本 0x46495245（edk2-rk3576 的 `npu-acpi` 分支，设置项默认关闭） |
| M2 | 驱动框架和测试工具 | 设备启动，没有错误代码 | 已在泰山派 3M 上完成（2026-11-22）：`ACPI\RKCP3576` 已启动，`rknputest info` 全部检查通过（[docs/M2.md](docs/M2.md)） |
| M3 | 缓冲区、提交、中断 | GEMM 结果和 CPU 完全一致 | 已在泰山派 3M 上完成（2026-11-22）：`rknputest m3` 两个核心都逐位一致，最大到 16x1024x1024（NPU 1.9 ms，CPU 24.7 ms），没有超时（[docs/M3.md](docs/M3.md)） |
| M4 | 移植 charsiu | Llama-3.2-1B 输出的文字和 Linux 上一样 | 代码已完成，尚未在板子上运行：charsiu 在 CI 里编译为 ARM64 Windows 程序，经 rknpu.sys 运行（[port](port)、[docs/M4.md](docs/M4.md)） |
| M5 | 性能 | 每秒 token 数和 Linux 的差距在给定范围内 | M6 在板子上的解码速度是 Linux 的 67%；M7 性能轮（分支 `m7-perf`，尚未在板子上运行）分析了差距的来源，并把一次解码调用的 IOCTL 从十个减到四个（[docs/M7.md](docs/M7.md)） |
| M6 | 驱动自己管理 NPU 的 IOMMU | 用分散的、4 GiB 以上的内存再跑一遍 M4 | 代码已完成（分支 `m6-iommu`），尚未在板子上运行（[docs/M6.md](docs/M6.md)） |
| M8 | 安装脚本、`charsiu serve` | 另一台机器能拿到回答 | 未开始 |

工作量估算和风险见研究报告。

## 查看 NPU 状态

Windows 11 23H2 的任务管理器没有 NPU 页面。rknpu.sys 改为提供 Windows
性能计数器：`RK3576 NPU`（每个核心：忙碌百分比、每秒作业数和任务数、平均作业时间、
提交到完成的延迟、超时、复位、IOMMU 错误、CBUF 等待）和 `RK3576 NPU Memory`，
可用 perfmon、`typeperf` 和 `Get-Counter "\RK3576 NPU(*)\*"` 查看
（[docs/PERFCOUNTERS.md](docs/PERFCOUNTERS.md)；分支 `npu-perfcounters`，尚未在板子上运行）。

## 开发板

立创泰山派 3M 和 ArmSoM CM5-IO，即
[woa-rk3576](https://github.com/gahingwoo/woa-rk3576) 支持的板子；
Windows 11 23H2，edk2-rk3576 固件。

## 许可证

GPL-2.0-only（[LICENSE](LICENSE)），和 charsiu 以及所参照的 Linux 驱动一致。
