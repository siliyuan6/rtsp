/**
 * @file rtp.h
 * @brief RTP数据发送层接口定义
 * 
 * 提供RTP数据封装和发送功能
 */

#ifndef RTP_H
#define RTP_H

#include "api/rtsp_api.h"

#define RTP_VERSION 2
#define RTP_HEADER_SIZE 12
#define RTP_MAX_PAYLOAD_SIZE 1400
#define RTP_SSRC 0x12345678

/**
 * @brief RTP包结构
 */
typedef struct
{
	unsigned char *data;
	int len;
} RtpPacket_t;

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
int RTPEncapsulate(RTSPStreamFormat_t format, const unsigned char *data,
	int len, RtpPacket_t *rtpPackets, int maxPackets);

/**
 * @brief 发送RTP数据
 * 
 * @param rtpFd RTP socket文件描述符
 * @param clientAddr 客户端地址
 * @param format 数据格式
 * @param data 原始数据
 * @param len 数据长度
 * @param seq 序列号
 * @param timestamp 时间戳
 * @return 成功返回0，失败返回-1
 */
int RTPSendData(int rtpFd, struct sockaddr_in *clientAddr,
	RTSPStreamFormat_t format, const unsigned char *data, int len,
	unsigned short *seq, unsigned int *timestamp);

/**
 * @brief 发送单个RTP包
 * 
 * @param rtpFd RTP socket文件描述符
 * @param clientAddr 客户端地址
 * @param nalu NALU数据
 * @param len NALU长度
 * @param seq 序列号
 * @param timestamp 时间戳
 * @return 成功返回0，失败返回-1
 */
int RTPSendSingle(int rtpFd, struct sockaddr_in *clientAddr,
	const unsigned char *nalu, int len, unsigned short *seq,
	unsigned int *timestamp);

/**
 * @brief 发送分片RTP包
 * 
 * @param rtpFd RTP socket文件描述符
 * @param clientAddr 客户端地址
 * @param nalu NALU数据
 * @param len NALU长度
 * @param seq 序列号
 * @param timestamp 时间戳
 * @return 成功返回0，失败返回-1
 */
int RTPSendFragmented(int rtpFd, struct sockaddr_in *clientAddr,
	const unsigned char *nalu, int len, unsigned short *seq,
	unsigned int *timestamp);

/**
 * @brief RTP发送线程
 * 
 * @param args RTSP句柄指针
 * @return 线程返回值
 */
void *RTPHandleThread(void *args);

#endif /* RTP_H */

