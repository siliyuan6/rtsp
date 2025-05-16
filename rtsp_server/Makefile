# 设置编译器
CC = gcc

# 编译选项
CFLAGS = -Wall -g

# 目标文件
TARGET = rtsp_server

# 源文件
SRCS = rtsp_server.c

# 生成目标文件
$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRCS) -lws2_32

# 清理生成的文件
clean:
	del $(TARGET).exe