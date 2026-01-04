/**
 * @file rtp.h
 * @brief RTP/RTCP协议处理头文件
 * 
 * 本文件定义了RTP/RTCP数据包解析相关的数据结构和函数接口。
 * 支持H.264视频流的RTP封装格式解析和重组。
 */

#ifndef RTP_H
#define RTP_H

#include <stdint.h>
#include "player.h"

typedef struct FU_INDICATOR_S
{
    uint8_t u5Type : 5;
    uint8_t u2Nri : 2;
    uint8_t u1F : 1;
} FU_INDICATOR_T;

typedef struct FU_HEADER_S
{
    uint8_t u5Type : 5;
    uint8_t u1R : 1;
    uint8_t u1E : 1;
    uint8_t u1S : 1;
} FU_HEADER_T;

typedef struct RTP_FIXED_HEADER_S
{
    uint8_t u4CSrcLen : 4;    /* 期望为0 */
    uint8_t u1Externsion : 1; /* 期望为1，见RTP_OP说明 */
    uint8_t u1Padding : 1;    /* 期望为0 */
    uint8_t u2Version : 2;    /* 期望为2 */

    uint8_t u7Payload : 7; /* RTP负载类型 */
    uint8_t u1Marker : 1;  /* 期望为1 */

    uint16_t u16SeqNum;

    uint32_t u32TimeStamp;

    uint32_t u32SSrc; /* 这里作为流编号使用 */
} RTP_FIXED_HEADER_T;

typedef struct NALU_HEADER_S
{
    uint8_t u5Type : 5;
    uint8_t u2Nri : 2;
    uint8_t u1F : 1;
} NALU_HEADER_T;

typedef enum
{
    NALU_SIGNEL = 0,     // 单个NALU包
    NALU_MUTIL,          // 多个NALU打包
    NALU_SLICE           // 分片NALU包
} NALU_E;

typedef enum
{
    FRAME_NOIDR = 1,   // 非IDR帧
    FRAME_PART_A,      // 分片A
    FRAME_PART_B,      // 分片B
    FRAME_PART_C,      // 分片C
    FRAME_IDR   = 5,   // IDR帧
    FRAME_SEI,         // SEI
    FRAME_SPS,         // SPS
    FRAME_PPS,         // PPS
    FRAME_ACCESS,      // 访问单元
    FRAME_END   = 10,  // 结束单元

    FRAME_FU_A  = 28,  // FU-A分片
    FRAME_FU_B  = 29,  // FU-B分片
} FRAME_E;

typedef struct
{
    int rtp_wsa_flag;               // Winsock初始化标志
    int rtp_listen_port[2];         // 监听端口 - 0: RTP; 1: RTCP
    int rtp_fd[2];                  // 套接字 - 0: RTP; 1: RTCP
    void *rtp_recv_buf;             // RTP接收缓冲区
    int rtp_recv_len;               // RTP接收长度

    FU_INDICATOR_T     fu_ind;      // FU指示字段
    FU_HEADER_T        fu_hdr;      // FU头
    RTP_FIXED_HEADER_T rtp_hdr;     // RTP固定头
    NALU_HEADER_T      nalu_hdr;    // NALU头
    NALU_E nalu_type;               // NALU类型

    FILE *fd;                       // 保存码流的文件指针
    char stream_filename[64];       // 码流文件名
    int last_frame_type;            // 上一帧类型
    int last_last_frame_type;       // 上上一帧类型
    size_t total_written;           // 已写入文件的总字节数

} rtp_t;

// 创建RTP上下文并初始化RTP/RTCP套接字
int rtp_create(void **ctx);
// 销毁RTP上下文并释放资源
int rtp_destroy(void *ctx);
// 解析RTP数据包并写入码流文件
int rtp_pkg_parse(void *ctx, void *buffer, int len);
// 解析RTCP数据包（当前为空实现）
int rtcp_pkg_parse(void *buffer, int len);

#endif /* RTP_H */
