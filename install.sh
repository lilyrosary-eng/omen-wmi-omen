#!/bin/bash
# ============================================================================
# hp-wmi-omen 安装脚本 —— 直接调用 HP 固件私有 WMI 接口的内核模块
#
# 用法（必须 root）：
#   sudo bash install.sh --genkey     生成签名密钥并导入 MOK（**需要重启一次**）
#   sudo bash install.sh --build      编译 + 签名 + 安装 + 加载
#   sudo bash install.sh --probe      加载后做只读探测并打印结果
#   sudo bash install.sh --uninstall  卸载并移除模块
#
# 关于 Secure Boot
# ----------------
# 本机 Secure Boot 是开启的，且处于 Lockdown integrity 模式，
# **未签名的模块一律拒绝加载**（我们此前用 ec_sys 验证过这条）。
#
# 但**不需要关闭 Secure Boot**：shim 的 MOK（Machine Owner Key）机制可以在
# 保留 Secure Boot 的前提下额外信任我们自己的证书。流程是：
#   1) --genkey：生成密钥对，把证书导入 MOK（此时只是"排队"）
#   2) 重启一次：shim 在启动时弹出蓝色界面让你确认导入（设一个一次性密码）
#   3) --build：用同一个密钥给模块签名 → 加载
#
# 之后每次重新编译模块只需要再 --build，不必再重启。
# ============================================================================
set -euo pipefail

MODULE=hp-wmi-omen
SRC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="/usr/src/${MODULE}-src"
KEY_DIR="/etc/hp-wmi-omen"
KEY="${KEY_DIR}/MOK.key"
CRT="${KEY_DIR}/MOK.der"
KVER="$(uname -r)"

# sysfs 位置（platform device 路线，见 hp-wmi-omen.c 注释）
SYSFS="/sys/devices/platform/omen-wmi"

die()  { echo "[错误] $*" >&2; exit 1; }
info() { echo "[信息] $*"; }
ok()   { echo "[完成] $*"; }
warn() { echo "[警告] $*" >&2; }

# 判断证书是否为 **DER** 格式。
# 依据：PEM 文本以 "-----BEGIN CERTIFICATE-----" 开头；DER 则是裸二进制，
# 且能被 `openssl x509 -inform DER` 成功解析。
cert_is_der() {
    local f="$1"
    [ -f "$f" ] || return 1
    head -c 27 "$f" 2>/dev/null | grep -q "BEGIN CERTIFICATE" && return 1
    openssl x509 -inform DER -in "$f" -noout >/dev/null 2>&1
}

# --status 是纯只读，且属组 plugdev 已授权读 ⇒ 允许非 root 运行。
# 用户态 GUI 要频繁查状态，若这里强制 sudo 会逼 GUI 每次提权。
case "${1:-}" in
    --status) : ;;   # 免 sudo
    *) [ "$(id -u)" -eq 0 ] || die "请用 sudo 运行：sudo bash $0 $*" ;;
esac
[ -f "$SRC_DIR/${MODULE}.c" ] || die "在 $SRC_DIR 找不到 ${MODULE}.c"

# ───────────────────────────────────────────────────────────── 生成密钥
gen_key() {
    if [ -f "$KEY" ] && [ -f "$CRT" ] && cert_is_der "$CRT"; then
        info "密钥已存在且格式正确，跳过生成：$KEY"
    else
        if [ -f "$CRT" ] && ! cert_is_der "$CRT"; then
            info "发现 $CRT 是 PEM 文本格式（mokutil 只接受 DER），重新生成…"
        fi
        info "生成 MOK 密钥对…"
        mkdir -p "$KEY_DIR"
        chmod 0700 "$KEY_DIR"
        # ⚠ `-outform DER` 不能省：openssl req 的 -out 默认写 **PEM 文本**，
        #   而 mokutil 只认 DER，会报
        #   "MOK.der is not a valid x509 certificate in DER format"。
        #   文件名写着 .der 不等于内容是 DER —— 必须显式指定输出格式。
        openssl req -new -x509 -newkey rsa:2048 -nodes \
            -keyout "$KEY" -out "$CRT" -outform DER -days 3650 \
            -subj "/CN=hp-wmi-omen MOK/" 2>/dev/null
        chmod 0600 "$KEY"; chmod 0644 "$CRT"
        cert_is_der "$CRT" || die "生成的证书不是 DER 格式，请检查 openssl 版本"
        ok "已生成 $KEY（DER 格式，已校验）"
    fi

    info "导入 MOK（这一步只是排队，需要重启后由 shim 确认）…"
    cat >&2 <<'BANNER'

  ┌──────────────────────────────────────────────────────────┐
  │  下面会要你设置一个「一次性密码」                         │
  │                                                          │
  │  · 至少 1 个字符，直接回车 = Abort（什么都不会写入）      │
  │  · 输入时**屏幕不显示任何字符**，这是正常的（防窥）        │
  │  · 会问两遍，两遍要一致                                    │
  │  · ⚠ 重启后在蓝色 MOK 界面还要再输一次，想用同一个密码    │
  └──────────────────────────────────────────────────────────┘
BANNER
    echo >&2
    OUT=$(mokutil --import "$CRT" 2>&1) || true
    # ⚠ 关键：mokutil 遇到空密码会打印 "password should be 1~256 characters"
    #   然后 Abort，但**退出码可能仍是 0** —— 必须看输出，不能只看返回值，
    #   否则会把"根本没导入"误报成"已排队"，白等一次重启。
    if echo "$OUT" | grep -qi "password should be"; then
        die "密码为空，mokutil 已中止，**没有导入任何东西**。请重新运行并输入 1~256 字符的密码。"
    fi
    if echo "$OUT" | grep -qi "abort"; then
        die "mokutil 中止：$(echo "$OUT" | tr '\n' ' ')"
    fi
    # ⚠ 不要硬编码 `MokListNewRT` —— Ubuntu shim 1.59 实际用的是 **MokNew**。
    #   （曾因查错变量名把"导入成功"误报成"固件拒写"，白折腾好几轮。）
    _q=""
    for _c in MokNew MokListNewRT MokListNew MokNewRT; do
        _h=$(ls /sys/firmware/efi/efivars 2>/dev/null | grep -m1 "^${_c}-")
        [ -n "$_h" ] && { _q="/sys/firmware/efi/efivars/$_h"; break; }
    done
    if [ -z "$_q" ] || [ ! -e "$_q" ]; then
        warn "导入命令已完成，但未找到待确认队列变量——若重启后没有 MOK 界面，请跑 mok-diag.sh 深查。"
    fi
    ok "MOK 已排队。**请现在重启一次**（sudo systemctl reboot -i），重启后在 MOK 界面输入刚才的密码确认导入。"
}

# ─────────────────────────────────────────────────────── 编译 + 签名 + 安装
build_install() {
    [ -f "$KEY" ] || die "没有签名密钥。请先运行：sudo bash $0 --genkey"

    # 预检。判据用**证书序列号**（modinfo 的 sig_key 就是它），无歧义。
    # 两种通过路径：
    #   ① 已装模块的签名序列号出现在固件已注册列表里 → 直接进签名校验
    #   ② 固件里能找到本模块的 CN，但签名对不上（证书重新生成过）
    #      → 仍然继续，重新编译+签名即可修好
    _ko_fp=$(modinfo "/lib/modules/${KVER}/extra/${MODULE}.ko" 2>/dev/null \
             | awk '/^sig_key:/{print tolower($2)}')
    _enr_all=$(mokutil --list-enrolled 2>/dev/null \
               | awk '/Serial Number:/{getline; gsub(/^[[:space:]]+/,""); print tolower($1)}' \
               | tr '\n' ' ')
    _enr_cn=$(mokutil --list-enrolled 2>/dev/null | grep -c "${MODULE} MOK" || true)

    if [ -n "$_ko_fp" ] && echo "$_enr_all" | grep -q "$_ko_fp"; then
        ok "预检：已装模块签名与固件 MOK 一致（序列号 $_ko_fp）"
    elif [ "${_enr_cn:-0}" -gt 0 ]; then
        warn "固件里已注册本模块 MOK，但已装模块的签名序列号不匹配："
        info "  固件里的序列号: $_enr_all"
        info "  旧模块 sig_key:  ${_ko_fp:-未知}"
        info "  → 会重新编译+签名，修复此问题"
    else
        info "预检：MOK 尚未被固件接受。"
        if [ -f "$CRT" ]; then
            CERTTIME=$(stat -c %Y "$CRT" 2>/dev/null || echo 0)
            NOW=$(date +%s)
            UP=$(awk '{printf "%d", $1}' /proc/uptime 2>/dev/null || echo 0)
            MINS=$(( (NOW - CERTTIME) / 60 ))
            if [ "$CERTTIME" -gt 0 ] && [ "$MINS" -lt "$(( UP / 60 ))" ]; then
                echo "  → 证书生成于 $MINS 分钟前，但系统已开机 $(( UP / 3600 )) 小时" >&2
                echo "    ⇒ **证书生成后没重启过**，固件还不认它。" >&2
                echo "    ⇒ 继续 --build 只会重复失败。**请先重启：**" >&2
                echo >&2
                echo "        sudo systemctl reboot -i      # 绕过 GNOME 拦截" >&2
                echo "        # 或从 GNOME 右上角菜单点「重启」" >&2
                echo >&2
                echo "    重启后在 MOK 界面确认导入，然后再跑本命令。" >&2
                exit 2
            else
                echo "  → 若已重启但固件仍不认，重做导入：" >&2
                echo "        sudo mokutil --revoke-import && sudo bash $0 --genkey" >&2
                echo "    然后再重启一次。" >&2
                exit 2
            fi
        fi
        die "固件里找不到本模块的 MOK 证书。请先运行：sudo bash $0 --genkey 并重启一次"
    fi
    # 检测"已装模块是用旧密钥签的"这种情况：
    #   证书重新生成过（如从 PEM 修成 DER）时，旧 .ko 的签名指纹
    #   与固件里已注册的 MOK 对不上，modprobe 会报 Key was rejected。
    # 症状与"MOK 没导入"完全一样，很容易误诊，所以在加载前主动比对。
    # 签名一致性检查：modinfo 的 sig_key **就是证书的 Serial Number**
    # （modinfo 大写 / mokutil 小写，比对前统一小写）。
    # 不一致 = 证书重新生成过（如从 PEM 修成 DER），旧 .ko 的签名不再被信任，
    # 症状与"MOK 没导入"完全一样，容易误诊 ⇒ 这里主动比对。
    if [ -f "/lib/modules/${KVER}/extra/${MODULE}.ko" ]; then
        KO_FP=$(modinfo "/lib/modules/${KVER}/extra/${MODULE}.ko" 2>/dev/null \
                | awk '/^sig_key:/{print tolower($2)}')
        # 已注册列表里，本模块证书的序列号（取所有 Serial Number，与 KO_FP 比对）
        # 注意：mokutil 输出形如 "        Serial Number:" 单独一行，
        #   值在**下一行**（缩进后的十六进制串），所以要取"匹配行的下一行"。
        ENR_FPS=$(mokutil --list-enrolled 2>/dev/null \
                  | awk '/Serial Number:/{getline; gsub(/^[[:space:]]+/,""); print tolower($1)}' \
                  | tr '\n' ' ')
        if [ -n "$KO_FP" ] && echo "$ENR_FPS" | grep -q "$KO_FP"; then
            ok "签名与已注册 MOK 一致（序列号 ${KO_FP}）"
        elif [ -n "$KO_FP" ]; then
            warn "已装模块的签名与已注册 MOK 不一致，可能是旧密钥签的："
            info "  模块 sig_key: $KO_FP"
            info "  已注册序列号: ${ENR_FPS:-读取失败}"
            info "  → 本次会重新编译+签名，修复此问题"
        fi
    fi

    # 必须拷贝到 Linux 原生文件系统编译：NTFS(D 盘) 缺 POSIX 语义，
    # kbuild 会留下大量临时目录并可能失败。
    info "拷贝源码到 $BUILD_DIR（原生文件系统）"
    rm -rf "$BUILD_DIR"; mkdir -p "$BUILD_DIR"
    cp "$SRC_DIR/${MODULE}.c" "$SRC_DIR/Makefile" "$BUILD_DIR/"

    info "编译…"
    make -C "$BUILD_DIR" KVER="$KVER" clean >/dev/null 2>&1 || true
    make -C "$BUILD_DIR" KVER="$KVER" 2>&1 | tail -3
    [ -f "$BUILD_DIR/${MODULE}.ko" ] || die "编译失败"

    info "用 MOK 签名…"
    SIGN=/usr/src/linux-headers-${KVER}/scripts/sign-file
    [ -x "$SIGN" ] || die "找不到签名工具 $SIGN"
    "$SIGN" sha256 "$KEY" "$CRT" "$BUILD_DIR/${MODULE}.ko"
    ok "签名完成"

    info "安装到 /lib/modules/${KVER}/extra/ …"
      install -D -m 0644 "$BUILD_DIR/${MODULE}.ko" \
          "/lib/modules/${KVER}/extra/${MODULE}.ko"
      cp "$BUILD_DIR/${MODULE}.ko" "$SRC_DIR/${MODULE}.ko"   # 留一份在工作区
      depmod -a "$KVER"

      # ⚠ 必须刷新 initramfs —— 这是"手动 rebuild 能用、一重启又坏"的真根因：
      #   Ubuntu 默认 MODULES=most 会把 /lib/modules/*/extra/ 下的第三方模块
      #   也打进 initrd；开机时 systemd-modules-load 用的正是 **initrd 里那份**。
      #   只更新 /lib/modules 而不刷新 initrd，重启后加载的还是旧模块
      #   （实测：内存 srcversion ≠ 磁盘 .ko 的 srcversion，且旧版本全盘搜不到）。
      if command -v update-initramfs >/dev/null 2>&1; then
          if update-initramfs -u -k "$KVER" >/dev/null 2>&1; then
              ok "已刷新 initramfs（防止开机加载旧模块副本）"
          else
              warn "initramfs 刷新失败 —— 本次会话可用，但重启后可能又回到旧模块"
          fi
      fi

    info "加载模块…"
    # ⚠ 加载必须加超时：实测过 module_init 卡死不返回的情况
    #   （/sys/module/<name>/initstate 一直是 "coming"，modprobe 无限等待）。
    #   15 秒拿不到结果就判定失败并打印诊断信息，不让用户干等。
    #   模块注册名用下划线（内核规范化），文件名用连字符。
    rmmod "${MODULE//-/_}" 2>/dev/null && info "已卸载旧实例"

    # 开机自动加载：/etc/modules-load.d/ 让 systemd 在 boot 时 modprobe。
    # ⚠ 缺了它，重启后必须手动 modprobe；而手动 modprobe **不会触发 udev 规则的
    #   ACTION=="add" 分支**（udev 规则只在设备首次 add 时跑），导致属组回root、
    #   写权限失效 —— 这是实测踩到的真实缺陷。
    # （ec_sys 同理：它提供 /sys/kernel/debug/ec/ec0/io，我们用它只读 EC）
    echo "$MODULE" > /etc/modules-load.d/omen-wmi.conf
    # ⚠ 不再自动加载 ec_sys：Secure Boot + lockdown=integrity 下它的
    #   write_support 是 unsafe module parameter，内核必拒（实测 insmod EPERM）。
    #   EC 读写已由本模块的 ec_byte 接口提供（走内核导出的 ec_read/ec_write）。
    rm -f /etc/modules-load.d/omen-ec.conf 2>/dev/null
    info "已配置开机自动加载（/etc/modules-load.d/）"

    # systemd oneshot：模块加载后把属组改回 plugdev。
    # ⚠ udev 规则的 ACTION=="add" 在某些场景（如 systemd 直接 modprobe）不跑，
    #   实测出现过"重启后手写 modprobe ⇒ 属组回 root ⇒ 写权限失效"。
    #   有了这个 oneshot，每次开机必然修一次，不依赖 udev 是否触发。
    cat > /etc/systemd/system/omen-wmi-perm.service <<'UNIT'
[Unit]
Description=OMEN WMI sysfs 属组授权（让 plugdev 组可写）
After=systemd-modules-load.service omen-wmi-build.service
Requires=systemd-modules-load.service

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for f in /sys/devices/platform/omen-wmi/*; do [ -e "$f" ] && chgrp plugdev "$f" 2>/dev/null; done; exit 0'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
UNIT
    systemctl daemon-reload
    systemctl enable omen-wmi-perm.service >/dev/null 2>&1
    systemctl start omen-wmi-perm.service
    info "已安装 omen-wmi-perm.service（开机自动修属组）"

    # ── 内核升级自愈：omen-wmi-build.service ──────────────────────────
    # 模块只装在当前内核的 /lib/modules/<ver>/extra/ 下；内核一升级就"消失"，
    # omen-fand 因 ConditionPathExists 不满足被 systemd **静默跳过**
    #（不是 failed，所以用户看到的是"开机不自启、界面开关也打不开"）。
    if [ -f "$SRC_DIR/system/omen-wmi-ensure.sh" ]; then
        install -D -m 0755 "$SRC_DIR/system/omen-wmi-ensure.sh" \
            /usr/local/sbin/omen-wmi-ensure.sh
        install -m 0644 "$SRC_DIR/system/omen-wmi-build.service" \
            /etc/systemd/system/omen-wmi-build.service
        systemctl daemon-reload
        systemctl enable omen-wmi-build.service >/dev/null 2>&1 || true
        ok "已安装 omen-wmi-build.service（内核升级后自动重建模块）"
    else
        warn "未找到 system/omen-wmi-ensure.sh，跳过开机自愈配置"
    fi

    MOD_OUT=$(timeout 15 modprobe -v "$MODULE" 2>&1); MOD_RC=$?
    if [ $MOD_RC -eq 124 ]; then
        # 超时：init 卡住
        warn "加载超时（15 秒）。模块 init 疑似卡在内核/固件交互上。"
        echo "      诊断信息：" >&2
        echo "        initstate: $(cat /sys/module/${MODULE//-/_}/initstate 2>/dev/null || echo 不可读)" >&2
        echo "        'coming' = init 未返回；'live' = init 已成功返回" >&2
        echo "        dmesg | tail -15   看看有没有本模块的日志" >&2
        echo >&2
        echo "      ⚠ 该模块已在内核里处于半加载状态，需先清理再试：" >&2
        echo "        sudo rmmod ${MODULE//-/_}   # 若卡住用 --force" >&2
        echo "        sudo reboot                  # 最稳妥：重启自动清理" >&2
        exit 1
    elif [ $MOD_RC -eq 0 ] && [ -d "$SYSFS" ]; then
        ok "加载成功"

        # 关键节点必须存在 —— 缺了它整条风扇链路（守护 / Condition / GUI 开关）全断。
        # 「11/11 属性」是旧版 .ko 的典型特征：源码数组有 12 项（含 fan_speed）。
        if [ -e "$SYSFS/fan_speed" ]; then
            ok "fan_speed 节点就绪"
        else
            warn "模块已加载但缺少 $SYSFS/fan_speed —— 这份 .ko 是旧版！"
            warn "  ⇒ omen-fand 会被 systemd 跳过（ConditionPathExists 不满足）"
            warn "  现有属性: $(ls "$SYSFS" 2>/dev/null | tr '\n' ' ')"
            warn "  若反复出现，检查 SRC_DIR 是否指到了旧源码：$SRC_DIR"
        fi

        # ── 权限：让普通用户（GUI）无需 sudo 即可写 ────────────────────
        # 模块侧把可写接口设为 0664 / 0660（属主 rw + 属组 w），但 sysfs 属组默认是 root。
        # 这里装 udev 规则把属组改成 plugdev，并立即对现有文件生效。
        RULE_SRC="$SRC_DIR/99-omen-wmi.rules"
        RULE_DST="/etc/udev/rules.d/99-omen-wmi.rules"
        if [ -f "$RULE_SRC" ]; then
            install -m 0644 "$RULE_SRC" "$RULE_DST"
            udevadm control --reload >/dev/null 2>&1
            udevadm trigger --subsystem-match=platform >/dev/null 2>&1
            # 立即生效（trigger 只处理新增设备，已存在的文件手动改）
            for f in "$SYSFS"/*; do
                [ -f "$f" ] && chgrp plugdev "$f" 2>/dev/null
            done
            ok "已授权 plugdev 组（普通用户可读写，无需 sudo）"
        else
            warn "未找到 99-omen-wmi.rules，普通用户将需要 sudo 才能写"
        fi
    else
        warn "加载失败（退出码 $MOD_RC），内核返回："
        echo "$MOD_OUT" | sed 's/^/      /' >&2
        echo >&2
        echo "      initstate: $(cat /sys/module/${MODULE//-/_}/initstate 2>/dev/null || echo 不可读)" >&2
        echo "      dmesg | tail -15   看内核侧的真实错误" >&2
        exit 1
    fi
    # 必需依赖：wmi 总线驱动
    if ! lsmod | grep -q '^wmi '; then
        modprobe wmi
    fi

    echo
    ls -l "$SYSFS"/ 2>/dev/null || true
    ok "安装完成。sysfs 目录：$SYSFS/"
}

# ───────────────────────────────────────────────────────────── 只读探测
probe() {
    [ -d "$SYSFS" ] || die "模块未加载。请先 sudo bash $0 --build"
    echo 1 > "$SYSFS"/probe_all
    echo "──────────── dmesg 探测结果 ────────────"
    dmesg | grep -E "hp-wmi-omen|hp_wmi_omen" | tail -30
}

# ───────────────────────────────────────────────────────────── 状态汇总
# 只读，不需要 root（属组 plugdev 已授权读）。GUI 与命令行共用这套输出。
status() {
    [ -d "$SYSFS" ] || die "模块未加载。请先 sudo bash $0 --build"
    echo "════════ OMEN 状态 ════════"
    printf '%-22s %s\n' "风扇数:"       "$(cat "$SYSFS"/fan_count 2>/dev/null || echo '?')"
    printf '%-22s %s\n' "风扇转速:"     "$(cat "$SYSFS"/fan_rpm 2>/dev/null | tr '\n' ' ' || echo '?')"
    printf '%-22s %s\n' "档位版本:"     "$(cat "$SYSFS"/profile_version 2>/dev/null || echo '?')"
    printf '%-22s %s\n' "限速解除:"     "$(cat "$SYSFS"/fan_unthrottle 2>/dev/null || echo '?')"
    printf '%-22s %s\n' "GPU 功耗模式:" "$(cat "$SYSFS"/gpu_modes 2>/dev/null || echo '?')"
    printf '%-22s %s\n' "设计数据:"     "$(cat "$SYSFS"/design_data 2>/dev/null || echo '?')"
    echo
    echo "可用档位（写入 performance_mode）：default | performance | cool"
    echo "  例: echo performance > $SYSFS/performance_mode"
    echo
    echo "不适用（Victus S 专属，EIO 属正常）：fan_table / fan_speed_victus_s"
}

# ───────────────────────────────────────────────────────────── 卸载
uninstall() {
    if rmmod "$MODULE" 2>/dev/null; then ok "已卸载模块"; else info "模块本来就没加载"; fi
    systemctl disable --now omen-wmi-perm.service 2>/dev/null
    rm -f /etc/systemd/system/omen-wmi-perm.service
    rm -f /etc/modules-load.d/omen-wmi.conf /etc/modules-load.d/omen-ec.conf
    rm -f /etc/udev/rules.d/99-omen-wmi.rules
    rm -f "/lib/modules/${KVER}/extra/${MODULE}.ko"
    depmod -a "$KVER"
    ok "已移除模块文件（签名密钥保留在 $KEY_DIR）"
}

case "${1:-}" in
    --genkey)   gen_key ;;
    --build)    build_install ;;
    --probe)    probe ;;
    --status)   status ;;
    --uninstall) uninstall ;;
    *)
        echo "用法: sudo bash $0 {--genkey|--build|--probe|--status|--uninstall}"
        echo
        echo "首次使用顺序："
        echo "  1) sudo bash $0 --genkey      # 生成密钥并导入 MOK"
        echo "  2) 重启一次                    # 在 MOK 界面确认导入"
        echo "  3) sudo bash $0 --build       # 编译 + 签名 + 安装 + 加载"
        echo "  4) sudo bash $0 --probe       # 只读探测，看固件认哪些命令"
        echo "  5) bash $0 --status          # 状态汇总（免 sudo）"
        exit 1
        ;;
esac