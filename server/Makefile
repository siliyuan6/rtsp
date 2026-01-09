# Makefile for RTSP Server Module

CC = /home/liyuan/code/sophgo/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
CFLAGS = -Wall -Wextra -O2 -g -fPIC
LDFLAGS = -lpthread
AR = ar
ARFLAGS = rcs

# 目录定义
SRC_DIRS = api network core sample
OBJ_DIR = obj
COMMON_DIR = ../common

# 包含路径：添加当前目录（server）和项目根目录，支持绝对路径风格的包含
# 例如：api/rtsp_api.h, common/log.h
INCLUDES = -I. -I.. $(addprefix -I, $(SRC_DIRS)) -I$(COMMON_DIR)

# 源文件（自动查找）
API_SRCS = $(wildcard api/*.c)
NETWORK_SRCS = $(wildcard network/*.c)
CORE_SRCS = $(wildcard core/*.c)
SAMPLE_SRCS = $(wildcard sample/*.c)
RINGBUF_SRCS = $(wildcard $(COMMON_DIR)/ringbuf/*.c)
LOG_SRCS = $(wildcard $(COMMON_DIR)/log.c)
H264_PARSER_SRCS = $(wildcard $(COMMON_DIR)/h264_parser/*.c)

# 目标文件（在obj目录中，保持目录结构）
API_OBJS = $(patsubst api/%.c, $(OBJ_DIR)/api/%.o, $(API_SRCS))
NETWORK_OBJS = $(patsubst network/%.c, $(OBJ_DIR)/network/%.o, $(NETWORK_SRCS))
CORE_OBJS = $(patsubst core/%.c, $(OBJ_DIR)/core/%.o, $(CORE_SRCS))
SAMPLE_OBJS = $(patsubst sample/%.c, $(OBJ_DIR)/sample/%.o, $(SAMPLE_SRCS))
# ringbuf 目标文件：从 ../common/ringbuf/ringbuf.c 转换为 obj/ringbuf/ringbuf.o
RINGBUF_OBJS = $(OBJ_DIR)/ringbuf/ringbuf.o
# log 目标文件：从 ../common/log.c 转换为 obj/log/log.o
LOG_OBJS = $(OBJ_DIR)/log/log.o
# h264_parser 目标文件：从 ../common/h264_parser/h264_parser.c 转换为 obj/h264_parser/h264_parser.o
H264_PARSER_OBJS = $(OBJ_DIR)/h264_parser/h264_parser.o

# 库文件目标（不包括sample）
LIB_OBJS = $(API_OBJS) $(NETWORK_OBJS) $(CORE_OBJS)

# 可执行文件（输出到当前目录，避免与sample目录冲突）
TARGET = sample_rtsp

# 库文件
STATIC_LIB = $(OBJ_DIR)/librtsp.a
SHARED_LIB = $(OBJ_DIR)/librtsp.so

# 编译输出控制：设置为 0 显示完整命令，设置为 1 显示简短信息
QUIET ?= 1

ifeq ($(QUIET),1)
    Q = @
    ECHO_CC = @echo "  CC      $<"
    ECHO_AR = @echo "  AR      $@"
    ECHO_LD = @echo "  LD      $@"
else
    Q =
    ECHO_CC =
    ECHO_AR =
    ECHO_LD =
endif

.PHONY: all clean sample static shared libs

all: $(OBJ_DIR) libs sample
	cp $(TARGET) /mnt/nfs

# 创建目录结构
$(OBJ_DIR):
	@mkdir -p $(addprefix $(OBJ_DIR)/, $(SRC_DIRS))
	@mkdir -p $(OBJ_DIR)/ringbuf
	@mkdir -p $(OBJ_DIR)/log
	@mkdir -p $(OBJ_DIR)/h264_parser

# 确保 ringbuf 目录存在
$(OBJ_DIR)/ringbuf:
	@mkdir -p $(OBJ_DIR)/ringbuf

# 确保 log 目录存在
$(OBJ_DIR)/log:
	@mkdir -p $(OBJ_DIR)/log

# 确保 h264_parser 目录存在
$(OBJ_DIR)/h264_parser:
	@mkdir -p $(OBJ_DIR)/h264_parser

# 通用编译规则：从源文件编译到obj目录
# 使用order-only依赖确保目录存在
$(OBJ_DIR)/api/%.o: api/%.c | $(OBJ_DIR)/api
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/network/%.o: network/%.c | $(OBJ_DIR)/network
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/core/%.o: core/%.c | $(OBJ_DIR)/core
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/sample/%.o: sample/%.c | $(OBJ_DIR)/sample
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ringbuf 编译规则
$(OBJ_DIR)/ringbuf/ringbuf.o: $(COMMON_DIR)/ringbuf/ringbuf.c | $(OBJ_DIR)/ringbuf
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# log 编译规则
$(OBJ_DIR)/log/log.o: $(COMMON_DIR)/log.c | $(OBJ_DIR)/log
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# h264_parser 编译规则
$(OBJ_DIR)/h264_parser/h264_parser.o: $(COMMON_DIR)/h264_parser/h264_parser.c | $(OBJ_DIR)/h264_parser
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 链接示例程序
sample: $(LIB_OBJS) $(SAMPLE_OBJS) $(RINGBUF_OBJS) $(LOG_OBJS) $(H264_PARSER_OBJS)
	$(ECHO_LD)
	$(Q)$(CC) $(LIB_OBJS) $(SAMPLE_OBJS) $(RINGBUF_OBJS) $(LOG_OBJS) $(H264_PARSER_OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Build completed: $(TARGET)"

# 编译静态库
static: $(LIB_OBJS)
	$(ECHO_AR)
	$(AR) $(ARFLAGS) $(STATIC_LIB) $(LIB_OBJS)
	@echo "Static library built: $(STATIC_LIB)"

# 编译动态库
shared: $(LIB_OBJS)
	$(ECHO_LD)
	$(Q)$(CC) -shared -o $(SHARED_LIB) $(LIB_OBJS) $(LDFLAGS)
	@echo "Shared library built: $(SHARED_LIB)"

# 编译所有库
libs: static shared
	@echo "All libraries built"

# 清理
clean:
	rm -rf $(OBJ_DIR)
	@if [ -f $(TARGET) ]; then rm -f $(TARGET); fi
	@echo "Clean completed"
