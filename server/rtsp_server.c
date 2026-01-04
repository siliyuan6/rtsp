/**
 * @file rtsp_server.c
 * @brief RTSP 服务器实现，支持 H.264 视频流推送
 * 
 * 本文件实现了基于 Linux 的 RTSP 服务器，能够读取 H.264 文件并通过 RTP 协议推送视频流。
 * 支持标准的 RTSP 方法：OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <sys/types.h>
#include <time.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <errno.h>

#include "log.h"

/* ==================== 常量定义 ==================== */
#define RTSP_LISTEN_BACKLOG 5         // RTSP 监听队列长度
#define RTSP_PORT 8554                // RTSP 服务端口
#define RTP_PORT 5000                 // RTP 数据端口
#define RTCP_PORT 5001                // RTCP 控制端口
#define MAX_REQUEST_SIZE 2048         // RTSP 请求最大长度
#define H264_PAYLOAD_TYPE 96          // H.264 RTP 负载类型
#define MAX_RTP_PACKET_SIZE 1400      // RTP 包最大大小
#define RTP_HEADER_SIZE 12            // RTP 固定头大小
#define H264_FU_HEADER_SIZE 2         // H.264 FU-A 分片头大小
#define H264_FILE_PATH "704x576.h264" // H.264 文件路径
#define FRAME_RATE 25                 // 视频帧率（fps）
#define RTP_CLOCK_RATE 90000          // RTP 时钟频率（Hz）
#define TIMESTAMP_INCREMENT (RTP_CLOCK_RATE / FRAME_RATE) // 每帧时间戳增量
#define RTSP_RECV_TIMEOUT_SEC 30      // RTSP 接收超时时间（秒）

/* ==================== 数据结构定义 ==================== */

/**
 * @brief RTP 固定头结构体
 */
typedef struct
{
    unsigned char csrcCount:4;     // CSRC 计数，低4位
    unsigned char extension:1;     // 扩展标志
    unsigned char padding:1;       // 填充标志
    unsigned char version:2;       // RTP 版本号，高2位

    unsigned char payloadType:7;   // 负载类型
    unsigned char marker:1;        // 标记位（帧结束标记）
    
    unsigned short sequence;       // 序列号
    unsigned int timestamp;        // 时间戳
    unsigned int ssrc;             // 同步源标识符
} __attribute__((packed)) RtpHeader_t;

/**
 * @brief FU identifier 标识符
 */
typedef struct
{
    unsigned char u5Type:5;        // Type 位
    unsigned char u2NRI:2;         // NRI 位
    unsigned char u1F:1;           // F 位
} __attribute__((packed)) FUIdentifier_t;

/**
 * @brief FU header 头
 */
typedef struct
{
    unsigned char u5Type:5;        // Type 位
    unsigned char u1R:1;           // Reserved 位
    unsigned char u1E:1;           // End 位
    unsigned char u1S:1;           // Start 位
} __attribute__((packed)) FUHeader_t;

/**
 * @brief 客户端会话信息结构体
 */
typedef struct
{
    int clientSock;                    // RTSP 控制套接字
    int rtpSock;                       // RTP 数据套接字
    char sessionId[32];                // 会话 ID
    struct sockaddr_in clientRtpAddr;  // 客户端 RTP 地址
    unsigned short rtpSequence;        // RTP 序列号
    unsigned int rtpTimestamp;         // RTP 时间戳
} ClientSession_t;

/**
 * @brief 码流上下文结构体
 */
typedef struct
{
    unsigned char* h264Data;    // H.264 文件数据缓冲区
    size_t h264DataSize;        // H.264 文件大小
    size_t h264DataPos;         // 当前读取位置
} StreamContext_t;

/**
 * @brief 客户端处理参数结构体
 */
typedef struct
{
    int clientSock;              // 客户端套接字
    StreamContext_t* streamCtx;       // 服务器上下文
} ClientHandlerParam_t;

/**
 * @brief RTSP 请求方法枚举
 */
typedef enum
{
    RTSP_METHOD_UNKNOWN = 0,
    RTSP_METHOD_OPTIONS,
    RTSP_METHOD_DESCRIBE,
    RTSP_METHOD_SETUP,
    RTSP_METHOD_PLAY,
    RTSP_METHOD_TEARDOWN
} RtspMethod_t;

/* ==================== 函数声明 ==================== */

static int LoadH264File(StreamContext_t* streamCtx, const char* filename);
static size_t FindNextNalu(const StreamContext_t* streamCtx, 
        size_t startPos);
static void SendSingleNaluRtp(int rtpSock, 
        struct sockaddr_in* clientAddr,
        unsigned char* naluData, size_t naluSize,
        unsigned short* sequence, unsigned int* timestamp);
static void SendFragmentedNaluRtp(int rtpSock, 
        struct sockaddr_in* clientAddr,
        unsigned char* naluData, size_t naluSize,
        unsigned short* sequence, unsigned int* timestamp);
static void SendNaluRtp(int rtpSock, 
        struct sockaddr_in* clientAddr,
        unsigned char* naluData, size_t naluSize,
        unsigned short* sequence, unsigned int* timestamp);
static int CreateRtspSocket(void);
static int CreateRtpSocket(void);
static int ParseCseq(const char* buffer);
static RtspMethod_t ParseRtspMethod(const char* buffer);
static void HandleOptions(int clientSock, int cseq);
static void HandleDescribe(int clientSock, int cseq);
static int HandleSetup(int clientSock, int cseq, 
        ClientSession_t* session);
static void HandlePlay(int clientSock, int cseq, 
        ClientSession_t* session);
static void HandleTeardown(int clientSock, int cseq, 
        ClientSession_t* session);
static void SendH264Stream(StreamContext_t* ctx, 
        ClientSession_t* session);
static int WaitAndReceiveRtspRequest(int clientSock, char* buffer, size_t bufferSize);
static void* HandleClient(void* arg);
static void CleanupServerContext(StreamContext_t* ctx);

/* ==================== H.264 文件处理函数 ==================== */

/**
 * @brief 加载 H.264 文件到内存
 * @param ctx 服务器上下文
 * @param filename 文件路径
 * @return 成功返回 0，失败返回 -1
 */
static int LoadH264File(StreamContext_t* ctx, const char* filename)
{
    if (NULL == ctx || NULL == filename)
    {
        return -1;
    }

    FILE* fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        perror("fopen");
        return -1;
    }

    // 获取文件大小
    fseek(fp, 0, SEEK_END);
    ctx->h264DataSize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    // 分配内存
    ctx->h264Data = (unsigned char*)malloc(ctx->h264DataSize);
    if (NULL == ctx->h264Data)
    {
        fclose(fp);
        return -1;
    }

    // 读取文件内容
    size_t readSize = fread(ctx->h264Data, 1, ctx->h264DataSize, fp);
    fclose(fp);

    if (readSize != ctx->h264DataSize)
    {
        free(ctx->h264Data);
        ctx->h264Data = NULL;
        return -1;
    }

    LOG("Loaded H.264 file: %zu bytes\n", ctx->h264DataSize);
    return 0;
}

/**
 * @brief 查找下一个 NALU 起始码位置
 * @param ctx 服务器上下文
 * @param startPos 起始搜索位置
 * @return 找到的 NALU 起始位置（跳过起始码），未找到返回-1
 * 
 * 支持两种起始码格式：
 * - 0x00000001 (4 字节)
 * - 0x000001 (3 字节)
 */
static size_t FindNextNalu(const StreamContext_t* streamCtx, size_t startPos)
{
    if ((NULL == streamCtx) || (startPos >= streamCtx->h264DataSize))
    {
        return (NULL == streamCtx) ? 0 : streamCtx->h264DataSize;
    }

    for (size_t i = startPos; i < streamCtx->h264DataSize - 3; i++)
    {
        if (0x00 == streamCtx->h264Data[i] && 
            0x00 == streamCtx->h264Data[i + 1])
        {
            if (0x01 == streamCtx->h264Data[i + 2])
            {
                return i + 3; // 找到 0x000001
            }
            else if (0x00 == streamCtx->h264Data[i + 2] && 
                    0x01 == streamCtx->h264Data[i + 3])
            {
                return i + 4; // 找到 0x00000001
            }
        }
    }
    return streamCtx->h264DataSize;
}

/* ==================== RTP 打包和发送函数 ==================== */

/**
 * @brief 发送单个 NALU（不分片）
 * @param rtpSock RTP 套接字
 * @param clientAddr 客户端地址
 * @param naluData NALU 数据（不包含起始码）
 * @param naluSize NALU 数据大小
 * @param sequence RTP 序列号指针（会被更新）
 * @param timestamp RTP 时间戳指针（会被更新）
 */
static void SendSingleNaluRtp(int rtpSock, struct sockaddr_in* clientAddr,
        unsigned char* naluData, size_t naluSize,
        unsigned short* sequence, unsigned int* timestamp)
{
    if (NULL == clientAddr || NULL == naluData || 
            NULL == sequence || NULL == timestamp)
    {
        return;
    }

    unsigned char packet[MAX_RTP_PACKET_SIZE];
    RtpHeader_t *rtpHdr = (RtpHeader_t*)packet;

    // 填充 RTP 头
    rtpHdr->version = 2; // RTP 版本
    rtpHdr->padding = 0; // 无填充
    rtpHdr->extension = 0; // 无扩展
    rtpHdr->csrcCount = 0; // 无 CSRC
    // IDR 帧标记
    rtpHdr->marker = 1; // 当前帧为结束帧
    rtpHdr->payloadType = H264_PAYLOAD_TYPE;
    rtpHdr->sequence = htons(*sequence);
    rtpHdr->timestamp = htonl(*timestamp);
    rtpHdr->ssrc = htonl(rand());

    FUIdentifier_t *FUId = (FUIdentifier_t *)(packet + RTP_HEADER_SIZE);
    FUId->u1F = (naluData[0] & 0x80) >> 7;
    FUId->u2NRI = (naluData[0] & 0x60) >> 5;
    FUId->u5Type = naluData[0] & 0x1F;

    // 复制 NALU 数据
    memcpy(packet + RTP_HEADER_SIZE + sizeof(FUIdentifier_t), 
        naluData, naluSize);

    // 发送 RTP 包
    ssize_t sent = sendto(rtpSock, packet, 
            RTP_HEADER_SIZE + sizeof(FUIdentifier_t) + naluSize, 0,
            (struct sockaddr*)clientAddr, sizeof(*clientAddr));
    if (sent < 0)
    {
        perror("sendto RTP");
        LOG("Failed to send RTP packet: rtpSock=%d, size=%zu\n", 
            rtpSock, RTP_HEADER_SIZE + naluSize);
    }

    (*sequence)++;
}

/**
 * @brief 发送分片的 NALU（FU-A 格式）
 * @param rtpSock RTP 套接字
 * @param clientAddr 客户端地址
 * @param naluData NALU 数据（不包含起始码）
 * @param naluSize NALU 数据大小
 * @param sequence RTP 序列号指针（会被更新）
 * @param timestamp RTP 时间戳指针（会被更新）
 */
static void SendFragmentedNaluRtp(int rtpSock, 
        struct sockaddr_in* clientAddr,
        unsigned char* naluData, size_t naluSize,
        unsigned short* sequence, unsigned int* timestamp)
{
    if (NULL == clientAddr || NULL == naluData || 
            NULL == sequence || NULL == timestamp)
    {
        return;
    }

    // 提取 NALU 头信息
    unsigned char naluType = naluData[0] & 0x1F; // NALU 类型
    unsigned char naluF = naluData[0] & 0x80;
    unsigned char naluNri = naluData[0] & 0x60;

    size_t offset = 1; // 跳过 NALU 头
    size_t remaining = naluSize - 1; // 剩余数据大小

    while (remaining > 0)
    {
        unsigned char packet[MAX_RTP_PACKET_SIZE];
        RtpHeader_t* rtpHdr = (RtpHeader_t*)packet;

        // 计算当前分片的负载大小
        size_t payloadSize = (remaining > MAX_RTP_PACKET_SIZE) ? 
                MAX_RTP_PACKET_SIZE : remaining;

        // 填充 RTP 头
        rtpHdr->version = 2; // RTP 版本
        rtpHdr->padding = 0; // 无填充
        rtpHdr->extension = 0; // 无扩展
        rtpHdr->csrcCount = 0; // 无 CSRC
        // 最后一个分片标记
        rtpHdr->marker = (remaining == payloadSize) ? 1 : 0; // 最后一个分片标记
        rtpHdr->payloadType = H264_PAYLOAD_TYPE;
        rtpHdr->sequence = htons(*sequence);
        rtpHdr->timestamp = htonl(*timestamp);
        rtpHdr->ssrc = htonl(rand());

        // FU indicator (F + NRI + Type=28 for FU-A)
        FUIdentifier_t *FUId = (FUIdentifier_t *)(packet + RTP_HEADER_SIZE);
        FUId->u1F = naluF;
        FUId->u2NRI = naluNri;
        FUId->u5Type = 28; // FU-A

        // FU header (Type + S + E + R)
        FUHeader_t *FUHdr = 
            (FUHeader_t *)(packet + RTP_HEADER_SIZE + sizeof(FUIdentifier_t));
        FUHdr->u5Type = naluType;
        FUHdr->u1R = 0;
        FUHdr->u1E = (remaining == payloadSize) ? 1 : 0;
        FUHdr->u1S = (1 == offset) ? 1 : 0;

        // 复制 NALU 数据
        memcpy(packet + RTP_HEADER_SIZE + H264_FU_HEADER_SIZE, 
                naluData + offset, 
                payloadSize);

        // 发送 RTP 包
        size_t packetSize = RTP_HEADER_SIZE + H264_FU_HEADER_SIZE + payloadSize;
        ssize_t sent = sendto(rtpSock, packet, packetSize, 0,
                (struct sockaddr*)clientAddr, sizeof(*clientAddr));
        if (sent < 0)
        {
            perror("sendto RTP fragment");
            LOG("Failed to send RTP fragment: rtpSock=%d, size=%zu\n", 
                rtpSock, packetSize);
        }

        offset += payloadSize;
        remaining -= payloadSize;
        (*sequence)++;
    }
}

/**
 * @brief 发送 NALU（自动选择单包或分片）
 * @param rtpSock RTP 套接字
 * @param clientAddr 客户端地址
 * @param naluData NALU 数据（不包含起始码）
 * @param naluSize NALU 数据大小
 * @param sequence RTP 序列号指针（会被更新）
 * @param timestamp RTP 时间戳指针（会被更新）
 */
static void SendNaluRtp(int rtpSock, struct sockaddr_in* clientAddr,
        unsigned char* naluData, size_t naluSize,
        unsigned short* sequence, unsigned int* timestamp)
{
    if (NULL == clientAddr || NULL == naluData || 
            NULL == sequence || NULL == timestamp)
    {
        return;
    }

    if (naluSize <= MAX_RTP_PACKET_SIZE)
    {
        // 单个 RTP 包
        SendSingleNaluRtp(rtpSock, clientAddr, 
                naluData, naluSize, 
                sequence, timestamp);
    }
    else
    {
        // FU-A 分片
        SendFragmentedNaluRtp(rtpSock, clientAddr, 
                naluData, naluSize, 
                sequence, timestamp);
    }

    // 更新时间戳（每帧递增）
    *timestamp += TIMESTAMP_INCREMENT;
}

/* ==================== 套接字创建函数 ==================== */

/**
 * @brief 创建 RTSP 监听套接字
 * @return 成功返回套接字描述符，失败返回 -1
 */
static int CreateRtspSocket(void)
{
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        perror("socket");
        return -1;
    }

    // 设置地址重用选项
    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        perror("setsockopt");
        close(sock);
        return -1;
    }

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RTSP_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(sock);
        return -1;
    }

    // 开始监听
    if (listen(sock, RTSP_LISTEN_BACKLOG) < 0)
    {
        perror("listen");
        close(sock);
        return -1;
    }

    return sock;
}

/**
 * @brief 创建 RTP 数据套接字
 * @return 成功返回套接字描述符，失败返回 -1
 */
static int CreateRtpSocket(void)
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
    {
        perror("socket RTP");
        return -1;
    }

    struct sockaddr_in rtpAddr;
    memset(&rtpAddr, 0, sizeof(rtpAddr));
    rtpAddr.sin_family = AF_INET;
    rtpAddr.sin_port = htons(RTP_PORT);
    rtpAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr*)&rtpAddr, sizeof(rtpAddr)) < 0)
    {
        perror("bind RTP");
        close(sock);
        return -1;
    }

    LOG("RTP socket bound to port %d\n", RTP_PORT);
    return sock;
}

/* ==================== RTSP 请求解析函数 ==================== */

/**
 * @brief 解析 RTSP 请求方法
 * @param buffer RTSP 请求缓冲区
 * @return RTSP 方法枚举值，未知方法返回 RTSP_METHOD_UNKNOWN
 */
static RtspMethod_t ParseRtspMethod(const char* buffer)
{
    if (NULL == buffer)
    {
        return RTSP_METHOD_UNKNOWN;
    }

    // RTSP 请求格式：METHOD rtsp://... RTSP/1.0\r\n
    // 提取第一行的第一个单词（方法名）
    if (0 == strncmp(buffer, "OPTIONS ", 8))
    {
        return RTSP_METHOD_OPTIONS;
    }
    else if (0 == strncmp(buffer, "DESCRIBE ", 9))
    {
        return RTSP_METHOD_DESCRIBE;
    }
    else if (0 == strncmp(buffer, "SETUP ", 6))
    {
        return RTSP_METHOD_SETUP;
    }
    else if (0 == strncmp(buffer, "PLAY ", 5))
    {
        return RTSP_METHOD_PLAY;
    }
    else if (0 == strncmp(buffer, "TEARDOWN ", 9))
    {
        return RTSP_METHOD_TEARDOWN;
    }

    return RTSP_METHOD_UNKNOWN;
}

/**
 * @brief 解析 RTSP 请求中的 CSeq 字段
 * @param buffer RTSP 请求缓冲区
 * @return CSeq 值，未找到返回 0
 */
static int ParseCseq(const char* buffer)
{
    if (NULL == buffer)
    {
        return 0;
    }

    char* cseqStart = strstr(buffer, "CSeq:");
    if (NULL != cseqStart)
    {
        int cseq = 0;
        sscanf(cseqStart, "CSeq: %d", &cseq);
        return cseq;
    }
    return 0;
}

/* ==================== RTSP 方法处理函数 ==================== */

/**
 * @brief 处理 OPTIONS 请求
 * @param clientSock 客户端套接字
 * @param cseq 请求序列号
 */
static void HandleOptions(int clientSock, int cseq)
{
    char response[512];
    snprintf(response, sizeof(response),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Data: RTSP Server\r\n"
            "Public: OPTIONS,DESCRIBE,SETUP,PLAY,TEARDOWN\r\n"
            "\r\n", cseq);
    send(clientSock, response, strlen(response), 0);
    LOG(">>>>>>>>>> response:\r\n%s\n", response);
}

/**
 * @brief 处理 DESCRIBE 请求，返回 SDP 描述
 * @param clientSock 客户端套接字
 * @param cseq 请求序列号
 */
static void HandleDescribe(int clientSock, int cseq)
{
    // 生成 SDP 描述体
    char sdpBody[512];
    int sdpLen = snprintf(sdpBody, sizeof(sdpBody),
            "v=0\r\n"
            "o=- 0 0 IN IP4 127.0.0.1\r\n"
            "s=H.264 Stream\r\n"
            "t=0 0\r\n"
            "m=video 0 RTP/AVP %d\r\n"
            "a=rtpmap:%d H264/90000\r\n"
            "a=fmtp:%d packetization-mode=1\r\n"
            "a=control:streamid=0\r\n",
            H264_PAYLOAD_TYPE, H264_PAYLOAD_TYPE, 
            H264_PAYLOAD_TYPE);

    // 生成 RTSP 响应
    char response[2048];
    snprintf(response, sizeof(response),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Content-Type: application/sdp\r\n"
            "Content-Length: %d\r\n"
            "\r\n"
            "%s",
            cseq, sdpLen, sdpBody);
    send(clientSock, response, strlen(response), 0);
    LOG(">>>>>>>>>> response:\r\n%s\n", response);
}

/**
 * @brief 处理 SETUP 请求，建立 RTP 传输通道
 * @param clientSock 客户端套接字
 * @param cseq 请求序列号
 * @param session 客户端会话信息（clientRtpAddr 应该已经填充）
 * @return 成功返回 0，失败返回 -1
 */
static int HandleSetup(int clientSock, int cseq, ClientSession_t* session)
{
    if (NULL == session)
    {
        return -1;
    }

    // 创建 RTP 套接字（如果尚未创建）
    if (session->rtpSock < 0)
    {
        session->rtpSock = CreateRtpSocket();
        if (session->rtpSock < 0)
        {
            LOG("Failed to create RTP socket\n");
            return -1;
        }
        LOG("RTP socket created: %d\n", session->rtpSock);
    }

    // 生成会话 ID
    pthread_t tid = pthread_self();
    snprintf(session->sessionId, sizeof(session->sessionId), 
            "%lu", (unsigned long)tid);

    // 生成 RTSP 响应
    char response[512];
    snprintf(response, sizeof(response),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Transport: RTP/AVP;unicast;server_port=%d-%d\r\n"
            "Session: %s\r\n"
            "\r\n",
            cseq, RTP_PORT, RTCP_PORT, session->sessionId);
    send(clientSock, response, strlen(response), 0);
    LOG(">>>>>>>>>> response:\r\n%s\n", response);

    return 0;
}

/**
 * @brief 处理 PLAY 请求，开始推送视频流
 * @param clientSock 客户端套接字
 * @param cseq 请求序列号
 * @param session 客户端会话信息
 */
static void HandlePlay(int clientSock, int cseq, ClientSession_t* session)
{
    if (NULL == session)
    {
        return;
    }

    // 发送 PLAY 响应
    char response[512];
    snprintf(response, sizeof(response),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %s\r\n"
            "Range: npt=0.000-\r\n"
            "\r\n", cseq, session->sessionId);
    send(clientSock, response, strlen(response), 0);
    LOG(">>>>>>>>>> response:\r\n%s\n", response);
}

/**
 * @brief 处理 TEARDOWN 请求，结束会话
 * @param clientSock 客户端套接字
 * @param cseq 请求序列号
 * @param session 客户端会话信息
 */
static void HandleTeardown(int clientSock, int cseq, ClientSession_t* session)
{
    if (NULL == session)
    {
        return;
    }

    char response[512];
    snprintf(response, sizeof(response),
            "RTSP/1.0 200 OK\r\n"
            "CSeq: %d\r\n"
            "Session: %s\r\n"
            "\r\n", cseq, session->sessionId);
    send(clientSock, response, strlen(response), 0);
    LOG(">>>>>>>>>> response:\r\n%s\n", response);
}

/* ==================== 视频流发送函数 ==================== */

/**
 * @brief 发送 H.264 视频流
 * @param ctx 服务器上下文
 * @param session 客户端会话信息
 */
static void SendH264Stream(StreamContext_t* streamCtx, ClientSession_t* session)
{
    if (NULL == streamCtx || NULL == session || NULL == streamCtx->h264Data)
    {
        return;
    }

    LOG("Start sending H.264 video stream\n");

    // 初始化流状态
    streamCtx->h264DataPos = 0;
    session->rtpSequence = 0;
    session->rtpTimestamp = 0;
    
    while (streamCtx->h264DataPos < streamCtx->h264DataSize)
    {
        // 查找下一个 NALU 起始位置
        size_t naluStart = FindNextNalu(streamCtx, streamCtx->h264DataPos);
        if (naluStart == streamCtx->h264DataSize)
        {
            // 到达文件末尾，循环播放
            streamCtx->h264DataPos = 0;
            continue;
        }

        // 查找当前 NALU 的结束位置（下一个NALU的起始位置）
        size_t naluEnd = FindNextNalu(streamCtx, naluStart);
        if (naluEnd == streamCtx->h264DataSize)
        {
            // 到达文件末尾，循环播放
            streamCtx->h264DataPos = 0;
            continue;
        }

        size_t naluSize = naluEnd - naluStart;
        
        // 调试 获取 NALU 类型用于日志
        // unsigned char naluType = (streamCtx->h264Data[naluStart] & 0x1F);
        // const char* naluTypeName = "UNKNOWN";
        // switch (naluType)
        // {
        //     case 1: naluTypeName = "Non-IDR"; break;
        //     case 5: naluTypeName = "IDR"; break;
        //     case 6: naluTypeName = "SEI"; break;
        //     case 7: naluTypeName = "SPS"; break;
        //     case 8: naluTypeName = "PPS"; break;
        //     default: naluTypeName = "OTHER"; break;
        // }
        // LOG("Found NALU: type=%d (%s), size=%zu, start=%zu, currentPos=%zu\n", 
        //     naluType, naluTypeName, naluSize, naluStart, streamCtx->h264DataPos);

        // 发送 NALU
        LOG("Sending NALU: start=%zu, size=%zu, rtpSock=%d, clientAddr=%s:%d\n",
            naluStart, naluSize, session->rtpSock,
            inet_ntoa(session->clientRtpAddr.sin_addr),
            ntohs(session->clientRtpAddr.sin_port));
        
        SendNaluRtp(session->rtpSock, &session->clientRtpAddr,
                streamCtx->h264Data + naluStart, naluSize,
                &session->rtpSequence, &session->rtpTimestamp);

        // 控制发送速率（约 25fps，每帧 40ms）
        usleep(40000);

        streamCtx->h264DataPos = naluStart;
    }

    LOG("Finished sending H.264 video stream\n");
}

/* ==================== 客户端处理函数 ==================== */

/**
 * @brief 清理服务器上下文资源
 * @param ctx 服务器上下文
 */
static void CleanupServerContext(StreamContext_t* ctx)
{
    if (NULL != ctx)
    {
        if (NULL != ctx->h264Data)
        {
            free(ctx->h264Data);
            ctx->h264Data = NULL;
        }
        ctx->h264DataSize = 0;
        ctx->h264DataPos = 0;
    }
}

/**
 * @brief 等待并接收 RTSP 请求
 * @param clientSock 客户端套接字
 * @param buffer 接收缓冲区
 * @param bufferSize 缓冲区大小
 * @return 成功返回接收的字节数(>0)，超时或连接关闭返回0，错误返回-1，被信号中断返回-2
 */
static int WaitAndReceiveRtspRequest(int clientSock, char* buffer, size_t bufferSize)
{
    // 使用 select 检查套接字是否可读，并设置超时
    fd_set readFds;
    FD_ZERO(&readFds);
    FD_SET(clientSock, &readFds);

    struct timeval timeout;
    timeout.tv_sec = RTSP_RECV_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    
    int ret = select(clientSock + 1, &readFds, NULL, NULL, &timeout);
    if (ret < 0)
    {
        // select 错误
        if (EINTR == errno)
        {
            // 被信号中断，需要重试
            return -2;
        }
        perror("select");
        return -1;
    }
    else if (0 == ret)
    {
        // 超时，客户端长时间无响应
        LOG("Client timeout, closing connection\n");
        return 0;
    }
    
    // 检查套接字是否可读
    if (!FD_ISSET(clientSock, &readFds))
    {
        return 0;
    }
    
    // 接收 RTSP 请求
    int len = recv(clientSock, buffer, bufferSize - 1, 0);
    if (len <= 0)
    {
        if (0 == len)
        {
            LOG("Client closed connection\n");
        }
        else
        {
            perror("recv");
        }
        return 0;
    }
    buffer[len] = '\0';

    LOG("<<<<<<<<<< request:\r\n%s\n", buffer);
    
    return len;
}

/**
 * @brief 处理单个 RTSP 客户端连接
 * @param arg 客户端处理参数指针
 * @return NULL
 */
static void* HandleClient(void* arg)
{
    if (NULL == arg)
    {
        return NULL;
    }

    ClientHandlerParam_t* param = (ClientHandlerParam_t*)arg;
    int clientSock = param->clientSock;
    StreamContext_t* streamCtx = param->streamCtx;

    char buffer[MAX_REQUEST_SIZE];
    ClientSession_t session = {0};

    session.clientSock = clientSock;
    session.rtpSock = -1;

    pid_t pid = getpid();
    pthread_t tid = pthread_self();
    LOG("Client handler thread started. [PID:%d][TID:%lu] \n", 
        pid, (unsigned long)tid);

    while (1)
    {
        // 等待并接收 RTSP 请求
        int len = WaitAndReceiveRtspRequest(clientSock, buffer, 
                        MAX_REQUEST_SIZE);
        if (len == -2)
        {
            // 被信号中断，继续循环
            continue;
        }
        else if (len <= 0)
        {
            // 超时、连接关闭或错误，退出循环
            break;
        }

        // 解析请求方法和 CSeq
        RtspMethod_t method = ParseRtspMethod(buffer);
        int cseq = ParseCseq(buffer);
        // 根据请求方法分发处理
        switch (method)
        {
            case RTSP_METHOD_OPTIONS:
            {
                HandleOptions(clientSock, cseq);
                break;
            }
            case RTSP_METHOD_DESCRIBE:
            {
                HandleDescribe(clientSock, cseq);
                break;
            }
            case RTSP_METHOD_SETUP:
            {
                if (HandleSetup(clientSock, cseq, &session) < 0)
                {
                    goto cleanup;
                }

                // 从buffer获取客户端端口
                char* portStart = strstr(buffer, "client_port=");
                if (NULL != portStart)
                {
                    portStart += strlen("client_port=");
                    char* portEnd = strchr(portStart, '-');
                    if (NULL != portEnd)
                    {
                        *portEnd = '\0';
                    }
                    session.clientRtpAddr.sin_port = htons(atoi(portStart));
                }
                else
                {
                    LOG("Failed to get client port from buffer\n");
                    goto cleanup;
                }

                // 根据socket获取ip和端口
                // memset(&session.clientRtpAddr, 0, 
                //     sizeof(session.clientRtpAddr));
                // socklen_t addrLen = sizeof(session.clientRtpAddr);
                // if (getpeername(session.rtpSock, 
                //         (struct sockaddr*)&session.clientRtpAddr, 
                //         &addrLen) < 0) 
                // {
                //     perror("getpeername");
                //     goto cleanup;
                // }
                session.clientRtpAddr.sin_family = AF_INET;
                session.clientRtpAddr.sin_addr.s_addr = 
                        inet_addr("192.168.0.102");
                
                break;
            }
            case RTSP_METHOD_PLAY:
            {
                HandlePlay(clientSock, cseq, &session);
                // 开始发送 H.264 视频流
                LOG("PLAY request received, rtpSock=%d, streamCtx=%p, h264Data=%p\n",
                    session.rtpSock, streamCtx, 
                    (streamCtx != NULL) ? streamCtx->h264Data : NULL);
                LOG("Client RTP address: %s:%d\n",
                    inet_ntoa(session.clientRtpAddr.sin_addr),
                    ntohs(session.clientRtpAddr.sin_port));
                
                if (session.rtpSock >= 0 && NULL != streamCtx && 
                        NULL != streamCtx->h264Data)
                {
                    SendH264Stream(streamCtx, &session);
                }
                else
                {
                    LOG("Cannot send stream: rtpSock=%d, streamCtx=%p, h264Data=%p\n",
                        session.rtpSock, streamCtx,
                        (streamCtx != NULL) ? streamCtx->h264Data : NULL);
                }
                break;
            }
            case RTSP_METHOD_TEARDOWN:
            {
                HandleTeardown(clientSock, cseq, &session);
                goto cleanup;
            }
            default:
            {
                LOG("Unknown RTSP method\n");
                break;
            }
        }
    }

cleanup:
    // 清理资源
    close(clientSock);
    if (session.rtpSock >= 0)
    {
        close(session.rtpSock);
    }

    LOG("[PID:%d][TID:%lu] Client handler thread ended\n", 
        pid, (unsigned long)tid);
    return NULL;
}

/* ==================== 主函数 ==================== */

/**
 * @brief 程序入口：启动 RTSP 服务器并等待客户端连接
 * @return 成功返回 0，失败返回 1
 */
int main(void)
{
    StreamContext_t streamCtx = {0};

    // 加载 H.264 文件
    if (LoadH264File(&streamCtx, H264_FILE_PATH) < 0)
    {
        LOG_ERR("Failed to load %s file\n", H264_FILE_PATH);
        return -1;
    }

    LOG("Main process PID: %d\n", getpid());

    // 创建 RTSP 监听套接字
    int rtspSock = CreateRtspSocket();
    if (rtspSock < 0)
    {
        LOG_ERR("Failed to create RTSP socket\n");
        CleanupServerContext(&streamCtx);
        return -1;
    }

    LOG("RTSP server listening on port %d\n", RTSP_PORT);
    LOG("RTSP client connection URL: rtsp://localhost:%d/\n", RTSP_PORT);

    // 主循环：接受客户端连接
    while (1)
    {
        struct sockaddr_in clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int client = accept(rtspSock, (struct sockaddr*)&clientAddr, 
            &clientLen);
        if (client < 0)
        {
            perror("accept");
            usleep(100000); // 休眠100ms
            continue;
        }

        LOG("Client connected: %s:%d\n", inet_ntoa(clientAddr.sin_addr), 
            ntohs(clientAddr.sin_port));

        // 为每个客户端创建独立线程
        ClientHandlerParam_t* param = 
            (ClientHandlerParam_t*)malloc(sizeof(ClientHandlerParam_t));
        if (NULL == param)
        {
            perror("malloc");
            close(client);
            continue;
        }

        param->clientSock = client;
        param->streamCtx = &streamCtx;

        pthread_t thread;
        if (pthread_create(&thread, NULL, HandleClient, param) != 0)
        {
            perror("pthread_create");
            close(client);
            free(param);
        }
        else
        {
            pthread_detach(thread);
        }
    }

    // 清理资源
    close(rtspSock);
    CleanupServerContext(&streamCtx);
    return 0;
}