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
#include <time.h>
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
 * @brief 帧信息结构体（存储在ringbuf中）
 */
typedef struct {
	unsigned int frameSize;        // 帧大小
	H264FrameType_t frameType;     // 帧类型
} RingbufFrameHeader_t;

/**
 * @brief 应用程序上下文结构体
 */
typedef struct {
	H264Parser_t *h264ParserHandle;      // H264解析器
	H264FrameClassifier_t *classifier;   // H264帧分类器
	RingBuffer_t *ringbuf;               // 环形缓冲区
	RTSPHandle_t *rtspHandle;            // RTSP句柄
	pthread_t parseThread;               // 解析线程ID
	int running;                         // 运行标志
	char *streamFilePath;                // 流文件路径
	int videoFps;                        // 帧率
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
 * @brief Pop最旧的一帧（用于覆盖写入）
 * 
 * @param rb ringbuf指针
 * @return 成功返回0，失败返回-1（无数据可pop）
 */
static int PopOldestFrame(RingBuffer_t *rb)
{
	if (rb == NULL)
	{
		return -1;
	}

	// 检查是否有数据
	if (RingBufferGetItemCount(rb) == 0)
	{
		return -1; // 无数据可pop
	}

	RingbufFrameHeader_t frameHeader;
	unsigned char *tempBuf = NULL;

	// 读取帧头（非阻塞）
	int ret = RingBufferPop(rb, &frameHeader, sizeof(frameHeader), 0);
	if (ret != 0 || frameHeader.frameSize == 0)
	{
		return -1; // 读取失败或无效长度
	}

	// 检查长度是否合理
	if (frameHeader.frameSize > 4 * 1024 * 1024)
	{
		LOG_WARN("[PopOldestFrame] Frame size too large: %u, skipping\n", frameHeader.frameSize);
		return -1;
	}

	// 分配临时缓冲区用于丢弃数据
	tempBuf = (unsigned char *)malloc(frameHeader.frameSize);
	if (tempBuf == NULL)
	{
		LOG_ERR("[PopOldestFrame] Failed to allocate temp buffer for frame size: %u\n", frameHeader.frameSize);
		return -1;
	}

	// 读取并丢弃帧数据（非阻塞）
	ret = RingBufferPop(rb, tempBuf, frameHeader.frameSize, 0);
	free(tempBuf);

	if (ret != 0)
	{
		LOG_DEBUG("[PopOldestFrame] Failed to pop frame data, ret=%d\n", ret);
		return -1;
	}

	LOG_DEBUG("[PopOldestFrame] Popped oldest frame: size=%u, type=%d\n", 
		frameHeader.frameSize, frameHeader.frameType);
	return 0;
}

/**
 * @brief 推送帧数据到ringbuf（覆盖写入模式）
 * 
 * 如果空间不足，会自动pop最旧的帧，直到有足够空间
 * 
 * @param rb ringbuf指针
 * @param frameData 帧数据指针
 * @param frameSize 帧大小
 * @param frameType 帧类型
 * @return 成功返回0，失败返回-1
 */
static int PushFrameToRingbuf(RingBuffer_t *rb, const unsigned char *frameData, 
	size_t frameSize, H264FrameType_t frameType)
{
	if (rb == NULL || frameData == NULL || frameSize == 0)
	{
		return -1;
	}

	// 计算需要的总空间：帧头（大小+类型） + 帧数据
	size_t requiredSize = sizeof(RingbufFrameHeader_t) + frameSize;
	size_t freeSize = RingBufferGetFreeSize(rb);

	// 如果空间不足，pop最旧的帧直到有足够空间
	int poppedCount = 0;
	while (freeSize < requiredSize)
	{
		if (PopOldestFrame(rb) < 0)
		{
			// 无法pop更多帧（可能ringbuf已空），但仍然尝试写入
			// RingBufferPush会在空间不足时阻塞等待，或者如果ringbuf已关闭则返回失败
			if (poppedCount > 0)
			{
				LOG_DEBUG("[PushFrameToRingbuf] Popped %d old frame(s), freeSize=%zu, required=%zu\n",
					poppedCount, freeSize, requiredSize);
			}
			break;
		}
		poppedCount++;
		freeSize = RingBufferGetFreeSize(rb);
	}
	
	if (poppedCount > 0)
	{
		LOG_DEBUG("[PushFrameToRingbuf] Popped %d old frame(s) to make room, new freeSize=%zu\n",
			poppedCount, freeSize);
	}

	// 构造帧头
	RingbufFrameHeader_t frameHeader;
	frameHeader.frameSize = (unsigned int)frameSize;
	frameHeader.frameType = frameType;

	// 先推送帧头（包含大小和类型）
	int ret = RingBufferPush(rb, &frameHeader, sizeof(frameHeader));
	if (ret < 0)
	{
		LOG_ERR("[PushFrameToRingbuf] Failed to push frame header to ringbuf\n");
		return -1;
	}

	// 再推送帧数据
	ret = RingBufferPush(rb, frameData, frameSize);
	if (ret < 0)
	{
		LOG_ERR("[PushFrameToRingbuf] Failed to push frame data to ringbuf\n");
		return -1;
	}

	LOG_DEBUG("[PushFrameToRingbuf] Frame pushed: size=%zu, ringbuf usage: %zu/%zu\n",
		frameSize, RingBufferGetUsedSize(rb), 
		RingBufferGetUsedSize(rb) + RingBufferGetFreeSize(rb));

	return 0;
}

/**
 * @brief 解析线程函数
 * 
 * 使用 h264ParserHandle 解析码流文件，将帧推送到 ringbuf
 * 使用帧分类器实现I帧组合（SPS+PPS+SEI+IDR）和P/B帧判断
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
	unsigned char *frameBuf = (unsigned char *)malloc(4 * 1024 * 1024); // 4MB（足够大以容纳组合后的I帧）
	if (frameBuf == NULL)
	{
		LOG_ERR("[ParserThread] Failed to allocate frame buffer\n");
		ctx->running = 0;
		return NULL;
	}

	H264Frame_t frame;
	H264ClassifiedFrame_t classifiedFrame;

	// 循环解析帧
	while (ctx->running)
	{
		// 获取下一帧
		int result = H264ParserNextFrame(ctx->h264ParserHandle, &frame);
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
		if (frame.size > 4 * 1024 * 1024)
		{
			LOG_ERR("[ParserThread] Frame too large: %zu bytes\n", frame.size);
			continue;
		}
		
		// 使用帧分类器处理帧
		int classifyResult = H264FrameClassifierProcess(ctx->classifier, &frame, &classifiedFrame);
		if (classifyResult < 0)
		{
			LOG_ERR("[ParserThread] Failed to classify frame\n");
			continue;
		}
		else if (classifyResult > 0)
		{
			// 需要继续处理（如SPS/PPS/SEI被缓存），不输出帧
			continue;
		}
		
		// 成功分类，立即拷贝帧数据到缓冲区（因为 classifiedFrame.data 指向内部缓冲区，可能被覆盖）
		if (classifiedFrame.size > 4 * 1024 * 1024)
		{
			LOG_ERR("[ParserThread] Classified frame too large: %zu bytes\n", classifiedFrame.size);
			continue;
		}
		
		memcpy(frameBuf, classifiedFrame.data, classifiedFrame.size);
		
		// 根据帧类型记录日志
		const char *frameTypeStr = "Unknown";
		switch (classifiedFrame.type)
		{
			case H264_FRAME_TYPE_I:
				frameTypeStr = "I";
				break;
			case H264_FRAME_TYPE_P:
				frameTypeStr = "P";
				break;
			case H264_FRAME_TYPE_B:
				frameTypeStr = "B";
				break;
			case H264_FRAME_TYPE_OTHER:
				frameTypeStr = "Other";
				break;
			default:
				break;
		}
		
		LOG_DEBUG("[ParserThread] %s frame, size: %zu\n", frameTypeStr, classifiedFrame.size);
		
		// 推送到 ringbuf（包含帧类型信息）
		if (PushFrameToRingbuf(ctx->ringbuf, frameBuf, classifiedFrame.size, classifiedFrame.type) < 0)
		{
			LOG_ERR("[ParserThread] Failed to push %s frame to ringbuf, stopping\n", frameTypeStr);
			break;
		}

		// 获取上一帧到现在的时间，控制帧率
		struct timespec tsTime;
		clock_gettime(CLOCK_MONOTONIC, &tsTime);

		static time_t lastFrameTimeUs = 0;
		int expectSleepUs = ctx->videoFps > 0 ? (1000000 / ctx->videoFps) : 33333; // 默认30fps
		if (lastFrameTimeUs != 0)
		{
			time_t elapsedUs = (tsTime.tv_sec * 1000000 + tsTime.tv_nsec / 1000 - lastFrameTimeUs);
			if (elapsedUs < expectSleepUs)
			{
				usleep(expectSleepUs - elapsedUs);
			}
		}
		lastFrameTimeUs = tsTime.tv_sec * 1000000 + tsTime.tv_nsec / 1000;
	}

	// 释放缓冲区
	free(frameBuf);

	LOG_INFO("[ParserThread] Thread exited, total frames parsed");
	return NULL;
}

/**
 * @brief 数据回调函数
 * 
 * 从循环队列中读取H.264帧数据（读取完整一帧）
 * 根据push规则：先读取帧头（大小+类型），再读取对应长度的帧数据
 * 
 * @param frameInfo 输出参数，返回帧信息（地址、长度、类型）
 * @return 成功返回0，失败返回-1，无数据返回1
 */
static int GetDataCallback(FrameInfo_t *frameInfo)
{
	int ret = 0;
	RingbufFrameHeader_t frameHeader;
	static unsigned char *frameBuf = NULL;
	static size_t frameBufSize = 0;

	if (frameInfo == NULL)
	{
		return -1;
	}

	// 初始化输出参数
	frameInfo->data = NULL;
	frameInfo->size = 0;
	frameInfo->type = H264_FRAME_TYPE_NONE;

	// 如果上下文未初始化，返回无数据
	if (g_appCtx == NULL || g_appCtx->ringbuf == NULL)
	{
		return 1; // 无数据
	}

	// 分配或扩展帧缓冲区（如果需要）
	if (frameBuf == NULL || frameBufSize < 4 * 1024 * 1024)
	{
		if (frameBuf != NULL)
		{
			free(frameBuf);
		}
		frameBufSize = 4 * 1024 * 1024; // 4MB
		frameBuf = (unsigned char *)malloc(frameBufSize);
		if (frameBuf == NULL)
		{
			LOG_ERR("[GetDataCallback] Failed to allocate frame buffer\n");
			return -1;
		}
	}

	// 第一步：从ringbuf读取帧头（包含大小和类型）
	ret = RingBufferPop(g_appCtx->ringbuf, &frameHeader, sizeof(frameHeader), 100);
	if (ret != 0)
	{
		// 超时或错误
		return (ret == 1) ? 1 : -1; // 1表示无数据，-1表示错误
	}
	
	// 检查帧大小是否合理
	if ((frameHeader.frameSize == 0) || (frameHeader.frameSize > 4 * 1024 * 1024))
	{
		LOG_ERR("[GetDataCallback] Invalid frame size: %u\n", frameHeader.frameSize);
		return -1;
	}
	
	// 检查缓冲区是否足够，如果不够则扩展
	if (frameBufSize < frameHeader.frameSize)
	{
		free(frameBuf);
		frameBufSize = frameHeader.frameSize;
		frameBuf = (unsigned char *)malloc(frameBufSize);
		if (frameBuf == NULL)
		{
			LOG_ERR("[GetDataCallback] Failed to allocate frame buffer for size: %u\n", frameHeader.frameSize);
			return -1;
		}
	}
	
	// 第二步：从ringbuf读取完整帧数据
	ret = RingBufferPop(g_appCtx->ringbuf, frameBuf, frameHeader.frameSize, 100);
	if (ret != 0)
	{
		LOG_ERR("[GetDataCallback] Failed to read complete frame, expected %u bytes\n", frameHeader.frameSize);
		return (ret == 1) ? 1 : -1;
	}
	
	// 设置输出参数
	frameInfo->data = frameBuf;
	frameInfo->size = frameHeader.frameSize;
	frameInfo->type = frameHeader.frameType;
	
	// 打印ringbuf数据信息
	size_t usedSize = RingBufferGetUsedSize(g_appCtx->ringbuf);
	size_t freeSize = RingBufferGetFreeSize(g_appCtx->ringbuf);
	size_t capacity = usedSize + freeSize;
	double usage = capacity > 0 ? (double)usedSize * 100.0 / capacity : 0.0;
	
	const char *frameTypeStr = "Unknown";
	switch (frameHeader.frameType)
	{
		case H264_FRAME_TYPE_I:
			frameTypeStr = "I";
			break;
		case H264_FRAME_TYPE_P:
			frameTypeStr = "P";
			break;
		case H264_FRAME_TYPE_B:
			frameTypeStr = "B";
			break;
		case H264_FRAME_TYPE_OTHER:
			frameTypeStr = "Other";
			break;
		default:
			break;
	}
	
	LOG_DEBUG("[GetDataCallback] Frame: %s, size=%u, ringbuf usage: %.2f%%\n",
		frameTypeStr, frameHeader.frameSize, usage);
	
	// 返回成功
	return 0;
}

/**
 * @brief RTCP统计回调函数
 */
static void RTCPStatsCallback(const RTCPStats_t *stats, void *userData)
{
	(void) userData;
	if (stats == NULL)
	{
		return;
	}
	LOG_WARN("[RTCPStats] SSRC: %u, FractionLost: %u, CumulativePacketsLost: %u, "
		"ExtendedHighestSeq: %u, Jitter: %u, LastSRTimestamp: %u, DelaySinceLastSR: %u\n",
		stats->ssrc,                  // SSRC
		stats->fractionLost,          // 丢包率
		stats->cumulativePacketsLost, // 累计丢包数
		stats->extendedHighestSeq,    // 最高序列号
		stats->jitter,                // 抖动
		stats->lastSRTimestamp,       // 最后SR时间戳
		stats->delaySinceLastSR);     // 自上次SR的延迟
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
	ctx.h264ParserHandle = H264ParserCreate(ctx.streamFilePath, 0, 1); // loop_enabled=1
	if (ctx.h264ParserHandle == NULL)
	{
		LOG_ERR("Failed to create H264 parser\n");
		return -1;
	}
	
	// 创建帧分类器
	ctx.classifier = H264FrameClassifierCreate(0); // 使用默认缓冲区大小
	if (ctx.classifier == NULL)
	{
		LOG_ERR("Failed to create H264 frame classifier\n");
		H264ParserDestroy(ctx.h264ParserHandle);
		return -1;
	}
	if (ctx.h264ParserHandle == NULL)
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
		H264FrameClassifierDestroy(ctx.classifier);
		H264ParserDestroy(ctx.h264ParserHandle);
		free(ctx.streamFilePath);
		return -1;
	}
	LOG_INFO("Ring buffer created, size: %zu bytes\n", ringbuf_size);

	// 3. 创建RTSP服务器（传入GetDataCallback）
	ret = RTSPCreate(&ctx.rtspHandle, &config, GetDataCallback);
	if (ret < 0)
	{
		LOG_ERR("RTSPCreate failed\n");
		RingBufferDestroy(ctx.ringbuf);
		H264FrameClassifierDestroy(ctx.classifier);
		H264ParserDestroy(ctx.h264ParserHandle);
		free(ctx.streamFilePath);
		return -1;
	}
	LOG_INFO("RTSP server created\n");

	// 4. 配置RTCP统计回调
	ret = RTSPSetRTCPStatsCallback(ctx.rtspHandle, RTCPStatsCallback, NULL);
	if (ret != 0)
	{
		LOG_ERR("Failed to set RTCP stats callback\n");
		RTSPDestroy(ctx.rtspHandle);
		RingBufferDestroy(ctx.ringbuf);
		H264FrameClassifierDestroy(ctx.classifier);
		H264ParserDestroy(ctx.h264ParserHandle);
		free(ctx.streamFilePath);
		return -1;
	}

	// 5. 创建解析线程（从H264解析器取帧推送到ringbuf）
	ctx.running = 1;
	ret = pthread_create(&ctx.parseThread, NULL, ParseThread, &ctx);
	if (ret != 0)
	{
		LOG_ERR("Failed to create parse thread\n");
		RTSPDestroy(ctx.rtspHandle);
		RingBufferDestroy(ctx.ringbuf);
		H264FrameClassifierDestroy(ctx.classifier);
		H264ParserDestroy(ctx.h264ParserHandle);
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
		if (ctx.rtspHandle != NULL)
		{
			ret = RTSPGetStatus(ctx.rtspHandle, &status);
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
	if (ctx.rtspHandle != NULL)
	{
		RTSPDestroy(ctx.rtspHandle);
		ctx.rtspHandle = NULL;
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
	if (ctx.h264ParserHandle != NULL)
	{
		H264ParserDestroy(ctx.h264ParserHandle);
		ctx.h264ParserHandle = NULL;
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

