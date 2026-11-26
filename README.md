# woa-rk3576-npu

[简体中文](README.zh-CN.md)

The Rockchip RK3576 NPU under Windows on Arm: a KMDF driver (`rknpu.sys`)
and a port of the [charsiu](https://github.com/gahingwoo/charsiu) LLM
runtime, so models that run on the NPU under Linux run on it under Windows.

New to these boards? Start at
[edk2-rk3576](https://github.com/gahingwoo/edk2-rk3576#from-a-new-board-to-windows-or-linux):
the firmware and Windows come first.

## Status

| Feature | Status |
|---|---|
| Driver, both NPU cores, driver-managed IOMMU | board-tested (TaishanPi 3M) |
| charsiu on the NPU, `charsiu_run` | board-tested (TaishanPi 3M) |
| Performance counters (`RK3576 NPU`) | board-tested (TaishanPi 3M) |
| SUBMIT phase timing (`rknputest submit-stats`) | CI only |
| `charsiu` commands, `charsiu serve`, installer | CI only |
| ArmSoM CM5-IO | not tried: its firmware has no NPU option |

No release yet: the driver and charsiu are CI artifacts.

## Result

Llama-3.2-1B-Instruct Q8_0 on the TaishanPi 3M, Windows 11 23H2, 2026-11-26,
fan on: **16.12 tok/s** decode (62.0 ms a token) with the defaults, 89% of
Linux's 18.15 tok/s on the same board. Every arm of the benchmark printed
the same text. More numbers: [performance](docs/dev/performance.md).

![Device Manager with Neural processors, Rockchip RK3576 NPU working properly; an NPU Chat console running Llama-3.2-1B on the NPU; a monitor widget](docs/imgs/tspi3m-2026-11-24-npu-chat.webp)

*TaishanPi 3M, 2026-11-24: the NPU under "Neural processors", a chat with
Llama-3.2-1B on the NPU, and rk-monitor showing CPU, GPU and NPU load.*

## Docs

- [Use](docs/use.md): turn the NPU on, install the driver, run a model
- [charsiu on Windows](docs/charsiu.md): the commands, the installer, who
  may use the NPU
- Developers: [architecture and settings](docs/dev/architecture.md),
  [testing](docs/dev/testing.md), [performance](docs/dev/performance.md),
  [performance counters](docs/dev/perfcounters.md),
  [tools/m0](tools/m0/README.md) (the Linux measurement kit)
- [History](docs/history.md): the milestones M0 to M8

Layout: `drivers/rknpu` (the driver), `include/` (IOCTL interface, register
streams, allow list), `port/` (charsiu's device layer and compat shims),
`win/` (the `charsiu` front door and installer), `external/charsiu`
(submodule), `tools/` (rknputest, m3gen, the board scripts).

## License

GPL-2.0-only ([LICENSE](LICENSE)), the same as charsiu and the Linux driver
this follows.
