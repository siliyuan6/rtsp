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
#include "../network/network.h"
#include "../core/rtsp.h"
#include "../../common/log.h"

/**
 * @brief 创建RTSP模块句柄
 * 
 * @param handle 输出参数，返回创建的句柄指针
 * @param config RTSP配置
 * @param getData 数据回调函数，用于获取码流数据
 * @return 成功返回0，失败返回-1
 */
int RTSPCreate(RTSPHandle_t **handle, const RTSPConfig_t *config,
	int (*getData)(unsigned char *buf, unsigned int bufSize))
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
	h->rtspFd = -1;
	h->rtpFd = -1;
	h->rtcpFd = -1;
	h->sessionId = 0; // 初始化为0，在SETUP时生成

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
	h->rtspFd = CreateRtspSocket(config->rtspPort);
	if (h->rtspFd < 0)
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
		LOG_ERR("pthread_create RTSP thread failed\n");
		close(h->rtspFd);
		pthread_mutex_destroy(&h->mutex);
		free(h);
		h = NULL;
		return -1;
	}

	LOG("RTSP server created, listening on port %d\n",
		config->rtspPort);

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
	if (handle->rtspFd >= 0)
	{
		close(handle->rtspFd);
		handle->rtspFd = -1;
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

	// 销毁互斥锁
	pthread_mutex_destroy(&handle->mutex);

	// 释放内存
	free(handle);
	handle = NULL;

	LOG("RTSP server destroyed\n");
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

