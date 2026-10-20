#!/usr/bin/env bash
# ================================================================
#  MOK 导入诊断 —— 判断"导入到底有没有真正写进固件"
#
#  用法: sudo bash mok-diag.sh
#
#  背景：排查 MOK 导入为何"看起来没成功"。本脚本把每一层都验证一遍。
#  ⚠ 曾两次误判，教训写在这里：
#     ① 硬编码 `MokListNewRT` 是**错的** —— Ubuntu shim 1.59 实际用 `MokNew`；
#        查错变量名会把"导入成功"误报成"固件拒写"。现改为动态探测候选名。
#     ② 空密码时 mokutil 会 Abort 且什么都不写，必须看输出内容而不是只看返回值。
# ================================================================
set -uo pipefail

CRT=/etc/hp-wmi-omen/MOK.der
SHIM_GUID=605dab50-e046-4300-abb6-3dd810dd8b23
# efivars 根目录
EFIVARS="/sys/firmware/efi/efivars"

# 已注册列表（MokListRT）—— 各版本命名基本一致
RT="$(ls "$EFIVARS" 2>/dev/null | grep -m1 '^MokListRT-')"
RT="$EFIVARS/${RT:-MokListRT-$SHIM_GUID}"

# 待确认队列 ⚠ **不要硬编码变量名** —— 不同 shim 版本命名不同：
#   Ubuntu shim 1.59 实测用 **MokNew**（不是 MokListNewRT！）
#   我曾硬编码 MokListNewRT，导致每次都误判"导入没成功"，白折腾好几轮。
# 所以这里在几个候选名里动态找。
QUEUE=""
for cand in MokNew MokListNewRT MokListNew MokNewRT; do
    hit=$(ls "$EFIVARS" 2>/dev/null | grep -m1 "^${cand}-")
    [ -n "$hit" ] && { QUEUE="$EFIVARS/$hit"; break; }
done

ok()   { printf "  \033[32m✅ %s\033[0m\n" "$*"; }
bad()  { printf "  \033[31m❌ %s\033[0m\n" "$*"; }
warn() { printf "  \033[33m⚠ %s\033[0m\n" "$*"; }
info() { printf "  \033[36m·\033[0m %s\n" "$*"; }
step() { printf "\n\033[1m【%s】\033[0m\n" "$*"; }

echo "════════════════════════════════════════════════════════"
echo " MOK 导入诊断"
echo "════════════════════════════════════════════════════════"

# efivars 只有 root 能读；非 root 会把"存在"误报成"不存在"，结论不可信。
[ "$(id -u)" -eq 0 ] || {
    echo
    echo "[错误] 请用 sudo 运行：sudo bash $0" >&2
    echo "       （efivars / MOK 列表只有 root 能读，否则结论会误导）" >&2
    exit 1
}

# ───────────────────────────────────────────────────────── 1
step "1 · Secure Boot / Lockdown 现状"
SB=$(mokutil --sb-state 2>/dev/null)
if echo "$SB" | grep -qi "SecureBoot enabled"; then
    ok "Secure Boot: enabled"
else
    warn "Secure Boot: ${SB:-未知}（若为 disabled，MOK 机制不生效）"
fi
LK=$(cat /sys/kernel/security/lockdown 2>/dev/null || echo "不可读")
info "Lockdown: $LK"
if echo "$LK" | grep -q "integrity"; then
    info "→ integrity 模式：**只影响模块加载**，不妨碍 efivars 写入"
fi

# ───────────────────────────────────────────────────────── 2
step "2 · shim 是否支持 MOK（efivars 变量是否存在）"
[ -e "$RT" ] && ok "MokListRT 存在（$(stat -c %s "$RT") 字节）——shim 提供 MOK 机制" \
             || bad "MokListRT 不存在 —— 本机 shim 可能不提供 MOK"
[ -e "/sys/firmware/efi/efivars/MokListTrustedRT-${SHIM_GUID}" ] \
    && ok "MokListTrustedRT 存在（证书链信任标志）" \
    || warn "MokListTrustedRT 不存在"

SHIMV=$(dpkg-query -W -f='${Version}' shim-signed 2>/dev/null)
info "shim-signed 版本: ${SHIMV:-未安装}"

# ───────────────────────────────────────────────────────── 3
step "3 · 本机证书与密钥"
if [ -f "$CRT" ]; then
    SUBJ=$(openssl x509 -inform DER -in "$CRT" -noout -subject 2>&1)
    if echo "$SUBJ" | grep -q "subject="; then
        ok "证书有效（DER 可解析）"
        info "$SUBJ"
    else
        bad "证书无法解析：$SUBJ"
    fi
    DAYS=$(openssl x509 -inform DER -in "$CRT" -noout -enddate 2>&1 | cut -d= -f2)
    info "有效期至: ${DAYS:-?}"
else
    bad "证书不存在：$CRT（请先跑 install.sh --genkey）"
fi
[ -f /etc/hp-wmi-omen/MOK.key ] && ok "私钥存在" || bad "私钥缺失"

# ───────────────────────────────────────────────────────── 4
step "4 · 当前注册状态"
ENR=$(mokutil --list-enrolled 2>/dev/null | grep -c "hp-wmi-omen")
NEW=$(mokutil --list-new 2>/dev/null | grep -c "hp-wmi-omen")
if [ "$ENR" -gt 0 ]; then
    ok "证书**已注册**（固件已接受）→ 模块应该能加载"
else
    warn "证书**未注册**（enrolled 列表里没有）"
fi
if [ "$NEW" -gt 0 ]; then
    ok "存在待确认请求（list-new 有）→ 重启时会弹 MOK 界面，需用一次性密码确认"
else
    bad "没有待确认请求（list-new 为空）"
fi
if [ -n "$QUEUE" ] && [ -e "$QUEUE" ]; then
    ok "待确认队列: $(basename "$QUEUE") 存在（$(stat -c %s "$QUEUE") 字节）"
else
    warn "未找到待确认队列变量（试过 MokNew / MokListNewRT / MokListNew / MokNewRT）"
fi

# ───────────────────────────────────────────────────────── 5
step "5 · 重新尝试导入（观察待确认队列是否出现）"
if [ ! -f "$CRT" ]; then
    bad "无证书，跳过"
else
    # 基线快照：记录导入前的固件状态，便于事后对比
    RT_BEFORE=$(sha256sum "$RT" 2>/dev/null | cut -c1-16)
    RT_SIZE_BEFORE=$(stat -c %s "$RT" 2>/dev/null || echo 0)
    info "导入前基线: MokListRT ${RT_SIZE_BEFORE} 字节 (sha ${RT_BEFORE:-不可读})"

    # 先撤销可能残留的请求，保证干净重试
    mokutil --revoke-import >/dev/null 2>&1
    info "已撤销旧请求，重新导入…"

    # ⚠ mokutil 用 getpass 读 /dev/tty，管道喂不进去（--generate-hash 等同样
    #   是交互式的，脚本无法自动化），所以只能让它自己在真终端里问。
    #   但它的提示 "input password:" 刷在捕获的输出里，**慢一步才看得到**，
    #   容易让人以为没提示。⇒ 这里用醒目横幅明确告知时机。
    cat >&2 <<'BANNER'

  ┌──────────────────────────────────────────────────────────┐
  │  下面会要你设置一个「一次性密码」                         │
  │                                                          │
  │  · 现在就把你想好，等下直接在光标处输入                    │
  │  · 至少 1 个字符，直接回车 = Abort（什么都不会写入）      │
  │  · 输入时**屏幕不显示任何字符**，这是正常的（防窥）        │
  │  · 会问两遍，两遍要一致                                    │
  │  · ⚠ 重启后在蓝色 MOK 界面还要再输一次，想用同一个密码    │
  └──────────────────────────────────────────────────────────┘
BANNER
    echo
    OUT=""
    if OUT=$(mokutil --import "$CRT" 2>&1); then
        if echo "${OUT:-}" | grep -qi "password should be"; then
            bad "密码为空 → mokutil 已 Abort，**没有写入任何东西**"
            info "最常见的失败原因。重新运行本脚本，在提示处输入非空密码。"
        else
            ok "mokutil --import 返回 0"
            info "输出: $(echo "$OUT" | tr '\n' ' ' | cut -c1-90)"
        fi
    else
        bad "mokutil --import 返回非零"
        echo "$OUT" | sed 's/^/      /'
        if echo "${OUT:-}" | grep -qi "password should be"; then
            echo >&2
            echo "      ↑ 密码为空导致 Abort，**不是固件拒绝**。重新运行并输入密码。" >&2
        fi
    fi
    sleep 1
    if [ -e "$NEWRT" ]; then
        ok "队列变量 **已出现**（$(basename "$QUEUE")，$(stat -c %s "$QUEUE") 字节）→ 导入真的生效了"
        ok "现在重启：sudo systemctl reboot -i"
        echo >&2
        echo "  重启后在蓝色 MOK 界面选择 Enroll MOK，输入你刚设的密码确认。" >&2
    else
        # 队列变量没出现时，再看已注册列表有没有变化 ——
        # 有些固件/shim 版本会"直接写 MokListRT"而跳过队列。
        RT_AFTER=$(sha256sum "$RT" 2>/dev/null | cut -c1-16)
        RT_SIZE_AFTER=$(stat -c %s "$RT" 2>/dev/null || echo 0)
        if [ "$RT_SIZE_BEFORE" != "$RT_SIZE_AFTER" ]; then
            warn "队列变量未出现，但 MokListRT **大小变了**（${RT_SIZE_BEFORE} → ${RT_SIZE_AFTER} 字节）"
            info "→ 说明导入被直接写进了已注册列表（跳过队列）。可以先试着加载模块："
            info "     sudo modprobe hp_wmi_omen && ls /sys/devices/platform/omen-wmi/"
        fi
        bad "队列变量仍未出现"
        echo >&2
        if echo "${OUT:-}" | grep -qi "password should be"; then
            echo "  ⇒ 原因是**密码为空导致导入未执行**，不是固件拒写。" >&2
            echo "    请重新运行本脚本，在 'input password:' 处输入 1~256 字符的密码。" >&2
        else
            echo "  ⇒ 这次密码正常但仍没写入 → **固件侧确实拒绝了 efivars 写入**" >&2
            echo "    可能原因：" >&2
            echo "      ① HP 固件限制 efivars 写入（OEM 常见做法）" >&2
            echo "      ② shim 版本过旧（$(dpkg-query -W -f='${Version}' shim-signed 2>/dev/null || echo '未知')）" >&2
            echo "      ③ HP 固件不提供 MOK 管理界面" >&2
        fi
    fi
fi

# ───────────────────────────────────────────────────────── 6
step "6 · 备选路径评估"
if [ -e "$NEWRT" ]; then
    info "主路径可用 → 完成上面重启即可"
else
    warn "主路径（自签 MOK）受阻。可评估的备选："
    echo
    echo "    A) 升级 shim 到最新版后重试（不关 Secure Boot）" >&2
    echo "       sudo apt install --only-upgrade shim-signed" >&2
    echo "    B) 临时关闭 Secure Boot（安全性下降，但立刻可用）" >&2
    echo "       → 之后内核模块无需签名即可加载，我们已经写好了" >&2
    echo "    C) 放弃内核模块，回到纯用户态方案" >&2
    echo "       （omen-control 的 CPU/GPU 控制不受影响，仍全部可用）" >&2
    echo
    echo "    注意：B 需要进 BIOS 关闭，且会降低整机引导链安全。" >&2
    echo "          是否采用由你决定，我不会替你改这个设置。" >&2
fi

echo
echo "════════════════════════════════════════════════════════"
echo "诊断结束。"
echo "════════════════════════════════════════════════════════"