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
#include <sys/prctl.h>
#include <errno.h>
#include <time.h>

#include "rtsp.h"
#include "rtp.h"
#include "rtcp.h"
#include "api/rtsp_api.h"
#include "network/network.h"
#include "common/log.h"

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

	LOG_DEBUG(">>>>>>>>>>> Send RTSP response: %s\n", response);
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
 * @brief Transport解析结果结构体
 */
typedef struct
{
	RTSPTransportMode_t mode;  // 传输模式
	int clientRtpPort;         // 客户端RTP端口 (UDP或TCP separate)
	int clientRtcpPort;        // 客户端RTCP端口
	unsigned char rtpChannel;  // RTP通道号 (interleaved模式)
	unsigned char rtcpChannel; // RTCP通道号 (interleaved模式)
} TransportParseResult_t;

/**
 * @brief 处理Interleaved数据包（RTCP数据）
 * 
 * Interleaved格式：$ + channel(1字节) + length(2字节, big-endian) + data(length字节)
 * 
 * @param clientFd 客户端socket文件描述符
 * @param handle RTSP句柄指针
 * @param firstByte 第一个字节（应该是'$'，0x24）
 * @param recvBuf 接收缓冲区（已包含firstByte）
 * @param recvLen 已接收的数据长度
 * @return 成功返回0，失败返回-1，需要继续接收返回1
 */
static int HandleInterleavedData(int clientFd, RTSPHandle_t *handle,
	unsigned char firstByte, unsigned char *recvBuf, int recvBufSize, int recvLen)
{
	unsigned char channel;
	unsigned short dataLength;
	int totalNeeded;
	int remaining;
	int received;
	RTSPTransportMode_t transportMode;
	
	if (firstByte != 0x24) // '$'
	{
		return -1; // 不是interleaved数据
	}
	
	// 检查是否在TCP interleaved模式下
	pthread_mutex_lock(&handle->mutex);
	transportMode = handle->transportMode;
	pthread_mutex_unlock(&handle->mutex);
	
	if (transportMode != RTSP_TRANSPORT_TCP_INTERLEAVED)
	{
		// 不在interleaved模式下，不应该收到interleaved数据
		LOG_WARN("Received interleaved data but not in TCP interleaved mode (mode: %d)\n",
			transportMode);
		return -1;
	}
	
	// 至少需要4字节：$ + channel + length(2字节)
	if (recvLen < 4)
	{
		// 需要继续接收
		return 1;
	}
	
	// 解析channel和length
	channel = recvBuf[1];
	dataLength = (recvBuf[2] << 8) | recvBuf[3];
	
	// 计算总共需要的字节数
	totalNeeded = 4 + dataLength;
	
	// 检查数据包是否太大（超过缓冲区大小）
	if (totalNeeded > recvBufSize)
	{
		LOG_WARN("Interleaved packet too large: %d bytes (max: %d), channel: %d\n",
			totalNeeded, recvBufSize, channel);
		// 跳过这个数据包：读取并丢弃剩余数据
		remaining = totalNeeded - recvLen;
		if (remaining > 0)
		{
			unsigned char discardBuf[4096];
			while (remaining > 0)
			{
				int toRead = (remaining > (int)sizeof(discardBuf)) ? (int)sizeof(discardBuf) : remaining;
				received = recv(clientFd, discardBuf, toRead, 0);
				if (received <= 0)
				{
					break;
				}
				remaining -= received;
			}
		}
		return -1;
	}
	
	if (recvLen < totalNeeded)
	{
		// 需要继续接收剩余数据
		remaining = totalNeeded - recvLen;
		if (remaining > recvBufSize - recvLen)
		{
			LOG_WARN("Interleaved packet would overflow buffer\n");
			return -1;
		}
		received = recv(clientFd, recvBuf + recvLen, remaining, 0);
		if (received <= 0)
		{
			if (received < 0 && (errno == ECONNRESET || errno == EPIPE || errno == ECONNABORTED))
			{
				LOG_INFO("Client disconnected during interleaved data receive\n");
			}
			else
			{
				LOG_ERR("recv interleaved data failed, errno: %d\n", errno);
			}
			return -1;
		}
		recvLen += received;
		
		// 如果还是不够，说明数据包太大，跳过
		if (recvLen < totalNeeded)
		{
			LOG_WARN("Interleaved packet incomplete, channel: %d, expected: %d, received: %d\n",
				channel, totalNeeded, recvLen);
			return -1;
		}
	}
	
	// 检查通道号
	pthread_mutex_lock(&handle->mutex);
	unsigned char rtpChannel = handle->rtpChannel;
	unsigned char rtcpChannel = handle->rtcpChannel;
	pthread_mutex_unlock(&handle->mutex);
	
	if (channel == rtcpChannel)
	{
		// RTCP数据包
		LOG_DEBUG("Received RTCP packet via interleaved, channel: %d, length: %d\n",
			channel, dataLength);
		
		// 解析RTCP包（跳过interleaved头，直接解析RTCP数据部分）
		// recvBuf的前4字节是interleaved头，RTCP数据从第5字节开始
		if (recvLen >= 4)
		{
			const unsigned char *rtcpData = recvBuf + 4;
			int rtcpDataLen = recvLen - 4;
			
			RTCPStats_t stats;
			int packetType = RTCPParsePacket(rtcpData, rtcpDataLen, &stats);
			
			if (packetType == RTCP_PT_RR)
			{
				// 成功解析RR包，更新统计信息并调用回调
				pthread_mutex_lock(&handle->mutex);
				handle->rtcpStats = stats;
				RTCPStatsCallback_t callback = handle->rtcpStatsCallback;
				void *userData = handle->rtcpStatsUserData;
				pthread_mutex_unlock(&handle->mutex);
				
				// 调用回调函数（在锁外调用，避免死锁）
				if (NULL != callback)
				{
					callback(&stats, userData);
				}
				
				LOG_DEBUG("RTCP RR parsed: SSRC=0x%08X, fraction_lost=%u, cumulative_lost=%d, "
					"ext_seq=%u, jitter=%u\n",
					stats.ssrc, stats.fractionLost, stats.cumulativePacketsLost,
					stats.extendedHighestSeq, stats.jitter);
			}
			else if (packetType >= 0)
			{
				// 其他类型的RTCP包（SR/SDES/BYE），已记录日志
				LOG_DEBUG("RTCP packet type %d received (not processed)\n", packetType);
			}
			else
			{
				// 解析失败
				LOG_DEBUG("Failed to parse RTCP packet\n");
			}
		}
	}
	else if (channel == rtpChannel)
	{
		// 这不应该发生，服务器只发送RTP，不接收
		LOG_WARN("Received RTP packet via interleaved (unexpected), channel: %d, length: %d\n",
			channel, dataLength);
	}
	else
	{
		LOG_WARN("Unknown interleaved channel: %d, expected RTP: %d, RTCP: %d\n",
			channel, rtpChannel, rtcpChannel);
	}
	
	return 0; // 成功处理
}

/**
 * @brief 解析Transport头，获取传输模式和参数
 * 
 * @param request 请求内容
 * @param result 输出解析结果
 * @return 成功返回0，失败返回-1
 */
static int ParseTransport(const char *request, TransportParseResult_t *result)
{
	char *transportLine = NULL;
	int isTcp = 0;
	int hasInterleaved = 0;

	if (NULL == request || NULL == result)
	{
		return -1;
	}

	memset(result, 0, sizeof(TransportParseResult_t));

	transportLine = strstr(request, "Transport:");
	if (NULL == transportLine)
	{
		return -1;
	}

	// 检测是否为TCP模式
	if (strstr(transportLine, "RTP/AVP/TCP") != NULL)
	{
		isTcp = 1;
	}
	else if (strstr(transportLine, "RTP/AVP") != NULL)
	{
		isTcp = 0;
	}
	else
	{
		LOG_ERR("Unsupported transport protocol\n");
		return -1;
	}

	// 检测interleaved参数 (TCP interleaved模式)
	char *interleavedStr = strstr(transportLine, "interleaved=");
	if (interleavedStr != NULL)
	{
		hasInterleaved = 1;
		int rtpChan = 0, rtcpChan = 1;
		if (sscanf(interleavedStr, "interleaved=%d-%d", &rtpChan, &rtcpChan) >= 1)
		{
			result->rtpChannel = (unsigned char)rtpChan;
			result->rtcpChannel = (unsigned char)rtcpChan;
		}
	}

	// 根据模式解析参数
	if (isTcp && hasInterleaved)
	{
		// TCP Interleaved模式
		result->mode = RTSP_TRANSPORT_TCP_INTERLEAVED;
		return 0;
	}
	else
	{
		// UDP模式，需要client_port
		char *portStr = strstr(transportLine, "client_port=");
		if (NULL == portStr)
		{
			LOG_ERR("UDP mode requires client_port\n");
			return -1;
		}

		int rtpPort = 0;
		int rtcpPort = 0;
		if (sscanf(portStr, "client_port=%d-%d", &rtpPort, &rtcpPort) == 2)
		{
			result->mode = RTSP_TRANSPORT_UDP;
			result->clientRtpPort = rtpPort;
			result->clientRtcpPort = rtcpPort;
			return 0;
		}
		return -1;
	}
}

/**
 * @brief 处理SETUP请求
 * 
 * @param clientFd 客户端socket文件描述符
 * @param request 请求内容
 * @param handle RTSP句柄指针
 * @return 成功返回0，失败返回-1
 */
int RTSPHandleSetup(int clientFd, const char *request, RTSPHandle_t *handle)
{
	int cseq = 0;
	TransportParseResult_t transportResult;
		int serverRtpPort = 0;
		int serverRtcpPort = 0;
		int rtpFd = -1;
		char headers[512];

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
	if (ParseTransport(request, &transportResult) < 0)
	{
		LOG_ERR("ParseTransport failed\n");
		return SendRTSPResponse(clientFd, 400, "Bad Request",
			NULL, NULL);
	}

	// 生成唯一的Session ID（使用时间戳+随机数）
	// 如果还没有Session ID，生成一个新的
	pthread_mutex_lock(&handle->mutex);
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
	handle->transportMode = transportResult.mode;
	pthread_mutex_unlock(&handle->mutex);

	// 根据传输模式处理
	if (transportResult.mode == RTSP_TRANSPORT_UDP)
	{
		// UDP模式
		rtpFd = CreateRtpSocket(handle->config.rtpPort, &serverRtpPort,
			&serverRtcpPort);
		if (rtpFd < 0)
		{
			LOG_ERR("CreateRtpSocket failed\n");
			return SendRTSPResponse(clientFd, 500, "Internal Server Error",
				NULL, NULL);
		}

		pthread_mutex_lock(&handle->mutex);
		handle->rtpFd = rtpFd;
		handle->rtcpFd = -1; // RTCP暂未使用
		handle->rtspClientFd = -1;
		// 设置RTP客户端地址（UDP），IP地址与RTSP相同，端口使用客户端RTP端口
		handle->clientRtpAddr = handle->clientAddr;
		handle->clientRtpAddr.sin_port = htons(transportResult.clientRtpPort);
		pthread_mutex_unlock(&handle->mutex);

		// 构建Transport响应头
		snprintf(headers, sizeof(headers),
			"CSeq: %d\r\n"
			"Transport: RTP/AVP;unicast;client_port=%d-%d;"
			"server_port=%d-%d\r\n"
			"Session: %u\r\n",
			cseq, transportResult.clientRtpPort, transportResult.clientRtcpPort,
			serverRtpPort, serverRtcpPort, sessionId);

		LOG_WARN("SETUP: UDP mode, RTP port: %d, RTCP port: %d\n",
			serverRtpPort, serverRtcpPort);
	}
	else if (transportResult.mode == RTSP_TRANSPORT_TCP_INTERLEAVED)
	{
		// TCP Interleaved模式 - 复用RTSP连接
		pthread_mutex_lock(&handle->mutex);
		handle->rtpFd = -1;
		handle->rtcpFd = -1;
		handle->rtspClientFd = clientFd; // 保存RTSP客户端socket
		handle->rtpChannel = transportResult.rtpChannel;
		handle->rtcpChannel = transportResult.rtcpChannel;
		pthread_mutex_unlock(&handle->mutex);

		// 构建Transport响应头
		snprintf(headers, sizeof(headers),
			"CSeq: %d\r\n"
			"Transport: RTP/AVP/TCP;unicast;interleaved=%d-%d\r\n"
			"Session: %u\r\n",
			cseq, transportResult.rtpChannel, transportResult.rtcpChannel,
			sessionId);

		LOG_WARN("SETUP: TCP Interleaved mode, RTP channel: %d, RTCP channel: %d\n",
			transportResult.rtpChannel, transportResult.rtcpChannel);
	}
	else
	{
		LOG_ERR("Unsupported transport mode: %d\n", transportResult.mode);
		return SendRTSPResponse(clientFd, 400, "Bad Request", NULL, NULL);
	}

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
 * @brief 停止RTP线程
 * 
 * @param handle RTSP句柄指针
 */
static void StopRtpThread(RTSPHandle_t *handle)
{
	if (handle == NULL)
	{
		return;
	}

	pthread_mutex_lock(&handle->mutex);
	
	// 如果RTP线程不存在，直接返回
	if (handle->rtpThread == 0)
	{
		pthread_mutex_unlock(&handle->mutex);
		return;
	}
	
	// 设置标志，让RTP线程退出
	handle->hasActiveRtpSession = 0;
	
	// 保存线程ID，然后清零，避免重复等待
	pthread_t rtpThread = handle->rtpThread;
	handle->rtpThread = 0;
	
	pthread_mutex_unlock(&handle->mutex);
	
	// 在锁外等待线程退出，避免死锁
	LOG_INFO("Waiting for RTP thread to exit...\n");
	pthread_join(rtpThread, NULL);
	LOG_INFO("RTP thread exited\n");
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

	// 获取Session ID并清理资源
	pthread_mutex_lock(&handle->mutex);
	sessionId = handle->sessionId;
	
	// 重置传输模式相关字段，让RTP线程知道要退出
	handle->rtspClientFd = -1;
	handle->transportMode = RTSP_TRANSPORT_INVALID;
	
	// TEARDOWN后清除Session ID
	handle->sessionId = 0;
	pthread_mutex_unlock(&handle->mutex);
	
	// 先停止RTP线程（会等待线程退出），再清理socket
	// 避免线程在使用socket时被关闭
	StopRtpThread(handle);
	
	// 清理RTP socket（如果存在，UDP或TCP Separate模式）
	pthread_mutex_lock(&handle->mutex);
	if (handle->rtpFd >= 0)
	{
		close(handle->rtpFd);
		handle->rtpFd = -1;
	}
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
	int ret = 0;
	int clientFd = -1;
	char requestBuf[RTSP_BUFFER_SIZE];
	int recvLen = 0;
	char method[32];
	char url[256];
	
	// 在线程函数最开始就设置线程名称，确保htop能正确显示
	// 必须在任何其他操作之前设置，包括检查handle
	ret = prctl(PR_SET_NAME, "RTSPHandle", 0, 0, 0);
	if (ret != 0)
	{
		LOG_WARN("Failed to set RTSPHandle thread name, errno=%d\n", errno);
	}
	else
	{
		LOG_INFO("RTSPHandle thread name set successfully (tid=%lu)\n", 
			(unsigned long)pthread_self());
	}
	
	if (NULL == handle)
	{
		LOG_ERR("Invalid handle\n");
		return (void*)-1;
	}

	LOG_INFO("RTSP thread started, listening on port %d, isRunning=%d\n", 
		handle->config.rtspPort, handle->isRunning);

	// 外层循环：不断接受新的客户端连接
	while (handle->isRunning)
	{
		// 使用select检查是否有新连接，同时可以响应isRunning的变化
		fd_set readfds;
		struct timeval timeout;
		int selectRet = 0;
		
		FD_ZERO(&readfds);
		FD_SET(handle->rtspListenFd, &readfds);
		
		// 设置超时时间（100ms），以便定期检查isRunning
		timeout.tv_sec = 0;
		timeout.tv_usec = 100000; // 100ms
		
		selectRet = select(handle->rtspListenFd + 1, &readfds, NULL, NULL, &timeout);
		
		// 检查是否应该退出
		if (!handle->isRunning)
		{
			break;
		}
		
		// 如果没有新连接，继续循环
		if (selectRet <= 0 || !FD_ISSET(handle->rtspListenFd, &readfds))
		{
			continue;
		}

		// 接受客户端连接
		struct sockaddr_in clientAddr;
		socklen_t clientAddrLen = sizeof(clientAddr);
		clientFd = accept(handle->rtspListenFd, (struct sockaddr*)&clientAddr, &clientAddrLen);
		if (clientFd < 0)
		{
			if (handle->isRunning)
			{
				LOG_ERR("accept failed\n");
			}
			continue; // 继续等待下一个连接
		}

		handle->clientAddr = clientAddr;
		LOG_INFO("Client connected from %s:%d\n", inet_ntoa(clientAddr.sin_addr), 
		ntohs(clientAddr.sin_port));

		// 内层循环：处理当前客户端的RTSP请求
		while (handle->isRunning)
		{
			// 接收请求（先接收至少1字节，检查是否是interleaved数据）
			recvLen = recv(clientFd, requestBuf, sizeof(requestBuf) - 1, 0);
			if (recvLen <= 0)
			{
				if (recvLen < 0)
				{
					// 检查是否是正常的连接关闭错误
					// ECONNRESET: 连接被对端重置（客户端关闭）
					// EPIPE: 管道破裂（连接已关闭）
					// 这些在客户端关闭时是正常情况
					if (errno == ECONNRESET || errno == EPIPE || errno == ECONNABORTED)
					{
						LOG_INFO("Client disconnected (connection reset)\n");
					}
					else
					{
						LOG_ERR("recv failed, errno: %d (%s)\n", errno, strerror(errno));
					}
				}
				else
				{
					LOG_INFO("Client disconnected (graceful close)\n");
				}
				// 先设置标志让RTP线程退出，等待线程退出后再清理socket
				// 避免线程在使用socket时被关闭
				pthread_mutex_lock(&handle->mutex);
				handle->rtspClientFd = -1;
				handle->transportMode = RTSP_TRANSPORT_INVALID;
				pthread_mutex_unlock(&handle->mutex);
				
				// 停止RTP线程（会等待线程退出）
				StopRtpThread(handle);
				
				// 清理RTP socket（如果存在，UDP或TCP Separate模式）
				pthread_mutex_lock(&handle->mutex);
				if (handle->rtpFd >= 0)
				{
					close(handle->rtpFd);
					handle->rtpFd = -1;
				}
				pthread_mutex_unlock(&handle->mutex);
				
				// 客户端断开，关闭连接并跳出内层循环，等待新连接
				close(clientFd);
				clientFd = -1;
				break;
			}

			// 检查是否是interleaved数据包（以'$'开头）
			if (recvLen > 0 && requestBuf[0] == 0x24) // '$'
			{
				// 这是interleaved数据包（通常是RTCP数据）
				int interleavedRet = HandleInterleavedData(clientFd, handle,
					(unsigned char)requestBuf[0], (unsigned char *)requestBuf, sizeof(requestBuf), recvLen);
				if (interleavedRet < 0)
				{
					// 处理失败，可能是连接关闭或数据错误
					// 继续循环，等待下一个数据包
				}
				// 成功处理或需要继续接收，继续循环
				continue;
			}

			// 不是interleaved数据，按RTSP请求处理
			requestBuf[recvLen] = '\0';
			LOG_DEBUG(">>>>>>>>>>> Received RTSP request:\n%s\n", requestBuf);

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
					// 检查传输模式和相关socket是否已创建
					pthread_mutex_lock(&handle->mutex);
					RTSPTransportMode_t transportMode = handle->transportMode;
					int rtpFd = handle->rtpFd;
					int rtspClientFd = handle->rtspClientFd;
					pthread_mutex_unlock(&handle->mutex);
					
					// 根据传输模式检查对应的socket
					int socketReady = 0;
					if (transportMode == RTSP_TRANSPORT_UDP)
					{
						socketReady = (rtpFd >= 0);
					}
					else if (transportMode == RTSP_TRANSPORT_TCP_INTERLEAVED)
					{
						socketReady = (rtspClientFd >= 0);
					}
					else
					{
						LOG_ERR("Invalid transport mode: %d\n", transportMode);
						socketReady = 0;
					}
					
					if (socketReady)
					{
						// 如果RTP线程已存在，先停止它
						StopRtpThread(handle);
						
						// 设置RTP会话标志
						pthread_mutex_lock(&handle->mutex);
						handle->hasActiveRtpSession = 1;
						pthread_mutex_unlock(&handle->mutex);
						
						// 启动RTP发送线程
						if (pthread_create(&handle->rtpThread, NULL,
							RTPHandleThread, handle) != 0)
						{
							LOG_ERR("pthread_create RTP thread failed\n");
							// 创建失败，清除标志
							pthread_mutex_lock(&handle->mutex);
							handle->hasActiveRtpSession = 0;
							pthread_mutex_unlock(&handle->mutex);
						}
						else
						{
							LOG_INFO("RTP thread created, transport mode: %d\n", transportMode);
						}
					}
					else
					{
						LOG_ERR("RTP socket not created yet, transport mode: %d\n", transportMode);
					}
				}
			}
			else if (strcmp(method, "TEARDOWN") == 0)
			{
				RTSPHandleTeardown(clientFd, requestBuf, handle);
				// 停止RTP线程
				StopRtpThread(handle);
				// TEARDOWN后关闭当前连接，但继续监听新连接
				close(clientFd);
				clientFd = -1;
				break; // 跳出内层循环，等待新连接
			}
			else
			{
				LOG_ERR("Unsupported method: %s\n", method);
				SendRTSPResponse(clientFd, 501, "Not Implemented", NULL, NULL);
			}
		} // 内层循环结束

		// 如果客户端连接还存在，关闭它
		if (clientFd >= 0)
		{
			close(clientFd);
			clientFd = -1;
		}
		
		// 确保清理所有资源，准备接受下一个连接
		LOG_INFO("Client session ended, ready for next connection\n");
	} // 外层循环结束

	// 确保关闭客户端连接
	if (clientFd >= 0)
	{
		close(clientFd);
	}

	LOG_INFO("RTSP thread exited\n");
	return (void*)0;
}

