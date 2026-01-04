# 设置编译器
# aarch64-linux-gnu-gcc
CC = aarch64-linux-gnu-gcc

# 编译选项
CFLAGS = -Wall -g -pthread

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