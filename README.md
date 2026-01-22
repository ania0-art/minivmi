# minivmi
![Language](https://img.shields.io/badge/language-C-00599C.svg) ![Platform](https://img.shields.io/badge/platform-Xen%20Project-orange.svg) ![Size](https://img.shields.io/badge/size-minimal-success) ![License](https://img.shields.io/badge/license-MIT-green.svg)

**A "Minimal Viable" Xen VMI learning project (Pure C).**

Designed for education, `minivmi` demonstrates the raw interaction with Xen hypercalls (`libxc`, `vm_event`) without the complexity of heavy abstraction libraries. It aims to be the "Hello World" for Xen Virtual Machine Introspection.

It performs three simple tasks to illustrate the VMI lifecycle:
1.  **List Domains**: Enumerate active domains (`list_domains`).
2.  **Attach**: Map the guest memory and prepare for introspection (UUID based).
3.  **Monitor**: Enable `vm_event` to trap **CR3 (Context Switch)** events and print them (`cr3trace_uuid`).

## Prerequisites

The project uses `pkg-config` to automatically handle compiler flags for:
- `xencontrol` (libxc)
- `xenstore`
- `xenevtchn`

# 中文说明 (Chinese Readme)
一个“最小可用”的 Xen VMI 学习项目（纯 C），只做三件事：

- 第1步：列出 domain（`list_domains`）
- 第2步：按 UUID attach 到某个 HVM guest（`cr3trace_uuid` 的准备阶段）
- 第3步：开启 vm_event 的 CR3 写入监控并打印事件（`cr3trace_uuid`）

## 构建

所有生成物都在 `_build/`（已在 `.gitignore` 忽略）。

```bash
make
```

如果系统有 pkg-config，Makefile 会用这些包名自动取编译/链接参数：

- `xencontrol`（libxc）
- `xenstore`
- `xenevtchn`

## 运行（需要在 dom0，以 root）

1) 列出当前 domain：

```bash
sudo _build/bin/list_domains
```

2) 监控某个 guest 的 CR3 写入（推荐用 UUID）：

```bash
sudo _build/bin/cr3trace_uuid --uuid <guest-uuid>
```

按 `Ctrl+C` 退出。

