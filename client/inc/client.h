/**
 * @file client.h
 * @brief RTSP客户端头文件
 * 
 * 本文件定义了RTSP客户端的数据结构和常量，用于与RTSP服务器通信。
 * 支持标准的RTSP协议流程：OPTIONS -> DESCRIBE -> SETUP -> PLAY -> TEARDOWN
 */

#ifndef RTSP_CLIENT_H
#define RTSP_CLIENT_H

#include <pthread.h>
#include "rtp.h"

#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)

// RTSP服务器配置（可根据实际情况修改）
#define RTSP_SERVER_IP "192.168.0.107"     // RTSP服务器IP地址
#define RTSP_SERVER_PORT 8554              // RTSP服务器端口
#define RTSP_SERVER_URL "rtsp://" RTSP_SERVER_IP ":"                          \
                              TO_STRING(RTSP_SERVER_PORT) "/live"

#define RTSP_REQUEST_OPTION                                                   \
        "OPTIONS " RTSP_SERVER_URL " RTSP/1.0\r\n"                            \
        "CSeq: 1\r\n"                                                         \
        "User-Agent: LibVLC/3.0.21 (LIVE555 Streaming Media v2016.11.28)\r\n" \
        "\r\n"

#define RTSP_REQUEST_DESCRIBE                                                 \
        "DESCRIBE " RTSP_SERVER_URL " RTSP/1.0\r\n"                           \
        "CSeq: 2\r\n"                                                         \
        "User-Agent: client\r\n"                                         \
        "Accept: application/sdp\r\n"                                         \
        "\r\n"

#define RTSP_REQUEST_SETUP_TRACK1                                             \
        "SETUP " RTSP_SERVER_URL "/track0 RTSP/1.0\r\n"                       \
        "CSeq: 3\r\n"                                                         \
        "User-Agent: LibVLC/3.0.21 (LIVE555 Streaming Media v2016.11.28)\r\n" \
        "Transport: RTP/AVP;unicast;client_port=%s-%s\r\n"                    \
        "\r\n"

#define RTSP_REQUEST_SETUP_TRACK2                                             \
        "SETUP " RTSP_SERVER_URL "/track1 RTSP/1.0\r\n"                       \
        "CSeq: 4\r\n"                                                         \
        "Transport: RTP/AVP;unicast;client_port=%s-%s\r\n"                    \
        "Session: %s\r\n"                                                     \
        "\r\n"

#define RTSP_REQUEST_PLAY                                                     \
        "PLAY " RTSP_SERVER_URL " RTSP/1.0\r\n"                               \
        "CSeq: 5\r\n"                                                         \
        "Session: %s\r\n"                                                     \
        "\r\n"

#define RTSP_REQUEST_TEARDOWN                                                 \
        "TEARDOWN " RTSP_SERVER_URL " RTSP/1.0\r\n"                           \
        "CSeq: 6\r\n"                                                         \
        "User-Agent: LibVLC/3.0.21 (LIVE555 Streaming Media v2016.11.28)\r\n" \
        "Session: %s\r\n"                                                     \
        "\r\n"

/**
 * @brief RTSP客户端上下文结构体
 * 
 * 包含RTSP客户端运行所需的所有状态信息，包括：
 * - 网络连接信息（套接字、会话ID等）
 * - 线程管理信息
 * - RTP上下文指针
 */
typedef struct
{
    pthread_t thread_id;        // RTSP工作线程ID
    int thread_status;          // RTSP线程运行状态（TRUE/FALSE）

    int rtsp_wsa_flag;          // Winsock初始化标志（Windows平台）
    int rtsp_fd;                // RTSP TCP通信套接字文件描述符
    char session_id[32];        // RTSP会话ID（从SETUP响应中获取）
    char rtsp_send_buf[1024];   // RTSP请求发送缓冲区
    char rtsp_recv_buf[1024];   // RTSP响应接收缓冲区

    rtp_t *rtp_ctx;             // RTP/RTCP上下文指针

} rtsp_client_t;

#endif /* RTSP_CLIENT_H */

