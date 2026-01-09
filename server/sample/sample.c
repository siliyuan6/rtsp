/**
 * @file sample.c
 * @brief RTSP服务器使用示例
 * 
 * 演示如何使用RTSP服务器API创建H.264流服务器
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include "api/rtsp_api.h"
#include "common/log.h"
#include "common/ringbuf/ringbuf.h"
#include "common/h264_parser/h264_parser.h"

/* 通过宏定义指定网卡名称，例如 wlan0, eth0 等 */
#ifndef NETWORK_INTERFACE
#define NETWORK_INTERFACE "wlan0"
#endif

/**
 * @brief 应用程序上下文结构体
 */
typedef struct {
	H264Parser_t *h264_parser;    // H264解析器
	RingBuffer_t *ringbuf;        // 环形缓冲区
	RTSPHandle_t *rtsp_handle;    // RTSP句柄
	pthread_t parseThread;        // 解析线程ID
	int running;                  // 运行标志
	char *streamFilePath;         // 流文件路径
	int videoFps;                 // 帧率
} AppContext_t;

// 全局上下文指针（用于回调函数访问）
static AppContext_t *g_appCtx = NULL;


/**
 * @brief 信号处理函数
 * 
 * @param sig 信号编号
 */
static void SignalHandler(int sig)
{
	if (sig == SIGINT || sig == SIGTERM)
	{
		LOG_INFO("Received signal %d, shutting down...\n", sig);
		if (g_appCtx != NULL)
		{
			g_appCtx->running = 0;
			if (g_appCtx->ringbuf != NULL)
			{
				RingBufferClose(g_appCtx->ringbuf);
			}
		}
		exit(0);
	}
}

/**
 * @brief 推送帧数据到ringbuf
 * 
 * @param rb ringbuf指针
 * @param frameData 帧数据指针
 * @param frameSize 帧大小
 * @return 成功返回0，失败返回-1
 */
static int PushFrameToRingbuf(RingBuffer_t *rb, const unsigned char *frameData, 
	size_t frameSize)
{
	if (rb == NULL || frameData == NULL || frameSize == 0)
	{
		return -1;
	}

	// 先推送4字节的长度字段（主机字节序）
	unsigned int frameSizeTmp = (unsigned int)frameSize;
	int ret = RingBufferPush(rb, &frameSizeTmp, sizeof(frameSizeTmp));
	if (ret < 0)
	{
		LOG_ERR("[ParserThread] Failed to push frame size to ringbuf\n");
		return -1;
	}

	// 再推送帧数据
	ret = RingBufferPush(rb, frameData, frameSize);
	if (ret < 0)
	{
		LOG_ERR("[ParserThread] Failed to push frame data to ringbuf\n");
		return -1;
	}

	LOG_DEBUG("[ParserThread] Frame pushed: size=%zu, ringbuf usage: %zu/%zu\n",
		frameSize, RingBufferGetUsedSize(rb), RingBufferGetUsedSize(rb) + RingBufferGetFreeSize(rb));

	return 0;
}

/**
 * @brief 清空ringbuf中的所有数据项
 * 
 * 通过逐个读取并丢弃ringbuf中的数据项来清空ringbuf
 * 数据格式：4字节长度字段 + 帧数据
 * 
 * @param rb ringbuf指针
 * @return 成功返回0，失败返回-1
 */
static int ClearRingbufAllItems(RingBuffer_t *rb)
{
	int ret = 0;
	unsigned int itemLen = 0;
	unsigned char *bufPtr = NULL;

	if (rb == NULL)
	{
		LOG_ERR("Invalid ringbuf pointer\n");
		return -1;
	}

	// 分配临时缓冲区用于丢弃数据
	bufPtr = (unsigned char *)malloc(1024 * 1024); // 1MB临时缓冲区
	if (bufPtr == NULL)
	{
		LOG_ERR("Failed to allocate temporary buffer for clearing ringbuf\n");
		return -1;
	}

	// 循环读取并丢弃所有数据项
	while (RingBufferGetItemCount(rb) > 0)
	{
		// 读取长度字段
		ret = RingBufferPop(rb, &itemLen, sizeof(unsigned int), 100);
		if (ret != 0 || itemLen <= 0)
		{
			LOG_DEBUG("Failed to read item length during ringbuf clear, ret=%d\n", ret);
			break;
		}

		// 检查长度是否合理
		if (itemLen > 1024 * 1024)
		{
			LOG_WARN("Item length too large: %u, skipping\n", itemLen);
			break;
		}

		// 读取并丢弃数据
		ret = RingBufferPop(rb, bufPtr, itemLen, 100);
		if (ret != 0)
		{
			LOG_DEBUG("Failed to read item data during ringbuf clear, ret=%d\n", ret);
			break;
		}
	}

	free(bufPtr);
	bufPtr = NULL;

	return 0;
}

/**
 * @brief 解析线程函数
 * 
 * 使用 h264_parser 解析码流文件，将帧推送到 ringbuf
 * 
 * @param arg 线程参数（AppContext_t*）
 * @return NULL
 */
static void* ParseThread(void *arg)
{
	AppContext_t *ctx = (AppContext_t *)arg;
	int ret = prctl(PR_SET_NAME, "H264Parser", 0, 0, 0);
	if (ret != 0)
	{
		LOG_WARN("[ParserThread] Failed to set thread name\n");
	}

	LOG_INFO("[ParserThread] Thread started, parsing file: %s\n", ctx->streamFilePath);

	// 分配帧缓冲区（用于拷贝帧数据，避免内部缓冲区被覆盖）
	unsigned char *frameBuf = (unsigned char *)malloc(2 * 1024 * 1024); // 2MB
	if (frameBuf == NULL)
	{
		LOG_ERR("[ParserThread] Failed to allocate frame buffer\n");
		ctx->running = 0;
		return NULL;
	}

	H264Frame_t frame;

	// 循环解析帧
	while (ctx->running)
	{
		size_t freeSize = RingBufferGetFreeSize(ctx->ringbuf);
		size_t minBufSize = 4 + 32; // 最小缓冲区需求：4字节长度 + 32字节帧数据
		if (freeSize < minBufSize)
		{
			// 缓冲区空间不足，稍作等待
			usleep(10*1000); // 10ms
			continue;
		}
		
		// 获取下一帧
		int result = H264ParserNextFrame(ctx->h264_parser, &frame);
		if (result < 0)
		{
			LOG_ERR("[ParserThread] Failed to get next frame\n");
			break;
		}
		else if (result > 0)
		{
			// EOF（仅在 loop_enabled=0 时）
			LOG_INFO("[ParserThread] End of file reached, total frames\n");
			break;
		}

		// 检查帧大小
		if (frame.size > 2 * 1024 * 1024)
		{
			LOG_ERR("[ParserThread] Frame too large: %zu bytes\n", frame.size);
			continue;
		}
		
		// 立即拷贝帧数据到缓冲区（因为 frame.data 指向内部缓冲区，可能被覆盖）
		memcpy(frameBuf, frame.data, frame.size);

		// 推送到 ringbuf（使用拷贝后的数据）
		if (PushFrameToRingbuf(ctx->ringbuf, frameBuf, frame.size) < 0)
		{
			LOG_ERR("[ParserThread] Failed to push frame to ringbuf, stopping\n");
			break;
		}

		// 控制读取速度（根据FPS动态计算）
		if (ctx->videoFps > 0)
		{
			unsigned int sleep_us = 1000000 / ctx->videoFps;
			usleep(sleep_us);
		}
	}

	free(frameBuf);

	LOG_INFO("[ParserThread] Thread exited, total frames parsed");
	return NULL;
}

/**
 * @brief 数据回调函数
 * 
 * 从循环队列中读取H.264帧数据（读取完整一帧）
 * 根据push规则：先读取4字节长度字段，再读取对应长度的帧数据
 * 
 * @param buf 数据缓冲区
 * @param bufSize 缓冲区大小
 * @return 成功返回数据长度，失败返回-1，无数据返回0
 */
static int GetDataCallback(unsigned char *buf, unsigned int bufSize)
{
	int ret = 0;
	unsigned int frameSize = 0;
	static int lastRtpSessionState = 0; // 上次RTP会话状态

	if (NULL == buf || bufSize <= 0)
	{
		return -1;
	}

	// 如果上下文未初始化，返回0（无数据）
	if (g_appCtx == NULL || g_appCtx->ringbuf == NULL)
	{
		return 0;
	}

	// 检测RTP会话状态变化：如果从非活跃变为活跃，说明是新客户端连接成功
	// 在第一次getData调用前清空ringbuf，保证数据是实时的
	int justCleared = 0;
	if (g_appCtx->rtsp_handle != NULL)
	{
		int currentRtpSessionState = 0;
		// 使用互斥锁安全访问hasActiveRtpSession
		pthread_mutex_lock(&g_appCtx->rtsp_handle->mutex);
		currentRtpSessionState = g_appCtx->rtsp_handle->hasActiveRtpSession;
		pthread_mutex_unlock(&g_appCtx->rtsp_handle->mutex);
		
		// 如果RTP会话从非活跃变为活跃，清空ringbuf
		if (currentRtpSessionState == 1 && lastRtpSessionState == 0)
		{
			LOG_INFO("[GetDataCallback] New RTSP client connected, clearing ringbuf to ensure real-time data\n");
			if (ClearRingbufAllItems(g_appCtx->ringbuf) < 0)
			{
				LOG_ERR("Failed to clear ringbuf\n");
				return -1;
			}
			justCleared = 1;
		}
		lastRtpSessionState = currentRtpSessionState;
	}

	// 第一步：从ringbuf读取4字节长度字段（主机字节序）
	ret = RingBufferPop(g_appCtx->ringbuf, &frameSize, sizeof(frameSize), 100);
	if (ret != 0)
	{
		// 超时或错误
		return (ret == 1) ? 0 : -1;
	}
	
	// 检查帧大小是否合理
	if ((frameSize == 0) || (frameSize > 10 * 1024 * 1024))
	{
		// 如果刚清空过ringbuf，可能是残留的不完整数据，返回0等待新数据
		if (justCleared)
		{
			LOG_DEBUG("[GetDataCallback] Invalid frame size after clear: %u, waiting for new data\n", frameSize);
			// 将无效的长度字段放回ringbuf（实际上无法放回，所以直接清空ringbuf并返回0）
			RingBufferClear(g_appCtx->ringbuf);
			return 0;
		}
		LOG_ERR("Invalid frame size: %u\n", frameSize);
		return -1;
	}
	
	// 检查缓冲区是否足够
	if (bufSize < frameSize)
	{
		LOG_ERR("Buffer too small: need %u bytes, got %u bytes\n", frameSize, bufSize);
		return -1;
	}
	
	// 第二步：从ringbuf读取完整帧数据
	ret = RingBufferPop(g_appCtx->ringbuf, buf, frameSize, 100);
	if (ret != 0)
	{
		LOG_ERR("Failed to read complete frame, expected %u bytes\n", frameSize);
		return (ret == 1) ? 0 : -1;
	}
	
	// 打印ringbuf数据信息
	size_t data_size = RingBufferGetUsedSize(g_appCtx->ringbuf);
	size_t free_size = RingBufferGetFreeSize(g_appCtx->ringbuf);
	size_t capacity = data_size + free_size;
	double usage = capacity > 0 ? (double)data_size * 100.0 / capacity : 0.0;
	LOG_DEBUG("[RingBuf Pop] Frame: %u bytes, DataSize: %zu bytes, FreeSize: %zu bytes, Usage: %.2f%%\n",
		frameSize, data_size, free_size, usage);
	
	// 返回完整帧长度
	return (int)frameSize;
}

/**
 * @brief 获取指定网卡的IP地址
 * 
 * @param interface_name 网卡名称，例如 "wlan0", "eth0" 等
 * @param ip_str 输出参数，存储IP地址字符串，至少需要 INET_ADDRSTRLEN 字节
 * @return 成功返回0，失败返回-1
 */
static int GetInterfaceIP(const char *interface_name, char *ip_str)
{
	struct ifaddrs *ifaddr = NULL;
	struct ifaddrs *ifa = NULL;
	int found = 0;

	if (NULL == interface_name || NULL == ip_str)
	{
		return -1;
	}

	// 获取所有网络接口信息
	if (getifaddrs(&ifaddr) == -1)
	{
		LOG_ERR("getifaddrs failed\n");
		return -1;
	}

	// 遍历所有网络接口
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next)
	{
		if (ifa->ifa_addr == NULL)
		{
			continue;
		}

		// 检查是否是目标网卡且是IPv4地址
		if (strcmp(ifa->ifa_name, interface_name) == 0 &&
			ifa->ifa_addr->sa_family == AF_INET)
		{
			struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
			
			// 将IP地址转换为字符串
			if (inet_ntop(AF_INET, &sin->sin_addr, ip_str, INET_ADDRSTRLEN) != NULL)
			{
				found = 1;
				break;
			}
		}
	}

	// 释放资源
	freeifaddrs(ifaddr);

	if (!found)
	{
		LOG_WARN("Failed to find IP address for interface: %s\n", interface_name);
		return -1;
	}

	return 0;
}

/**
 * @brief 主函数
 * 
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 成功返回0，失败返回-1
 */
int main(int argc, char *argv[])
{
	int ret = 0;
	const char *streamFilePath = NULL;
	size_t ringbuf_size = 2 * 1024 * 1024; // 2MB

	AppContext_t ctx = {0};
	RTSPConfig_t config;
	RTSPStatus_t status;

	// 解析命令行参数
	if (argc < 2)
	{
		LOG_ERR("Usage: %s <streamFilePath> [debug]\n", argv[0]);
		return -1;
	}
	streamFilePath = argv[1];

	// 检查是否有debug参数
	if (argc >= 3 && strcmp(argv[2], "debug") == 0)
	{
		SetLogDebugEnabled(1);
		LOG_INFO("Debug logging enabled\n");
	}

	// 注册信号处理
	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);

	// 设置全局上下文指针（用于回调函数）
	g_appCtx = &ctx;

	// 配置RTSP服务器
	memset(&config, 0, sizeof(config));
	config.rtspPort = 8554; // RTSP监听端口
	config.rtpPort = 5000;  // RTP端口
	config.format = RTSP_FORMAT_H264; // H.264格式
	config.fps = 30; // 30帧/秒
	ctx.videoFps = config.fps;

	LOG_INFO("RTSP Server Sample\n");
	LOG_INFO("RTSP Port: %d\n", config.rtspPort);
	LOG_INFO("RTP Port: %d\n", config.rtpPort);
	LOG_INFO("Format: H.264\n");
	LOG_INFO("FPS: %d\n", config.fps);

	// 1. 创建H264解析模块
	ctx.streamFilePath = strdup(streamFilePath);
	if (ctx.streamFilePath == NULL)
	{
		LOG_ERR("Failed to allocate memory for stream file path\n");
		return -1;
	}
	ctx.h264_parser = H264ParserCreate(ctx.streamFilePath, 0, 1); // loop_enabled=1
	if (ctx.h264_parser == NULL)
	{
		LOG_ERR("Failed to create H264 parser\n");
		free(ctx.streamFilePath);
		return -1;
	}
	LOG_INFO("H264 parser created\n");

	// 2. 创建RingBuf模块
	ctx.ringbuf = RingBufferCreate(ringbuf_size);
	if (ctx.ringbuf == NULL)
	{
		LOG_ERR("Failed to create ring buffer\n");
		H264ParserDestroy(ctx.h264_parser);
		free(ctx.streamFilePath);
		return -1;
	}
	LOG_INFO("Ring buffer created, size: %zu bytes\n", ringbuf_size);

	// 3. 创建RTSP服务器（传入GetDataCallback）
	ret = RTSPCreate(&ctx.rtsp_handle, &config, GetDataCallback);
	if (ret < 0)
	{
		LOG_ERR("RTSPCreate failed\n");
		RingBufferDestroy(ctx.ringbuf);
		H264ParserDestroy(ctx.h264_parser);
		free(ctx.streamFilePath);
		return -1;
	}
	LOG_INFO("RTSP server created\n");

	// 4. 创建解析线程（从H264解析器取帧推送到ringbuf）
	ctx.running = 1;
	ret = pthread_create(&ctx.parseThread, NULL, ParseThread, &ctx);
	if (ret != 0)
	{
		LOG_ERR("Failed to create parse thread\n");
		RTSPDestroy(ctx.rtsp_handle);
		RingBufferDestroy(ctx.ringbuf);
		H264ParserDestroy(ctx.h264_parser);
		free(ctx.streamFilePath);
		return -1;
	}
	LOG_INFO("Parse thread created\n");

	LOG_INFO("RTSP server started successfully\n");
	
	// 获取指定网卡的IP地址
	char server_ip[INET_ADDRSTRLEN] = "localhost";
	if (GetInterfaceIP(NETWORK_INTERFACE, server_ip) == 0)
	{
		LOG_INFO("Network interface %s IP: %s\n", NETWORK_INTERFACE, server_ip);
		LOG_WARN("You can connect using: rtsp://%s:%d/live\n",
			server_ip, config.rtspPort);
	}
	else
	{
		LOG_WARN("Failed to get IP for interface %s, using localhost\n", 
			NETWORK_INTERFACE);
		LOG_WARN("You can connect using: rtsp://localhost:%d/live\n",
			config.rtspPort);
	}
	LOG_INFO("Press Ctrl+C to stop\n");

	// 5. 主循环：检查RTSP状态和ringbuf状态
	while (1)
	{
		sleep(5);

		// 检查RTSP状态
		if (ctx.rtsp_handle != NULL)
		{
			ret = RTSPGetStatus(ctx.rtsp_handle, &status);
			if (ret == 0)
			{
				if (status == RTSP_STATUS_STOPPED)
				{
					LOG_INFO("RTSP server stopped\n");
					break;
				}
			}
		}
		else
		{
			break;
		}

		// 检查运行标志
		if (!ctx.running)
		{
			LOG_INFO("Received shutdown signal\n");
			break;
		}

		// 检查ringbuf状态（可选，用于监控）
		if (ctx.ringbuf != NULL)
		{
			size_t data_size = RingBufferGetUsedSize(ctx.ringbuf);
			size_t free_size = RingBufferGetFreeSize(ctx.ringbuf);
			size_t itemCount = RingBufferGetItemCount(ctx.ringbuf);
			LOG_INFO("[MainLoop] Ringbuf: DataSize=%zu, FreeSize=%zu, ItemCount=%zu\n", 
				data_size, free_size, itemCount/2); // 每帧占2个item（长度+数据）
		}
	}

	// 6. 清理：依次销毁 RTSP、ringbuf、H264解析器
	LOG_INFO("Cleaning up resources...\n");

	// 停止线程
	ctx.running = 0;
	if (ctx.ringbuf != NULL)
	{
		RingBufferClose(ctx.ringbuf);
	}

	// 等待解析线程结束
	if (ctx.parseThread != 0)
	{
		pthread_join(ctx.parseThread, NULL);
		ctx.parseThread = 0;
	}

	// 销毁RTSP
	if (ctx.rtsp_handle != NULL)
	{
		RTSPDestroy(ctx.rtsp_handle);
		ctx.rtsp_handle = NULL;
		LOG_INFO("RTSP destroyed\n");
	}

	// 销毁ringbuf
	if (ctx.ringbuf != NULL)
	{
		RingBufferDestroy(ctx.ringbuf);
		ctx.ringbuf = NULL;
		LOG_INFO("Ringbuf destroyed\n");
	}

	// 销毁H264解析器
	if (ctx.h264_parser != NULL)
	{
		H264ParserDestroy(ctx.h264_parser);
		ctx.h264_parser = NULL;
		LOG_INFO("H264 parser destroyed\n");
	}

	// 释放文件路径
	if (ctx.streamFilePath != NULL)
	{
		free(ctx.streamFilePath);
		ctx.streamFilePath = NULL;
	}

	g_appCtx = NULL;

	LOG_INFO("RTSP server sample exited\n");
	return 0;
}

