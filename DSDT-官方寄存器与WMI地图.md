# DSDT 反编译：这台机器的**官方寄存器与 WMI 命令地图**

> 2026-10-10 从本机 ACPI 表反编译得到（`iasl` 20251212）。
> **这份地图是 HP 自己的固件给出的**，不是社区推断、也不是我们猜的。
> 证据等级：除特别标注外，全部为 **【实测本机】**。

---

## 0. 怎么拿到的（踩过一个坑）

```bash
sudo apt install -y acpica-tools
sudo acpidump -o /tmp/acpi.dat
acpixtract -a /tmp/acpi.dat          # ⚠ 没有 -d <目录> 这个选项！文件落在**当前目录**
iasl -d dsdt.dat                     # → dsdt.dsl（3.8 MB / 112,281 行）
for f in ssdt*.dat; do iasl -d "$f"; done
```
> ⚠ 坑：`acpixtract` **没有 `-d` 目录参数**，写了会被忽略，44 个表全落在 cwd。
> 我第一次把 `-d /tmp/` 写进命令，结果 `iasl -d /tmp/dsdt.dat` 找不到文件。

---

## 1. WMI 命令全集：**这台固件实现了 `GM01`–`GM37`（55 条）**

DSDT 里每个命令就是一个方法，**方法名就是命令号**（`GM27` = 命令 `0x27`）：

```
01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F 10 11 12 13 14 15 16 17 18 19 1A 1B 1C 1D 1E 1F
20 21 22 23 24 25 26 27 28 29 2A 2B 2C 2D 2E 2F 30 31 32 33 34 35 36 37
```

我们此前**只用了 5 条**：

| 命令 | 我们叫它 | DSDT 里的实现 |
| :- | :- | :- |
| `0x10` | `fan_count` | `GM10`：返回风扇数 2，并置 `EC0.OMCC = 1` |
| `0x11` | `fan_rpm` | `GM11`：按 `WBUF[0]` 选风扇，返回 `[FMR, FSUS, RPM_H, RPM_L]` |
| `0x1A` | `performance_mode` | 档位（`0x30/0x31/0x50`） |
| `0x21/0x22` | `gpu_modes` | cTGP / PPAB / dstate / slowdown_temp |
| `0x26/0x27` | `fan_unthrottle` | **`GM27` 就是写 `EC0.FFFS`**（见下） |
| `0x28` | `design_data` | 设计数据（含 thermal policy version） |

**⇒ 还有约 50 条命令我们从没碰过。** 其中 `0x12` 已经确认是**风扇转速设置**。

---

## 2. ★ 新发现：`0x12` = 按百分比设风扇（DSDT 原文）

```asl
Method (GM12, 0, NotSerialized)
{
    Debug = "HP WMI Command type 0x12 for WMI 20008h command"
    Local1 = WBUF                                    // WMI 输入数据
    Local2 = DerefOf (Local1 [Zero])                 // [0] = 选择器
    Local3 = DerefOf (Local1 [One])                  // [1] = 值（百分制，0x64 = 100）
    If ((Local2 == Zero))        { SFS1 = Local3 } // 0 → 只设风扇1
    ElseIf ((Local2 == One))     { SFS2 = Local3 } // 1 → 只设风扇2
    Else {
        SFS1 = Local3
        SFS2 = Local3
        If   (((Local2 == 0xFF) & (Local3 == 0x64))) { FFFS = One  }   // 100% 同时置「强制满速」
        ElseIf (((Local2 == 0xFF) & (Local3 == 0xFF))) { FFFS = Zero } // 0xFF/0xFF = 交回固件
    }
    Return (Local0)
}
```

**⇒ 厂商自己走的就是这条路**（HP 的 Windows 工具发的也是 `GM12`）。

### 顺带解开了 `0xEC` 之谜

`GM27`（我们一直在用的 `fan_unthrottle`）的源码：

```asl
Method (GM27, 0, NotSerialized)
{
    Debug = "HP WMI Command type 0x27 for WMI 20008h command"
    Local1 = WBUF
    HMFM = DerefOf (Local1 [Zero])
    If ((^^PC00.LPCB.EC0.ECOK == One)) { ^^PC00.LPCB.EC0.FFFS = HMFM }   // ★ 写 FFFS
    Return (Local0)
}
```

`GM26`（读）返回的也是 `FFFS`。⇒ **`fan_unthrottle` 就是 EC `0xEC` 的 bit1（`FFFS`）**，
我们的实测（写 `0xEC` 能拉满风扇）与 DSDT 完全一致。✅

而 `0xEC` 的位域（DSDT 只给 bit0/bit1 命名）：

| 位 | 名字 | 含义 |
| :- | :- | :- |
| bit0 | `FFFF` | 未见于任何逻辑（写了没用） |
| bit1 | `FFFS` | **强制满速旗标** = `fan_unthrottle` |
| bit2/3 | （未具名） | 实测写 `0x0C`（bits 2+3）也会满速 —— EC 固件内部用 |

---

## 3. ★ EC 的内存映射寄存器块（曲线相关字段在这里）

```asl
OperationRegion (H2RA, SystemMemory, 0xFE400000, 0x1000)   // 4 KB MMIO
Field (H2RA, AnyAcc, NoLock, Preserve)
{
    Offset (0x1B7),  FSUS,  8,        // 与 FMR 一起被 GM11 返回
    Offset (0x447),  GBCS,  8,
    Offset (0x527),  FMR1,  8,        // 风扇1「速率」（GM11 返回的第 0 字节）
    Offset (0x52F),  FMR2,  8,        // 风扇2
                     FS1H,  8,        // 风扇1 转速 高字节
                     FS1L,  8,        // 风扇1 转速 低字节
                     FS2H,  8,        // 风扇2 转速 高/低
                     FS2L,  8,
                     FAS1,  8,        // 风扇1 实际值（未被任何 ASL 引用）
                     FAS2,  8,
    Offset (0x53F),  SFS1,  8,        // ★ Set Fan 1 Speed —— GM12 写它
    Offset (0x8C4),  PDST,  8,
    Offset (0x8CC),  SFS2,  8,        // ★ Set Fan 2 Speed
    ...
}
```

**这张表解释了为什么"写 ACPI EC RAM 没用"** ——
风速的设定值 **不在 ACPI EC RAM（0x00–0xFF）里**，而在
**EC 的 MMIO 寄存器块（0xFE400000 + 0x53F / 0x8CC）**。
EC 固件在自己的地址空间里读它，我们在错误的空间里找了整整四轮。

> ⚠ 注：`GM12` 经由 ACPI 方法执行时，MMIO 写由 **ACPICA** 完成（内核标准路径），
> 不需要我们直接 `ioremap`/`/dev/mem`。`CONFIG_STRICT_DEVMEM=y` 也限制不了这条路。

---

## 4. ACPI EC RAM 的完整具名地图（250 字节，150 个字段）

`OperationRegion (ERAM, EmbeddedControl, Zero, 0xFF)`

**热/风扇速查：**

| 偏移 | 名字 |
| :- | :- |
| `0x47` | `TNT2` |
| `0x48` | `TNT3` |
| `0x49` | `TNT4` |
| `0x4B` | `TNT5` |
| `0x57` | `CPUT` |
| `0x58` | `RTMP` |
| `0x59` | `TNT1` |
| `0x61` | `SHPM` |
| `0x89` | `GPUT` |
| `0x95` | `HPCM` |
| `0xB0` | `RPM1` |
| `0xB1` | `RPM2` |
| `0xB2` | `RPM3` |
| `0xB3` | `RPM4` |
| `0xB7` | `GPTM` |
| `0xB8` | `EPWM` |
| `0xBA` | `CLOW` |
| `0xBB` | `CMAX` |
| `0xEC` | `FFFF` |
| `0xEC` | `FFFS` |
| `0xF4` | `SFAN` |

**全表：**

| 偏移 | 位宽 | 名字 | 我们实测到的 |
| :- | :- | :- | :- |
| `0x00` | 8 | `SMPR` |  |
| `0x01` | 8 | `SMST` |  |
| `0x02` | 8 | `SMAD` |  |
| `0x03` | 8 | `SMCM` |  |
| `0x04` | 256 | `SMD0` |  |
| `0x24` | 8 | `BCNT` |  |
| `0x25` | 8 | `SMAA` |  |
| `0x30` | 8 | `BTPL` |  |
| `0x31` | 8 | `BTPH` |  |
| `0x32` | 8 | `BCLC` |  |
| `0x34` | 8 | `SRP1` |  |
| `0x35` | 8 | `SRP2` |  |
| `0x37` | 8 | `ECL1` |  |
| `0x38` | 8 | `ECL2` |  |
| `0x39` | 8 | `ECL4` |  |
| `0x3A` | 8 | `EL1R` |  |
| `0x3B` | 8 | `EL2R` |  |
| `0x3C` | 8 | `EL4R` |  |
| `0x40` | 1 | `ACCC` |  |
| `0x40` | 1 | `SW2S` |  |
| `0x40` | 1 | `TRPM` |  |
| `0x41` | 1 | `QWOS` |  |
| `0x41` | 1 | `RFLG` |  |
| `0x41` | 1 | `SUSE` |  |
| `0x41` | 1 | `W7OS` |  |
| `0x42` | 1 | `CALS` |  |
| `0x42` | 1 | `KBBL` |  |
| `0x43` | 1 | `ACKY` |  |
| `0x43` | 1 | `ACPS` |  |
| `0x43` | 1 | `GFXT` |  |
| `0x44` | 1 | `DSMB` |  |
| `0x47` | 8 | `TNT2` |  |
| `0x48` | 8 | `TNT3` |  |
| `0x49` | 8 | `TNT4` |  |
| `0x4A` | 8 | `IRSN` |  |
| `0x4B` | 8 | `TNT5` |  |
| `0x4C` | 8 | `STRM` |  |
| `0x4E` | 1 | `LIDE` |  |
| `0x50` | 1 | `DPTL` |  |
| `0x50` | 1 | `IHEF` |  |
| `0x50` | 1 | `PTHM` |  |
| `0x50` | 1 | `S3CA` |  |
| `0x52` | 1 | `ECLS` |  |
| `0x53` | 1 | `CPHK` |  |
| `0x55` | 8 | `EC45` |  |
| `0x57` | 8 | `CPUT` | 实测：温度（随负载变） |
| `0x58` | 8 | `RTMP` | 实测：温度 |
| `0x59` | 8 | `TNT1` | 实测：温度 |
| `0x5B` | 8 | `HPTC` |  |
| `0x61` | 8 | `SHPM` |  |
| `0x62` | 1 | `OMCC` | 实测：flags，会浮动 |
| `0x67` | 1 | `GC6R` |  |
| `0x67` | 1 | `IGC6` |  |
| `0x67` | 1 | `LDBG` |  |
| `0x68` | 1 | `PLGS` |  |
| `0x69` | 1 | `BCTF` |  |
| `0x69` | 1 | `BF10` |  |
| `0x69` | 1 | `BMNF` |  |
| `0x69` | 1 | `BTVD` |  |
| `0x6C` | 8 | `GWKR` |  |
| `0x70` | 16 | `BADC` |  |
| `0x72` | 16 | `BFCC` |  |
| `0x74` | 8 | `BVLB` |  |
| `0x75` | 8 | `BVHB` |  |
| `0x76` | 8 | `BDVO` |  |
| `0x7F` | 1 | `ECTB` |  |
| `0x82` | 8 | `MBST` |  |
| `0x83` | 16 | `MCUR` |  |
| `0x85` | 16 | `MBRM` |  |
| `0x87` | 16 | `MBCV` |  |
| `0x89` | 8 | `GPUT` |  |
| `0x8B` | 3 | `LEDM` |  |
| `0x8D` | 1 | `MBFC` |  |
| `0x90` | 8 | `NVDO` |  |
| `0x91` | 8 | `ECDO` |  |
| `0x94` | 1 | `GSMS` |  |
| `0x94` | 1 | `GSSU` |  |
| `0x95` | 8 | `HPCM` | 实测：档位字节（default=0x30 / perf=0x31 / cool=0x50） |
| `0xA0` | 8 | `QBHK` |  |
| `0xA2` | 8 | `QBBB` |  |
| `0xA3` | 4 | `ADPH` |  |
| `0xA3` | 4 | `ADPI` |  |
| `0xA4` | 1 | `BACR` |  |
| `0xA4` | 1 | `MBTS` |  |
| `0xA6` | 8 | `MBDC` |  |
| `0xA8` | 1 | `ENWD` |  |
| `0xA8` | 1 | `TMPR` |  |
| `0xAA` | 1 | `RCDS` |  |
| `0xAA` | 1 | `SE1N` |  |
| `0xAA` | 1 | `SE2N` |  |
| `0xAA` | 1 | `SMSZ` |  |
| `0xAA` | 1 | `SOIE` |  |
| `0xAD` | 8 | `SADP` |  |
| `0xB0` | 8 | `RPM1` | 实测：fan0 转速低字节 ✅ 交叉验证通过 |
| `0xB1` | 8 | `RPM2` | 实测：fan0 转速高字节 ✅ |
| `0xB2` | 8 | `RPM3` | 实测：fan1 转速低字节 ✅ |
| `0xB3` | 8 | `RPM4` | 实测：fan1 转速高字节 ✅ |
| `0xB7` | 8 | `GPTM` | 实测：GPU 温度 |
| `0xB8` | 8 | `EPWM` | 实测：恒 30~33（不随转速变） |
| `0xBA` | 8 | `CLOW` | 实测：恒 1 |
| `0xBB` | 8 | `CMAX` | 实测：恒 5 |
| `0xC1` | 8 | `DPPC` |  |
| `0xC5` | 1 | `SHB1` |  |
| `0xC5` | 1 | `SHB2` |  |
| `0xC5` | 1 | `SHB3` |  |
| `0xC5` | 1 | `SHB4` |  |
| `0xC5` | 1 | `SHEN` |  |
| `0xC5` | 1 | `SHFL` |  |
| `0xC5` | 1 | `SHNP` |  |
| `0xC5` | 1 | `SHOK` |  |
| `0xC6` | 1 | `CVTS` |  |
| `0xCA` | 8 | `CSFG` |  |
| `0xD0` | 1 | `EBPL` |  |
| `0xD2` | 8 | `S1A1` |  |
| `0xD3` | 8 | `S2A1` |  |
| `0xD4` | 8 | `PSHD` |  |
| `0xD5` | 8 | `PSLD` |  |
| `0xD6` | 8 | `DBPL` |  |
| `0xD7` | 8 | `STSP` |  |
| `0xDA` | 8 | `PSIN` |  |
| `0xDB` | 1 | `PSKB` |  |
| `0xDB` | 1 | `PSTP` |  |
| `0xDB` | 1 | `PWOL` |  |
| `0xDB` | 1 | `RTCE` |  |
| `0xDC` | 8 | `S1A0` |  |
| `0xDD` | 8 | `S2A0` |  |
| `0xDE` | 8 | `NVDX` |  |
| `0xDF` | 8 | `ECDX` |  |
| `0xE0` | 8 | `DLYT` |  |
| `0xE1` | 8 | `DLY2` |  |
| `0xE2` | 8 | `KBT0` |  |
| `0xE6` | 8 | `SFHK` |  |
| `0xE9` | 8 | `DTMT` |  |
| `0xEA` | 8 | `PL12` |  |
| `0xEB` | 8 | `ETMT` |  |
| `0xEC` | 1 | `FFFF` | 实测：位域 —— bit0=FFFF, bit1=FFFS(=fan_unthrottle 旗标)；bit2/3 未具名但写 0x0C 会强制满速 |
| `0xEC` | 1 | `FFFS` | 实测：位域 —— bit0=FFFF, bit1=FFFS(=fan_unthrottle 旗标)；bit2/3 未具名但写 0x0C 会强制满速 |
| `0xF0` | 1 | `MUCR` |  |
| `0xF0` | 1 | `PARS` |  |
| `0xF2` | 1 | `ENPA` |  |
| `0xF2` | 1 | `ZPDD` |  |
| `0xF4` | 8 | `SFAN` | ★ SFAN（8 位，实测恒 0，从未写过） |
| `0xF7` | 1 | `HDMI` |  |
| `0xF8` | 1 | `NVDS` |  |
| `0xF9` | 1 | `FTHM` |  |
| `0xF9` | 1 | `GFXM` |  |

### ✅ 交叉验证（证明这张表可信）

| 我们实测出来的 | DSDT 里的名字 | 偏移 |
| :- | :- | :- |
| `0xB0`/`0xB1` = fan0 RPM 低/高字节 | `RPM1` / `RPM2` | 完全一致 ✅ |
| `0xB2`/`0xB3` = fan1 RPM 低/高字节 | `RPM3` / `RPM4` | 完全一致 ✅ |
| `0x57/0x58/0x59` = 温度 | `CPUT` / `RTMP` / `TNT1` | 一致 ✅ |
| `0xB7` = GPU 温度 | `GPTM` | 一致 ✅ |
| `0x95` = 档位 | `HPCM` | 一致 ✅ |
| `0x2E`/`0x2F` = 当前 PWM | **DSDT 里没有名字**（EC 内部用） | — |

---

## 5. 其它顺带确认的事

- **`FSSP(Arg0)`**（DSDT 第 111384 行）：`Arg0 != 0 → SFAN=0`，`Arg0 == 0 → SFAN=2`。
  **`FSSP` 在全库（DSDT + 26 个 SSDT）里从未被调用** ⇒ 死代码。
  `SFAN`（EC `0xF4`）实测恒为 0。
- **`EPWM`（0xB8）不是风扇**：ssdt13 里 `EC0.EPWM = LEDB`（LED 亮度 PWM）。
- **`HPCM`（0x95）** = 档位寄存器，与 `platform_profile` 无关。
- **ssdt3 里的 `SFAN`** 是 NVIDIA `_DSM` 返回缓冲的 DWord（`_SB.PC00.PEG1.PEGP.GPSP`），
  **是上报给 NVIDIA 驱动的信息**，与 EC `0xF4` 的 `SFAN` 同名但不同物。

---

## 6. 下一步（已实现，待实测）

| 项 | 状态 |
| :- | :- |
| 模块加 `fan_speed` 属性（发 `GM12`） | ✅ 已写好并**编译通过**（437 KB `.ko`） |
| 读写接口 | `cat fan_speed` → `fanN: rpm=V set=<百分比\|auto>`；`echo "0 50" > fan_speed`；`echo release > fan_speed` |
| 测试脚本 | `system/fan-speed-test.sh`（控制组 100% → release → 关键组 50%） |
| 实测 | ⏳ **未做** —— 需要 `sudo bash install.sh --build` 重装模块 |

**判据**：写 50% 之后
- 冲顶到 ~4600 ⇒ 不是档位（`SFS` 只当开关用）
- **停在中间（~3000）⇒ 百分比控制成立 = 真正的连续曲线**
