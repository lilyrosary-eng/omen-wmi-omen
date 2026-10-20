# hp-wmi-omen — 直接调用 HP 固件私有 WMI 接口的内核模块

## 为什么需要它

上游内核的 `hp-wmi` 驱动**不认识本机主板 `8A1A`**，所以 `platform_profile` 切档时
`setup_active_thermal_profile_params()` 提前 `return` —— 内核**根本不会把命令下发到硬件**。

实测（切 cool/quiet/balanced/performance 四档）：

| 观察项 | 结果 |
| :- | :- |
| EPP / 频率上限 / 睿频 / `max_perf_pct` | **完全不变** |
| GPU 功耗墙（`Current Power Limit`） | **完全不变** |
| EC 档位寄存器 `0x95` | 恒为 `48`（= 内核常量 `DEFAULT`） |

而 OMEN 的"性能模式"（GPU cTGP/TGP、风扇曲线）在固件层是 **HP 私有 WMI 命令**：

- Windows：OmenSuperHub 走 `root\WMI` 的 `HPQOEM_WmiInterface`，**任何进程都能直接发命令**；
- Linux：这套接口只留在内核里（`hp-wmi` → `wmi_evaluate_method`），
  又**没有 `/dev/wmi`** 之类的用户态接口 ⇒ 第三方工具在用户态无路可走。

这个模块的做法：**不碰 in-tree 的 hp-wmi**，而是另起一个极小的模块，
把内核已导出的 `wmi_evaluate_method()` 当通道，直接向固件发送 HP 自己的命令。

## 安全性

| 项 | 事实 |
| :- | :- |
| 直接读写 EC | **有，但是显式接口**：`ec_byte`（0600 root）走内核 `ec_read/ec_write`，用于调试/探测，不是常规路径 |
| 风扇调速 | 走 WMI `GM12` → 写 EC 的 `SFS1/SFS2`。**这是真实硬件写**，不是"零风险" |
| 未知机型 | **默认只读**：所有 store（`fan_speed` / `fan_unthrottle` / `gpu_modes` / `performance_mode` / `fan_speed_victus_s` / `ec_byte`）在非白名单机型上返回 `-EPERM`。要放行必须显式 `modprobe hp-wmi-omen allow_unsafe_writes=1` |
| 命令码来源 | 上游 `hp-wmi` + DSDT 反编译（仅用于确认 `GM12` 语义），无猜测的寄存器地址 |
| 校验 | 写接口做范围/枚举校验；`gpu_modes` 写入前先读 `slowdown_temp` 再原样回填 |
| 日志 | 每条命令的固件返回码都进 dmesg，可完整审计 |

### 支持机型

| 机型 | 状态 |
| :- | :- |
| HP OMEN 16-k0xxx（board `8A1A`，BIOS F.23） | ✅ 全部读写接口实测 |
| 其它 HP 机型 | ⚠️ 只读（`probe_all` / 温度 / `fan_rpm` / `design_data` 可用，写被拒） |

想把自己的机型加进白名单：先只读跑 `echo 1 > probe_all`，确认 `fan_count` / `design_data`
合理、`GM12` 不会返回异常，再提 PR 附 dmesg。

## 协议（全部来自上游 `drivers/platform/x86/hp/hp-wmi.c`）

```
GUID     = 5FB7F034-2C63-45E9-BE91-3D44E2C707E4   (HPQOEM_WmiInterface)
instance = 0
method_id 只编码输出缓冲区大小（encode_outsize_for_pvsz）

请求：struct bios_args { u32 signature=0x55434553; u32 command;
                         u32 commandtype=0x20008; u32 datasize; u8 data[]; }
响应：struct bios_return { u32 sigpass; u32 return_code; } + 输出数据
```

| 命令码 | 含义 | sysfs 接口 |
| :- | :- | :- |
| `0x10` | 风扇数量 | `fan_count`（读） |
| `0x11` | 风扇转速（**需传风扇索引**，取返回字节 [2][3]） | `fan_rpm`（读，逐风扇输出） |
| `0x1A` | **性能模式** `{0xFF, mode}` | `performance_mode`（写） |
| `0x21` | **GPU 功耗模式**（cTGP/PPAB/D-state） | `gpu_modes`（读） |
| `0x22` | **写 GPU 功耗模式** | `gpu_modes`（写） |
| `0x26`/`0x27` | **风扇限速开关**（0/1，解除限速让风扇跑满） | `fan_unthrottle`（读/写） |
| `0x28` | 设计数据 | `design_data`（读） |
| `0x2D` | 风扇目标转速（**仅 Victus S**） | `fan_speed_victus_s`（读） |
| `0x2E` | 写风扇目标转速 `{rpm/100, rpm/100}`（**仅 Victus S**） | `fan_speed_victus_s`（写） |
| `0x2F` | 风扇曲线表（**仅 Victus S**，本机 EIO 属正常） | `fan_table`（读） |

## 性能模式（thermal profile）

OMEN 有**两套**档位值，由 `design_data` 的 `buffer[3]`（thermal policy version）决定，**写错那套固件不认**：

| tp_version | default | performance | cool |
| :- | :- | :- | :- |
| v0（`== 0`） | `0x00` | `0x01` | `0x02` |
| v1（`!= 0`） | `0x30` | `0x31` | `0x50` |

写入格式固定 `{0xFF, mode}`。本机实测 `design_data = 4a 01 35 01 01 d7 00 0c`
⇒ `buffer[3] = 0x01` ⇒ **走 v1**。

```bash
# 先查该用哪套
cat  /sys/devices/platform/omen-wmi/profile_version

# 写（接受字符串，也接受 1/2/3 = default/performance/cool）
echo performance > /sys/devices/platform/omen-wmi/performance_mode
echo default    > /sys/devices/platform/omen-wmi/performance_mode
echo raw:0x31   > /sys/devices/platform/omen-wmi/performance_mode   # 绕过映射
```

## ⚠ 哪些接口在本机（OMEN）不适用

上游把三个命令限制在 **Victus S** 的 DMI 白名单里，OMEN 机型固件不认识，
读它们会返回 `Input/output error` —— **这是正常现象，不是 bug**：

| 接口 | 底层命令 | 说明 |
| :- | :- | :- |
| `fan_table` | `0x2F` | 风扇曲线表，仅 Victus S |
| `fan_speed_victus_s` | `0x2D` / `0x2E` | 直接读写风扇 RPM，仅 Victus S |

**OMEN 系没有公开的"直接写风扇 RPM"命令** —— 上游对 OMEN 只提供
profile 档位（`0x1A`）与解除限速（`0x27`）。想调风扇转速，只能：
1. 切 profile 档位（`performance` / `default` / `cool`）
2. 解除限速让它跑满（`fan_unthrottle=1`）
3. 或者直接读 EC（需要 `hp_wmi` 的 EC 通道，比 WMI 复杂）

---

## 权限（普通用户免 sudo）

内核侧：可写接口 **0664**（属主+属组），读接口 0444。
udev 规则 `99-omen-wmi.rules` 把属组设为 **plugdev** —— 该组成员（桌面登录用户）可直接读写，
用户态 GUI 无需提权。`install.sh --build` 会自动安装规则并立即生效。

```bash
ls -l /sys/devices/platform/omen-wmi/performance_mode
# -rw-rw---- 1 root plugdev ... ← 属组是 plugdev 即可写
```

⚠ 为什么不是 0666：内核 `VERIFY_OCTAL_PERMISSIONS` 硬性禁止 other-writable
（`BUILD_BUG_ON_ZERO((perms) & 2)`），传 0666 会**编译失败**。只能走属组授权。

---

## 权限（普通用户免 sudo）

内核侧：可写接口 **0664**（属主+属组），读接口 0444。
udev 规则 `99-omen-wmi.rules` 把属组设为 **plugdev** —— 该组成员（桌面登录用户）可直接读写，
用户态 GUI 无需提权。`install.sh --build` 会自动安装规则并立即生效。

```bash
ls -l /sys/devices/platform/omen-wmi/performance_mode
# -rw-rw---- 1 root plugdev ... ← 属组是 plugdev 即可写
```

⚠ 为什么不是 0666：内核 `VERIFY_OCTAL_PERMISSIONS` 硬性禁止 other-writable
（`BUILD_BUG_ON_ZERO((perms) & 2)`），传 0666 会**编译失败**。只能走属组授权。

---

## 风扇限速（临时全速）

```bash
echo 1 > /sys/devices/platform/omen-wmi/fan_unthrottle   # 解除限速，风扇跑满
echo 0 > /sys/devices/platform/omen-wmi/fan_unthrottle   # 恢复 BIOS 曲线控制
```
| — | 只读逐条试调，看固件认哪些 | `probe_all`（写触发） |

## 安装

```bash
sudo bash install.sh --genkey      # ① 生成签名密钥并导入 MOK（会问你要一次性密码）
# ② 重启（**必须真重启**，见下方"卡在 Key was rejected"）
sudo systemctl reboot -i           #    或从 GNOME 菜单点「重启」
# ③ 重启后在蓝色 MOK 界面用同一密码确认导入
sudo bash install.sh --build       # ④ 编译 + 签名 + 安装 + 加载
sudo bash install.sh --probe       # ⑤ 只读探测，看固件认哪些命令
```

卸载：`sudo bash install.sh --uninstall`

### 卡在 `Key was rejected by service`（实测最常见的坑）

这一步几乎总是同一个原因：**`sudo reboot` 被 GNOME 拦住了，根本没重启。**

```
Operation inhibited by "rosary" (PID ... "gnome-session-s"), reason is "user session inhibited"
```

拦截者是 `gsd-media-keys`（`handle-power-key`，block 级 inhibitor）和 `gnome-shell`。
确认一下就知道：

```bash
uptime -s        # 开机时间
mokutil --list-enrolled | grep -c "hp-wmi-omen MOK"   # 应为 1
```

**绕过办法**（任选其一）：

| 方式 | 命令 | 说明 |
| :- | :- | :- |
| 推荐 | `sudo systemctl reboot -i` | `-i` = 忽略 inhibitor |
| 最省事 | GNOME 右上角菜单 →「重启」 | MOK 界面会正常弹出 |
| 兜底 | `sudo systemctl reboot -f` | ⚠ 跳过正常关机流程，可能丢数据 |

本脚本在 `--build` 开头有**预检**：检测到"证书生成后没重启过"会直接拦下，
并告诉你重启命令，不再让你白编译一遍再看到同一个报错。

### 卡在 `MokListNewRT 不存在`（第二个坑：密码）

诊断脚本第 5 段会重新执行导入。如果 `input password:` 处**直接回车**，
mokutil 会连报三次 `password should be 1~256 characters` 然后 `Abort`，
**一个字节都没写进固件**——所以 `MokListNewRT` 当然不会出现。

```bash
# 密码要求 1~256 字符；输入时终端不显示任何字符，属正常现象
# 建议用 4~8 位简单数字，重启时在 MOK 界面要输入同一个
sudo bash mok-diag.sh
```

⚠ **不存在"免交互"的导入方式**：`mokutil` 用 `getpass` 读 `/dev/tty`，
`--generate-hash` / `--import-hash` 同样是交互式的，脚本无法代劳（会卡住）。

**判据分级**：只有"密码正常输入 + `MokListNewRT` 仍不出现"才能归因于
**固件侧拒绝 efivars 写入**；此时可考虑升级 shim 或改走备选路径。

### 关于 Secure Boot（重要）

本机 Secure Boot 开启 + Lockdown integrity，**未签名模块一律拒绝加载**
（此前用 `ec_sys write_support=1` 验证过这条）。

但**不需要关闭 Secure Boot**——shim 的 MOK 机制可以在保留 Secure Boot 的前提下
额外信任我们自己的证书。代价只是**一次重启 + 在 MOK 管理界面确认**。
之后每次重新编译模块只要再 `--build`，不必再重启。

### 踩过的坑：证书格式

首次运行会报：

```
Abort!!! /etc/hp-wmi-omen/MOK.der is not a valid x509 certificate in DER format
```

原因是 `openssl req -out MOK.der` 的 `-out` **默认写 PEM 文本**——
文件名写着 `.der` 不代表内容是 DER。必须加 `-outform DER`。
本脚本已强制指定，并在生成后用 `openssl x509 -inform DER` 实测校验；
若检测到旧的 PEM 证书会提示并重新生成。

后续的 `Key was rejected by service` 是这个 bug 的**连带结果**：
证书从未真正进过 MOK 列表，签名自然不被承认。

### 怎么确认 MOK 到底有没有被固件接受

```bash
mokutil --list-enrolled | grep -c "hp-wmi-omen MOK"    # 应为 1
```

⚠ **不要用 `mokutil --test-key` 判断**——实测它对未注册的证书也返回 0
（stdout 明明打印 `is not enrolled`，退出码却是 0），会误报成"已导入"。
`--list-enrolled` 会打印完整证书，按 Subject 里的 CN 匹配才可靠。

### 为什么不用升级内核

简答：**升级拿不到支持**。判定完全靠 `DMI_BOARD_NAME` 精确匹配一张
**编译期硬编码的白名单**，没有任何运行时探测；最新主线（2026-10）的表里
`8A1A` 依然不存在（8A 系列只收了 `8A15`/`8A42`/`8A43`）。
完整证据链见 **[内核支持说明.md](内核支持说明.md)**。

## 为什么需要它

## 第一步该做什么：只读探测

**先探测，不要急着写。** `probe_all` 只发读命令，不做任何写入：

```
$ sudo bash install.sh --probe
cmd 0x10 : 固件接受
cmd 0x11 : 固件接受
cmd 0x21 : 固件接受        ← GPU 功耗模式可读 = TGP 控制有戏
cmd 0x2D : ★ 风扇目标命令可用: fan1=xxxx fan2=xxxx RPM   ← 如果这条也接受，说明能做自定义风扇曲线
cmd 0x2F : ★ 风扇曲线表可用: num_fans=N                ← 如果这条也接受，说明能读到固件的曲线
```

社区驱动作者认为 OMEN **不支持** `0x2D/0x2E/0x2F`（Victus S 的 GM 风扇命令），
但那只是别人的判断 —— **我们直接问固件**。

## 之后能做什么（取决于探测结果）

| 探测结果 | 能做的事 |
| :- | :- |
| `0x21/0x22` 可用 | **GPU TGP 控制**（ctgp_enable）—— 这正是 Linux 下拿不到 175 W 的原因 |
| `0x2D/0x2E` 可用 | **自定义风扇曲线 / 手动锁定转速** |
| `0x2F` 可用 | 读取并展示固件的原始风扇曲线 |
| `0x1A` 可用 | 真正生效的 ACPI 性能档 |
| 全都拒绝 | 固件层面锁死，那就在 README 里如实记录"此路不通" |

## 文件

```
hp-wmi-omen.c   模块源码（约 600 行，含完整协议注释）
Makefile        编译
dkms.conf       DKMS 配置（可选，用于内核升级后自动重编）
install.sh      编译 / 签名 / 安装 / 探测 / 卸载
hp-wmi-omen.ko  已编译好的模块（vermagic 7.0.0-38-generic）
```

> 注意：**内核模块必须在 Linux 原生文件系统上编译**（脚本会拷到 `/usr/src/`），
> 不能直接在 D 盘 NTFS 上 `make` —— 会留下大量临时目录并可能失败。

## 许可证

**GPL-2.0**，见 `LICENSE`。模块声明 `MODULE_LICENSE("GPL")`，
用到 `wmi_evaluate_method` 等 `EXPORT_SYMBOL_GPL` 符号，必须如此。
