# woa-rk3576-npu

[English](README.md)

在 Windows on Arm 上使用 Rockchip RK3576 的 NPU：一个 KMDF 驱动，加上
[charsiu](https://github.com/gahingwoo/charsiu) 大模型运行时的移植，让在
Linux 上能用 NPU 跑的模型，在 Windows 上也能跑。

现状：规划阶段，还没有任何东西在硬件上跑过。可行性研究见
[docs/FEASIBILITY.md](docs/FEASIBILITY.md)（英文）。

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

| # | 内容 | 完成标准 |
|---|---|---|
| M0 | 在 Linux 上测量 | 关掉 IOMMU 分页后，一个 NPU 任务的结果和 CPU 完全一致 |
| M1 | 固件：上电、时钟、ACPI | 设备出现在 ACPI 里，能读到版本寄存器 |
| M2 | 驱动框架和测试工具 | 设备启动，没有错误代码 |
| M3 | 缓冲区、提交、中断 | GEMM 结果和 CPU 完全一致 |
| M4 | 移植 charsiu | Llama-3.2-1B 输出的文字和 Linux 上一样 |
| M5 | 性能 | 每秒 token 数和 Linux 的差距在给定范围内 |
| M6 | 驱动自己管理 NPU 的 IOMMU | 用分散的、4 GiB 以上的内存再跑一遍 M4 |
| M7 | 安装脚本、`charsiu serve` | 另一台机器能拿到回答 |

工作量估算和风险见研究报告。

## 开发板

立创泰山派 3M 和 ArmSoM CM5-IO，即
[woa-rk3576](https://github.com/gahingwoo/woa-rk3576) 支持的板子；
Windows 11 23H2，edk2-rk3576 固件。

## 许可证

GPL-2.0-only（[LICENSE](LICENSE)），和 charsiu 以及所参照的 Linux 驱动一致。
