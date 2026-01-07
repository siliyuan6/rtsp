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
 * @brief RTSP模块句柄结构体
 */
typedef struct
{
	int rtspFd;            // RTSP socket文件描述符
	int rtpFd;             // RTP socket文件描述符
	int rtcpFd;            // RTCP socket文件描述符（预留）
	RTSPConfig_t config;   // 配置信息
	pthread_t rtspThread;  // RTSP处理线程ID
	pthread_t rtpThread;   // RTP发送线程ID
	int isRunning;         // 运行标志
	struct sockaddr_in clientAddr; // 客户端地址
	int (*getData)(unsigned char *buf, unsigned int bufSize); // 数据回调函数
	pthread_mutex_t mutex; // 互斥锁
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
	int (*getData)(unsigned char *buf, unsigned int bufSize));

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

#endif /* RTSP_API_H */

