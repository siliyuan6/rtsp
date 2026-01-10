/**
 * @file rtsp_api.h
 * @brief RTSP服务器API接口定义
 * 
 * 提供RTSP服务器的创建、销毁和状态查询功能
 */

#ifndef RTSP_API_H
#define RTSP_API_H

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stddef.h>
#include "common/h264_parser/h264_parser.h"

/**
 * @brief 流格式枚举
 */
typedef enum
{
	RTSP_FORMAT_H264,
	RTSP_FORMAT_H265,
	RTSP_FORMAT_MAX
} RTSPStreamFormat_t;

/**
 * @brief RTSP配置结构体
 */
typedef struct
{
	int rtspPort;          // RTSP监听端口（默认8554）
	int rtpPort;           // RTP端口（默认5000）
	RTSPStreamFormat_t format; // 数据格式
	int fps;               // 帧率（用于时间戳计算）
} RTSPConfig_t;

/**
 * @brief RTSP状态枚举
 */
typedef enum
{
	RTSP_STATUS_STOPPED,
	RTSP_STATUS_RUNNING,
	RTSP_STATUS_ERROR
} RTSPStatus_t;

/**
 * @brief RTP传输模式枚举
 */
typedef enum
{
	RTSP_TRANSPORT_INVALID = 0,		 // 无效传输模式
	RTSP_TRANSPORT_UDP,              // UDP传输
	RTSP_TRANSPORT_TCP_INTERLEAVED,  // TCP interleaved (复用RTSP连接)
} RTSPTransportMode_t;

/**
 * @brief RTCP统计信息结构体
 */
typedef struct
{
	unsigned int ssrc;                // SSRC of packet sender
	unsigned char fractionLost;       // 丢包率（0-255，表示0-100%）
	unsigned int cumulativePacketsLost; // 累计丢包数（24位，最高位为符号位）
	unsigned int extendedHighestSeq;  // 最高序列号（32位）
	unsigned int jitter;              // 延迟抖动（RTP时间戳单位）
	unsigned int lastSRTimestamp;     // 最后SR时间戳（32位）
	unsigned int delaySinceLastSR;    // 自上次SR的延迟（32位，单位1/65536秒）
} RTCPStats_t;

/**
 * @brief RTCP统计回调函数类型
 * 
 * @param stats RTCP统计信息
 * @param userData 用户数据
 */
typedef void (*RTCPStatsCallback_t)(const RTCPStats_t *stats, void *userData);

/**
 * @brief 帧信息结构体（用于GetDataCallback返回）
 */
typedef struct {
	const unsigned char *data;    // 帧数据地址
	size_t size;                   // 帧大小
	H264FrameType_t type;         // 帧类型
} FrameInfo_t;

/**
 * @brief 数据回调函数类型
 * 
 * @param frameInfo 输出参数，返回帧信息（地址、长度、类型）
 * @return 成功返回0，失败返回-1，无数据返回1
 */
typedef int (*GetDataCallback_t)(FrameInfo_t *frameInfo);

/**
 * @brief RTSP模块句柄结构体
 */
typedef struct
{
	int rtspListenFd;                  // RTSP socket文件描述符（监听socket）
	int rtspClientFd;                  // RTSP客户端socket (用于TCP interleaved模式，复用RTSP连接)
	int rtpFd;                         // RTP socket文件描述符 (UDP或TCP separate模式)
	int rtcpFd;                        // RTCP socket文件描述符（预留）
	unsigned char rtpChannel;          // RTP通道号 (interleaved模式，默认0)
	unsigned char rtcpChannel;         // RTCP通道号 (interleaved模式，默认1)
	struct sockaddr_in clientAddr;     // RTSP客户端地址和端口
	struct sockaddr_in clientRtpAddr;  // RTP客户端地址和端口 (UDP或TCP separate模式)

	RTSPConfig_t config;               // 配置信息
	pthread_t rtspThread;              // RTSP处理线程ID
	GetDataCallback_t getData;         // 数据回调函数

	int isRunning;                     // 运行标志
	int hasActiveRtpSession;           // RTP会话是否活跃
	pthread_t rtpThread;               // RTP发送线程ID
	pthread_mutex_t mutex;             // 互斥锁
	unsigned int sessionId;            // RTSP会话ID
	RTSPTransportMode_t transportMode; // 传输模式
	
	// RTCP统计回调函数
	RTCPStatsCallback_t rtcpStatsCallback; // RTCP统计回调函数
	void *rtcpStatsUserData;              // 回调函数用户数据
	RTCPStats_t rtcpStats;                // 当前RTCP统计信息
} RTSPHandle_t;

/**
 * @brief 创建RTSP模块句柄
 * 
 * @param handle 输出参数，返回创建的句柄指针
 * @param config RTSP配置
 * @param getData 数据回调函数，用于获取码流数据
 * @return 成功返回0，失败返回-1
 */
int RTSPCreate(RTSPHandle_t **handle, const RTSPConfig_t *config,
	GetDataCallback_t getData);

/**
 * @brief 销毁RTSP模块
 * 
 * @param handle RTSP句柄指针
 * @return 成功返回0，失败返回-1
 */
int RTSPDestroy(RTSPHandle_t *handle);

/**
 * @brief 获取当前RTSP状态
 * 
 * @param handle RTSP句柄指针
 * @param status 输出参数，返回状态
 * @return 成功返回0，失败返回-1
 */
int RTSPGetStatus(RTSPHandle_t *handle, RTSPStatus_t *status);

/**
 * @brief 设置RTCP统计回调函数
 * 
 * @param handle RTSP句柄指针
 * @param callback RTCP统计回调函数（可为NULL）
 * @param userData 用户数据（可为NULL）
 * @return 成功返回0，失败返回-1
 */
int RTSPSetRTCPStatsCallback(RTSPHandle_t *handle, RTCPStatsCallback_t callback, 
	void *userData);

#endif /* RTSP_API_H */

