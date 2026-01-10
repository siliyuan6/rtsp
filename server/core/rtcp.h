/**
 * @file rtcp.h
 * @brief RTCP包解析接口定义
 * 
 * 提供RTCP包的解析功能，主要用于解析Receiver Report (RR)包
 */

#ifndef RTCP_H
#define RTCP_H

#include "api/rtsp_api.h"

// RTCP包类型
#define RTCP_PT_SR   200  // Sender Report
#define RTCP_PT_RR   201  // Receiver Report
#define RTCP_PT_SDES 202  // Source Description
#define RTCP_PT_BYE  203  // Goodbye

/**
 * @brief 解析RTCP包
 * 
 * @param data RTCP包数据（不包含interleaved头）
 * @param len 数据长度
 * @param stats 输出参数，解析出的统计信息（仅RR包有效）
 * @return 成功返回包类型（200-203），失败返回-1
 */
int RTCPParsePacket(const unsigned char *data, int len, RTCPStats_t *stats);

/**
 * @brief 解析Receiver Report (RR)包
 * 
 * @param data RR包数据（不包含RTCP公共头）
 * @param len 数据长度
 * @param stats 输出参数，解析出的统计信息
 * @return 成功返回0，失败返回-1
 */
int RTCPParseRR(const unsigned char *data, int len, RTCPStats_t *stats);

#endif /* RTCP_H */

