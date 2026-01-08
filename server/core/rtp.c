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
#include <time.h>
#define closesocket close

#include "rtp.h"
#include "api/rtsp_api.h"
#include "common/log.h"

#define FU_A_START 0x80
#define FU_A_MID 0x00
#define FU_A_END 0x40

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
 * @return 成功返回0，失败返回-1
 */
static int RTPEncapsulateH264Single(const unsigned char *nalu, int len,
	RtpPacket_t *rtpPacket)
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
	CreateRtpHeader(rtpPacket->data, 96, 0, 0, RTP_SSRC);

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
	int len, RtpPacket_t *rtpPackets, int maxPackets)
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
		LOG_ERR("Too many fragments: %d\n", fragmentCount);
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
		CreateRtpHeader(rtpPackets[i].data, 96, 0, 0, RTP_SSRC);

		// 设置M标记（最后一个分片）
		if (i == fragmentCount - 1)
		{
			rtpPackets[i].data[1] |= 0x80;
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
	RtpPacket_t *rtpPackets, int maxPackets)
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
				if (RTPEncapsulateH264Single(data, len, &rtpPackets[0]) < 0)
				{
					return -1;
				}
				return 1;
			}
			else
			{
				// 分片
				return RTPEncapsulateH264Fragmented(data, len,
					rtpPackets, maxPackets);
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

	// 封装RTP包
	if (RTPEncapsulateH264Single(nalu, len, &rtpPacket) < 0)
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
	RtpPacket_t rtpPackets[32];
	int fragmentCount = 0;
	int sent = 0;

	if (rtpFd < 0 || NULL == clientAddr || NULL == nalu ||
		len <= 0 || NULL == seq || NULL == timestamp)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	memset(rtpPackets, 0, sizeof(rtpPackets));

	// 封装RTP包
	fragmentCount = RTPEncapsulateH264Fragmented(nalu, len,
		rtpPackets, 32);
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
int RTPSendData(int rtpFd, struct sockaddr_in *clientAddr,
	RTSPStreamFormat_t format, const unsigned char *data, int len,
	unsigned short *seq, unsigned int *timestamp)
{
	RtpPacket_t rtpPackets[32];
	int packetCount = 0;
	int sent = 0;

	if (rtpFd < 0 || NULL == clientAddr || NULL == data ||
		len <= 0 || NULL == seq || NULL == timestamp)
	{
		LOG_ERR("Invalid parameters\n");
		return -1;
	}

	memset(rtpPackets, 0, sizeof(rtpPackets));

	// 封装RTP包
	packetCount = RTPEncapsulate(format, data, len, 
		rtpPackets, sizeof(rtpPackets));
	if (packetCount <= 0)
	{
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
 * @brief RTP发送线程
 * 
 * @param args RTSP句柄指针
 * @return 线程返回值
 */
/**
 * @brief 获取NALU类型
 * 
 * @param data H.264数据（包含起始码）
 * @param len 数据长度
 * @return NALU类型，失败返回-1
 */
static int GetNALUType(const unsigned char *data, int len)
{
	int startCodeLen = GetH264StartCodeLen(data, len);
	if (startCodeLen <= 0 || startCodeLen >= len)
	{
		return -1;
	}
	
	// NALU类型在起始码后的第一个字节的低5位
	return data[startCodeLen] & 0x1F;
}

void *RTPHandleThread(void *args)
{
	RTSPHandle_t *handle = (RTSPHandle_t*)args;
	int ret = 0;
	unsigned char *dataBuf = NULL;
	int dataLen = 0;
	unsigned short seq = 0;
	unsigned int timestamp = 0;
	int timestampIncrement = 0;
	int naluType = 0;

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

	// 分配数据缓冲区
	dataBuf = (unsigned char*)malloc(1024 * 1024); // 1MB
	if (NULL == dataBuf)
	{
		LOG_ERR("malloc failed for data buffer\n");
		return (void*)-1;
	}

	// 计算时间戳增量（90000 / fps）
	if (handle->config.fps > 0)
	{
		timestampIncrement = 90000 / handle->config.fps;
	}
	else
	{
		timestampIncrement = 3000; // 默认30fps
	}

	LOG_INFO("RTP thread started, timestamp increment: %d\n",
		timestampIncrement);

	// 循环发送数据
	while (handle->isRunning)
	{
		// 从回调函数获取数据
		if (NULL != handle->getData)
		{
			dataLen = handle->getData(dataBuf, 1024 * 1024);
			if (dataLen > 0)
			{
				// 获取NALU类型
				naluType = GetNALUType(dataBuf, dataLen);
				
				// 判断是否是参数集（SPS、PPS、SEI）或IDR帧
				// 这些NALU属于同一个访问单元，应该使用相同的时间戳
				if (!(naluType == NALU_TYPE_PPS || 
					  naluType == NALU_TYPE_SEI ||
					  naluType == NALU_TYPE_IDR))
				{
					timestamp += timestampIncrement;
				}
				
				// 发送RTP数据
				ret = RTPSendData(handle->rtpFd, &handle->clientRtpAddr,
					handle->config.format, 
					dataBuf, dataLen, 
					&seq, &timestamp);
				if (ret < 0)
				{
					LOG_ERR("RTPSendData failed\n");
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

		// 控制发送速率（简单延时）
		usleep(1000000 / handle->config.fps); // 微秒
	}

	if (NULL != dataBuf)
	{
		free(dataBuf);
		dataBuf = NULL;
	}

	LOG_INFO("RTP thread exited\n");
	return (void*)0;
}

