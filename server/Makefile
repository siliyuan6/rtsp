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

# 包含路径
INCLUDES = $(addprefix -I, $(SRC_DIRS)) -I$(COMMON_DIR)

# 源文件（自动查找）
API_SRCS = $(wildcard api/*.c)
NETWORK_SRCS = $(wildcard network/*.c)
CORE_SRCS = $(wildcard core/*.c)
SAMPLE_SRCS = $(wildcard sample/*.c)
RINGBUF_SRCS = $(wildcard $(COMMON_DIR)/ringbuf/*.c)
LOG_SRCS = $(wildcard $(COMMON_DIR)/log.c)

# 目标文件（在obj目录中，保持目录结构）
API_OBJS = $(patsubst api/%.c, $(OBJ_DIR)/api/%.o, $(API_SRCS))
NETWORK_OBJS = $(patsubst network/%.c, $(OBJ_DIR)/network/%.o, $(NETWORK_SRCS))
CORE_OBJS = $(patsubst core/%.c, $(OBJ_DIR)/core/%.o, $(CORE_SRCS))
SAMPLE_OBJS = $(patsubst sample/%.c, $(OBJ_DIR)/sample/%.o, $(SAMPLE_SRCS))
# ringbuf 目标文件：从 ../common/ringbuf/ringbuf.c 转换为 obj/ringbuf/ringbuf.o
RINGBUF_OBJS = $(OBJ_DIR)/ringbuf/ringbuf.o
# log 目标文件：从 ../common/log.c 转换为 obj/log/log.o
LOG_OBJS = $(OBJ_DIR)/log/log.o

# 库文件目标（不包括sample）
LIB_OBJS = $(API_OBJS) $(NETWORK_OBJS) $(CORE_OBJS)

# 可执行文件（输出到当前目录，避免与sample目录冲突）
TARGET = sample_rtsp

# 库文件
STATIC_LIB = $(OBJ_DIR)/librtsp.a
SHARED_LIB = $(OBJ_DIR)/librtsp.so

.PHONY: all clean sample static shared libs

all: $(OBJ_DIR) libs sample

# 创建目录结构
$(OBJ_DIR):
	@mkdir -p $(addprefix $(OBJ_DIR)/, $(SRC_DIRS))
	@mkdir -p $(OBJ_DIR)/ringbuf
	@mkdir -p $(OBJ_DIR)/log

# 确保 ringbuf 目录存在
$(OBJ_DIR)/ringbuf:
	@mkdir -p $(OBJ_DIR)/ringbuf

# 确保 log 目录存在
$(OBJ_DIR)/log:
	@mkdir -p $(OBJ_DIR)/log

# 通用编译规则：从源文件编译到obj目录
# 使用order-only依赖确保目录存在
$(OBJ_DIR)/api/%.o: api/%.c | $(OBJ_DIR)/api
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/network/%.o: network/%.c | $(OBJ_DIR)/network
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/core/%.o: core/%.c | $(OBJ_DIR)/core
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(OBJ_DIR)/sample/%.o: sample/%.c | $(OBJ_DIR)/sample
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# ringbuf 编译规则
$(OBJ_DIR)/ringbuf/ringbuf.o: $(COMMON_DIR)/ringbuf/ringbuf.c | $(OBJ_DIR)/ringbuf
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# log 编译规则
$(OBJ_DIR)/log/log.o: $(COMMON_DIR)/log.c | $(OBJ_DIR)/log
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 链接示例程序
sample: $(LIB_OBJS) $(SAMPLE_OBJS) $(RINGBUF_OBJS) $(LOG_OBJS)
	$(CC) $(LIB_OBJS) $(SAMPLE_OBJS) $(RINGBUF_OBJS) $(LOG_OBJS) -o $(TARGET) $(LDFLAGS)
	@echo "Build completed: $(TARGET)"

# 编译静态库
static: $(LIB_OBJS)
	$(AR) $(ARFLAGS) $(STATIC_LIB) $(LIB_OBJS)
	@echo "Static library built: $(STATIC_LIB)"

# 编译动态库
shared: $(LIB_OBJS)
	$(CC) -shared -o $(SHARED_LIB) $(LIB_OBJS) $(LDFLAGS)
	@echo "Shared library built: $(SHARED_LIB)"

# 编译所有库
libs: static shared
	@echo "All libraries built"

# 清理
clean:
	rm -rf $(OBJ_DIR)
	@if [ -f $(TARGET) ]; then rm -f $(TARGET); fi
	@echo "Clean completed"
