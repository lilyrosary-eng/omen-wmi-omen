# 上游化（upstream）进展

| 项 | 状态 |
|:-|:-|
| 目标 | 让 in-tree `hp-wmi` 认识 8A1A，至少在 hwmon 里给出风扇转速 |
| patch | `0001-platform-x86-hp-wmi-add-OMEN-16-k0xxx-board-8A1A.patch` |
| 基线 | Linux master（2026-10-20 抓取） |
| `git apply --check` | 通过 |
| `checkpatch.pl` | 0 errors, 0 warnings |
| 编译 | **未验证**（本机只有 7.0.14 的头文件，无法编译 master） |
| 启动测试 | **未做**（会替换当前正在工作的驱动） |
| 发送 | **未发送** —— 需要提交者本人的邮箱与 Signed-off-by |

## 怎么发

```bash
# 1) 准备一个干净的 linux 树
git clone --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git
cd linux && git am /path/to/0001-*.patch

# 2) 配 send-email（Gmail 需要应用专用密码）
git config sendemail.smtpserver smtp.gmail.com
git config sendemail.smtpserverport 587
git config sendemail.smtpencryption tls
git config sendemail.smtpuser lilyrosary0@gmail.com

# 3) 发（正文用 cover letter，patch 作为附件）
git send-email \
  --to="platform-driver-x86@vger.kernel.org" \
  --cc="linux-kernel@vger.kernel.org" \
  --cc="Hans de Goede <hdegoede@redhat.com>" \
  --cc="Ilpo Järvinen <ilpo.jarvinen@linux.intel.com>" \
  --annotate 0001-*.patch
```

⚠ 发之前必须确认：
- `Signed-off-by` 用的是你本人可收信的邮箱（DCO）；
- 不要用 HTML 邮件、不要同时发 HTML 版；
- 第一次发建议先 `--dry-run`；
- 这是 RFC，标题里保留 `[RFC PATCH]`，正文说明"未 boot-test"。

## 为什么只做了"读"

风扇**写**（GM12）的取值空间是 0=交回固件、255=钉死满速，
和 hwmon 的 `pwm1`（0=停转、255=满速）语义不一致。
这个映射需要 maintainer 先表态，所以没有硬塞进 patch；
cover letter 里已经把这个问题和实测数据写清楚了。
