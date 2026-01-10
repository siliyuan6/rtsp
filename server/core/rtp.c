/**
 * @file rtp.c
 * @brief RTP数据发送层实现
 * 
 * 实现H.264数据的RTP封装和发送
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/prctl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#include "rtp.h"
#include "api/rtsp_api.h"
#include "common/log.h"

#define FU_A_START 0x80
#define FU_A_MID 0x00
#define FU_A_END 0x40

// 静态RTP包数组大小，支持大帧（4K I帧约需300个包，设置为512以留有余量）
#define MAX_RTP_PACKETS_STATIC 512

/**
 * @brief H.264 NALU类型
 */
#define NALU_TYPE_SEI 6
#define NALU_TYPE_SPS 7
#define NALU_TYPE_PPS 8
#define NALU_TYPE_IDR 5
#define NALU_TYPE_NON_IDR 1

/**
 * @brief 检测H.264起始码长度
 * 
 * @param data 数据缓冲区
 * @param len 数据长度
 * @return 起始码长度（3或4），如果没有起始码返回0
 */
static int GetH264StartCodeLen(const unsigned char *data, int len)
{
	if (len >= 4 && data[0] == 0 && data[1] == 0 &&
		data[2] == 0 && data[3] == 1)
	{
		return 4; // 4字节起始码 0x00000001
	}
	else if (len >= 3 && data[0] == 0 && data[1] == 0 &&
		data[2] == 1)
	{
		return 3; // 3字节起始码 0x000001
	}
	return 0; // 没有起始码
}

/**
 * @brief 创建RTP头
 * 
 * @param header RTP头缓冲区（至少12字节）
 * @param payloadType 负载类型（H.264为96）
 * @param seq 序列号
 * @param timestamp 时间戳
 * @param ssrc SSRC标识
 */
static void CreateRtpHeader(unsigned char *header, int payloadType,
	unsigned short seq, unsigned int timestamp, unsigned int ssrc)
{
	// RTP头固定12字节
	header[0] = (RTP_VERSION << 6) | 0x00; // V=2, P=0, X=0, CC=0
	header[1] = (payloadType & 0x7F); // M=0, PT
	header[2] = (seq >> 8) & 0xFF;
	header[3] = seq & 0xFF;
	header[4] = (timestamp >> 24) & 0xFF;
	header[5] = (timestamp >> 16) & 0xFF;
	header[6] = (timestamp >> 8) & 0xFF;
	header[7] = timestamp & 0xFF;
	header[8] = (ssrc >> 24) & 0xFF;
	header[9] = (ssrc >> 16) & 0xFF;
	header[10] = (ssrc >> 8) & 0xFF;
	header[11] = ssrc & 0xFF;
}

/**
 * @brief H.264 RTP封装（单包）
 * 
 * @param nalu NALU数据（包含起始码）
 * @param len NALU长度
 * @param rtpPacket 输出的RTP包
 * @param ssrc SSRC标识
 * @return 成功返回0，失败返回-1
 */
static int RTPEncapsulateH264Single(const unsigned char *nalu, int len,
	RtpPacket_t *rtpPacket, unsigned int ssrc)
{
	int payloadLen = 0;
	unsigned char *payload = NULL;

	if (NULL == nalu || len <= 0 || NULL == rtpPacket)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	// 跳过起始码（0x00000001或0x000001）
	int naluStart = GetH264StartCodeLen(nalu, len);

	// 分配RTP包内存
	payloadLen = len - naluStart;
	rtpPacket->data = (unsigned char*)malloc(RTP_HEADER_SIZE + payloadLen);
	if (NULL == rtpPacket->data)
	{
		LOG_ERR("malloc failed\n");
		return -1;
	}

	// 创建RTP头（序列号和时间戳稍后填充）
	CreateRtpHeader(rtpPacket->data, 96, 0, 0, ssrc);

	// 检查NALU类型，如果是视频帧（IDR/NON-IDR），设置Marker位
	// Marker位表示访问单元（Access Unit）的结束，VLC等播放器依赖此识别帧边界
	int naluType = nalu[naluStart] & 0x1F;
	if (naluType == NALU_TYPE_IDR || naluType == NALU_TYPE_NON_IDR)
	{
		rtpPacket->data[1] |= 0x80; // 设置Marker位（访问单元结束）
	}

	// 填充负载：F+NRI+Type
	payload = rtpPacket->data + RTP_HEADER_SIZE;
	payload[0] = nalu[naluStart] & 0x60; // F+NRI
	payload[0] |= (nalu[naluStart] & 0x1F); // Type

	// 复制NALU数据
	memcpy(payload + 1, nalu + naluStart + 1, len - naluStart - 1);

	rtpPacket->len = RTP_HEADER_SIZE + payloadLen;
	return 0;
}

/**
 * @brief H.264 RTP封装（FU-A分片）
 * 
 * @param nalu NALU数据（包含起始码）
 * @param len NALU长度
 * @param rtpPackets 输出的RTP包数组
 * @param maxPackets 最大包数量
 * @return 成功返回包数量，失败返回-1
 */
static int RTPEncapsulateH264Fragmented(const unsigned char *nalu,
	int len, RtpPacket_t *rtpPackets, int maxPackets, unsigned int ssrc)
{
	int naluStart = 0;
	int naluType = 0;
	int payloadSize = 0;
	int fragmentCount = 0;
	int offset = 0;
	unsigned char *payload = NULL;

	if (NULL == nalu || len <= 0 || NULL == rtpPackets ||
		maxPackets <= 0)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	// 跳过起始码
	naluStart = GetH264StartCodeLen(nalu, len);

	naluType = nalu[naluStart] & 0x1F;
	int naluDataLen = len - naluStart - 1; // 减去NALU头
	int maxFragmentSize = RTP_MAX_PAYLOAD_SIZE - RTP_HEADER_SIZE - 2;

	// 计算需要的分片数量
	fragmentCount = (naluDataLen + maxFragmentSize - 1) /
		maxFragmentSize;
	if (fragmentCount > maxPackets)
	{
		LOG_ERR("Frame too large: need %d packets, max %d (frame size: %d bytes)\n", 
			fragmentCount, maxPackets, len);
		return -1;
	}

	offset = naluStart + 1; // 跳过NALU头
	for (int i = 0; i < fragmentCount; i++)
	{
		int fragmentLen = (i == fragmentCount - 1) ?
			(naluDataLen - i * maxFragmentSize) :
			maxFragmentSize;

		// 分配RTP包内存
		payloadSize = fragmentLen + 2; // +2 for FU indicator + FU header
		rtpPackets[i].data = (unsigned char*)malloc(
			RTP_HEADER_SIZE + payloadSize);
		if (NULL == rtpPackets[i].data)
		{
			LOG_ERR("malloc failed for fragment %d\n", i);
			// 释放已分配的内存
			for (int j = 0; j < i; j++)
			{
				if (NULL != rtpPackets[j].data)
				{
					free(rtpPackets[j].data);
					rtpPackets[j].data = NULL;
				}
			}
			return -1;
		}

		// 创建RTP头
		CreateRtpHeader(rtpPackets[i].data, 96, 0, 0, ssrc);

		// 设置M标记（Marker位）
		// 对于视频帧（IDR/NON-IDR），在最后一个分片设置Marker位（访问单元结束）
		// VLC等播放器依赖Marker位来识别帧边界
		if (i == fragmentCount - 1 && 
			(naluType == NALU_TYPE_IDR || naluType == NALU_TYPE_NON_IDR))
		{
			rtpPackets[i].data[1] |= 0x80; // 设置Marker位
		}

		// 填充FU indicator和FU header
		payload = rtpPackets[i].data + RTP_HEADER_SIZE;
		payload[0] = (nalu[naluStart] & 0x60) | 28; // FU-A type
		if (i == 0)
		{
			payload[1] = FU_A_START | naluType;
		}
		else if (i == fragmentCount - 1)
		{
			payload[1] = FU_A_END | naluType;
		}
		else
		{
			payload[1] = FU_A_MID | naluType;
		}

		// 复制NALU数据
		memcpy(payload + 2, nalu + offset, fragmentLen);
		offset += fragmentLen;

		rtpPackets[i].len = RTP_HEADER_SIZE + payloadSize;
	}

	return fragmentCount;
}

/**
 * @brief RTP封装数据
 * 
 * @param format 数据格式
 * @param data 原始数据
 * @param len 数据长度
 * @param rtpPackets 输出的RTP包数组
 * @param maxPackets 最大包数量
 * @return 成功返回包数量，失败返回-1
 */
int RTPEncapsulate(RTSPStreamFormat_t format, 
	const unsigned char *data, int len, 
	RtpPacket_t *rtpPackets, int maxPackets, unsigned int ssrc)
{
	if (NULL == data || len <= 0 || NULL == rtpPackets ||
		maxPackets <= 0)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	switch (format)
	{
		case RTSP_FORMAT_H264:
		{
			// 计算NALU大小（不包括起始码）
			int startCodeLen = GetH264StartCodeLen(data, len);
			int naluDataSize = len - startCodeLen;

			// 判断是否需要分片
			if (naluDataSize <= RTP_MAX_PAYLOAD_SIZE - RTP_HEADER_SIZE - 1)
			{
				// 单包
				if (RTPEncapsulateH264Single(data, len, &rtpPackets[0], ssrc) < 0)
				{
					return -1;
				}
				return 1;
			}
			else
			{
				// 分片
				return RTPEncapsulateH264Fragmented(data, len,
					rtpPackets, maxPackets, ssrc);
			}
		}
		case RTSP_FORMAT_H265:
		{
			// H.265暂未实现
			LOG_ERR("H.265 format not implemented yet\n");
			return -1;
		}
		default:
			LOG_ERR("Unsupported format: %d\n", format);
			return -1;
	}
}

/**
 * @brief 发送单个RTP包
 * 
 * @param rtpFd RTP socket文件描述符
 * @param clientAddr 客户端地址
 * @param nalu NALU数据
 * @param len NALU长度
 * @param seq 序列号（输入输出参数）
 * @param timestamp 时间戳（输入输出参数）
 * @return 成功返回0，失败返回-1
 */
int RTPSendSingle(int rtpFd, struct sockaddr_in *clientAddr,
	const unsigned char *nalu, int len, unsigned short *seq,
	unsigned int *timestamp)
{
	RtpPacket_t rtpPacket;
	int sent = 0;

	if (rtpFd < 0 || NULL == clientAddr || NULL == nalu ||
		len <= 0 || NULL == seq || NULL == timestamp)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	memset(&rtpPacket, 0, sizeof(rtpPacket));

	// 封装RTP包（使用默认SSRC，这些函数可能不再使用）
	if (RTPEncapsulateH264Single(nalu, len, &rtpPacket, 0x12345678) < 0)
	{
		return -1;
	}

	// 更新序列号和时间戳
	rtpPacket.data[2] = (*seq >> 8) & 0xFF;
	rtpPacket.data[3] = *seq & 0xFF;
	(*seq)++;

	rtpPacket.data[4] = (*timestamp >> 24) & 0xFF;
	rtpPacket.data[5] = (*timestamp >> 16) & 0xFF;
	rtpPacket.data[6] = (*timestamp >> 8) & 0xFF;
	rtpPacket.data[7] = *timestamp & 0xFF;

	// 发送RTP包
	sent = sendto(rtpFd, rtpPacket.data, rtpPacket.len, 0,
		(struct sockaddr*)clientAddr, sizeof(*clientAddr));
	if (sent != rtpPacket.len)
	{
		LOG_ERR("sendto failed, sent: %d, expected: %d\n",
			sent, rtpPacket.len);
		free(rtpPacket.data);
		rtpPacket.data = NULL;
		return -1;
	}

	free(rtpPacket.data);
	rtpPacket.data = NULL;
	return 0;
}

/**
 * @brief 发送分片RTP包
 * 
 * @param rtpFd RTP socket文件描述符
 * @param clientAddr 客户端地址
 * @param nalu NALU数据
 * @param len NALU长度
 * @param seq 序列号（输入输出参数）
 * @param timestamp 时间戳（输入输出参数）
 * @return 成功返回0，失败返回-1
 */
int RTPSendFragmented(int rtpFd, struct sockaddr_in *clientAddr,
	const unsigned char *nalu, int len, unsigned short *seq,
	unsigned int *timestamp)
{
	RtpPacket_t rtpPackets[MAX_RTP_PACKETS_STATIC];
	int fragmentCount = 0;
	int sent = 0;

	if (rtpFd < 0 || NULL == clientAddr || NULL == nalu ||
		len <= 0 || NULL == seq || NULL == timestamp)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	memset(rtpPackets, 0, sizeof(rtpPackets));

	// 封装RTP包（使用默认SSRC，这些函数可能不再使用）
	fragmentCount = RTPEncapsulateH264Fragmented(nalu, len,
		rtpPackets, MAX_RTP_PACKETS_STATIC, 0x12345678);
	if (fragmentCount <= 0)
	{
		return -1;
	}

	// 发送所有分片
	for (int i = 0; i < fragmentCount; i++)
	{
		// 更新序列号和时间戳
		rtpPackets[i].data[2] = (*seq >> 8) & 0xFF;
		rtpPackets[i].data[3] = *seq & 0xFF;
		(*seq)++;

		rtpPackets[i].data[4] = (*timestamp >> 24) & 0xFF;
		rtpPackets[i].data[5] = (*timestamp >> 16) & 0xFF;
		rtpPackets[i].data[6] = (*timestamp >> 8) & 0xFF;
		rtpPackets[i].data[7] = *timestamp & 0xFF;

		// 发送分片
		sent = sendto(rtpFd, rtpPackets[i].data, rtpPackets[i].len,
			0, (struct sockaddr*)clientAddr, sizeof(*clientAddr));
		if (sent != rtpPackets[i].len)
		{
			LOG_ERR("sendto failed for fragment %d\n", i);
			// 释放所有包的内存
			for (int j = 0; j < fragmentCount; j++)
			{
				if (NULL != rtpPackets[j].data)
				{
					free(rtpPackets[j].data);
					rtpPackets[j].data = NULL;
				}
			}
			return -1;
		}
	}

	// 释放所有包的内存
	for (int i = 0; i < fragmentCount; i++)
	{
		if (NULL != rtpPackets[i].data)
		{
			free(rtpPackets[i].data);
			rtpPackets[i].data = NULL;
		}
	}

	return 0;
}

/**
 * @brief 发送RTP数据
 * 
 * @param rtpFd RTP socket文件描述符
 * @param clientAddr 客户端地址
 * @param format 数据格式
 * @param data 原始数据
 * @param len 数据长度
 * @param seq 序列号（输入输出参数）
 * @param timestamp 时间戳（输入输出参数）
 * @return 成功返回0，失败返回-1
 */
int RTPSendDataUdp(int rtpFd, struct sockaddr_in *clientAddr,
	RTSPStreamFormat_t format, const unsigned char *data, int len,
	unsigned short *seq, unsigned int *timestamp, unsigned int ssrc)
{
	RtpPacket_t rtpPackets[MAX_RTP_PACKETS_STATIC];
	int packetCount = 0;
	int sent = 0;
	int isVideoFrame = 0; // 是否是视频帧（IDR/NON-IDR）

	if (rtpFd < 0 || NULL == clientAddr || NULL == data ||
		len <= 0 || NULL == seq || NULL == timestamp)
	{
		LOG_ERR("Invalid parameters, rtpFd: %d, clientAddr: %p, data: %p, len: %d, seq: %p, timestamp: %p\n",
			rtpFd, clientAddr, data, len, seq, timestamp);
		return -1;
	}

	memset(rtpPackets, 0, sizeof(rtpPackets));

	// 检查是否是视频帧（用于设置Marker位）
	if (format == RTSP_FORMAT_H264)
	{
		int startCodeLen = GetH264StartCodeLen(data, len);
		if (startCodeLen > 0 && startCodeLen < len)
		{
			int naluType = data[startCodeLen] & 0x1F;
			isVideoFrame = (naluType == NALU_TYPE_IDR || naluType == NALU_TYPE_NON_IDR);
		}
	}

	// 封装RTP包
	packetCount = RTPEncapsulate(format, data, len, rtpPackets, 
		MAX_RTP_PACKETS_STATIC, ssrc);
	if (packetCount <= 0)
	{
		LOG_ERR("RTPEncapsulate failed, format: %d, data len: %d\n", format, len);
		return -1;
	}
	
	// 发送所有RTP包
	for (int i = 0; i < packetCount; i++)
	{
		// 更新序列号和时间戳
		rtpPackets[i].data[2] = (*seq >> 8) & 0xFF;
		rtpPackets[i].data[3] = *seq & 0xFF;
		(*seq)++;

		rtpPackets[i].data[4] = (*timestamp >> 24) & 0xFF;
		rtpPackets[i].data[5] = (*timestamp >> 16) & 0xFF;
		rtpPackets[i].data[6] = (*timestamp >> 8) & 0xFF;
		rtpPackets[i].data[7] = *timestamp & 0xFF;

		// 对于视频帧，在最后一个包设置Marker位（访问单元结束标记）
		// VLC等播放器依赖Marker位来识别帧边界
		// 注意：分片模式下，最后一个分片已经在封装时设置了Marker位
		// 单包模式下，也在封装时设置了Marker位
		// 这里再次确保设置，以防万一
		if (isVideoFrame && i == packetCount - 1)
		{
			rtpPackets[i].data[1] |= 0x80; // 设置Marker位
		}

		// 发送RTP包
		sent = sendto(rtpFd, rtpPackets[i].data, rtpPackets[i].len,
			0, (struct sockaddr*)clientAddr, sizeof(*clientAddr));
		if (sent != rtpPackets[i].len)
		{
			LOG_ERR("sendto failed for packet %d\n", i);
			// 释放所有包的内存
			for (int j = 0; j < packetCount; j++)
			{
				if (NULL != rtpPackets[j].data)
				{
					free(rtpPackets[j].data);
					rtpPackets[j].data = NULL;
				}
			}
			return -1;
		}
		
		// 添加包间延时，避免UDP丢包和网络拥塞
		// 策略：根据包数量动态调整延时
		// - 小帧（<=10包）：每个包后延时50微秒
		// - 中帧（11-50包）：每个包后延时100微秒
		// - 大帧（>50包）：每个包后延时150微秒，每10个包额外延时50微秒
		if (i < packetCount - 1) // 最后一个包不需要延时
		{
			if (packetCount <= 10)
			{
				usleep(50); // 50微秒
			}
			else if (packetCount <= 50)
			{
				usleep(100); // 100微秒
			}
			else
			{
				usleep(150); // 150微秒
				// 大帧每10个包额外延时
				if ((i + 1) % 10 == 0)
				{
					usleep(50); // 额外50微秒
				}
			}
		}
	}

	// 释放所有包的内存
	for (int i = 0; i < packetCount; i++)
	{
		if (NULL != rtpPackets[i].data)
		{
			free(rtpPackets[i].data);
			rtpPackets[i].data = NULL;
		}
	}

	return 0;
}

/**
 * @brief 通过TCP Interleaved方式发送RTP数据
 * 
 * @param rtspClientFd RTSP客户端socket文件描述符
 * @param rtpChannel RTP通道号
 * @param format 数据格式
 * @param data 原始数据
 * @param len 数据长度
 * @param seq 序列号（输入输出参数）
 * @param timestamp 时间戳（输入输出参数）
 * @return 成功返回0，失败返回-1
 */
int RTPSendDataTCPInterleaved(int rtspClientFd, unsigned char rtpChannel,
	RTSPStreamFormat_t format, const unsigned char *data, int len,
	unsigned short *seq, unsigned int *timestamp, unsigned int ssrc)
{
	RtpPacket_t rtpPackets[MAX_RTP_PACKETS_STATIC];
	int packetCount = 0;
	int sent = 0;
	unsigned char interleavedHeader[4];

	if (rtspClientFd < 0 || NULL == data || len <= 0 ||
		NULL == seq || NULL == timestamp)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	memset(rtpPackets, 0, sizeof(rtpPackets));

	// 封装RTP包
	packetCount = RTPEncapsulate(format, data, len, 
		rtpPackets, MAX_RTP_PACKETS_STATIC, ssrc);
	if (packetCount <= 0)
	{
		LOG_ERR("RTPEncapsulate failed, format: %d, data len: %d\n", format, len);
		return -1;
	}
	
	// 发送所有RTP包
	for (int i = 0; i < packetCount; i++)
	{
		// 更新序列号和时间戳
		rtpPackets[i].data[2] = (*seq >> 8) & 0xFF;
		rtpPackets[i].data[3] = *seq & 0xFF;
		(*seq)++;

		rtpPackets[i].data[4] = (*timestamp >> 24) & 0xFF;
		rtpPackets[i].data[5] = (*timestamp >> 16) & 0xFF;
		rtpPackets[i].data[6] = (*timestamp >> 8) & 0xFF;
		rtpPackets[i].data[7] = *timestamp & 0xFF;

		// 构建Interleaved头: $ + channel(1) + length(2, big-endian)
		interleavedHeader[0] = 0x24; // '$'
		interleavedHeader[1] = rtpChannel;
		interleavedHeader[2] = (rtpPackets[i].len >> 8) & 0xFF;
		interleavedHeader[3] = rtpPackets[i].len & 0xFF;

		// 先发送Interleaved头（使用MSG_NOSIGNAL避免SIGPIPE）
		sent = send(rtspClientFd, interleavedHeader, 4, MSG_NOSIGNAL);
		if (sent != 4)
		{
			// 检查是否是客户端关闭连接导致的错误
			if (sent < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ECONNABORTED))
			{
				LOG_INFO("Client disconnected during RTP send (interleaved header)\n");
			}
			else
			{
				LOG_ERR("send interleaved header failed for packet %d, sent: %d, errno: %d\n", 
					i, sent, errno);
			}
			// 释放所有包的内存
			for (int j = 0; j < packetCount; j++)
			{
				if (NULL != rtpPackets[j].data)
				{
					free(rtpPackets[j].data);
					rtpPackets[j].data = NULL;
				}
			}
			return -1;
		}

		// 发送RTP包数据（使用MSG_NOSIGNAL避免SIGPIPE）
		sent = send(rtspClientFd, rtpPackets[i].data, rtpPackets[i].len, MSG_NOSIGNAL);
		if (sent != rtpPackets[i].len)
		{
			// 检查是否是客户端关闭连接导致的错误
			if (sent < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ECONNABORTED))
			{
				LOG_INFO("Client disconnected during RTP send (RTP data)\n");
			}
			else
			{
				LOG_ERR("send RTP data failed for packet %d, sent: %d/%d, errno: %d\n", 
					i, sent, rtpPackets[i].len, errno);
			}
			// 释放所有包的内存
			for (int j = 0; j < packetCount; j++)
			{
				if (NULL != rtpPackets[j].data)
				{
					free(rtpPackets[j].data);
					rtpPackets[j].data = NULL;
				}
			}
			return -1;
		}
	}

	// 释放所有包的内存
	for (int i = 0; i < packetCount; i++)
	{
		if (NULL != rtpPackets[i].data)
		{
			free(rtpPackets[i].data);
			rtpPackets[i].data = NULL;
		}
	}

	return 0;
}

/**
 * @brief 通过TCP独立连接发送RTP数据
 * 
 * @param rtpFd TCP RTP socket文件描述符
 * @param format 数据格式
 * @param data 原始数据
 * @param len 数据长度
 * @param seq 序列号（输入输出参数）
 * @param timestamp 时间戳（输入输出参数）
 * @return 成功返回0，失败返回-1
 */
int RTPSendDataTCPSeparate(int rtpFd, RTSPStreamFormat_t format,
	const unsigned char *data, int len, unsigned short *seq,
	unsigned int *timestamp, unsigned int ssrc)
{
	RtpPacket_t rtpPackets[MAX_RTP_PACKETS_STATIC];
	int packetCount = 0;
	int sent = 0;

	if (rtpFd < 0 || NULL == data || len <= 0 ||
		NULL == seq || NULL == timestamp)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	memset(rtpPackets, 0, sizeof(rtpPackets));

	// 封装RTP包
	packetCount = RTPEncapsulate(format, data, len, 
		rtpPackets, MAX_RTP_PACKETS_STATIC, ssrc);
	if (packetCount <= 0)
	{
		LOG_ERR("RTPEncapsulate failed, format: %d, data len: %d\n", format, len);
		return -1;
	}
	
	// 发送所有RTP包
	for (int i = 0; i < packetCount; i++)
	{
		// 更新序列号和时间戳
		rtpPackets[i].data[2] = (*seq >> 8) & 0xFF;
		rtpPackets[i].data[3] = *seq & 0xFF;
		(*seq)++;

		rtpPackets[i].data[4] = (*timestamp >> 24) & 0xFF;
		rtpPackets[i].data[5] = (*timestamp >> 16) & 0xFF;
		rtpPackets[i].data[6] = (*timestamp >> 8) & 0xFF;
		rtpPackets[i].data[7] = *timestamp & 0xFF;

		// 通过TCP发送RTP包（无前缀，直接发送，使用MSG_NOSIGNAL避免SIGPIPE）
		sent = send(rtpFd, rtpPackets[i].data, rtpPackets[i].len, MSG_NOSIGNAL);
		if (sent != rtpPackets[i].len)
		{
			LOG_ERR("send TCP RTP data failed for packet %d\n", i);
			// 释放所有包的内存
			for (int j = 0; j < packetCount; j++)
			{
				if (NULL != rtpPackets[j].data)
				{
					free(rtpPackets[j].data);
					rtpPackets[j].data = NULL;
				}
			}
			return -1;
		}
	}

	// 释放所有包的内存
	for (int i = 0; i < packetCount; i++)
	{
		if (NULL != rtpPackets[i].data)
		{
			free(rtpPackets[i].data);
			rtpPackets[i].data = NULL;
		}
	}

	return 0;
}

void *RTPHandleThread(void *args)
{
	RTSPHandle_t *handle = (RTSPHandle_t*)args;
	int ret = 0;
	unsigned char *dataBuf = NULL;
	int dataLen = 0;
	unsigned short seq = 0;
	unsigned int timestamp = 0;
	unsigned int ssrc = 0; // SSRC标识（每个线程随机生成）
	int timestampIncrement = 0;
	RTSPTransportMode_t transportMode;
	int firstFrameFlag = 1;

	if (NULL == handle)
	{
		LOG_ERR("Invalid handle\n");
		return (void*)-1;
	}

	// 设置线程名称
	ret = prctl(PR_SET_NAME, "RTPHandle", 0, 0, 0);
	if (ret != 0)
	{
		LOG_WARN("Failed to set RTPHandle thread name\n");
	}

	// 忽略SIGPIPE信号，避免向已关闭的socket发送数据时线程被终止
	signal(SIGPIPE, SIG_IGN);

	// 分配数据缓冲区
	dataBuf = (unsigned char*)malloc(1024 * 1024); // 1MB
	if (NULL == dataBuf)
	{
		LOG_ERR("malloc failed for data buffer\n");
		return (void*)-1;
	}

	// 生成随机SSRC（RFC 3550建议随机生成，避免冲突）
	// 使用时间戳和线程ID作为随机种子
	srand((unsigned int)time(NULL) ^ (unsigned int)(uintptr_t)pthread_self());
	ssrc = (unsigned int)rand() | ((unsigned int)rand() << 16);
	// 确保SSRC不为0（0是无效值）
	if (ssrc == 0)
	{
		ssrc = 0x12345678; // 如果随机生成失败，使用默认值
	}
	LOG_DEBUG("RTP thread SSRC: 0x%08X\n", ssrc);

	// 计算时间戳增量（90000 / fps）
	if (handle->config.fps > 0)
	{
		timestampIncrement = 90000 / handle->config.fps;
	}
	else
	{
		timestampIncrement = 3000; // 默认30fps
	}

	// 获取传输模式
	pthread_mutex_lock(&handle->mutex);
	transportMode = handle->transportMode;
	pthread_mutex_unlock(&handle->mutex);

	LOG_INFO("RTP thread started, transport mode: %d, timestamp increment: %d\n",
		transportMode, timestampIncrement);

	// 循环发送数据
	while (handle->isRunning && handle->hasActiveRtpSession)
	{
		// 从回调函数获取数据
		if (NULL != handle->getData)
		{
			FrameInfo_t frameInfo;
			int ret = handle->getData(&frameInfo);
			if (ret == 0 && frameInfo.data != NULL && frameInfo.size > 0)
			{
				// 拷贝帧数据到缓冲区（因为frameInfo.data可能指向内部缓冲区）
				if (frameInfo.size > 1024 * 1024)
				{
					LOG_ERR("Frame too large: %zu bytes\n", frameInfo.size);
					continue;
				}
				memcpy(dataBuf, frameInfo.data, frameInfo.size);
				dataLen = (int)frameInfo.size;
				
				// 确保首帧是SPS
				if (firstFrameFlag)
				{
					if (frameInfo.type != H264_FRAME_TYPE_I)
					{
						LOG_DEBUG("First frame is not I Frame, type: %d\n", 
							frameInfo.type);
						continue; // 丢弃非SPS首帧
					}
					else
					{
						firstFrameFlag = 0;
					}
				}
				
				timestamp += timestampIncrement;
				
				// 根据传输模式选择发送函数
				pthread_mutex_lock(&handle->mutex);
				transportMode = handle->transportMode;
				pthread_mutex_unlock(&handle->mutex);

				if (transportMode == RTSP_TRANSPORT_UDP)
				{
					// UDP模式
					pthread_mutex_lock(&handle->mutex);
					int rtpFd = handle->rtpFd;
					struct sockaddr_in clientRtpAddr = handle->clientRtpAddr;
					pthread_mutex_unlock(&handle->mutex);
					
					if (rtpFd >= 0)
					{
						ret = RTPSendDataUdp(rtpFd, &clientRtpAddr,
											 handle->config.format, 
											 dataBuf, dataLen, 
											 &seq, &timestamp, ssrc);
					}
					else
					{
						LOG_INFO("RTP socket closed, exiting RTP thread\n");
						ret = -1;
					}
				}
				else if (transportMode == RTSP_TRANSPORT_TCP_INTERLEAVED)
				{
					// TCP Interleaved模式
					pthread_mutex_lock(&handle->mutex);
					int rtspClientFd = handle->rtspClientFd;
					unsigned char rtpChannel = handle->rtpChannel;
					pthread_mutex_unlock(&handle->mutex);

					if (rtspClientFd >= 0)
					{
						ret = RTPSendDataTCPInterleaved(rtspClientFd, rtpChannel,
														handle->config.format,
														dataBuf, dataLen,
														&seq, &timestamp, ssrc);
					}
					else
					{
						LOG_ERR("RTSP client socket not available\n");
						ret = -1;
					}
				}
				else
				{
					LOG_ERR("Unknown transport mode: %d\n", transportMode);
					ret = -1;
				}

				if (ret < 0)
				{
					// 检查是否是连接关闭导致的错误（TCP模式）
					if (transportMode == RTSP_TRANSPORT_TCP_INTERLEAVED)
					{
						// TCP模式下，发送失败通常表示客户端已关闭连接
						// 这是正常情况，应该优雅退出，不影响RTSP线程继续监听
						LOG_INFO("RTP send failed (client disconnected), mode: %d, exiting RTP thread\n", transportMode);
					}
					else
					{
						LOG_ERR("RTP send failed, mode: %d\n", transportMode);
					}
					// 退出RTP线程，但不清除hasActiveRtpSession标志
					// 让RTSP线程在检测到连接关闭时统一处理
					break;
				}
			}
			else if (dataLen < 0)
			{
				LOG_ERR("getData callback returned error\n");
				break;
			}
		}
		else
		{
			LOG_ERR("getData callback is NULL\n");
			break;
		}
	}

	if (NULL != dataBuf)
	{
		free(dataBuf);
		dataBuf = NULL;
	}

	LOG_INFO("RTPHandleThread exited\n");
	return (void*)0;
}

