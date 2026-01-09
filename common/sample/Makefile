# Makefile for Common Sample Programs

CC = /home/liyuan/code/sophgo/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
CFLAGS = -Wall -Wextra -O2 -g
LDFLAGS = -lpthread

# 目录定义
COMMON_DIR = ..
OBJ_DIR = obj
SAMPLE_DIR = .

# 包含路径
INCLUDES = -I$(COMMON_DIR)

# 源文件
H264_PARSER_SRCS = $(wildcard $(COMMON_DIR)/h264_parser/*.c)
RINGBUF_SRCS = $(wildcard $(COMMON_DIR)/ringbuf/*.c)
LOG_SRCS = $(wildcard $(COMMON_DIR)/log.c)
SAMPLE_SRCS = $(wildcard $(SAMPLE_DIR)/*.c)

# 目标文件
H264_PARSER_OBJS = $(OBJ_DIR)/h264_parser/h264_parser.o
RINGBUF_OBJS = $(OBJ_DIR)/ringbuf/ringbuf.o
LOG_OBJS = $(OBJ_DIR)/log/log.o
SAMPLE_OBJS = $(patsubst $(SAMPLE_DIR)/%.c, $(OBJ_DIR)/%.o, $(SAMPLE_SRCS))

# 可执行文件
TARGET = file_parser_sample

# 编译输出控制：设置为 0 显示完整命令，设置为 1 显示简短信息
QUIET ?= 1

ifeq ($(QUIET),1)
    Q = @
    ECHO_CC = @echo "  CC      $<"
    ECHO_LD = @echo "  LD      $@"
else
    Q =
    ECHO_CC =
    ECHO_LD =
endif

.PHONY: all clean file_parser_sample

all: $(OBJ_DIR) file_parser_sample
	@echo "Build completed"

# 创建目录结构
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)/h264_parser
	@mkdir -p $(OBJ_DIR)/ringbuf
	@mkdir -p $(OBJ_DIR)/log

# h264_parser 编译规则
$(OBJ_DIR)/h264_parser/h264_parser.o: $(COMMON_DIR)/h264_parser/h264_parser.c | $(OBJ_DIR)/h264_parser
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

# sample 编译规则
$(OBJ_DIR)/%.o: $(SAMPLE_DIR)/%.c | $(OBJ_DIR)
	$(ECHO_CC)
	$(Q)$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# 编译 file_parser_sample
$(TARGET): $(OBJ_DIR)/file_parser_sample.o $(H264_PARSER_OBJS) $(RINGBUF_OBJS) $(LOG_OBJS)
	$(ECHO_LD)
	$(Q)$(CC) $^ -o $@ $(LDFLAGS)
	@echo "Build completed: $@"
	cp $(TARGET) /mnt/nfs/

# 清理
clean:
	rm -rf $(OBJ_DIR)
	@if [ -f $(TARGET) ]; then rm -f $(TARGET); fi
	@echo "Clean completed"

