/**
 * @file rtsp_api.c
 * @brief RTSP服务器API实现
 * 
 * 实现RTSP服务器的创建、销毁和状态查询功能
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "rtsp_api.h"
#include "network/network.h"
#include "core/rtsp.h"
#include "common/log.h"

/**
 * @brief 创建RTSP模块句柄
 * 
 * @param handle 输出参数，返回创建的句柄指针
 * @param config RTSP配置
 * @param getData 数据回调函数，用于获取码流数据
 * @return 成功返回0，失败返回-1
 */
int RTSPCreate(RTSPHandle_t **handle, const RTSPConfig_t *config,
	GetDataCallback_t getData)
{
	RTSPHandle_t *h = NULL;
	int ret = 0;

	if (NULL == handle || NULL == config || NULL == getData)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	// 分配句柄内存
	h = (RTSPHandle_t*)malloc(sizeof(RTSPHandle_t));
	if (NULL == h)
	{
		LOG_ERR("malloc failed\n");
		return -1;
	}

	memset(h, 0, sizeof(RTSPHandle_t));

	// 复制配置
	h->config = *config;
	h->getData = getData;
	h->isRunning = 1;
	h->hasActiveRtpSession = 0; // 初始化为0，表示没有活跃的RTP会话
	h->rtspListenFd = -1;
	h->rtpFd = -1;
	h->rtcpFd = -1;
	h->sessionId = 0; // 初始化为0，在SETUP时生成
	h->transportMode = RTSP_TRANSPORT_INVALID;
	h->rtspClientFd = -1;
	h->rtpChannel = 0; // 默认RTP通道号
	h->rtcpChannel = 1; // 默认RTCP通道号
	
	// 初始化RTCP统计回调函数
	h->rtcpStatsCallback = NULL;
	h->rtcpStatsUserData = NULL;
	memset(&h->rtcpStats, 0, sizeof(RTCPStats_t));

	// 初始化互斥锁
	ret = pthread_mutex_init(&h->mutex, NULL);
	if (ret != 0)
	{
		LOG_ERR("pthread_mutex_init failed\n");
		free(h);
		h = NULL;
		return -1;
	}

	// 创建RTSP监听socket
	h->rtspListenFd = CreateRtspSocket(config->rtspPort);
	if (h->rtspListenFd < 0)
	{
		LOG_ERR("CreateRtspSocket failed\n");
		pthread_mutex_destroy(&h->mutex);
		free(h);
		h = NULL;
		return -1;
	}

	// 创建RTSP处理线程
	ret = pthread_create(&h->rtspThread, NULL, RTSPHandleThread, h);
	if (ret != 0)
	{
		LOG_ERR("pthread_create RTSP thread failed, ret=%d\n", ret);
		close(h->rtspListenFd);
		pthread_mutex_destroy(&h->mutex);
		free(h);
		h = NULL;
		return -1;
	}

	LOG_INFO("RTSP server created, listening on port %d, RTSP thread created (tid=%lu)\n",
		config->rtspPort, (unsigned long)h->rtspThread);

	*handle = h;
	return 0;
}

/**
 * @brief 销毁RTSP模块
 * 
 * @param handle RTSP句柄指针
 * @return 成功返回0，失败返回-1
 */
int RTSPDestroy(RTSPHandle_t *handle)
{
	if (NULL == handle)
	{
		LOG_ERR("Invalid handle\n");
		return -1;
	}

	// 停止运行标志
	pthread_mutex_lock(&handle->mutex);
	handle->isRunning = 0;
	handle->hasActiveRtpSession = 0; // 停止RTP会话
	pthread_mutex_unlock(&handle->mutex);

	// 等待RTSP线程结束
	if (handle->rtspThread != 0)
	{
		pthread_join(handle->rtspThread, NULL);
	}

	// 等待RTP线程结束
	if (handle->rtpThread != 0)
	{
		pthread_join(handle->rtpThread, NULL);
	}

	// 关闭socket
	if (handle->rtspListenFd >= 0)
	{
		close(handle->rtspListenFd);
		handle->rtspListenFd = -1;
	}

	if (handle->rtpFd >= 0)
	{
		close(handle->rtpFd);
		handle->rtpFd = -1;
	}

	if (handle->rtcpFd >= 0)
	{
		close(handle->rtcpFd);
		handle->rtcpFd = -1;
	}

	// 注意：rtpFd 在 UDP 和 TCP Separate 模式下都会使用，已在上面关闭
	// 注意：rtspClientFd 是客户端连接，由 RTSP 线程管理，不需要在这里关闭

	// 销毁互斥锁
	pthread_mutex_destroy(&handle->mutex);

	// 释放内存
	free(handle);
	handle = NULL;

	LOG_INFO("RTSP server destroyed\n");
	return 0;
}

/**
 * @brief 获取当前RTSP状态
 * 
 * @param handle RTSP句柄指针
 * @param status 输出参数，返回状态
 * @return 成功返回0，失败返回-1
 */
int RTSPGetStatus(RTSPHandle_t *handle, RTSPStatus_t *status)
{
	if (NULL == handle || NULL == status)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	pthread_mutex_lock(&handle->mutex);
	if (handle->isRunning)
	{
		*status = RTSP_STATUS_RUNNING;
	}
	else
	{
		*status = RTSP_STATUS_STOPPED;
	}
	pthread_mutex_unlock(&handle->mutex);

	return 0;
}

/**
 * @brief 设置RTCP统计回调函数
 * 
 * @param handle RTSP句柄指针
 * @param callback RTCP统计回调函数（可为NULL）
 * @param userData 用户数据（可为NULL）
 * @return 成功返回0，失败返回-1
 */
int RTSPSetRTCPStatsCallback(RTSPHandle_t *handle, RTCPStatsCallback_t callback,
	void *userData)
{
	if (NULL == handle)
	{
		LOG_ERR("Invalid handle\n");
		return -1;
	}

	pthread_mutex_lock(&handle->mutex);
	handle->rtcpStatsCallback = callback;
	handle->rtcpStatsUserData = userData;
	pthread_mutex_unlock(&handle->mutex);

	LOG_INFO("RTCP stats callback %s\n", callback ? "set" : "cleared");
	return 0;
}

