#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#define access _access
#else
#include <unistd.h>
#endif

#include "rtp.h"

#define PLAYER_COMMAND "ffplay -loglevel warning -fflags nobuffer -framedrop -f h264 -i -"

#define RTP_RECV_LEN 5*1024*1024
#define RTP_STREAM_FILENAME "tmp_stream.h264"
// #define RTP_DEBUG_ENABLE  // 打开调试打印

// 绑定RTP/RTCP本地端口并创建UDP套接字
static int rtp_bind_port(rtp_t *rtp_ctx, int port)
{
    int ret = 0;

    // 创建UDP套接字
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET)
    {
        printf("Socket creation failed\n");
        return -1;
    }

    ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&port, sizeof(port));
    if (ret != 0)
    {
        printf("setsockopt failed\n");
        closesocket(sock);
        return -1;
    }

    // 绑定本地端口
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(port);

    ret = bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr));
    if (ret < 0)
    {
        printf("bind failed\n");
        closesocket(sock);
        return -1;
    }

    return sock;
}

// 创建RTP上下文并初始化端口和缓存
int rtp_create(void **ctx)
{
    int ret = 0;
    rtp_t *rtp_ctx = NULL;
    WSADATA wsaData;

    rtp_ctx = (rtp_t*)malloc(sizeof(rtp_t));
    if (rtp_ctx == NULL)
    {
        printf("malloc failed\n");
        return -1;
    }

    // 初始化Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        printf("WSAStartup failed\n");
        return -1;
    }
    rtp_ctx->rtp_wsa_flag = 1;

    for (int port = 1025; port < 65535; port++)
    {
        rtp_ctx->rtp_fd[0] = 0;
        rtp_ctx->rtp_fd[1] = 0;
        rtp_ctx->rtp_listen_port[0] = 0;
        rtp_ctx->rtp_listen_port[1] = 0;

        int rtp_fd0 = rtp_bind_port(rtp_ctx, port);
        if (ret < 0)
        {
            continue;
        }
        
        int rtp_fd1 = rtp_bind_port(rtp_ctx, port + 1);
        if (ret < 0)
        {
            continue;
        }
        
        rtp_ctx->rtp_fd[0] = rtp_fd0;
        rtp_ctx->rtp_fd[1] = rtp_fd1;
        rtp_ctx->rtp_listen_port[0] = port;
        rtp_ctx->rtp_listen_port[1] = port + 1;
        printf("bind port to %d and %d. \n", port, port + 1);
        break;
    }

    rtp_ctx->rtp_recv_len = RTP_RECV_LEN;
    rtp_ctx->rtp_recv_buf = malloc(RTP_RECV_LEN);
    if (rtp_ctx->rtp_recv_buf == NULL)
    {
        printf("malloc failed\n");
        return -1;
    }

    snprintf(rtp_ctx->stream_filename
        , sizeof(rtp_ctx->stream_filename)
        , "%s"
        , RTP_STREAM_FILENAME);

    rtp_ctx->last_frame_type = FRAME_IDR; // 不确保首帧为IDR
    // rtp_ctx->last_frame_type = -1; // 确保首帧为IDR
    rtp_ctx->last_last_frame_type = -1;

    printf("RTP create success.\n");

    if (player_open(&rtp_ctx->player, PLAYER_COMMAND) < 0)
    {
        printf("player_open failed; playback will remain disabled.\n");
    }

    *ctx = rtp_ctx;

    return 0;
}

// 释放RTP上下文并关闭套接字
int rtp_destroy(void *ctx)
{
    rtp_t *rtp_ctx = (rtp_t *)ctx;

    if (rtp_ctx->rtp_recv_buf)
    {
        free(rtp_ctx->rtp_recv_buf);
    }

    rtp_ctx->rtp_listen_port[0] = 0;
    rtp_ctx->rtp_listen_port[1] = 0;

    if (rtp_ctx->rtp_fd[0])
    {
        closesocket(rtp_ctx->rtp_fd[0]);
    }

    if (rtp_ctx->rtp_fd[1])
    {
        closesocket(rtp_ctx->rtp_fd[1]);
    }
    
    player_close(&rtp_ctx->player);
    
    if (rtp_ctx->rtp_wsa_flag)
    {
        WSACleanup();
    }

    if (ctx)
    {
        free(ctx);
    }

    printf("RTP destroy success.\n");

    return 0;
}

#if RTP_DEBUG_ENABLE
// 打印内存内容用于调试RTP包
void rtp_print_memory(const void *addr, size_t size)
{
    const unsigned char *p = (const unsigned char *)addr;
    for (size_t i = 0; i < size; i++)
    {
        // 每16个字节换行
        if (i % 16 == 0 && i != 0)
        {
            printf("\n");
        }
        // 打印每个字节的16进制
        printf("%02x ", p[i]);
    }
    printf("\n");
}
#endif

// 处理接收到的码流数据
int rtp_stream_process(void *ctx, const void *buffer, int len)
{
    rtp_t *rtp_ctx = (rtp_t *)ctx;

#if RTP_DEBUG_ENABLE
    printf("==========start==========\n");
    printf("rtp_stream_process. len:%d.\n", len);
    rtp_print_memory(buffer, len);
    printf("==========done==========\n");
#endif

#ifdef rtp_stream_process_ENABLE
    rtp_ctx->fd = fopen(rtp_ctx->stream_filename, "ab");
    if (rtp_ctx->fd == NULL)
    {
        printf("fopen failed. filename:%s. \n"
            , rtp_ctx->stream_filename);
        return -1;
    }

    fwrite(buffer, 1, len, rtp_ctx->fd);
    fflush(rtp_ctx->fd);
    fclose(rtp_ctx->fd);
#endif

    player_feed(&rtp_ctx->player, buffer, len);

    return 0;
}

#define SLICE_NORMAL    0	// 常规单包
#define SLICE_START     1   // 多包开始
#define SLICE_MID       2   // 多包中包
#define SLICE_END       3   // 多包尾包

// 解析RTP包并根据NALU类型重组H.264码流
int rtp_pkg_parse(void *ctx, void *buffer, int len)
{
    rtp_t *rtp_ctx = (rtp_t *)ctx;
    int offset = 0;
    unsigned int h264_startcode = 0x01000000;

    // 解析RTP头
    rtp_ctx->rtp_hdr = *(RTP_FIXED_HEADER_T*)buffer;

    // 丢弃音频负载
    if (rtp_ctx->rtp_hdr.u7Payload == 97)
    {
        return 0;
    }

    // 当NALU类型为28或29时
    // NALU头复用到FU头中
    rtp_ctx->nalu_hdr = 
        *(NALU_HEADER_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T));

    if (rtp_ctx->nalu_hdr.u5Type <= 23)
    {
        rtp_ctx->nalu_type = NALU_SIGNEL;
    }
    else if (rtp_ctx->nalu_hdr.u5Type >= 24 && rtp_ctx->nalu_hdr.u5Type <= 27)
    {
        rtp_ctx->nalu_type = NALU_MUTIL;
    }
    else if (rtp_ctx->nalu_hdr.u5Type == 28 || rtp_ctx->nalu_hdr.u5Type == 29)
    {
        rtp_ctx->nalu_type = NALU_SLICE;
    }
    else
    {
        printf("NALU type no support. u5Type:%d. \n", rtp_ctx->nalu_hdr.u5Type);
        return -1;
    }

    // printf("**********u5Type:%d**********. \n", rtp_ctx->nalu_hdr.u5Type);
    if (rtp_ctx->last_frame_type != FRAME_IDR)
    {
        // 丢弃非IDR起始帧以保证首帧为IDR
        if (rtp_ctx->nalu_hdr.u5Type == FRAME_SPS)
        {
            rtp_ctx->last_frame_type      = FRAME_SPS;
        }
        else if (rtp_ctx->nalu_hdr.u5Type == FRAME_PPS
            && rtp_ctx->last_frame_type     == FRAME_SPS)
        {
            rtp_ctx->last_frame_type      = FRAME_PPS;
            rtp_ctx->last_last_frame_type = FRAME_SPS;
        }
        else if (((rtp_ctx->nalu_hdr.u5Type  == FRAME_IDR)
            || (rtp_ctx->nalu_hdr.u5Type       == FRAME_FU_A))
            && rtp_ctx->last_frame_type        == FRAME_PPS
            && rtp_ctx->last_last_frame_type   == FRAME_SPS)
        {
            rtp_ctx->last_frame_type = FRAME_IDR;
        }
        else
        {
            rtp_ctx->last_frame_type = -1;
            rtp_ctx->last_last_frame_type = -1;
            printf("drop frame. u5Type:%d. \n", rtp_ctx->nalu_hdr.u5Type);
            return 0;
        }
    }

    if (rtp_ctx->nalu_type == NALU_SIGNEL)
    {
        offset = sizeof(RTP_FIXED_HEADER_T);

        // 在NALU头前插入起始码
        memcpy((char *)buffer + offset - sizeof(h264_startcode)
            , &h264_startcode
            , sizeof(h264_startcode));

        // 处理接收到的码流数据
        rtp_stream_process(ctx, (char *)buffer + offset - 4, len - offset + 4);
    }
    else if (rtp_ctx->nalu_type == NALU_MUTIL)
    {
        // TODO：处理NALU_MUTIL
        printf("NALU_MUTIL not support. \n");
    }
    else if (rtp_ctx->nalu_type == NALU_SLICE)
    {
        rtp_ctx->fu_ind = 
            *(FU_INDICATOR_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T));
        rtp_ctx->fu_hdr = 
            *(FU_HEADER_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T) + 1);
        
        int SliceSta;
        if (rtp_ctx->fu_hdr.u1S == 1 && rtp_ctx->fu_hdr.u1E == 0)
        {
            SliceSta = SLICE_START;
        }
        else if (rtp_ctx->fu_hdr.u1S == 0 && rtp_ctx->fu_hdr.u1E == 0)
        {
            SliceSta = SLICE_MID;
        }
        else if (rtp_ctx->fu_hdr.u1S == 0 && rtp_ctx->fu_hdr.u1E == 1)
        {
            SliceSta = SLICE_END;
        }

        offset = sizeof(RTP_FIXED_HEADER_T)
                + sizeof(FU_INDICATOR_T)
                + sizeof(FU_HEADER_T);
        
        if (SliceSta == SLICE_START)
        {
            // 在NALU头前插入起始码
            memcpy((char *)buffer + offset - sizeof(h264_startcode) - sizeof(NALU_HEADER_T)
                , &h264_startcode
                , sizeof(h264_startcode));
            *((char *)buffer + offset - 1) = rtp_ctx->fu_ind.u1F << 7
                                           | rtp_ctx->fu_ind.u2Nri << 5
                                           | (rtp_ctx->fu_hdr.u5Type & 0x1F);
            // 处理接收到的码流数据
            rtp_stream_process(ctx, (char *)buffer + offset - 5, len - offset + 5);
        }
        else if (SliceSta == SLICE_MID)
        {
            // 处理接收到的码流数据
            rtp_stream_process(ctx, (char *)buffer + offset, len - offset);
        }
        else if (SliceSta == SLICE_END)
        {
            // 处理接收到的码流数据
            rtp_stream_process(ctx, (char *)buffer + offset, len - offset);
        }
    }
    return 0;
}

// 解析RTCP包（当前返回成功）
int rtcp_pkg_parse(void *buffer, int len)
{
    return 0;
}
