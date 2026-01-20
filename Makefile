CC ?= gcc
AR ?= ar
PKG_CONFIG ?= pkg-config

# 约定：所有生成物只落在 `_build/`，避免后续 git push 误提交产物。
BUILD_DIR := _build
OBJ_DIR   := $(BUILD_DIR)/obj
BIN_DIR   := $(BUILD_DIR)/bin
LIB_DIR   := $(BUILD_DIR)/lib

LIB_NAME := minivmi
LIB_A    := $(LIB_DIR)/lib$(LIB_NAME).a

# Xen 的公开头文件会用到 GNU C 扩展（匿名 union、asm 等），
# 所以这里必须用 `-std=gnu11`（不要用严格的 `-std=c11`）。
# 说明：Xen 的 ring.h 里有一些 (long) 指针运算宏，配合 -Wconversion 会产生大量 -Wsign-conversion 噪声；
# 这里保留 -Wconversion，但关闭 sign-conversion，避免输出刷屏影响学习体验。
CFLAGS_BASE := -std=gnu11 -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -O2 -g -fPIC -Iinclude

# 优先用 pkg-config 自动找头文件路径与链接参数；没有就 fallback 到 -lxxx。
XEN_CFLAGS := $(shell $(PKG_CONFIG) --cflags xencontrol xenstore xenevtchn 2>/dev/null)
XEN_LIBS   := $(shell $(PKG_CONFIG) --libs   xencontrol xenstore xenevtchn 2>/dev/null)

ifeq ($(strip $(XEN_LIBS)),)
XEN_LIBS := -lxenctrl -lxenstore -lxenevtchn
endif

CFLAGS  := $(CFLAGS_BASE) $(XEN_CFLAGS)
LDFLAGS :=

LIB_SRCS := src/minivmi_xen.c
LIB_OBJS := $(patsubst src/%.c,$(OBJ_DIR)/src/%.o,$(LIB_SRCS))

EXES := \
  $(BIN_DIR)/list_domains \
  $(BIN_DIR)/cr3trace_uuid

.PHONY: all clean
all: $(LIB_A) $(EXES)

$(OBJ_DIR)/src/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/examples/%.o: examples/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIB_A): $(LIB_OBJS)
	@mkdir -p $(LIB_DIR)
	$(AR) rcs $@ $^

$(BIN_DIR)/%: $(OBJ_DIR)/examples/%.o $(LIB_A)
	@mkdir -p $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(XEN_LIBS)

clean:
	rm -rf $(BUILD_DIR)
