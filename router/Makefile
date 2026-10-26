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

test: $(OUT)/cnkrouter
	./$(OUT)/cnkrouter --selftest
	python3 tests/e2e_test.py ./$(OUT)/cnkrouter

clean:
	rm -rf $(OUT)

.PHONY: all static test clean
