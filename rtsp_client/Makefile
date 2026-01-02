# RTSP客户端 Makefile
# 支持Windows和Linux平台

# 检测操作系统
ifeq ($(OS),Windows_NT)
    PLATFORM = WINDOWS
    TARGET = rtsp_client.exe
    LDFLAGS = -lws2_32 -lpthread
    RM = del /Q
    RMDIR = rmdir /S /Q
    RM_CMD = @if exist $(TARGET) $(RM) $(TARGET) 2>nul || true; \
             @if exist obj $(RMDIR) obj 2>nul || true
    CFLAGS = -Wall -Wextra -std=c99 -O2 -Iinc -D_FORTIFY_SOURCE=0
else
    PLATFORM = LINUX
    TARGET = rtsp_client
    LDFLAGS = -lpthread
    RM = rm -rf
    RM_CMD = $(RM) $(TARGET) obj
    CFLAGS = -Wall -Wextra -std=c99 -O2 -Iinc
endif

# 编译器设置
CC = gcc

# 目录设置
SRC_DIR = src
OBJ_DIR = obj
INC_DIR = inc

# 源文件
SOURCES = rtsp_client.c rtp.c player.c
SRC_FILES = $(addprefix $(SRC_DIR)/, $(SOURCES))
OBJECTS = $(addprefix $(OBJ_DIR)/, $(SOURCES:.c=.o))

# 默认目标
.PHONY: all clean help

all: $(OBJ_DIR) $(TARGET)

# 创建 obj 目录
$(OBJ_DIR):
ifeq ($(PLATFORM),LINUX)
	@mkdir -p $(OBJ_DIR)
else
	@if not exist $(OBJ_DIR) mkdir $(OBJ_DIR)
endif

# 链接目标文件生成可执行文件
$(TARGET): $(OBJECTS)
	$(CC) $(OBJECTS) -o $(TARGET) $(LDFLAGS)
	@echo "Build $(TARGET) done."

# 编译源文件为目标文件
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	$(CC) $(CFLAGS) -c $< -o $@

# 清理构建产物
clean:
	@echo "Clean build files."
	$(RM_CMD)
	@echo "Clean done."

# 显示帮助信息
help:
	@echo "RTSP client build system"
	@echo ""
	@echo "Platform: $(PLATFORM)"
	@echo ""
	@echo "available targets:"
	@echo "  make        - build $(TARGET)"
	@echo "  make clean  - clean build files"
	@echo "  make help   - show this help information"
	@echo ""
	@echo "dependencies:"
	@echo "  - GCC compiler"
ifeq ($(PLATFORM),WINDOWS)
	@echo "  - Winsock2 library (ws2_32)"
endif
	@echo "  - pthread library"
