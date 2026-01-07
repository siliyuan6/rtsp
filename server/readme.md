# RTSP服务器模块

## 模块介绍

本模块实现了一个支持多种视频码流格式的RTSP服务器，采用分层架构设计，支持格式扩展。

### 主要特性

- 支持自定义RTSP和RTP端口
- 支持H.264格式（H.265可扩展）
- 完整的RTSP协议支持（OPTIONS、DESCRIBE、SETUP、PLAY、TEARDOWN）
- RTP数据封装和发送（支持单包和FU-A分片）
- 多线程架构，支持并发连接
- 符合代码规范，易于维护和扩展

### 架构设计

```
api/rtsp_api.c      - 对外API接口层（支持格式配置）
network/network.c   - 网络层（支持自定义端口）
core/rtsp.c         - RTSP协议处理层（动态SDP生成）
core/rtp.c          - RTP数据发送层（格式无关封装）
sample/sample.c     - 使用示例
```

## 支持的格式

- **H.264**: 完整支持，包括单包和FU-A分片封装
- **H.265**: 接口已预留，可扩展实现

## 编译方法

### 前置要求

- GCC编译器
- pthread库
- Linux/Unix系统（Windows需要相应调整）

### 编译步骤

1. 进入server目录：
```bash
cd server
```

2. 编译：
```bash
make
```

3. 编译示例程序：
```bash
make sample
```

4. 清理编译文件：
```bash
make clean
```

### 编译输出

- 所有目标文件（.o）存放在`obj/`目录
- 示例程序可执行文件：`obj/sample`

## 使用方法

### 基本使用

1. 创建RTSP配置：
```c
RTSPConfig_t config;
config.rtspPort = 8554;  // RTSP监听端口
config.rtpPort = 5000;   // RTP端口
config.format = RTSP_FORMAT_H264; // 数据格式
config.fps = 25;         // 帧率
```

2. 实现数据回调函数：
```c
int GetDataCallback(unsigned char *buf, int bufSize)
{
	// 从编码器或文件读取H.264数据
	// 返回数据长度，无数据返回0，错误返回-1
}
```

3. 创建RTSP服务器：
```c
RTSPHandle_t *handle = NULL;
RTSPCreate(&handle, &config, GetDataCallback);
```

4. 运行服务器（主循环）：
```c
// 服务器在后台线程运行
// 主线程可以执行其他任务或等待
```

5. 销毁服务器：
```c
RTSPDestroy(handle);
```

### 完整示例

参考`sample/sample.c`文件，包含完整的使用示例。

### 客户端连接

使用RTSP客户端（如VLC、ffplay）连接：
```bash
rtsp://localhost:8554/live
```

## API说明

### RTSPCreate

创建RTSP服务器实例。

**函数原型：**
```c
int RTSPCreate(RTSPHandle_t **handle, const RTSPConfig_t *config,
	int (*getData)(unsigned char *buf, unsigned int bufSize));
```

**参数：**
- `handle`: 输出参数，返回创建的句柄指针
- `config`: RTSP配置结构体
- `getData`: 数据回调函数，用于获取码流数据

**返回值：**
- 成功返回0，失败返回-1

### RTSPDestroy

销毁RTSP服务器实例。

**函数原型：**
```c
int RTSPDestroy(RTSPHandle_t *handle);
```

**参数：**
- `handle`: RTSP句柄指针

**返回值：**
- 成功返回0，失败返回-1

### RTSPGetStatus

获取RTSP服务器当前状态。

**函数原型：**
```c
int RTSPGetStatus(RTSPHandle_t *handle, RTSPStatus_t *status);
```

**参数：**
- `handle`: RTSP句柄指针
- `status`: 输出参数，返回状态

**返回值：**
- 成功返回0，失败返回-1

**状态值：**
- `RTSP_STATUS_STOPPED`: 已停止
- `RTSP_STATUS_RUNNING`: 运行中
- `RTSP_STATUS_ERROR`: 错误状态

## 扩展新格式

### 添加新格式的步骤

1. 在`api/rtsp_api.h`中的`RTSPStreamFormat_t`枚举添加新格式：
```c
typedef enum {
	RTSP_FORMAT_H264,
	RTSP_FORMAT_H265,
	RTSP_FORMAT_NEW,  // 新格式
	RTSP_FORMAT_MAX
} RTSPStreamFormat_t;
```

2. 在`core/rtsp.c`的`GenerateSDP`函数中添加新格式的SDP生成逻辑：
```c
case RTSP_FORMAT_NEW:
	formatStr = "NEW";
	payloadType = 96;
	break;
```

3. 在`core/rtp.c`中实现对应的RTP封装函数：
```c
static int RTPEncapsulateNew(const unsigned char *data, int len,
	RtpPacket_t *rtpPackets, int maxPackets)
{
	// 实现新格式的RTP封装逻辑
}
```

4. 在`core/rtp.c`的`RTPEncapsulate`函数中添加格式分发：
```c
case RTSP_FORMAT_NEW:
	return RTPEncapsulateNew(data, len, rtpPackets, maxPackets);
```

5. 更新文档，说明新格式的使用方法

## 注意事项

1. **端口配置**：确保RTSP和RTP端口未被占用
2. **数据格式**：数据回调函数必须提供符合格式要求的原始数据（如H.264需要包含起始码）
3. **线程安全**：API函数是线程安全的，可以在多线程环境中使用
4. **资源管理**：使用完毕后必须调用`RTSPDestroy`释放资源

## 故障排查

### 常见问题

1. **端口被占用**
   - 检查端口是否被其他程序占用
   - 修改配置使用其他端口

2. **客户端无法连接**
   - 检查防火墙设置
   - 确认RTSP服务器已启动
   - 检查网络连接

3. **数据无法播放**
   - 确认数据格式正确
   - 检查数据回调函数是否正常返回数据
   - 确认帧率配置合理

## 代码规范

本模块严格遵循`code-format.md`中的代码规范：
- 行宽限制：80字符（特殊情况120字符以内）
- 使用Tab缩进
- 函数名大驼峰，变量名小驼峰
- 完整的Doxygen风格注释
- 严格的错误处理和内存管理

## 许可证

（根据项目许可证填写）

