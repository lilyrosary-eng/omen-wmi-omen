# hp-wmi-omen —— 直接调用 HP 私有 WMI 接口的内核模块
obj-m += hp-wmi-omen.o

# 内核树位置：优先用正在运行的内核，其次用当前目录的 Kbuild 变量
KVER  ?= $(shell uname -r)
KDIR  ?= /lib/modules/$(KVER)/build
PWD   := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# 用自签名 MOK 签名（Secure Boot 开启时必需）
sign:
	@if [ -f /var/lib/shim-signed/mok/MOK.der ]; then \
		echo "使用已有 MOK 签名"; \
		scripts/sign-file sha256 /var/lib/shim-signed/mok/MOK.der /var/lib/shim-signed/mok/MOK.key hp-wmi-omen.ko; \
	else \
		echo "未找到 MOK，稍后先跑 install.sh 的 --genkey"; \
	fi

.PHONY: all clean sign
