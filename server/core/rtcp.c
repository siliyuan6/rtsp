/**
 * @file rtcp.c
 * @brief RTCP包解析实现
 * 
 * 实现RTCP包的解析功能，主要用于解析Receiver Report (RR)包
 */

#include <stdio.h>
#include <string.h>

#include "rtcp.h"
#include "common/log.h"

/**
 * @brief 解析RTCP公共头
 * 
 * RTCP Common Header格式（4字节）:
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |V=2|P|    RC   |      PT       |         length                |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 
 * @param data RTCP包数据
 * @param len 数据长度
 * @param version 输出参数，版本号（应为2）
 * @param padding 输出参数，填充标志
 * @param count 输出参数，报告计数
 * @param packetType 输出参数，包类型
 * @param length 输出参数，长度（以32位字为单位，不包括头）
 * @return 成功返回0，失败返回-1
 */
static int ParseRTCPHeader(const unsigned char *data, int len,
	unsigned char *version, unsigned char *padding, unsigned char *count,
	unsigned char *packetType, unsigned short *length)
{
	if (NULL == data || len < 4)
	{
		return -1;
	}

	// 解析第一个字节
	unsigned char firstByte = data[0];
	*version = (firstByte >> 6) & 0x03;
	*padding = (firstByte >> 5) & 0x01;
	*count = firstByte & 0x1F;

	// 解析第二个字节（包类型）
	*packetType = data[1];

	// 解析长度（16位，网络字节序）
	*length = (data[2] << 8) | data[3];

	// 验证版本号（应为2）
	if (*version != 2)
	{
		LOG_DEBUG("Invalid RTCP version: %d (expected 2)\n", *version);
		return -1;
	}

	return 0;
}

/**
 * @brief 解析Receiver Report (RR)包
 * 
 * RR包格式:
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                 SSRC of packet sender                        |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * | fraction lost |       cumulative number of packets lost      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |       extended highest sequence number received              |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                      interarrival jitter                     |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |                 last SR (LSR)                                |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * |              delay since last SR (DLSR)                      |
 * +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 * 
 * @param data RR包数据（不包含RTCP公共头）
 * @param len 数据长度
 * @param stats 输出参数，解析出的统计信息
 * @return 成功返回0，失败返回-1
 */
int RTCPParseRR(const unsigned char *data, int len, RTCPStats_t *stats)
{
	if (NULL == data || NULL == stats || len < 24)
	{
		LOG_DEBUG("Invalid RR packet: data=%p, stats=%p, len=%d\n", data, stats, len);
		return -1;
	}

	memset(stats, 0, sizeof(RTCPStats_t));

	// 解析SSRC of sender (4字节，网络字节序，大端)
	// 使用安全的方式读取，避免对齐问题
	stats->ssrc = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];

	// 解析fraction lost (1字节) 和 cumulative packets lost (3字节)
	stats->fractionLost = data[4];
	
	// 累计丢包数是24位有符号数（最高位为符号位）
	unsigned int lost24 = (data[5] << 16) | (data[6] << 8) | data[7];
	// 处理符号扩展（如果最高位为1，则为负数）
	if (lost24 & 0x800000)
	{
		stats->cumulativePacketsLost = lost24 | 0xFF000000; // 符号扩展
	}
	else
	{
		stats->cumulativePacketsLost = lost24;
	}

	// 解析extended highest sequence number (4字节，网络字节序，大端)
	stats->extendedHighestSeq = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];

	// 解析interarrival jitter (4字节，网络字节序，大端)
	stats->jitter = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];

	// 解析last SR timestamp (4字节，网络字节序，大端)
	stats->lastSRTimestamp = (data[16] << 24) | (data[17] << 16) | (data[18] << 8) | data[19];

	// 解析delay since last SR (4字节，网络字节序，大端)
	stats->delaySinceLastSR = (data[20] << 24) | (data[21] << 16) | (data[22] << 8) | data[23];

	return 0;
}

/**
 * @brief 解析RTCP包
 * 
 * @param data RTCP包数据（不包含interleaved头）
 * @param len 数据长度
 * @param stats 输出参数，解析出的统计信息（仅RR包有效）
 * @return 成功返回包类型（200-203），失败返回-1
 */
int RTCPParsePacket(const unsigned char *data, int len, RTCPStats_t *stats)
{
	unsigned char version = 0;
	unsigned char padding = 0;
	unsigned char count = 0;
	unsigned char packetType = 0;
	unsigned short length = 0;
	int packetLen = 0;

	if (NULL == data || len < 4)
	{
		LOG_DEBUG("Invalid RTCP packet: data=%p, len=%d\n", data, len);
		return -1;
	}

	// 解析RTCP公共头
	if (ParseRTCPHeader(data, len, &version, &padding, &count, &packetType, &length) < 0)
	{
		LOG_DEBUG("Failed to parse RTCP header\n");
		return -1;
	}

	// 计算包的总长度（以字节为单位）
	// length字段表示32位字的数量，不包括头本身
	packetLen = (length + 1) * 4; // +1是因为length不包括头

	// 检查数据长度是否足够
	if (len < packetLen)
	{
		LOG_DEBUG("RTCP packet incomplete: expected %d bytes, got %d bytes\n", packetLen, len);
		return -1;
	}

	// 根据包类型处理
	switch (packetType)
	{
		case RTCP_PT_RR:
		{
			// Receiver Report
			if (NULL != stats)
			{
				// 跳过RTCP公共头（4字节），解析RR包体
				if (RTCPParseRR(data + 4, len - 4, stats) < 0)
				{
					LOG_DEBUG("Failed to parse RR packet\n");
					return -1;
				}
			}
			LOG_DEBUG("RTCP RR packet parsed, SSRC: 0x%08X, fraction lost: %u, cumulative lost: %d\n",
				stats ? stats->ssrc : 0,
				stats ? stats->fractionLost : 0,
				stats ? stats->cumulativePacketsLost : 0);
			return RTCP_PT_RR;
		}
		case RTCP_PT_SR:
		{
			// Sender Report（服务器端通常不接收，但需要识别）
			LOG_DEBUG("RTCP SR packet received (not processed)\n");
			return RTCP_PT_SR;
		}
		case RTCP_PT_SDES:
		{
			// Source Description
			LOG_DEBUG("RTCP SDES packet received (not processed)\n");
			return RTCP_PT_SDES;
		}
		case RTCP_PT_BYE:
		{
			// Goodbye
			LOG_DEBUG("RTCP BYE packet received (not processed)\n");
			return RTCP_PT_BYE;
		}
		default:
		{
			LOG_DEBUG("Unknown RTCP packet type: %d\n", packetType);
			return -1;
		}
	}
}

