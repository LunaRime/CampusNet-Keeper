# 路由器端构建（Linux / musl / glibc）
#
#   make                     本地构建（glibc，用于 x86 路由器或开发机）
#   make static             静态链接（musl 工具链，最省心）
#   make CROSS=mipsel-openwrt-linux-   交叉编译
#   make test                自检 + 端到端（需要 python3）
#   make clean

CC     ?= cc
CROSS  ?=
CROSSCC = $(CROSS)$(CC)
CFLAGS ?= -O2 -Wall -Wextra -std=c99 -pedantic
LDFLAGS ?=
SRC = src/crypto.c src/net.c src/portal.c src/main.c
OUT ?= build

# 构建身份：把 git 短哈希与 UTC 构建时间烧进二进制，真机 `--version` 就能核对
# 跑的是哪个提交（2026-11-10 审查发现旧产物只有 "0.1.0"，无法追溯）。
# SDK / 无 git 环境退化成 unknown，绝不让构建因此失败。
GIT_COMMIT := $(shell git rev-parse --short HEAD 2>/dev/null || echo unknown)
BUILD_TIME := $(shell date -u +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)
# 用 override：命令行传了 CFLAGS（交叉编译时常见）也不能把版本宏丢掉
override CFLAGS += -DCNK_BUILD_COMMIT='"$(GIT_COMMIT)"' -DCNK_BUILD_TIME='"$(BUILD_TIME)"'

ifeq ($(STATIC),1)
LDFLAGS += -static
endif

all: $(OUT)/cnkrouter

$(OUT)/cnkrouter: $(SRC) src/cnk.h
	@mkdir -p $(OUT)
	$(CROSSCC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
	@ls -l $@ | awk '{print "构建完成:", $$9, $$5, "字节"}'

static:
	$(MAKE) STATIC=1 all

# 一条命令跑完本机能跑的全部验证（G1：CI 与本地共用同一个入口，
# 免得 CI 里手写的序列和本地漂移、新测试加了却没人跑）。
# 需要 python3；test_reload.py 依赖 SIGHUP，非 Linux 上会自己跳过。
test: $(OUT)/cnkrouter
	$(OUT)/cnkrouter --selftest
	python3 tests/test_config_keys.py
	sh tests/test_init_render.sh
	sh tests/test_cleanup.sh
	python3 tests/e2e_test.py $(OUT)/cnkrouter
	python3 tests/test_reload.py $(OUT)/cnkrouter

clean:
	rm -rf $(OUT)

.PHONY: all static test clean
