/**
 * @file rtsp.c
 * @brief RTSP协议处理层实现
 * 
 * 实现RTSP协议请求处理和SDP生成
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>

#include "rtsp.h"
#include "../api/rtsp_api.h"
#include "../network/network.h"
#include "../core/rtp.h"
#include "../../common/log.h"

#define RTSP_BUFFER_SIZE 4096
#define RTSP_VERSION "RTSP/1.0"

/**
 * @brief 发送RTSP响应
 * 
 * @param clientFd 客户端socket文件描述符
 * @param statusCode 状态码
 * @param statusText 状态文本
 * @param headers 响应头（可为NULL）
 * @param body 响应体（可为NULL）
 * @return 成功返回0，失败返回-1
 */
static int SendRTSPResponse(int clientFd, int statusCode,
	const char *statusText, const char *headers, const char *body)
{
	char response[RTSP_BUFFER_SIZE];
	int len = 0;

	if (clientFd < 0)
	{
		LOG_ERR("Invalid client socket\n");
		return -1;
	}

	// 构建响应
	len = snprintf(response, sizeof(response),
		"%s %d %s\r\n", RTSP_VERSION, statusCode, statusText);

	if (NULL != headers)
	{
		len += snprintf(response + len, sizeof(response) - len,
			"%s", headers);
	}

	if (NULL != body)
	{
		// 插入头部与实体的分隔空行，然后写入实体
		len += snprintf(response + len, sizeof(response) - len,
			"\r\n%s", body);
	}
	else
	{
		// 没有实体时仍需在头部后补充结束空行
		len += snprintf(response + len, sizeof(response) - len, "\r\n");
	}

	LOG(">>>>>>>>>>> Send RTSP response: %s\n", response);
	// 发送响应
	int sent = send(clientFd, response, len, 0);
	if (sent != len)
	{
		LOG_ERR("send failed, sent: %d, expected: %d\n", sent, len);
		return -1;
	}

	return 0;
}

/**
 * @brief 解析CSeq头
 * 
 * @param request 请求内容
 * @return 成功返回CSeq值，失败返回-1
 */
static int ParseCSeq(const char *request)
{
	char *cseqLine = strstr(request, "CSeq:");
	if (NULL == cseqLine)
	{
		return -1;
	}

	int cseq = 0;
	if (sscanf(cseqLine, "CSeq: %d", &cseq) == 1)
	{
		return cseq;
	}

	return -1;
}

/**
 * @brief 生成SDP描述
 * 
 * @param config RTSP配置
 * @param clientFd 客户端socket文件描述符（用于获取服务器IP）
 * @param sdpBuf SDP缓冲区
 * @param bufSize 缓冲区大小
 * @return 成功返回SDP长度，失败返回-1
 */
int GenerateSDP(const RTSPConfig_t *config, int clientFd, char *sdpBuf, int bufSize)
{
	int len = 0;
	const char *formatStr = NULL;
	int payloadType = 96;

	if (NULL == config || NULL == sdpBuf || bufSize <= 0)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	// 根据格式选择参数
	switch (config->format)
	{
	case RTSP_FORMAT_H264:
		formatStr = "H264";
		payloadType = 96;
		break;
	case RTSP_FORMAT_H265:
		formatStr = "H265";
		payloadType = 96;
		break;
	default:
		LOG_ERR("Unsupported format: %d\n", config->format);
		return -1;
	}

	// 获取服务器IP地址（从socket获取本地地址）
	char serverIP[INET_ADDRSTRLEN] = "127.0.0.1"; // 默认值
	if (clientFd >= 0)
	{
		struct sockaddr_in localAddr;
		socklen_t localAddrLen = sizeof(localAddr);
		if (getsockname(clientFd, (struct sockaddr*)&localAddr, &localAddrLen) == 0)
		{
			// 如果绑定的是 0.0.0.0，使用客户端连接的本地接口地址
			// 对于绑定 0.0.0.0 的情况，getsockname 返回的可能是 0.0.0.0
			// 这种情况下，我们使用默认的 127.0.0.1（仅用于本地测试）
			// 实际部署时应该配置具体的服务器IP
			if (localAddr.sin_addr.s_addr != INADDR_ANY && 
				localAddr.sin_addr.s_addr != 0)
			{
				inet_ntop(AF_INET, &localAddr.sin_addr, serverIP, INET_ADDRSTRLEN);
			}
		}
	}

	// 生成会话ID和时间戳（用于o=行）
	unsigned int sessionId = (unsigned int)time(NULL);
	unsigned int sessionVersion = sessionId;
	
	// 生成SDP内容（完整的SDP格式）
	if (config->format == RTSP_FORMAT_H264)
	{
		// H.264 SDP格式
		// 注意：sprop-parameter-sets 应该从实际码流中提取SPS/PPS并Base64编码
		// 这里使用通用值，实际应用中应该动态提取
		len = snprintf(sdpBuf, bufSize,
			"v=0\r\n"
			"o=- %u %u IN IP4 %s\r\n"  // 会话ID、版本、网络类型、地址类型、地址
			"s=%s Stream\r\n"           // 会话名称
			"i=%s Live Stream\r\n"      // 会话信息
			"c=IN IP4 %s\r\n"           // 连接信息（媒体传输地址）
			"t=0 0\r\n"                 // 时间描述（0 0表示永久会话）
			"a=control:*\r\n"           // 控制URL（*表示使用请求URL）
			"m=video %d RTP/AVP %d\r\n" // 媒体描述（端口、协议、负载类型）
			"a=rtpmap:%d %s/90000\r\n"  // RTP映射（负载类型、编码格式、时钟频率）
			"a=control:track0\r\n",     // 媒体控制URL
			sessionId, sessionVersion, serverIP,  // o=行参数
			formatStr,                  // s=行参数
			formatStr,                  // i=行参数
			serverIP,                   // c=行参数
			config->rtpPort,            // m=行端口（使用配置的RTP端口）
			payloadType,                // m=行负载类型
			payloadType, formatStr);    // a=rtpmap参数
	}
	else if (config->format == RTSP_FORMAT_H265)
	{
		len = snprintf(sdpBuf, bufSize,
			"v=0\r\n"
			"o=- %u %u IN IP4 %s\r\n"
			"s=%s Stream\r\n"
			"i=%s Live Stream\r\n"
			"c=IN IP4 %s\r\n"
			"t=0 0\r\n"
			"a=tool:RTSP Server\r\n"
			"a=type:broadcast\r\n"
			"a=control:*\r\n"
			"m=video %d RTP/AVP %d\r\n"
			"a=rtpmap:%d %s/90000\r\n"
			"a=control:track0\r\n",
			sessionId, sessionVersion, serverIP,
			formatStr,
			formatStr,
			serverIP,
			config->rtpPort,
			payloadType,
			payloadType, formatStr);
	}
	else
	{
		LOG_ERR("Unsupported format for SDP generation\n");
		return -1;
	}

	return len;
}

/**
 * @brief 处理OPTIONS请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleOptions(int clientFd, const char *request)
{
	int cseq = 0;
	char headers[256];

	if (clientFd < 0 || NULL == request)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	cseq = ParseCSeq(request);
	if (cseq < 0)
	{
		cseq = 1;
	}

	snprintf(headers, sizeof(headers),
		"CSeq: %d\r\n"
		"Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n",
		cseq);

	return SendRTSPResponse(clientFd, 200, "OK", headers, NULL);
}

/**
 * @brief 处理DESCRIBE请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @param config RTSP配置
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleDescribe(int clientFd, const char *request,
	const RTSPConfig_t *config)
{
	int cseq = 0;
	char headers[256];
	char sdpBuf[1024];
	int sdpLen = 0;

	if (clientFd < 0 || NULL == request || NULL == config)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	cseq = ParseCSeq(request);
	if (cseq < 0)
	{
		cseq = 2;
	}

	// 生成SDP
	sdpLen = GenerateSDP(config, clientFd, sdpBuf, sizeof(sdpBuf));
	if (sdpLen < 0)
	{
		LOG_ERR("GenerateSDP failed\n");
		return -1;
	}

	snprintf(headers, sizeof(headers),
		"CSeq: %d\r\n"
		"Content-Type: application/sdp\r\n"
		"Content-Length: %d\r\n",
		cseq, sdpLen);

	return SendRTSPResponse(clientFd, 200, "OK", headers, sdpBuf);
}

/**
 * @brief 解析Transport头，获取客户端RTP端口
 * 
 * @param request 请求内容
 * @param clientRtpPort 输出客户端RTP端口
 * @param clientRtcpPort 输出客户端RTCP端口
 * @return 成功返回0，失败返回-1
 */
static int ParseTransport(const char *request, int *clientRtpPort,
	int *clientRtcpPort)
{
	char *transportLine = strstr(request, "Transport:");
	if (NULL == transportLine)
	{
		return -1;
	}

	// 查找client_port
	char *portStr = strstr(transportLine, "client_port=");
	if (NULL == portStr)
	{
		return -1;
	}

	int rtpPort = 0;
	int rtcpPort = 0;
	if (sscanf(portStr, "client_port=%d-%d", &rtpPort, &rtcpPort) == 2)
	{
		if (NULL != clientRtpPort)
		{
			*clientRtpPort = rtpPort;
		}
		if (NULL != clientRtcpPort)
		{
			*clientRtcpPort = rtcpPort;
		}
		return 0;
	}

	return -1;
}

/**
 * @brief 处理SETUP请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @param handle RTSP句柄指针
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleSetup(int clientFd, const char *request,
	RTSPHandle_t *handle)
{
	int cseq = 0;
	int clientRtpPort = 0;
	int clientRtcpPort = 0;
	int serverRtpPort = 0;
	int serverRtcpPort = 0;
	int rtpFd = -1;
	char headers[256];

	if (clientFd < 0 || NULL == request || NULL == handle)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	cseq = ParseCSeq(request);
	if (cseq < 0)
	{
		cseq = 3;
	}

	// 解析Transport头
	if (ParseTransport(request, &clientRtpPort, &clientRtcpPort) < 0)
	{
		LOG_ERR("ParseTransport failed\n");
		return SendRTSPResponse(clientFd, 400, "Bad Request",
			NULL, NULL);
	}

	// 创建RTP socket
	rtpFd = CreateRtpSocket(handle->config.rtpPort, &serverRtpPort,
		&serverRtcpPort);
	if (rtpFd < 0)
	{
		LOG_ERR("CreateRtpSocket failed\n");
		return SendRTSPResponse(clientFd, 500, "Internal Server Error",
			NULL, NULL);
	}

	// 生成唯一的Session ID（使用时间戳+随机数）
	// 如果还没有Session ID，生成一个新的
	pthread_mutex_lock(&handle->mutex);
	handle->rtpFd = rtpFd;
	handle->rtcpFd = -1; // RTCP暂未使用
	// 设置RTP客户端地址（UDP），IP地址与RTSP相同，端口使用客户端RTP端口
	handle->clientRtpAddr = handle->clientAddr;
	handle->clientRtpAddr.sin_port = htons(clientRtpPort);
	if (handle->sessionId == 0)
	{
		// 使用时间戳的低32位 + 简单的随机数生成Session ID
		handle->sessionId = (unsigned int)time(NULL) ^ 
			((unsigned int)rand() << 16) ^ 
			((unsigned int)rand());
		// 确保Session ID不为0
		if (handle->sessionId == 0)
		{
			handle->sessionId = 1;
		}
	}
	unsigned int sessionId = handle->sessionId;
	pthread_mutex_unlock(&handle->mutex);

	// 构建Transport响应头
	snprintf(headers, sizeof(headers),
		"CSeq: %d\r\n"
		"Transport: RTP/AVP;unicast;client_port=%d-%d;"
		"server_port=%d-%d\r\n"
		"Session: %u\r\n",
		cseq, clientRtpPort, clientRtcpPort,
		serverRtpPort, serverRtcpPort, sessionId);

	return SendRTSPResponse(clientFd, 200, "OK", headers, NULL);
}

/**
 * @brief 处理PLAY请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @return 成功返回0，失败返回-1
 */
int RTSPHandlePlay(int clientFd, const char *request, RTSPHandle_t *handle)
{
	int cseq = 0;
	char headers[256];
	unsigned int sessionId = 0;

	if (clientFd < 0 || NULL == request || NULL == handle)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	cseq = ParseCSeq(request);
	if (cseq < 0)
	{
		cseq = 4;
	}

	// 获取Session ID
	pthread_mutex_lock(&handle->mutex);
	sessionId = handle->sessionId;
	pthread_mutex_unlock(&handle->mutex);
	
	// 如果Session ID为0，说明还没有SETUP，返回错误
	if (sessionId == 0)
	{
		LOG_ERR("Session not established, SETUP required\n");
		return SendRTSPResponse(clientFd, 454, "Session Not Found", NULL, NULL);
	}

	snprintf(headers, sizeof(headers),
		"CSeq: %d\r\n"
		"Session: %u\r\n"
		"Range: npt=0.000-\r\n",
		cseq, sessionId);

	return SendRTSPResponse(clientFd, 200, "OK", headers, NULL);
}

/**
 * @brief 处理TEARDOWN请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @param handle RTSP句柄指针
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleTeardown(int clientFd, const char *request, RTSPHandle_t *handle)
{
	int cseq = 0;
	char headers[256];
	unsigned int sessionId = 0;

	if (clientFd < 0 || NULL == request || NULL == handle)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	cseq = ParseCSeq(request);
	if (cseq < 0)
	{
		cseq = 5;
	}

	// 获取Session ID
	pthread_mutex_lock(&handle->mutex);
	sessionId = handle->sessionId;
	// TEARDOWN后清除Session ID
	handle->sessionId = 0;
	pthread_mutex_unlock(&handle->mutex);
	
	// 如果Session ID为0，使用默认值（兼容性处理）
	if (sessionId == 0)
	{
		sessionId = 12345678;
	}

	snprintf(headers, sizeof(headers),
		"CSeq: %d\r\n"
		"Session: %u\r\n",
		cseq, sessionId);

	return SendRTSPResponse(clientFd, 200, "OK", headers, NULL);
}

/**
 * @brief 解析RTSP请求
 * 
 * @param request 请求内容
 * @param method 输出请求方法
 * @param url 输出请求URL
 * @return 成功返回0，失败返回-1
 */
static int ParseRTSPRequest(const char *request, char *method, char *url)
{
	if (NULL == request || NULL == method || NULL == url)
	{
		return -1;
	}

	if (sscanf(request, "%s %s", method, url) == 2)
	{
		return 0;
	}

	return -1;
}

/**
 * @brief RTSP处理线程
 * 
 * @param args RTSP句柄指针
 * @return 线程返回值
 */
void *RTSPHandleThread(void *args)
{
	RTSPHandle_t *handle = (RTSPHandle_t*)args;
	int clientFd = -1;
	char requestBuf[RTSP_BUFFER_SIZE];
	int recvLen = 0;
	char method[32];
	char url[256];

	if (NULL == handle)
	{
		LOG_ERR("Invalid handle\n");
		return (void*)-1;
	}

	LOG("RTSP thread started, listening on port %d\n", handle->config.rtspPort);

	// 外层循环：不断接受新的客户端连接
	while (handle->isRunning)
	{
		// 使用select检查是否有新连接，同时可以响应isRunning的变化
		fd_set readfds;
		struct timeval timeout;
		int selectRet = 0;
		
		FD_ZERO(&readfds);
		FD_SET(handle->rtspFd, &readfds);
		
		// 设置超时时间（100ms），以便定期检查isRunning
		timeout.tv_sec = 0;
		timeout.tv_usec = 100000; // 100ms
		
		selectRet = select(handle->rtspFd + 1, &readfds, NULL, NULL, &timeout);
		
		// 检查是否应该退出
		if (!handle->isRunning)
		{
			break;
		}
		
		// 如果没有新连接，继续循环
		if (selectRet <= 0 || !FD_ISSET(handle->rtspFd, &readfds))
		{
			continue;
		}

	// 接受客户端连接
	struct sockaddr_in clientAddr;
	socklen_t clientAddrLen = sizeof(clientAddr);
	clientFd = accept(handle->rtspFd, (struct sockaddr*)&clientAddr,
		&clientAddrLen);
	if (clientFd < 0)
		{
			if (handle->isRunning)
	{
		LOG_ERR("accept failed\n");
			}
			continue; // 继续等待下一个连接
	}

	handle->clientAddr = clientAddr;
	LOG("Client connected from %s:%d\n", inet_ntoa(clientAddr.sin_addr), 
		ntohs(clientAddr.sin_port));

		// 内层循环：处理当前客户端的RTSP请求
	while (handle->isRunning)
	{
		// 接收请求
		recvLen = recv(clientFd, requestBuf, sizeof(requestBuf) - 1, 0);
		if (recvLen <= 0)
		{
			if (recvLen < 0)
			{
				LOG_ERR("recv failed\n");
			}
				else
				{
					LOG("Client disconnected\n");
				}
				// 客户端断开，关闭连接并跳出内层循环，等待新连接
				close(clientFd);
				clientFd = -1;
			break;
		}

		requestBuf[recvLen] = '\0';
		LOG(">>>>>>>>>>> Received RTSP request:\n%s\n", requestBuf);

		// 解析请求
		if (ParseRTSPRequest(requestBuf, method, url) < 0)
		{
			LOG_ERR("ParseRTSPRequest failed\n");
			SendRTSPResponse(clientFd, 400, "Bad Request", NULL, NULL);
			continue;
		}

		// 处理不同的请求方法
		if (strcmp(method, "OPTIONS") == 0)
		{
			RTSPHandleOptions(clientFd, requestBuf);
		}
		else if (strcmp(method, "DESCRIBE") == 0)
		{
			RTSPHandleDescribe(clientFd, requestBuf, &handle->config);
		}
		else if (strcmp(method, "SETUP") == 0)
		{
			RTSPHandleSetup(clientFd, requestBuf, handle);
		}
		else if (strcmp(method, "PLAY") == 0)
		{
				if (RTSPHandlePlay(clientFd, requestBuf, handle) == 0)
			{
				// 检查RTP socket是否已创建
				if (handle->rtpFd >= 0)
				{
					// 启动RTP发送线程
					if (pthread_create(&handle->rtpThread, NULL,
						RTPHandleThread, handle) != 0)
					{
						LOG_ERR("pthread_create RTP thread failed\n");
					}
					else
					{
						LOG("RTP thread created\n");
					}
				}
				else
				{
					LOG_ERR("RTP socket not created yet\n");
				}
			}
		}
		else if (strcmp(method, "TEARDOWN") == 0)
		{
				RTSPHandleTeardown(clientFd, requestBuf, handle);
				// TEARDOWN后关闭当前连接，但继续监听新连接
				close(clientFd);
				clientFd = -1;
				break; // 跳出内层循环，等待新连接
		}
		else
		{
			LOG_ERR("Unsupported method: %s\n", method);
			SendRTSPResponse(clientFd, 501, "Not Implemented",
				NULL, NULL);
		}
		} // 内层循环结束

		// 如果客户端连接还存在，关闭它
		if (clientFd >= 0)
		{
			close(clientFd);
			clientFd = -1;
		}
	} // 外层循环结束

	// 确保关闭客户端连接
	if (clientFd >= 0)
	{
		close(clientFd);
	}

	LOG("RTSP thread exited\n");
	return (void*)0;
}

