// SPDX-License-Identifier: GPL-2.0
/*
 * 来源与派生说明（Attribution）
 * -----------------------------
 * 本文件不是从零发明 WMI：**传输层与命令码表派生自 Linux 内核的**
 *   drivers/platform/x86/hp/hp-wmi.c        （GPL-2.0）
 * 具体派生部分：
 *   · struct bios_args 的报文布局、encode_outsize_for_pvsz() 的 method_id 编码；
 *   · 命令码常量（HPWMI_GM / FAN_COUNT_GET 0x10 / FAN_SPEED_GET 0x11 /
 *     SET_FAN_SPEED 0x12 / SET_PERFORMANCE_MODE 0x1A / GPU_THERMAL 0x21,0x22 …）；
 *   · omen_gpu_power_modes 等结构体布局、omen_thermal_profile_set() 的分支语义；
 *   · fan_rpm 的读取方式（逐条对应 hp_wmi_get_fan_speed()）。
 * 上述代码的版权归上游 hp-wmi 的作者所有 —— 见内核源码该文件的版权头与
 * git 历史；本文件按 GPL-2.0 整体分发（这也是它必须是 GPL 的原因）。
 *
 * 本模块**新增**的部分：
 *   · 用 GM12(0x12) 写风扇百分比的用法与 0/255/release 的实测语义；
 *   · OMEN 16-k0xxx（board 8A1A）的探测顺序与 sysfs 接口集合；
 *   · DMI 白名单与 allow_unsafe_writes 安全阀；
 *   · 对 0x2D/0x2E（Victus S 专用）命令在本机返回 EIO 的实测记录。
 *
 * 如果你要把这里的代码搬回 in-tree hp-wmi，请以那个文件为基准，
 * 不要把本文件的注释当作上游语义的唯一依据。
 */

/*
 * hp-wmi-omen — 直接向 HP 固件发送 OMEN 私有 WMI 命令
 *
 * 背景
 * ----
 * 本机是 HP OMEN 16-k0xxx（主板 8A1A，i9-12900HX + RTX 3080 Ti Laptop）。
 * 上游内核的 hp-wmi 驱动**不认识这块板子**（8A1A 不在它的 DMI 表里），
 * 于是 platform_profile 切档时 setup_active_thermal_profile_params() 提前
 * return —— 内核根本不会把命令下发到硬件。实测：切四档后
 * EPP / 频率上限 / 睿频 / max_perf_pct / GPU 功耗墙全部不变，EC 0x95 恒为 0x30。
 *
 * OMEN 的"性能模式"（含 GPU cTGP/TGP、风扇曲线）在固件层是厂商私有 WMI 命令。
 * Windows 上的 OmenSuperHub 走 root\WMI 的 HPQOEM_WmiInterface 直接调它；
 * Linux 把这套接口只留在内核里（hp-wmi → wmi_evaluate_method），
 * 又没有 /dev/wmi 之类的用户态接口，所以第三方工具在用户态无路可走。
 *
 * 本模块的思路
 * ------------
 * 与其 patch 上游驱动（要替换 in-tree 的 hp-wmi、DKMS 覆盖、冲突风险大），
 * 不如另起一个极小的模块：把内核已经导出的 wmi_evaluate_method() 当作通道，
 * 直接向固件发送 HP 自己的命令，并把能力以 sysfs 暴露。
 *
 * 这样做的好处：
 *  - 完全不碰 in-tree 的 hp-wmi，不存在模块替换 / 版本冲突；
 *  - 只用 HP 自己定义的命令码与报文格式（不是逆向猜的寄存器）；
 *  - 每一步都有 pr_info 日志可审计，出错立刻能看出是哪条命令不被接受；
 *  - 不使用任何"不安全模块参数"，签名后可在 Secure Boot 开启状态下加载。
 *
 * 协议（全部来自上游 hp-wmi 驱动，非猜测）
 * ----------------------------------------
 *  传输：ACPI WMI method
 *        GUID     = 5FB7F034-2C63-45E9-BE91-3D44E2C707E4 (HPQOEM_WmiInterface)
 *        instance = 0
 *        method_id 只编码输出缓冲区大小（encode_outsize_for_pvsz）
 *
 *        请求缓冲区：
 *          struct bios_args {        // __packed
 *              u32 signature;        // 0x55434553 ("SECS")
 *              u32 command;          // 命令码
 *              u32 commandtype;      // 命令码（0x10 风扇数 / 0x11 转速 / 0x28 设计数据）
 *              u32 datasize;         // 有效数据长度
 *              u8  data[];           // max(insize,128) 字节
 *          };
 *
 *        响应缓冲区：
 *          struct bios_return { u32 sigpass; u32 return_code; } + 输出数据
 *
 *  命令码（enum hp_wmi_gm_commandtype）
 *    0x10 FAN_COUNT_GET          0x11 FAN_SPEED_GET
 *    0x1A SET_PERFORMANCE_MODE   0x21 GET_GPU_THERMAL_MODES
 *    0x22 SET_GPU_THERMAL_MODES  0x26/0x27 风扇最大值 读 / 设
 *    0x28 GET_SYSTEM_DESIGN_DATA
 *    0x2D VICTUS_S_FAN_SPEED_GET 0x2E VICTUS_S_FAN_SPEED_SET
 *    0x2F VICTUS_S_GET_FAN_TABLE
 *
 *  返回码：0=成功 2=签名错 3=未知命令 4=未知命令类型 5=参数非法
 *
 * 安全边界
 * --------
 *  - 本模块不直接读写 EC 寄存器。EC 由固件自己操作，我们只发厂商命令。
 *  - 所有写接口都做范围 / 枚举校验。
 *  - 风扇转速命令（0x2E）要求固件实现 Victus S 的 GM 风扇命令 —— 社区驱动作者
 *    认为 OMEN 不支持。本模块不假设、只探测：探测入口是
 *    /sys/kernel/hp_omen/probe_all，它只读试调并打印返回码，不做任何写入。
 *
 * 说明：这是照着 HP 公开在 Linux 内核里的协议实现，不是逆向 EC 寄存器布局。
 */

#include <linux/acpi.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/delay.h>
#include <linux/dmi.h>

/*
 * ── 机型白名单：开源发布的安全阀 ──────────────────────────────────────
 *
 * 本模块直接通过 WMI GM 命令往 EC 写风扇百分比 / 性能档 / EC 字节。
 * **这些命令的语义是机型相关的**：同一条 GM 0x12 在别的 HP 机型上未必是
 * "设置风扇转速"，写错可能改到别的寄存器（风扇/电源/充放电）。
 *
 * 因此默认策略是：**未知机型只读**。所有 store 在非白名单机型上返回 -EPERM，
 * 读接口（temperature / fan_rpm / design_data / probe_all）照常可用，
 * 方便别人先探测、再决定要不要冒险。
 *
 * 确认自己知道在做什么的人，可以显式放行：
 *     sudo modprobe hp-wmi-omen allow_unsafe_writes=1
 *
 * 已知机型：HP OMEN 16-k0xxx（board 8A1A，i9-12900HX + RTX 3080 Ti Laptop）
 * 若你的机器通过了 probe_all 的只读探测，欢迎提 PR 把 DMI 加进白名单。
 */
static bool allow_unsafe_writes;
module_param(allow_unsafe_writes, bool, 0444);
MODULE_PARM_DESC(allow_unsafe_writes,
	"Allow sysfs writes on machines outside the known-good DMI list (default: 0)");

static bool hp_omen_writes_allowed(void)
{
	if (allow_unsafe_writes)
		return true;

	return dmi_match(DMI_SYS_VENDOR, "HP") &&
	       dmi_match(DMI_BOARD_NAME, "8A1A");
}

#define DRV_NAME "hp-wmi-omen"
#define HPWMI_BIOS_GUID "5FB7F034-2C63-45E9-BE91-3D44E2C707E4"

/*
 * 内核导出的 WMI 入口（Module.symvers 里是 EXPORT_SYMBOL_GPL）。
 *
 * 注意 7.0 的 <linux/wmi.h> 只声明了按设备的 wmidev_* 版本，而
 * wmi_register_driver **没有导出**给外部模块，所以无法注册 wmi 驱动；
 * 只能像内核自己的 hp-wmi 那样按 GUID 直接调用。
 * 若下面的原型与内核不符，modpost 会在加载前报
 * "disagrees about version control symbol"，不会静默出错。
 */
/*
 * 这两个函数由 <linux/acpi.h> 声明：
 *   extern acpi_status wmi_evaluate_method(const char *guid, u8 instance,
 *                       u32 method_id, const struct acpi_buffer *input,
 *                       struct acpi_buffer *output);
 *   extern bool wmi_has_guid(const char *guid);
 * 且都在 Module.symvers 里是 EXPORT_SYMBOL_GPL，可以直接链接调用。
 */

/* ── 协议常量（取自上游 drivers/platform/x86/hp/hp-wmi.c）────────────── */

#define HPWMI_GM			0x20008

#define HPWMI_FAN_COUNT_GET_QUERY	0x10
#define HPWMI_FAN_SPEED_GET_QUERY	0x11
/*
 * 0x12 = 设置风扇转速。
 * 依据：DSDT 反编译出的 Method (GM12)（2026-10-10）——
 *     Local2 = WBUF[0]（选择器：0=风扇1, 1=风扇2, 其它=两个都改）
 *     Local3 = WBUF[1]（值，百分制；0x64=100）
 *     if (Local2==0) SFS1 = Local3; else if (Local2==1) SFS2 = Local3;
 *     else { SFS1 = SFS2 = Local3;
 *            if (Local2==0xFF && Local3==0x64) FFFS = 1;    // 100% 且置"强制满速"
 *            if (Local2==0xFF && Local3==0xFF) FFFS = 0; }  // 0xFF/0xFF = 交回固件
 * 其中 SFS1/SFS2 是 EC 的 MMIO 寄存器（OperationRegion H2RA @ 0xFE400000，
 * 偏移 0x53F / 0x8CC）；FFFS 是 ACPI EC RAM 0xEC 的 bit1 —— 即本模块
 * fan_unthrottle 用的那个旗标（GM27 就是写它）。
 */
#define HPWMI_SET_FAN_SPEED		0x12
#define HPWMI_SET_PERFORMANCE_MODE	0x1A
#define HPWMI_GET_GPU_THERMAL_MODES	0x21
#define HPWMI_SET_GPU_THERMAL_MODES	0x22
#define HPWMI_FAN_SPEED_MAX_GET_QUERY	0x26
#define HPWMI_FAN_SPEED_MAX_SET_QUERY	0x27
#define HPWMI_GET_SYSTEM_DESIGN_DATA	0x28
#define HPWMI_VICTUS_S_FAN_SPEED_GET	0x2D
#define HPWMI_VICTUS_S_FAN_SPEED_SET	0x2E
#define HPWMI_VICTUS_S_GET_FAN_TABLE	0x2F

#define HPWMI_RET_UNKNOWN_COMMAND	0x03
#define HPWMI_RET_UNKNOWN_CMDTYPE	0x04

#define HPWMI_BIOS_SIG			0x55434553	/* "SECS" */


/* 风扇转速命令以 100 RPM 为单位（值本身是 u8） */
#define FAN_SPEED_UNIT_RPM		100

struct omen_bios_return {
	u32 sigpass;
	u32 return_code;
} __packed;

struct omen_bios_args {
	u32 signature;
	u32 command;
	u32 commandtype;
	u32 datasize;
	u8 data[];
} __packed;

/* GPU 功耗模式（上游 struct victus_gpu_power_modes）*/
struct omen_gpu_power_modes {
	u8 ctgp_enable;		/* cTGP —— GPU 最大 TGP 的开关 */
	u8 ppab_enable;		/* PPAB —— 动态加功耗 */
	u8 dstate;		/* GPU D-state */
	u8 gpu_slowdown_temp;	/* 原样回填，不改动 */
} __packed;

struct omen_fan_table_header {
	u8 num_fans;
	u8 unknown;
} __packed;

struct omen_fan_table_entry {
	u8 cpu_rpm;
	u8 gpu_rpm;
	u8 noise_db;
} __packed;

/*
 * HP 私有 WMI 接口是否可用：**三态**（未探测 / 可用 / 不可用）。
 * ⚠ 关键：绝不能在 module_init 里调用 wmi_has_guid()。
 *   实测那样做会让 init 卡住不返回（/sys/module/hp_wmi_omen/initstate
 *   一直是 "coming"，modprobe 无限等待）。init 必须尽快返回，
 *   固件交互一律推迟到用户读写 sysfs 时。
 */
static int hp_omen_wmi_state;   /* 0=未探测  1=可用  -1=不可用 */

/* 惰性探测：第一次真正发命令前才查 GUID 是否存在 */
static inline bool hp_omen_wmi_ok(void)
{
	if (hp_omen_wmi_state == 0)
		hp_omen_wmi_state = wmi_has_guid(HPWMI_BIOS_GUID) ? 1 : -1;
	return hp_omen_wmi_state == 1;
}

/* method_id 只编码输出大小（上游 encode_outsize_for_pvsz）*/
static int encode_outsize_for_pvsz(int outsize)
{
	if (outsize > 4096)
		return -EINVAL;
	if (outsize > 1024)
		return 5;
	if (outsize > 128)
		return 4;
	if (outsize > 4)
		return 3;
	if (outsize > 0)
		return 2;
	return 1;
}

/*
 * 向固件发一条 GM 命令。in / out 可为 NULL。
 * 返回 0 表示固件 return_code == 0；否则 -EIO，并把固件返回码写入 *fwret。
 */
static int omen_gm_query(u32 command, const void *in, int insize,
			 void *out, int outsize, u32 *fwret)
{
	struct acpi_buffer input = { ACPI_ALLOCATE_BUFFER, NULL };
	struct acpi_buffer output = { ACPI_ALLOCATE_BUFFER, NULL };
	struct omen_bios_args *args;
	union acpi_object *obj;
	struct omen_bios_return *hdr;
	int mid, actual_insize;
	acpi_status status;

	if (!hp_omen_wmi_ok())
		return -ENODEV;

	mid = encode_outsize_for_pvsz(outsize);
	if (mid < 0)
		return mid;

	/* 内核同样要求输入缓冲区至少 128 字节 */
	actual_insize = max_t(int, insize, 128);

	args = kmalloc(struct_size(args, data, actual_insize), GFP_KERNEL);
	if (!args)
		return -ENOMEM;

	input.length = struct_size(args, data, actual_insize);
	input.pointer = args;
	/*
	 * ⚠ 这两个字段千万别填反（我曾填反，导致固件直接拒绝、sysfs 全部返回 -EIO）：
	 *     args->command     = HPWMI_GM (0x20008)  ← 固定标记，不是命令码
	 *     args->commandtype = command            ← 命令码在这里（0x10/0x11/0x28…）
	 * 对照上游：hp_wmi_perform_query(query=0x10, command=HPWMI_GM)
	 *         → args->commandtype = query;  args->command = command;
	 */
	args->signature   = HPWMI_BIOS_SIG;
	args->command     = HPWMI_GM;
	args->commandtype = command;
	args->datasize    = insize;
	if (insize && in)
		memcpy(args->data, in, insize);
	else
		memset(args->data, 0, actual_insize);

	status = wmi_evaluate_method(HPWMI_BIOS_GUID, 0, mid, &input, &output);
	kfree(args);
	if (status) {
		pr_err("cmd 0x%02x: ACPI 传输失败, status=0x%x\n", command, status);
		return status;
	}


	obj = output.pointer;
	if (!obj || obj->type != ACPI_TYPE_BUFFER) {
		pr_err("cmd 0x%02x: 返回类型不是 buffer\n", command);
		kfree(obj);
		return -EINVAL;
	}


	hdr = (struct omen_bios_return *)obj->buffer.pointer;
	if (fwret)
		*fwret = hdr->return_code;

	int ret = 0;

	if (hdr->return_code) {
		if (hdr->return_code != HPWMI_RET_UNKNOWN_COMMAND &&
		    hdr->return_code != HPWMI_RET_UNKNOWN_CMDTYPE)
			pr_warn("cmd 0x%02x: 固件返回 0x%x\n", command,
				hdr->return_code);
		ret = -EIO;
	} else if (out && outsize) {
		int avail = obj->buffer.length - (int)sizeof(*hdr);

		if (avail > 0) {
			int n = min(outsize, avail);

			memcpy(out, (u8 *)obj->buffer.pointer + sizeof(*hdr), n);
			if (n < outsize)
				memset((u8 *)out + n, 0, outsize - n);
		} else {
			memset(out, 0, outsize);
		}
	}

	kfree(obj);
	return ret;
}


/* ── sysfs 读 ────────────────────────────────────────────────────────── */

static ssize_t fan_count_show(struct device *dev, struct device_attribute *a,
			      char *buf)
{
	u8 v = 0;
	int ret;

	ret = omen_gm_query(HPWMI_FAN_COUNT_GET_QUERY, NULL, 0, &v, sizeof(v), NULL);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", v);
}


/*
 * 性能模式（thermal profile）—— 写
 *
 * 用法：echo <字符串> > performance_mode
 *   default | balanced  → 平衡
 *   performance         → 性能/ turbo
 *   cool | quiet        → 冷静
 *   raw:<十进制>        → 直接写原始字节（绕过映射，调试用）
 *
 * ⚠ 关键：OMEN 有**两套**档位值，由 design data 的 buffer[3]
 *   （thermal policy version）决定，写错那套固件不认：
 *     v0（version==0）: default=0x00  performance=0x01  cool=0x02
 *     v1（version!=0）: default=0x30  performance=0x31  cool=0x50
 *   对照上游 omen_thermal_profile_set() 的分支逻辑。
 *   ⚠ 写入格式固定为 {0xFF, mode}（模仿 OMEN Control Center 的行为）。
 *
 * 本机实测 design data = 4a 01 35 01 01 d7 00 0c ⇒ buffer[3] = 0x01
 *   ⇒ 走 **v1** 那一套。
 */
static int omen_tp_version(void)
{
	u8 d[8] = { 0 };
	int ret;

	ret = omen_gm_query(HPWMI_GET_SYSTEM_DESIGN_DATA, NULL, 0,
			    d, sizeof(d), NULL);
	if (ret)
		return 0;	/* 读不到就按 v0 保守处理（老机型） */
	return d[3];
}

/*
 * fan_rpm —— 每个风扇的实际转速（RPM），每行一个，顺序即风扇序号
 *
 * ⚠ 这条命令 0x11 的三个易错点（我全踩了，读出的 15395 是错的）：
 *
 *  1. **必须传风扇索引作为输入**：`char fan_data[4] = { fan, 0, 0, 0 }`。
 *     我早前传 NULL（无输入），固件返回的不是风扇数据 ⇒ 数值完全不对。
 *  2. **返回的转速从偏移 [2]/[3] 取**（大端 u16），不是 [0]/[1]：
 *         fsh = fan_data[2]; fsl = fan_data[3];  →  (fsh << 8) | fsl
 *     固件返回的是**完整 4 字节**，前 2 字节是别的字段。
 *  3. **返回值就是 RPM，不要乘 100**。我曾定义 FAN_SPEED_UNIT_RPM=100 去乘，
 *     结果 15395 = 154 × 100 —— 那个 154 其实是字节错位读出来的垃圾值。
 *
 * 对照上游 hp_wmi_get_fan_speed(fan) 的实现，逐条对应。
 *
 * 对照验证：hp_wmi 自己的 hwmon 读出 fan1_input/fan2_input，
 * 本接口的对应行应与之吻合（本机实测 fan1≈1782）。
 */
static ssize_t fan_rpm_show(struct device *dev, struct device_attribute *a,
			     char *buf)
{
	u8 count = 0;
	int ret, i, off = 0;

	ret = omen_gm_query(HPWMI_FAN_COUNT_GET_QUERY, NULL, 0,
			    &count, sizeof(count), NULL);
	if (ret)
		return ret;
	if (!count)
		return sysfs_emit(buf, "0\n");

	for (i = 0; i < count; i++) {
		u8 data[4] = { (u8)i, 0, 0, 0 };
		int rpm;

		ret = omen_gm_query(HPWMI_FAN_SPEED_GET_QUERY,
				    data, sizeof(data),
				    data, sizeof(data), NULL);
		if (ret) {
			off += sysfs_emit_at(buf, off, "fan%d: EIO\n", i);
			continue;
		}

		/* 转速在 [2]/[3]，大端 u16，单位就是 RPM（不换算） */
		rpm = (data[2] << 8) | data[3];
		off += sysfs_emit_at(buf, off, "fan%d: %d\n", i, rpm);
	}
	return off;
}

/*
 * 风扇限速开关（fan_unthrottle）—— 0/1
 *
 * ⚠ 命名纠正：这条命令**不是"最大转速数值"**，而是一个布尔开关（0/1）。
 *   0x26 = 读当前是否解除限速；0x27 = 写（1=解除限速，让风扇跑满）。
 *   上游用它配合 "Fn+C 临时全速" 的用户态轮询（hp_wmi_fan_speed_max_set/reset）。
 *   我早前把它叫 "fan_max"（最大转速）——**语义误导，已改名**。
 *
 * 写：echo 1 > fan_unthrottle   解除限速（固件层面强制最高转速）
 *     echo 0 > fan_unthrottle   恢复 BIOS 曲线控制
 */
static ssize_t fan_unthrottle_show(struct device *dev,
				    struct device_attribute *a, char *buf)
{
	u8 v = 0;
	int ret;

	ret = omen_gm_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, NULL, 0,
			    &v, sizeof(v), NULL);
	if (ret)
		return ret;
	return sysfs_emit(buf, "%u\n", v);
}

static ssize_t fan_unthrottle_store(struct device *dev,
				     struct device_attribute *a,
				     const char *buf, size_t count)
{
	if (!hp_omen_writes_allowed()) {
		pr_warn_once(DRV_NAME ": 本机不在机型白名单内，写操作被拒绝（只读模式）；"
			     "确认风险后可 modprobe hp-wmi-omen allow_unsafe_writes=1\n");
		return -EPERM;
	}
	unsigned int on;
	u32 fwret = 0;
	int ret;

	if (kstrtouint(buf, 10, &on))
		return -EINVAL;
	if (on > 1)
		return -ERANGE;

	ret = omen_gm_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, &on, sizeof(on),
			    NULL, 0, &fwret);
	pr_info("set fan unthrottle -> %u (固件 rc=%u ret=%d)\n", on, fwret, ret);
	return ret ? ret : count;
}

/*
 * fan_speed —— 按百分比设置风扇转速（WMI GM 命令 0x12）
 *
 * 读：每个风扇一行 "fanN: rpm=<转速> set=<百分比|auto>"
 *     set 是本模块自己记的「我们写入的设定值」（EC 没有可读的 SFS）；
 *     ⚠ **不再输出** GM11 的 FMR：它在自动模式下由固件填写、与设定值无关。
 * 写："<选择器> <百分比>"  选择器 0=风扇1，1=风扇2，255=两个都设
 *     "50"        等价于 "255 50"
 *     "release"   等价于 "255 255" —— 把控制权交回固件（DSDT 里这条会同时清 FFFS）
 *
 * ⚠ 这是**厂商自己走的那条路**（HP Windows 工具发的也是 GM12），但它的行为
 *   **在本机尚未验证**：EC 会不会在没有"手动模式"旗标的情况下接受 SFS1/SFS2，
 *   只有实测能回答。所以测试顺序必须是：
 *     ① 记基线（fan_rpm + fan_speed）② 用 100% 当对照（应等于 fan_unthrottle=1）
 *     ③ 试中间值 ④ 最后必须发 release 收尾
 *   任何异常（转速掉到 1500 以下 / 乱跳）→ 立刻 `echo release > fan_speed`。
 */
/*
 * 我们通过 fan_speed 写入的百分比，下标 = 风扇号；0 表示未接管（交回固件）。
 *
 * 为什么要在内核侧自己记：**EC 没有可读的 SFS**（DSDT 里 GM12 只写不读），
 * 而 GM11 返回的第一个字节（FMR）在**自动模式下由固件填写** ——
 * 实测它恒定 35、与实际转速无关，第一次看到会误判成「手动 35% 没交回」
 * （2026-10-10 真踩过）。所以设定值只能自己记。
 *
 * 不加锁：sysfs 写入本就串行化，且这只是诊断信息，差一拍无害。
 */
static u8 fan_set_pct[2];

static ssize_t fan_speed_show(struct device *dev, struct device_attribute *a,
			      char *buf)
{
	u8 count = 0;
	unsigned int rpm;
	int ret, i, off = 0;

	ret = omen_gm_query(HPWMI_FAN_COUNT_GET_QUERY, NULL, 0,
			    &count, sizeof(count), NULL);
	if (ret)
		return ret;
	if (!count)
		return sysfs_emit(buf, "0\n");

	for (i = 0; i < count; i++) {
		u8 data[4] = { (u8)i, 0, 0, 0 };

		ret = omen_gm_query(HPWMI_FAN_SPEED_GET_QUERY,
				    data, sizeof(data), data, sizeof(data), NULL);
		if (ret) {
			off += sysfs_emit_at(buf, off, "fan%d: EIO\n", i);
			continue;
		}
		/*
		 * GM11 返回：[0]=FMR(固件侧的速率), [1]=FSUS, [2..3]=RPM 大端。
		 *
		 * ⚠ **不再输出 data[0]**：它在自动模式下由固件填写，实测恒定 35
		 * 而与实际转速无关 —— 会让人误判成「手动 35% 没交回」。
		 * 这里只报「转速」和「我们记下的设定值」。
		 */
		rpm = (data[2] << 8) | data[3];
		if (i < (int)ARRAY_SIZE(fan_set_pct) && fan_set_pct[i])
			off += sysfs_emit_at(buf, off,
					     "fan%d: rpm=%u set=%u%%\n",
					     i, rpm, fan_set_pct[i]);
		else
			off += sysfs_emit_at(buf, off,
					     "fan%d: rpm=%u set=auto\n", i, rpm);
	}
	return off;
}

static ssize_t fan_speed_store(struct device *dev, struct device_attribute *a,
			       const char *buf, size_t count)
{
	if (!hp_omen_writes_allowed()) {
		pr_warn_once(DRV_NAME ": 本机不在机型白名单内，写操作被拒绝（只读模式）；"
			     "确认风险后可 modprobe hp-wmi-omen allow_unsafe_writes=1\n");
		return -EPERM;
	}
	unsigned int sel = 0xFF, val = 0;
	unsigned int on;
	u32 fwret = 0;
	u8 data[2];
	int n, ret;

	if (sysfs_streq(buf, "release")) {
		/*
		 * 【2026-10-10 实测修正】原实现发 (0xFF, 0xFF) —— **这是错的**。
		 * 实测：EC 把 SFS=255 当作「最大速度」⇒ 风扇被钉在满速
		 * （fan0 4990 RPM），而且**没有任何 ASL 路径**能把它设回自动
		 * —— GM12 的 0xFF/0xFF 分支只清 FFFS，根本不动 SFS1/SFS2。
		 * ⇒ 用户会遇到「风扇一直满速不回落」。已实测确认。
		 *
		 * 「交回固件」的值是 **0**，且本机实测需要**两步**才完全恢复：
		 *   ① GM12 (sel=0xFF, val=0)  → SFS1 = SFS2 = 0（EC 重用自己的曲线）
		 *   ② GM27 写 1 → 写 0       → 强制 EC 重新评估（只做①时转速下降很慢）
		 * 实测结果：4990 → 1790 RPM（BIOS 曲线在 50°C 的值）。
		 */
		data[0] = 0xFF;
		data[1] = 0x00;
		omen_gm_query(HPWMI_SET_FAN_SPEED, data, sizeof(data), NULL, 0, &fwret);
		pr_info("fan release: SFS1=SFS2=0 (固件 rc=%u)\n", fwret);

		on = 1;
		omen_gm_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, &on, sizeof(on), NULL, 0, &fwret);
		msleep(1200);
		on = 0;
		omen_gm_query(HPWMI_FAN_SPEED_MAX_SET_QUERY, &on, sizeof(on), NULL, 0, &fwret);
		pr_info("fan release: unthrottle 1->0 完成 —— 风扇应回落到曲线值\n");
		fan_set_pct[0] = 0;
		fan_set_pct[1] = 0;
		return count;
	} else {
		n = sscanf(buf, "%u %u", &sel, &val);
		if (n == 1) {		/* 只给一个数 = 两个风扇都设成这个百分比 */
			val = sel;
			sel = 0xFF;
		} else if (n != 2) {
			return -EINVAL;
		}
	}

	if (sel != 0 && sel != 1 && sel != 0xFF)
		return -ERANGE;
	if (val > 100 && val != 0xFF)
		return -ERANGE;
	if (val == 0xFF)
		pr_warn("fan_speed: 显式写 0xFF —— EC 会当作「最大速度」且无 ASL 退路，"
			"想交回固件请 `echo release > fan_speed`\n");

	data[0] = (u8)sel;
	data[1] = (u8)val;
	ret = omen_gm_query(HPWMI_SET_FAN_SPEED, data, sizeof(data),
			    NULL, 0, &fwret);
	pr_info("set fan speed sel=%u val=%u (固件 rc=%u ret=%d)\n",
		sel, val, fwret, ret);

	/* 记下设定值供 show 显示（EC 侧没有可读的 SFS） */
	if (!ret) {
		if (sel == 0xFF) {
			fan_set_pct[0] = (u8)val;
			fan_set_pct[1] = (u8)val;
		} else if (sel < ARRAY_SIZE(fan_set_pct)) {
			fan_set_pct[sel] = (u8)val;
		}
	}
	return ret ? ret : count;
}

static struct device_attribute dev_attr_fan_speed =
	__ATTR(fan_speed, 0664, fan_speed_show, fan_speed_store);

static ssize_t design_data_show(struct device *dev, struct device_attribute *a,
				char *buf)
{
	u8 v[8] = { 0 };
	int ret, i, off = 0;

	ret = omen_gm_query(HPWMI_GET_SYSTEM_DESIGN_DATA, NULL, 0, v, sizeof(v), NULL);
	if (ret)
		return ret;
	for (i = 0; i < (int)sizeof(v); i++)
		off += sysfs_emit_at(buf, off, "%02x ", v[i]);
	return off + sysfs_emit_at(buf, off, "\n");
}

/*
 * profile_version —— 只读：报告 thermal policy version 与对应的档位值表
 *
 * 上游 omen_get_thermal_policy_version() 读 design data 的 buffer[3]，
 * 用它决定用 v0 还是 v1 的 profile 值。写错那套固件不认 —— 所以这个接口
 * 让用户态/脚本**先查再用**，不用猜。
 */
static ssize_t profile_version_show(struct device *dev,
				    struct device_attribute *a, char *buf)
{
	int v = omen_tp_version();

	if (v)
		return sysfs_emit(buf,
			"v1 default=0x30 performance=0x31 cool=0x50 (design_data[3]=0x%02x)\n", v);
	return sysfs_emit(buf,
		"v0 default=0x00 performance=0x01 cool=0x02 (design_data[3]=0x00)\n");
}

static ssize_t gpu_modes_show(struct device *dev, struct device_attribute *a,
			      char *buf)
{
	struct omen_gpu_power_modes m = { 0 };
	int ret;

	ret = omen_gm_query(HPWMI_GET_GPU_THERMAL_MODES, NULL, 0, &m, sizeof(m), NULL);
	if (ret)
		return ret;
	return sysfs_emit(buf, "ctgp=%u ppab=%u dstate=%u slowdown=%u\n",
			  m.ctgp_enable, m.ppab_enable, m.dstate,
			  m.gpu_slowdown_temp);
}

/*
 * ⚠⚠ fan_speed_victus_s —— 本机（OMEN）不适用，仅 Victus S 系列。
 *
 *   底层是 0x2D / 0x2E 这对命令；上游 hp_wmi_get/set_fan_speed_victus_s()
 *   只在 Victus S 的 DMI 白名单里调用。本机读它返回 EIO（固件不认识该命令）
 *   —— 这是正常现象，不是 bug。
 *
 *   OMEN 系没有公开的"直接写风扇 RPM"命令：上游对 OMEN 只提供
 *   profile 档位（0x1A）与解除限速（0x27）。
 *
 *   初版我把它叫 "fan_speed"（听起来像通用风扇控制），语义误导，已改名。
 */
static ssize_t fan_speed_victus_s_show(struct device *dev, struct device_attribute *a,
			      char *buf)
{
	u16 v[2] = { 0 };
	int ret, i, off = 0;

	ret = omen_gm_query(HPWMI_VICTUS_S_FAN_SPEED_GET, NULL, 0, v, sizeof(v), NULL);
	if (ret)
		return ret;
	for (i = 0; i < 2; i++)
		off += sysfs_emit_at(buf, off, "%s%u", i ? " " : "", v[i]);
	return off + sysfs_emit_at(buf, off, "\n");
}

/* 风扇曲线表：先读表头拿风扇数，再读整表 */
static ssize_t fan_table_show(struct device *dev, struct device_attribute *a,
			       char *buf)
{
	struct omen_fan_table_header hdr;
	u8 *table;
	int ret, i, off = 0, entries, bytes;

	ret = omen_gm_query(HPWMI_VICTUS_S_GET_FAN_TABLE, NULL, 0,
			    &hdr, sizeof(hdr), NULL);
	if (ret)
		return ret;
	if (hdr.num_fans == 0)
		return sysfs_emit(buf, "num_fans=0\n");

	entries = hdr.num_fans;
	bytes = sizeof(hdr) + entries * (int)sizeof(struct omen_fan_table_entry);
	table = kmalloc(bytes, GFP_KERNEL);
	if (!table)
		return -ENOMEM;

	ret = omen_gm_query(HPWMI_VICTUS_S_GET_FAN_TABLE, NULL, 0, table, bytes, NULL);
	if (ret) {
		kfree(table);
		return ret;
	}

	off += sysfs_emit_at(buf, off, "num_fans=%u\n", hdr.num_fans);
	for (i = 0; i < entries; i++) {
		struct omen_fan_table_entry *e =
			(struct omen_fan_table_entry *)(table + sizeof(hdr)) + i;

		off += sysfs_emit_at(buf, off, "%u %u %u\n",
				     e->cpu_rpm * FAN_SPEED_UNIT_RPM,
				     e->gpu_rpm * FAN_SPEED_UNIT_RPM,
				     e->noise_db);
	}
	kfree(table);
	return off;
}

/* ── sysfs 写 ────────────────────────────────────────────────────────── */

/* 风扇目标转速：写入 "<rpm1> <rpm2>"，单位 RPM */
static ssize_t fan_speed_victus_s_store(struct device *dev, struct device_attribute *at,
			       const char *buf, size_t count)
{
	if (!hp_omen_writes_allowed()) {
		pr_warn_once(DRV_NAME ": 本机不在机型白名单内，写操作被拒绝（只读模式）；"
			     "确认风险后可 modprobe hp-wmi-omen allow_unsafe_writes=1\n");
		return -EPERM;
	}
	unsigned int r1, r2, r3;
	u8 data[2];
	u32 fwret = 0;
	int ret;

	if (sscanf(buf, "%u %u %u", &r1, &r2, &r3) < 2)
		return -EINVAL;
	if (r1 / FAN_SPEED_UNIT_RPM > 255 || r2 / FAN_SPEED_UNIT_RPM > 255)
		return -ERANGE;

	data[0] = r1 / FAN_SPEED_UNIT_RPM;
	data[1] = r2 / FAN_SPEED_UNIT_RPM;

	ret = omen_gm_query(HPWMI_VICTUS_S_FAN_SPEED_SET, data, sizeof(data),
			    NULL, 0, &fwret);
	pr_info("set fan speed -> %u/%u RPM (固件 rc=%u ret=%d)\n", r1, r2, fwret, ret);
	return ret ? ret : count;
}

/* GPU 功耗模式：写入 "<ctgp> <ppab> <dstate>"，slowdown_temp 原样回填 */
static ssize_t gpu_modes_store(struct device *dev, struct device_attribute *at,
			       const char *buf, size_t count)
{
	if (!hp_omen_writes_allowed()) {
		pr_warn_once(DRV_NAME ": 本机不在机型白名单内，写操作被拒绝（只读模式）；"
			     "确认风险后可 modprobe hp-wmi-omen allow_unsafe_writes=1\n");
		return -EPERM;
	}
	struct omen_gpu_power_modes cur, m;
	unsigned int ctgp, ppab, dstate;
	u32 fwret = 0;
	int ret;

	if (sscanf(buf, "%u %u %u", &ctgp, &ppab, &dstate) != 3)
		return -EINVAL;
	if (ctgp > 1 || ppab > 1 || dstate > 3)
		return -ERANGE;

	/* 必须先读出 slowdown_temp 再写回 —— 内核实现也是这样做的 */
	ret = omen_gm_query(HPWMI_GET_GPU_THERMAL_MODES, NULL, 0, &cur, sizeof(cur), NULL);
	if (ret) {
		pr_err("读当前 GPU 功耗模式失败(%d)，为避免把 slowdown 温度写成 0，放弃写入\n",
		       ret);
		return ret;
	}

	m.ctgp_enable = ctgp;
	m.ppab_enable = ppab;
	m.dstate = dstate;
	m.gpu_slowdown_temp = cur.gpu_slowdown_temp;

	ret = omen_gm_query(HPWMI_SET_GPU_THERMAL_MODES, &m, sizeof(m),
			    NULL, 0, &fwret);
	pr_info("set gpu modes ctgp=%u ppab=%u dstate=%u (固件 rc=%u ret=%d)\n",
		ctgp, ppab, dstate, fwret, ret);
	return ret ? ret : count;
}


static ssize_t performance_mode_store(struct device *dev,
				      struct device_attribute *a,
				      const char *buf, size_t count)
{
	if (!hp_omen_writes_allowed()) {
		pr_warn_once(DRV_NAME ": 本机不在机型白名单内，写操作被拒绝（只读模式）；"
			     "确认风险后可 modprobe hp-wmi-omen allow_unsafe_writes=1\n");
		return -EPERM;
	}
	char str[32];
	char *s = str, *e;
	int v = omen_tp_version();
	unsigned int raw;
	u8 data[2] = { 0xFF, 0 };
	u32 fwret = 0;
	int ret;

	/* 只取第一行，忽略末尾换行与可能的 \r（Windows 编辑过的文件） */
	if (!sscanf(buf, "%31[^\r\n]", str))
		return -EINVAL;
	(void)s; (void)e;

	/* 显式原始字节（调试/高级用） */
	if (!strncmp(s, "raw:", 4)) {
		if (kstrtouint(s + 4, 0, &raw))
			return -EINVAL;
		if (raw > 0xff)
			return -ERANGE;
		data[1] = (u8)raw;
		goto send;
	}

	if (!strcasecmp(s, "default") || !strcasecmp(s, "balanced")) {
		data[1] = v ? 0x30 : 0x00;
	} else if (!strcasecmp(s, "performance") || !strcasecmp(s, "turbo")) {
		data[1] = v ? 0x31 : 0x01;
	} else if (!strcasecmp(s, "cool") || !strcasecmp(s, "quiet")) {
		data[1] = v ? 0x50 : 0x02;
	} else {
		/* 也接受纯数字（1/2/3 = default/performance/cool），方便脚本 */
		if (!kstrtouint(s, 10, &raw)) {
			data[1] = (u8)(raw <= 3
				? (v ? (u8[]){ 0x30, 0x31, 0x30, 0x50 }[raw]
				     : (u8[]){ 0x00, 0x01, 0x02, 0x00 }[raw])
				: raw);
			goto send;
		}
		return -EINVAL;
	}

send:
	ret = omen_gm_query(HPWMI_SET_PERFORMANCE_MODE, data, sizeof(data),
			    NULL, 0, &fwret);
	pr_info("set profile '%s' -> 0x%02x (tp_version=%d, 固件 rc=%u ret=%d)\n",
		str, data[1], v, fwret, ret);
	return ret ? ret : count;
}

/*
 * probe_all —— 只读地逐条试调所有已知命令，把固件的接受情况打进 dmesg。
 * 这是探测阶段的工具：不做任何写入，只告诉我们固件认哪些命令。
 */
static ssize_t probe_all_store(struct device *dev, struct device_attribute *a,
			       const char *buf, size_t count)
{
	struct omen_gpu_power_modes g;
	struct omen_fan_table_header th;
	u16 fan[2] = { 0 };
	u8 fc = 0, fmax = 0, design[8] = { 0 };
	u32 fwret;
	int i;
	static const u32 reads[] = {
		HPWMI_FAN_COUNT_GET_QUERY, HPWMI_FAN_SPEED_GET_QUERY,
		HPWMI_FAN_SPEED_MAX_GET_QUERY, HPWMI_GET_SYSTEM_DESIGN_DATA,
		HPWMI_GET_GPU_THERMAL_MODES, HPWMI_VICTUS_S_FAN_SPEED_GET,
		HPWMI_VICTUS_S_GET_FAN_TABLE,
	};

	pr_info("=== 开始只读探测（不做任何写入）===\n");
	for (i = 0; i < (int)ARRAY_SIZE(reads); i++) {
		fwret = 0;
		if (!omen_gm_query(reads[i], NULL, 0, NULL, 0, &fwret))
			pr_info("cmd 0x%02x : 固件接受\n", reads[i]);
		else
			pr_info("cmd 0x%02x : 被拒，固件返回 0x%x\n", reads[i], fwret);
	}

	if (!omen_gm_query(HPWMI_FAN_COUNT_GET_QUERY, NULL, 0, &fc, sizeof(fc), NULL))
		pr_info("  风扇数量 = %u\n", fc);
	if (!omen_gm_query(HPWMI_FAN_SPEED_GET_QUERY, NULL, 0, fan, sizeof(fan), NULL))
		pr_info("  风扇转速 = %u RPM\n", fan[0]);
	if (!omen_gm_query(HPWMI_FAN_SPEED_MAX_GET_QUERY, NULL, 0, &fmax, sizeof(fmax), NULL))
		pr_info("  最大风扇模式 = %u\n", fmax);
	if (!omen_gm_query(HPWMI_GET_SYSTEM_DESIGN_DATA, NULL, 0, design, sizeof(design), NULL))
		pr_info("  设计数据 = %02x %02x %02x %02x %02x %02x %02x %02x\n",
			design[0], design[1], design[2], design[3],
			design[4], design[5], design[6], design[7]);
	if (!omen_gm_query(HPWMI_GET_GPU_THERMAL_MODES, NULL, 0, &g, sizeof(g), NULL))
		pr_info("  GPU 功耗模式: ctgp=%u ppab=%u dstate=%u slowdown=%u\n",
			g.ctgp_enable, g.ppab_enable, g.dstate, g.gpu_slowdown_temp);

	if (!omen_gm_query(HPWMI_VICTUS_S_FAN_SPEED_GET, NULL, 0, fan, sizeof(fan), NULL))
		pr_info("  >> Victus S 风扇目标命令可用: fan1=%u fan2=%u RPM\n",
			fan[0], fan[1]);
	else
		pr_info("  >> Victus S 风扇目标命令被拒（本机风扇只能自动）\n");

	if (!omen_gm_query(HPWMI_VICTUS_S_GET_FAN_TABLE, NULL, 0, &th, sizeof(th), NULL))
		pr_info("  >> 风扇曲线表可用: num_fans=%u\n", th.num_fans);
	else
		pr_info("  >> 风扇曲线表被拒\n");

	pr_info("=== 探测结束 ===\n");
	return count;
}

/* ── sysfs 装配 ──────────────────────────────────────────────────────── */

/*
 * 用 device_attribute + ATTRIBUTE_GROUPS（与上游 drivers/platform/x86/hp/hp-wmi.c
 * 完全相同的做法）。
 *
 * ⚠ 曾试过 kobject + kobj_attribute + `(struct attribute **)` 强转 ——
 *   ATTRIBUTE_GROUPS 的 _Generic 只接受 `struct attribute **` 与
 *   `const struct attribute *const *`，kobj_attribute 两边都不匹配；
 *   硬转属内存布局 hack，sysfs_create_group 遍历时读到位移数据，
 *   触发内核 WARNING 并**卡死在 init 里**（initstate 永远 coming）。
 *   ⇒ 必须走 device 路线，不要再碰这个 hack。
 */
/*
 * sysfs 属性定义
 *
 * 权限问题：DEVICE_ATTR_WO 的 mode 硬编码 0200（ls 显示 --w-------），
 * DEVICE_ATTR_RW 是 0644（仅属主可写）。这意味着**普通用户无法写任何接口**
 * —— 而用户态 GUI（Tauri 面板）是普通用户跑的，每改一次设置都要 sudo，
 * 而且 GUI 自己还得实现提权。
 *
 * 故这里用 __ATTR_RW_MODE / __ATTR_RO_MODE 自己包一层，把可写接口设为
 * 0664（属主 + 属组可写），读接口 0444。
 *
 * 为什么不能用 0666：**内核明确禁止 other-writable**。
 * include/linux/sysfs.h 的 VERIFY_OCTAL_PERMISSIONS 里有这条：
 *     "OTHER_WRITABLE?  Generally considered a bad idea."
 *     BUILD_BUG_ON_ZERO((perms) & 2)
 * 传 0666 会直接编译失败（static assertion failed: "(0666) & 2 is true"）。
 * => 0666 在内核 sysfs 里根本不可能，必须走属组授权。
 *
 * 属组由 udev 规则设为 plugdev（见 99-omen-wmi.rules）：
 * 该组成员（非 root）即可读写，既满足 GUI 需求，又不给 other 写权限。
 */
#define OmenATTR_RO(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RO_MODE(_name, 0444)
#define OmenATTR_RW(_name) \
	struct device_attribute dev_attr_##_name = __ATTR_RW_MODE(_name, 0664)
/*
 * write-only 属性。
 *
 * ⚠⚠ mode **不能带other 的读位**，否则内核在 device_create_file() 时判定
 *    "声明了可读却没有 show 回调"，发出：
 *        Attribute performance_mode: read permission without 'show'
 *        WARNING: drivers/base/core.c:3117 at device_create_file
 *   （实测踩到过，功能虽不受影响，但是内核明确指出的缺陷）
 *
 * 0x204 = rw-------：仅属主可读写，仍需属组授权时请另加 rw-rw---- 之外的方案。
 * ⇒ 这里给 0660：属主 rw + 属组 w + other 无权限，且**不含 other 读位**。
 *   属组（plugdev）仍可写，普通 GUI 依旧能用。
 */
#define OmenATTR_WO(_name) \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_name, 0660, NULL, _name##_store)

/*
 * ── EC 直访：替代内核自带的 ec_sys ────────────────────────────────
 *
 * 为什么要另做一份：
 *   Secure Boot 开启 + lockdown=integrity 时，ec_sys **没法**做成可写 ——
 *   它的 write_support 是"unsafe module parameter"，内核直接拒绝：
 *       Lockdown: insmod: unsafe module parameters is restricted
 *       ec_sys: `1' invalid for parameter `write_support'
 *   （实测：补签后模块能加载，但 write_support=1 被丢，insmod 整体 EPERM）
 *   而 ec_read() / ec_write() 是内核**普通导出符号**（Module.symvers 里是
 *   EXPORT_SYMBOL，非 _GPL），没有这层参数门控 ⇒ 由本模块直接调。
 *
 * ⚠ 这等于**绕过了内核为 EC 写设的安全闸门**。是有意为之：
 *   本机 EC 0x2E/0x2F 已确认是风扇 PWM 占空比，要自定义曲线就必须能写。
 *   代价是**这里没有内核级护栏**，写错偏移就是写错。
 *   因此：① 权限设 0600（仅 root）；② store 侧做严格范围校验；
 *        ③ 每次写都 pr_info 记录 addr/val/ret，可审计。
 *
 * 接口：
 *   echo 46       > ec_byte     # 选定偏移（支持十进制与 0x 前缀）
 *   cat ec_byte                 # 读出该偏移当前值
 *   echo 46 36    > ec_byte     # 直接写：偏移 46 ← 值 36
 *   echo 0x2e 0x24 > ec_byte    # 十六进制亦可
 */
/* 部分发行版头文件未声明这两个（只保证符号存在），显式给原型。 */
extern int ec_read(u8 addr, u8 *val);
extern int ec_write(u8 addr, u8 val);

static unsigned int omen_ec_addr;	/* 当前选定的偏移（供 show 用） */

static ssize_t ec_byte_show(struct device *dev,
			    struct device_attribute *a, char *buf)
{
	u8 val = 0;
	int ret;

	if (omen_ec_addr > 0xFF)
		return -EINVAL;

	ret = ec_read((u8)omen_ec_addr, &val);
	if (ret)
		return ret;

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t ec_byte_store(struct device *dev, struct device_attribute *a,
			     const char *buf, size_t count)
{
	if (!hp_omen_writes_allowed()) {
		pr_warn_once(DRV_NAME ": 本机不在机型白名单内，写操作被拒绝（只读模式）；"
			     "确认风险后可 modprobe hp-wmi-omen allow_unsafe_writes=1\n");
		return -EPERM;
	}
	unsigned int addr = 0, val = 0;
	int n, ret;

	n = sscanf(buf, "%i %i", &addr, &val);	/* %i 认 0x 前缀 */
	if (n < 1)
		return -EINVAL;
	if (addr > 0xFF)
		return -ERANGE;

	if (n == 1) {				/* 只选地址 */
		omen_ec_addr = addr;
		pr_info("hp_omen: ec addr -> 0x%02x\n", addr);
		return count;
	}

	if (val > 0xFF)
		return -ERANGE;

	ret = ec_write((u8)addr, (u8)val);
	pr_info("hp_omen: ec write 0x%02x = 0x%02x (ret=%d)\n", addr, val, ret);
	if (ret)
		return ret;

	omen_ec_addr = addr;
	return count;
}

/*
 * EC 属性：**权限 0600 —— 仅 root**。
 * 不能用 OmenATTR_RW（0664）：那会让 plugdev 组也能写 EC，
 * 而 EC 能改风扇/电源/充放电，写错字节后果不可预期。
 */
#define OmenATTR_EC(_name) \
	struct device_attribute dev_attr_##_name = \
		__ATTR(_name, 0600, _name##_show, _name##_store)

static OmenATTR_EC(ec_byte);

static OmenATTR_RO(fan_count);
static OmenATTR_RO(fan_rpm);
static OmenATTR_RW(fan_unthrottle);
static OmenATTR_RO(profile_version);
static OmenATTR_RO(design_data);
static OmenATTR_RW(gpu_modes);
static OmenATTR_RW(fan_speed_victus_s);
static OmenATTR_RO(fan_table);
static OmenATTR_WO(performance_mode);
static OmenATTR_WO(probe_all);


/* ── 驱动 ────────────────────────────────────────────────────────────── */

/*
 * 用 platform device 暴露 sysfs（而不是自建 kobject）。
 * sysfs 位置：/sys/devices/platform/omen-wmi/
 *
 * 为什么不用 kobject + kobj_attribute：
 *   ATTRIBUTE_GROUPS 的 _Generic 只接受 `struct attribute **` 与
 *   `const struct attribute *const *`，而 kobj_attribute 两边都不匹配；
 *   用 `(struct attribute **)` 强转是**内存布局 hack**，
 *   sysfs_create_group 遍历时读到错位数据 → 内核 WARNING 并**卡死在 init**
 *   （表现为 initstate 永远 coming、lsmod 里 Loading、sysfs 目录不出现）。
 *   ⇒ 只能走 device 路线。
 */
static struct platform_device *hp_omen_pdev;

static int __init hp_omen_init(void)
{
	int ret;

	/* init 分阶段日志：模块卡住时靠它定位（dmesg | grep hp_omen） */
	pr_info("hp_omen: [1/3] init entered\n");

	/*
	 * ⚠ init 里**绝不做固件交互**。实测在此调 wmi_has_guid() 会让 init
	 *   永不返回（initstate 永远 coming）。探测一律推迟到 sysfs 读写时。
	 */
	pr_info("hp_omen: [2/3] WMI probe deferred to first use\n");
	pr_info("hp_omen: writes %s (board %s, vendor %s)\n",
		hp_omen_writes_allowed() ? "allowed" : "BLOCKED - read-only on unknown board",
		dmi_get_system_info(DMI_BOARD_NAME) ?: "?",
		dmi_get_system_info(DMI_SYS_VENDOR) ?: "?");

	hp_omen_pdev = platform_device_register_simple("omen-wmi", -1, NULL, 0);
	if (IS_ERR(hp_omen_pdev)) {
		ret = PTR_ERR(hp_omen_pdev);
		pr_err("platform_device_register_simple failed: %d\n", ret);
		return ret;
	}

	/*
	 * platform_device_register_simple 只登记设备，**不会**挂 sysfs 属性。
	 * 必须逐个 device_create_file（该 API 只接单个 dev_attr，
	 * 不接受 attribute_group）。挂在 pdev->dev 上。
	 */
	{
		static struct device_attribute *dev_attrs[] = {
			&dev_attr_fan_count,
			&dev_attr_fan_rpm,
			&dev_attr_fan_unthrottle,
			&dev_attr_fan_speed,
			&dev_attr_profile_version,
			&dev_attr_design_data,
			&dev_attr_gpu_modes,
			&dev_attr_fan_speed_victus_s,
			&dev_attr_fan_table,
			&dev_attr_performance_mode,
			&dev_attr_probe_all,
			&dev_attr_ec_byte,
			NULL
		};
		int i, n = 0;

		for (i = 0; dev_attrs[i]; i++) {
			ret = device_create_file(&hp_omen_pdev->dev,
						(const struct device_attribute *)dev_attrs[i]);
			if (ret)
				pr_err("  create_file[%d] failed: %d\n", i, ret);
			else
				n++;
		}
		pr_info("hp_omen: sysfs files created: %d/%d\n", n, i);
	}

	pr_info("hp_omen: [3/3] device registered\n");
	pr_info("hp_omen: sysfs at /sys/devices/platform/omen-wmi/\n");
	pr_info("hp_omen: probe with: echo 1 > /sys/devices/platform/omen-wmi/probe_all\n");
	return 0;
}

static void __exit hp_omen_exit(void)
{
	platform_device_unregister(hp_omen_pdev);
	pr_info("hp_omen: unloaded\n");
}
module_init(hp_omen_init);
module_exit(hp_omen_exit);

MODULE_LICENSE("GPL");
/*
 * ⚠ description / author 必须**纯 ASCII**。
 *   实测写成中文后，`readelf -p .modinfo` 输出乱码，
 *   而 kmod 的 modprobe 在解析 .modinfo 时会**段错误崩溃**
 *   （file/modinfo 都正常，只有 modprobe 崩 —— 极易误判为内核问题）。
 *   内核里所有 MODULE_* 描述性字段都不要放非 ASCII。
 */
MODULE_AUTHOR("OMEN Control Project");
MODULE_DESCRIPTION("Send OMEN private WMI commands to HP firmware: performance mode / GPU TGP / fan");
MODULE_VERSION("0.1.0");