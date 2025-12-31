#ifndef RTSP_H
#define RTSP_H

#include <pthread.h>
#include "rtp.h"

#define STRINGIFY(x) #x
#define TO_STRING(x) STRINGIFY(x)

#define RTSP_SERVER_IP "192.168.0.107"
#define RTSP_SERVER_PORT 8554
#define RTSP_SERVER_URL "rtsp://" RTSP_SERVER_IP ":"                          \
                              TO_STRING(RTSP_SERVER_PORT) "/live"

#define RTSP_REQUEST_OPTION                                                   \
        "OPTIONS " RTSP_SERVER_URL " RTSP/1.0\r\n"                            \
        "CSeq: 2\r\n"                                                         \
        "User-Agent: LibVLC/3.0.21 (LIVE555 Streaming Media v2016.11.28)\r\n" \
        "\r\n"

#define RTSP_REQUEST_DESCRIBE                                                 \
        "DESCRIBE " RTSP_SERVER_URL " RTSP/1.0\r\n"                           \
        "CSeq: 3\r\n"                                                         \
        "User-Agent: rtsp_client\r\n"                                         \
        "Accept: application/sdp\r\n"                                         \
        "\r\n"

#define RTSP_REQUEST_SETUP_TRACK1                                             \
        "SETUP " RTSP_SERVER_URL "/track1 RTSP/1.0\r\n"                       \
        "CSeq: 4\r\n"                                                         \
        "User-Agent: LibVLC/3.0.21 (LIVE555 Streaming Media v2016.11.28)\r\n" \
        "Transport: RTP/AVP;unicast;client_port=%s-%s\r\n"                    \
        "\r\n"

#define RTSP_REQUEST_SETUP_TRACK2                                             \
        "SETUP " RTSP_SERVER_URL "/track2 RTSP/1.0\r\n"                       \
        "CSeq: 5\r\n"                                                         \
        "Transport: RTP/AVP;unicast;client_port=%s-%s\r\n"                    \
        "Session: %s\r\n"                                                     \
        "\r\n"

#define RTSP_REQUEST_PLAY                                                     \
        "PLAY " RTSP_SERVER_URL " RTSP/1.0\r\n"                               \
        "CSeq: 6\r\n"                                                         \
        "Session: %s\r\n"                                                     \
        "\r\n"

#define RTSP_REQUEST_TEARDOWN                                                 \
        "TEARDOWN " RTSP_SERVER_URL " RTSP/1.0\r\n"                           \
        "CSeq: 7\r\n"                                                         \
        "User-Agent: LibVLC/3.0.21 (LIVE555 Streaming Media v2016.11.28)\r\n" \
        "Session: %s\r\n"                                                     \
        "\r\n"

typedef struct
{
    pthread_t thread_id;        // RTSP线程ID
    int thread_status;          // RTSP线程状态

    int rtsp_wsa_flag;          // Winsock初始化标志
    int rtsp_fd;                // RTSP通信套接字
    char session_id[32];        // RTSP会话ID
    char rtsp_send_buf[1024];   // RTSP发送缓冲区
    char rtsp_recv_buf[1024];   // RTSP接收缓冲区

    rtp_t *rtp_ctx;             // RTP上下文

} rtsp_clinet_t;

#endif
