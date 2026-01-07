# RTSP 客户端

一个基于C语言实现的RTSP客户端，支持从RTSP服务器接收H.264视频流并通过ffplay播放。

## 功能特性

- ✅ 完整的RTSP协议实现（OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN）
- ✅ RTP/RTCP数据包接收和解析
- ✅ H.264 NALU单元重组（支持单包和FU-A分片）
- ✅ 实时视频播放（通过ffplay）
- ✅ Windows平台支持（使用Winsock）
- ✅ 多线程架构（RTSP工作线程）

## 目录结构

```
client/
├── README.md          # 本文件
├── Makefile           # 构建脚本
├── client.c      # RTSP客户端主程序
├── client.h      # RTSP客户端相关定义和数据结构
├── rtp.c              # RTP/RTCP协议实现
└── rtp.h              # RTP相关定义和数据结构
```

## 依赖要求

### 编译依赖
- GCC编译器（支持C99标准）
- Windows平台：MinGW或MSYS2环境
- pthread库（Windows下通常包含在MinGW中）

### 运行时依赖
- **ffplay**（FFmpeg的一部分）
  - 用于播放H.264视频流
  - 下载地址：https://ffmpeg.org/download.html
  - 确保ffplay在系统PATH中，或修改`rtp.c`中的`PLAYER_COMMAND`宏

## 构建说明

### Windows平台

```bash
cd client
make
```

编译成功后会在当前目录生成 `client.exe` 可执行文件。

### 清理构建产物

```bash
make clean
```

## 配置

### 修改RTSP服务器地址

编辑 `client.h` 文件，修改以下宏定义：

```c
#define RTSP_SERVER_IP "192.168.0.103"    // 修改为实际的RTSP服务器IP
#define RTSP_SERVER_PORT 8554              // 修改为实际的RTSP服务器端口
```

### 修改播放器命令

如果需要使用其他播放器或修改ffplay参数，编辑 `rtp.c` 文件中的宏：

```c
#define PLAYER_COMMAND "ffplay -loglevel warning -fflags nobuffer -framedrop -f h264 -i -"
```

## 使用方法

### 1. 启动RTSP服务器

确保RTSP服务器已启动并监听在配置的IP和端口上。

### 2. 运行客户端

```bash
./client.exe
```

### 3. 查看输出

程序运行时会输出：
- RTSP请求和响应报文
- RTP端口绑定信息
- 视频流处理状态

### 4. 停止客户端

按 `Ctrl+C` 发送SIGINT信号，客户端会优雅地关闭连接。

## 工作流程

1. **初始化**：创建RTSP TCP连接，初始化RTP UDP套接字
2. **OPTIONS**：查询服务器支持的方法
3. **DESCRIBE**：获取媒体流描述（SDP）
4. **SETUP**：建立RTP/RTCP传输通道（track1和track2）
5. **PLAY**：开始播放媒体流
6. **接收数据**：在后台线程中接收RTP数据包
7. **解析重组**：解析RTP包，重组H.264 NALU单元
8. **播放**：将重组后的数据通过管道发送给ffplay播放
9. **保活**：每5秒发送OPTIONS请求保持连接
10. **TEARDOWN**：停止播放并关闭连接

## 代码架构

### 模块划分

- **client.c**: RTSP协议层，处理RTSP请求/响应
- **rtp.c**: RTP协议层，处理RTP/RTCP数据包解析

### 关键数据结构

- `rtsp_client_t`: RTSP客户端上下文
- `rtp_t`: RTP处理上下文
- `player_t`: 播放器上下文

## 常见问题

### Q: 编译时提示找不到pthread库

**A**: Windows平台需要安装pthread库。如果使用MinGW，通常已包含。也可以尝试：
```bash
gcc -o client client.c rtp.c -lws2_32 -lpthread
```

### Q: 运行时提示找不到ffplay

**A**: 确保ffplay已安装并在系统PATH中，或修改`PLAYER_COMMAND`宏使用完整路径。

### Q: 连接服务器失败

**A**: 
1. 检查RTSP服务器是否运行
2. 检查IP地址和端口配置是否正确
3. 检查防火墙设置

### Q: 无法播放视频

**A**:
1. 确认ffplay正常工作：`ffplay -version`
2. 检查RTP端口是否被占用
3. 查看控制台输出的错误信息

## 开发说明

### 调试模式

在 `rtp.c` 中取消注释以下行以启用调试输出：
```c
#define RTP_DEBUG_ENABLE
```

### 保存码流到文件

在 `rtp.c` 中取消注释以下行以保存码流：
```c
#define rtp_stream_process_ENABLE
```

码流将保存到 `tmp_stream.h264` 文件中。

## 许可证

本项目遵循项目根目录的许可证规定。

## 贡献

欢迎提交Issue和Pull Request来改进本项目。

