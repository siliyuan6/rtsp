# 设置编译器
# aarch64-linux-gnu-gcc
# /home/liyuan/code/sophgo/host-tools/gcc/riscv64-linux-x86_64/bin/riscv64-unknown-linux-gnu-gcc
# /home/liyuan/code/sophgo/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc
CC = /home/liyuan/code/sophgo/host-tools/gcc/riscv64-linux-musl-x86_64/bin/riscv64-unknown-linux-musl-gcc

# 编译选项
CFLAGS = -Wall -g -pthread -I../common

# 目标文件
TARGET = rtsp_server

# 源文件
SRCS = rtsp_server.c

# 生成目标文件
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS)

# 清理生成的文件
clean:
	rm -f $(TARGET)