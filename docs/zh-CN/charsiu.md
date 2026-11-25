# Windows 上的 charsiu

和 Linux 上同一个 charsiu，命令也相同：`charsiu`（对话）、`charsiu run`、`charsiu pull`、`charsiu serve` 等。[English](../charsiu.md)

状态：在 `charsiu-windows` 分支上代码已完成，CI 检查通过（Windows 构建；前门脚本和安装脚本在 Linux 上用 pwsh 跑，charsiu 的 Linux 程序代替 Windows 程序）。还没有在开发板上跑过。

## 安装

在 PowerShell 里（不需要管理员）：

```
irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1 | iex
```

带参数：

```
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1))) --dry-run
```

`--dry-run` 只列出会做什么，不改任何东西。`--driver`（管理员）只 stage rknpu.sys 并在 SYSTEM 下设 ConfigFlags 0x20，重启后才绑定，绝不在运行中的设备上安装驱动。在管理员 PowerShell 里运行时，默认让普通用户也能用 NPU（`--admin-only` 不这样做），重启生效。

## 位置

| | Windows | Linux |
|---|---|---|
| 程序 | `%LOCALAPPDATA%\Programs\charsiu` | `/usr/bin`、`/opt/charsiu` |
| 配置 | `%LOCALAPPDATA%\charsiu\config.ini` | `~/.charsiu/config.ini` |
| 模型 | `%LOCALAPPDATA%\charsiu\models` | `~/.charsiu/models` |

Windows 上权重缓存默认开启（第一次约 130 秒写缓存，之后几秒）。`charsiu` 默认按配置走 NPU，用不了时先说明原因再用 CPU。

## 谁能用 NPU

rknpu.sys 默认只允许管理员和 SYSTEM 打开 `\\.\RKNPU`。设了 `Device Parameters\UserAccess = 1` 后，驱动在 IOMMU 模式下也允许交互式用户；旁路模式下仍然只限管理员。

多用户机器要注意：所有句柄共用一个 IOMMU 地址空间，一个用户的任务可能读写另一个用户的 NPU 缓冲区。这块板子是单用户的，没有影响；多人共用的机器请用 `--admin-only`。

板测步骤见英文页的 "Board test" 一节。
