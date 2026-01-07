/**
 * @file rtsp.h
 * @brief RTSP协议处理层接口定义
 * 
 * 提供RTSP协议请求处理和SDP生成功能
 */

#ifndef RTSP_H
#define RTSP_H

#include "../api/rtsp_api.h"

/**
 * @brief RTSP处理线程
 * 
 * @param args RTSP句柄指针
 * @return 线程返回值
 */
void *RTSPHandleThread(void *args);

/**
 * @brief 处理OPTIONS请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleOptions(int clientFd, const char *request);

/**
 * @brief 处理DESCRIBE请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @param config RTSP配置
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleDescribe(int clientFd, const char *request,
	const RTSPConfig_t *config);

/**
 * @brief 处理SETUP请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @param handle RTSP句柄指针
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleSetup(int clientFd, const char *request,
	RTSPHandle_t *handle);

/**
 * @brief 处理PLAY请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @return 成功返回0，失败返回-1
 */
int RTSPHandlePlay(int clientFd, const char *request);

/**
 * @brief 处理TEARDOWN请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleTeardown(int clientFd, const char *request);

/**
 * @brief 生成SDP描述
 * 
 * @param config RTSP配置
 * @param sdpBuf SDP缓冲区
 * @param bufSize 缓冲区大小
 * @return 成功返回SDP长度，失败返回-1
 */
int GenerateSDP(const RTSPConfig_t *config, int clientFd, char *sdpBuf, int bufSize);

#endif /* RTSP_H */

