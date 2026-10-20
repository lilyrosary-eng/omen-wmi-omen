#!/bin/bash
# 开机时确保**当前内核**有一份可加载的 hp-wmi-omen.ko。
#
# 背景（用户实测：每次内核升级后都要手动 install.sh --build）
# ------------------------------------------------------------
# 模块装在 /lib/modules/<ver>/extra/ 下，内核一升级目录名就变了 ⇒ 开机时
# systemd-modules-load 找不到模块，omen-fand.service 的
#   ConditionPathExists=/sys/devices/platform/omen-wmi/fan_speed
# 直接判 no ⇒ 服务被 **skip**（不是 failed，所以界面上只看到"开关打不开"）。
#
# 本脚本由 omen-wmi-build.service（oneshot，排在本服务之前）调用：
#   · .ko 在  → modprobe 就完事（快路径，正常开机零额外成本）
#   · .ko 不在 → 用 /usr/src 里的源码现场编译 + 同一把 MOK 签名 + 安装 + 加载
#
# 设计原则：**任何失败都不阻断开机**（一律 exit 0），原因写进 journal：
#   journalctl -u omen-wmi-build -b
set -u

MOD=hp-wmi-omen
KVER=$(uname -r)
KO="/lib/modules/$KVER/extra/$MOD.ko"
SRC="/usr/src/$MOD-src"
KEY=/etc/hp-wmi-omen/MOK.key
CRT=/etc/hp-wmi-omen/MOK.der
KDIR="/lib/modules/$KVER/build"
NODE=/sys/devices/platform/omen-wmi/fan_speed

log() { echo "omen-wmi-ensure: $*"; }

# ── 快路径 ────────────────────────────────────────────────────────────
if [ -f "$KO" ]; then
    modprobe "$MOD" 2>/dev/null || log "modprobe 失败（.ko 在但加载不了 —— 查签名/MOK）"
    if [ ! -e "$NODE" ] && [ -d /sys/module/hp_wmi_omen ]; then
        # 开机时从 **initramfs** 加载了旧副本（MODULES=most 会把 extra/ 打进去），
        # 内存里的版本可能比磁盘旧（实测 srcversion 不一致）⇒ 没有 fan_speed。
        # 换掉它：rmmod 后 modprobe 会读磁盘上的新版本。
        log "已加载但无 fan_speed —— 疑似 initramfs 旧副本，重载磁盘版本"
        if rmmod hp_wmi_omen 2>/dev/null; then
            modprobe "$MOD" || log "重载失败"
        else
            log "rmmod 失败（模块被占用？）"
        fi
    fi
    if [ ! -e "$NODE" ]; then
        log "已加载但没有 fan_speed 节点 —— .ko 可能是旧版，需重新 install.sh --build"
    fi
    exit 0
fi

log "内核 $KVER 缺少 $MOD.ko，尝试重建"

# ── 前置条件 ──────────────────────────────────────────────────────────
command -v gcc >/dev/null 2>&1 \
    || { log "缺 gcc，跳过（sudo apt install build-essential）"; exit 0; }
[ -d "$KDIR" ] \
    || { log "缺内核头文件 $KDIR，跳过（sudo apt install linux-headers-$KVER）"; exit 0; }
[ -f "$KEY" ] && [ -f "$CRT" ] \
    || { log "缺 MOK 密钥 $KEY，跳过（先跑 install.sh --genkey）"; exit 0; }
[ -f "$SRC/$MOD.c" ] && [ -f "$SRC/Makefile" ] \
    || { log "缺源码 $SRC，跳过"; exit 0; }

SIGN="$(readlink -f "$KDIR")/scripts/sign-file"
[ -x "$SIGN" ] || { log "找不到签名工具 $SIGN，跳过"; exit 0; }

# ── 编译（必须在原生文件系统，NTFS 上 kbuild 会失败）────────────────────
BUILD="/usr/src/$MOD-build"
rm -rf "$BUILD"
mkdir -p "$BUILD"
cp "$SRC/$MOD.c" "$SRC/Makefile" "$BUILD/" || { log "拷贝源码失败"; exit 0; }

if ! make -C "$KDIR" M="$BUILD" modules >/tmp/omen-wmi-build.log 2>&1; then
    log "编译失败，详见 /tmp/omen-wmi-build.log"
    exit 0
fi
[ -f "$BUILD/$MOD.ko" ] || { log "编译没有产出 .ko"; exit 0; }

"$SIGN" sha256 "$KEY" "$CRT" "$BUILD/$MOD.ko" || { log "签名失败"; exit 0; }

install -D -m 0644 "$BUILD/$MOD.ko" "$KO" || { log "安装 .ko 失败"; exit 0; }
depmod -a "$KVER"

modprobe "$MOD" || { log "modprobe 失败（签名未被 MOK 接受？）"; exit 0; }
[ -e "$NODE" ] && log "已为 $KVER 重建并加载，fan_speed 就绪" \
               || log "加载成功但仍无 fan_speed 节点"
exit 0
