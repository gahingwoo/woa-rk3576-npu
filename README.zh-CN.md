# woa-rk3576-npu

[English](README.md)（本页对应 2026-11-24 的英文版）

在 Windows on Arm 上使用 Rockchip RK3576 的 NPU：一个 KMDF 驱动，加上 [charsiu](https://github.com/gahingwoo/charsiu) 大模型运行时的移植，让在 Linux 上能用 NPU 跑的模型，在 Windows 上也能跑。

第一次接触这些板子？请从 [edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576/blob/main/README.zh-CN.md) 开始：先装好固件和 Windows。

## 状态（2026-11-24）

在立创泰山派 3M 的 Windows 11 23H2 上可用：Llama-3.2-1B 在 NPU 上的解码速度约每秒 14 个 token（默认设置 14.23，基准测试中最好的几组 15.5 到 16.6），同一块板子在 Linux 下是 18.15，输出文字和 CPU 上的完全一样。两个 NPU 核心同时工作，经过驱动自己管理的 IOMMU。没有在 ArmSoM CM5-IO 上试过，它的固件不提供 NPU 选项。还没有正式版：驱动和 charsiu 都是 CI 产物。

使用方法：[docs/zh-CN/use.md](docs/zh-CN/use.md)。

## 里程碑

| # | 内容 | 结果 |
|---|---|---|
| M0 | Linux 下的测量：关闭 IOMMU 分页跑一个 NPU 任务 | 通过，2026-11-22 |
| M1 | 固件：供电、时钟、ACPI | 通过，2026-11-22 |
| M2 | 驱动框架和测试工具 | 通过，2026-11-22 |
| M3 | 缓冲区、提交、中断：GEMM 和 CPU 结果完全一致 | 通过，2026-11-22 |
| M4 | 移植 charsiu：Llama-3.2-1B 输出和 Linux 一样 | 2026-11-24 运行；NPU 部分需要 M6 的内存管理 |
| M5 | 性能 | 由 M6 和 M7 完成 |
| M6 | 驱动管理 NPU 的 IOMMU、可缓存缓冲区、双核 | 通过，2026-11-24：12.12 tok/s |
| M7 | 和 Linux 对比的解码性能 | 2026-11-24 合并：默认设置 14.23 tok/s |
| M8 | 安装脚本、`charsiu serve` | 未开始 |

每个里程碑的计划和板上记录见[历史记录](docs/history/README.md)（英文）。

## 开发文档（英文）

[架构](docs/dev/architecture.md)、[测试](docs/dev/testing.md)、[性能](docs/dev/performance.md)、[tools/m0](tools/m0/README.md)。

## 许可证

GPL-2.0-only（[LICENSE](LICENSE)），与 charsiu 以及所参照的 Linux 驱动相同。
