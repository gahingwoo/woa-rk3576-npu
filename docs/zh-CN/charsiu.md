# Windows 上的 charsiu

和 Linux 上同一个 charsiu，命令也相同：`charsiu`（对话）、`charsiu run`、`charsiu pull`、`charsiu serve` 等。[English](../charsiu.md)

状态：已在 main 上，只经过 CI 检查（Windows 构建；前门脚本和安装脚本在 Linux 上用 pwsh 跑）。还没有在开发板上跑过。NPU 本身已经通过 `charsiu_run` 板测（[use.md](use.md)）。

## 安装

在 PowerShell 里（不需要管理员）：

```
irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1 | iex
```

带参数：

```
& ([scriptblock]::Create((irm https://raw.githubusercontent.com/gahingwoo/woa-rk3576-npu/main/win/charsiu-install.ps1))) --dry-run
```

还没有正式版，所以要给它 CI 产物：`--zip charsiu-windows-arm64.zip --driver rknpu-windows-arm64.zip`。

| 参数 | 作用 |
|---|---|
| `--dry-run` | 列出每个动作（`would ...`），不改任何东西；检查照常运行 |
| `--driver [ZIP 或文件夹]` | （管理员）暂存 rknpu.sys，并标记设备在下次重启时重装，即 [use.md](use.md) 第 4、5 步。绝不在运行中的设备上安装 |
| `--zip FILE` | 用这个 charsiu 包代替正式版的 |
| `--model NAME` | 要下载的模型；磁盘放得下它和权重缓存（2.6 GB）时默认 Llama-3.2-1B Q8_0，否则 Qwen3-0.6B |
| `--no-model` | 不下载模型 |
| `--admin-only` | 不让普通用户打开 NPU |
| `--uninstall`、`--purge` | 删除程序和 PATH 项；加 `--purge` 还删除 `%LOCALAPPDATA%\charsiu`（配置和模型） |

其余参数在 `win/charsiu-install.ps1` 开头。

## 位置

| | Windows | Linux |
|---|---|---|
| 程序 | `%LOCALAPPDATA%\Programs\charsiu` | `/usr/bin`、`/opt/charsiu` |
| 配置 | `%LOCALAPPDATA%\charsiu\config.ini` | `~/.charsiu/config.ini` |
| 模型 | `%LOCALAPPDATA%\charsiu\models` | `~/.charsiu/models` |
| 权重缓存 | 模型旁边；模型所在文件夹不可写时放在 `%LOCALAPPDATA%\charsiu\cache` | 模型旁边 |

`CHARSIU_HOME`、`CHARSIU_CONFIG`、`CHARSIU_MODELS` 可以改位置。配置项和 Linux 相同，两个默认值不同：`[run] cache = yes`（权重缓存；第一次写入约 130 秒，2026-11-24 实测，之后几秒）；`[run] cpus` 不设（移植层自己把解码线程绑到大核）。

## 命令

`charsiu --help` 列出全部。`charsiu pull` 从 Hugging Face 下载，核对 sha256，再用 `charsiu_check` 检查，跑不了就删掉。`charsiu serve` 监听 127.0.0.1:11434，提供 `/v1/chat/completions`、`/v1/messages`、`/v1/models`、`/health`。`charsiu doctor` 显示 NPU、驱动、权限和程序版本。

默认走 NPU：`charsiu`、`charsiu serve` 和桌面应用按配置的 `[npu]` 设好 NPU 变量。用不了 NPU 时先说明原因，再用 CPU。

## 谁能用 NPU

rknpu.sys 默认只允许管理员和 SYSTEM 打开 `\\.\RKNPU`。设了 `Device Parameters\UserAccess = 1`（安装脚本默认会设，除非 `--admin-only`）后，驱动在 IOMMU 模式下也允许交互式用户；旁路模式下，或者 IOMMU 没能启动时，仍然只限管理员。

多用户机器要注意：所有句柄共用一个 IOMMU 地址空间，一个用户的任务可能读写另一个用户的 NPU 缓冲区。多人共用的机器请用 `--admin-only`。

## 板测

步骤见英文页的 "Board test" 一节。
