/**
 * @file network.c
 * @brief 网络层实现
 * 
 * 实现RTSP和RTP socket的创建和绑定
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define SOCKET int
#define INVALID_SOCKET (-1)

#include "network.h"
#include "common/log.h"

/**
 * @brief 创建RTSP TCP监听socket
 * 
 * @param port RTSP监听端口
 * @return 成功返回socket文件描述符，失败返回-1
 */
int CreateRtspSocket(int port)
{
	int ret = 0;
	int sock = -1;
	struct sockaddr_in serverAddr;

	if (port <= 0 || port > 65535)
	{
		LOG_ERR("Invalid port: %d\n", port);
		return -1;
	}

	// 创建TCP socket
	sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if ((SOCKET)sock == INVALID_SOCKET)
	{
		LOG_ERR("Socket creation failed\n");
		return -1;
	}

	// 设置套接字选项，允许地址重用
	int reuse = 1;
	ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
		(char*)&reuse, sizeof(reuse));
	if (ret != 0)
	{
		LOG_ERR("setsockopt SO_REUSEADDR failed\n");
		close(sock);
		return -1;
	}

	// 绑定本地地址和端口
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_addr.s_addr = INADDR_ANY;
	serverAddr.sin_port = htons(port);

	ret = bind(sock, (struct sockaddr*)&serverAddr,
		sizeof(serverAddr));
	if (ret < 0)
	{
		LOG_ERR("bind failed, port: %d\n", port);
		close(sock);
		return -1;
	}

	// 开始监听
	ret = listen(sock, 5);
	if (ret < 0)
	{
		LOG_ERR("listen failed\n");
		close(sock);
		return -1;
	}

	LOG_INFO("RTSP socket created, listening on port %d\n", port);
	return sock;
}

/**
 * @brief 创建UDP socket并绑定到指定端口
 * 
 * @param port 要绑定的端口号
 * @return 成功返回socket文件描述符，失败返回-1
 */
static int CreateUdpSocket(int port)
{
	int ret = 0;
	int sock = -1;
	struct sockaddr_in localAddr;

	if (port <= 0 || port > 65535)
	{
		LOG_ERR("Invalid port: %d\n", port);
		return -1;
	}

	// 创建UDP socket
	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if ((SOCKET)sock == INVALID_SOCKET)
	{
		LOG_ERR("UDP socket creation failed\n");
		return -1;
	}

	// 设置套接字选项，允许地址重用
	int reuse = 1;
	ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
		(char*)&reuse, sizeof(reuse));
	if (ret != 0)
	{
		LOG_ERR("setsockopt SO_REUSEADDR failed\n");
		close(sock);
		return -1;
	}

	// 绑定本地端口
	memset(&localAddr, 0, sizeof(localAddr));
	localAddr.sin_family = AF_INET;
	localAddr.sin_addr.s_addr = INADDR_ANY;
	localAddr.sin_port = htons(port);

	ret = bind(sock, (struct sockaddr*)&localAddr,
		sizeof(localAddr));
	if (ret < 0)
	{
		LOG_ERR("UDP bind failed, port: %d\n", port);
		close(sock);
		return -1;
	}

	return sock;
}

/**
 * @brief 创建RTP UDP socket
 * 
 * @param port 指定的RTP端口
 * @param rtpPortOut 输出实际分配的RTP端口
 * @param rtcpPortOut 输出实际分配的RTCP端口
 * @return 成功返回RTP socket文件描述符，失败返回-1
 */
int CreateRtpSocket(int port, int *rtpPortOut, int *rtcpPortOut)
{
	int rtpSock = -1;
	int rtcpSock = -1;
	int rtpPort = port;
	int rtcpPort = port + 1;

	if (NULL == rtpPortOut || NULL == rtcpPortOut)
	{
		LOG_ERR("Output parameters are NULL\n");
		return -1;
	}

	// 尝试使用指定端口
	rtpSock = CreateUdpSocket(rtpPort);
	if (rtpSock < 0)
	{
		// 如果指定端口不可用，自动查找可用端口
		LOG_INFO("Port %d not available, searching for available port\n",
			rtpPort);
		for (rtpPort = 5000; rtpPort < 65535; rtpPort += 2)
		{
			rtpSock = CreateUdpSocket(rtpPort);
			if (rtpSock >= 0)
			{
				rtcpPort = rtpPort + 1;
				break;
			}
		}
		if (rtpSock < 0)
		{
			LOG_ERR("Failed to find available RTP port\n");
			return -1;
		}
	}
	else
	{
		// RTP端口可用，尝试创建RTCP端口
		rtcpSock = CreateUdpSocket(rtcpPort);
		if (rtcpSock < 0)
		{
			LOG_ERR("RTCP port %d not available\n", rtcpPort);
			close(rtpSock);
			return -1;
		}
		close(rtcpSock);
	}

	*rtpPortOut = rtpPort;
	*rtcpPortOut = rtcpPort;

	LOG_INFO("RTP socket created, RTP port: %d, RTCP port: %d\n",
		rtpPort, rtcpPort);

	return rtpSock;
}

