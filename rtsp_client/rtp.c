#include <stdio.h>
#include <string.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "rtp.h"

#define RTP_RECV_LEN 5*1024*1024
#define RTP_STREAM_FILENAME "rtsp_stream.h264"
// #define RTP_DEBUG_ENABLE

static int rtp_bind_port(rtp_t *rtp_ctx, int port)
{
    int ret = 0;

    // Create socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == INVALID_SOCKET) {
        printf("Socket creation failed\n");
        return -1;
    }

    ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&port, sizeof(port));
    if (ret != 0) {
        printf("setsockopt failed\n");
        closesocket(sock);
        return -1;
    }

    // bind socket to local port
    struct sockaddr_in local_addr;
    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(port);

    ret = bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr));
    if (ret < 0) {
        printf("bind failed\n");
        closesocket(sock);
        return -1;
    }

    return sock;
}

int rtp_create(void **ctx)
{
    int ret = 0;
    rtp_t *rtp_ctx = NULL;
    WSADATA wsaData;

    rtp_ctx = (rtp_t*)malloc(sizeof(rtp_t));
    if (rtp_ctx == NULL) {
        printf("malloc failed\n");
        return -1;
    }

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        printf("WSAStartup failed\n");
        return -1;
    }
    rtp_ctx->rtp_wsa_flag = 1;

    for (int port = 1025; port < 65535; port++) {
        rtp_ctx->rtp_fd[0] = 0;
        rtp_ctx->rtp_fd[1] = 0;
        rtp_ctx->rtp_listen_port[0] = 0;
        rtp_ctx->rtp_listen_port[1] = 0;

        int rtp_fd0 = rtp_bind_port(rtp_ctx, port);
        if (ret < 0)
            continue;
        
        int rtp_fd1 = rtp_bind_port(rtp_ctx, port + 1);
        if (ret < 0)
            continue;
        
        rtp_ctx->rtp_fd[0] = rtp_fd0;
        rtp_ctx->rtp_fd[1] = rtp_fd1;
        rtp_ctx->rtp_listen_port[0] = port;
        rtp_ctx->rtp_listen_port[1] = port + 1;
        printf("bind port to %d and %d. \n", port, port + 1);
        break;
    }

    rtp_ctx->rtp_recv_len = RTP_RECV_LEN;
    rtp_ctx->rtp_recv_buf = malloc(RTP_RECV_LEN);
    if (rtp_ctx->rtp_recv_buf == NULL) {
        printf("malloc failed\n");
        return -1;
    }

    snprintf(rtp_ctx->stream_filename
        , sizeof(rtp_ctx->stream_filename)
        , "%s"
        , RTP_STREAM_FILENAME);

    rtp_ctx->last_frame_type = -1;
    rtp_ctx->last_last_frame_type = -1;

    printf("RTP create success.\n");

    *ctx = rtp_ctx;

    return 0;
}

int rtp_destroy(void *ctx)
{
    rtp_t *rtp_ctx = (rtp_t *)ctx;

    if (rtp_ctx->rtp_recv_buf)
        free(rtp_ctx->rtp_recv_buf);

    rtp_ctx->rtp_listen_port[0] = 0;
    rtp_ctx->rtp_listen_port[1] = 0;

    if (rtp_ctx->rtp_fd[0])
        closesocket(rtp_ctx->rtp_fd[0]);

    if (rtp_ctx->rtp_fd[1])
        closesocket(rtp_ctx->rtp_fd[1]);
    
    if (rtp_ctx->rtp_wsa_flag)
        WSACleanup();

    if (ctx)
        free(ctx);

    printf("RTP destroy success.\n");

    return 0;
}

#if RTP_DEBUG_ENABLE
void rtp_print_memory(const void *addr, size_t size) {
    const unsigned char *p = (const unsigned char *)addr;
    for (size_t i = 0; i < size; i++) {
        // 每16个字节换行
        if (i % 16 == 0 && i != 0) {
            printf("\n");
        }
        // 打印每个字节的16进制值
        printf("%02x ", p[i]);
    }
    printf("\n");
}
#endif

int rtp_save_stream(void *ctx, const void *buffer, int len) {
    rtp_t *rtp_ctx = (rtp_t *)ctx;

#if RTP_DEBUG_ENABLE
    printf("==========start==========\n");
    printf("rtp_save_stream. len:%d.\n", len);
    rtp_print_memory(buffer, len);
    printf("==========done==========\n");
#endif

    rtp_ctx->fd = fopen(rtp_ctx->stream_filename, "ab");
    if (rtp_ctx->fd == NULL) {
        printf("fopen failed. filename:%s. \n"
            , rtp_ctx->stream_filename);
        return -1;
    }

    fwrite(buffer, 1, len, rtp_ctx->fd);
    fflush(rtp_ctx->fd);
    fclose(rtp_ctx->fd);

    return 0;
}

#define SLICE_NORMAL    0	// 常规单包
#define SLICE_START     1   // 多包开始
#define SLICE_MID       2   // 多包中包
#define SLICE_END       3   // 多包尾包

int rtp_pkg_parse(void *ctx, void *buffer, int len)
{
    rtp_t *rtp_ctx = (rtp_t *)ctx;
    int offset = 0;
    unsigned int h264_startcode = 0x01000000;

    // parse RTP header
    rtp_ctx->rtp_hdr = *(RTP_FIXED_HEADER_T*)buffer;

    // drop audio frame
    if (rtp_ctx->rtp_hdr.u7Payload == 97)
        return 0;

    // When the NALU type is equal to 28 or 29
    // the nalu hdr is multiplexed into fu hdr
    rtp_ctx->nalu_hdr = 
        *(NALU_HEADER_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T));

    if (rtp_ctx->nalu_hdr.u5Type <= 23)
        rtp_ctx->nalu_type = NALU_SIGNEL;
    else if (rtp_ctx->nalu_hdr.u5Type >= 24 && rtp_ctx->nalu_hdr.u5Type <= 27)
        rtp_ctx->nalu_type = NALU_MUTIL;
    else if (rtp_ctx->nalu_hdr.u5Type == 28 || rtp_ctx->nalu_hdr.u5Type == 29)
        rtp_ctx->nalu_type = NALU_SLICE;
    else {
        printf("NALU type no support. u5Type:%d. \n", rtp_ctx->nalu_hdr.u5Type);
        return -1;
    }

    // printf("**********u5Type:%d**********. \n", rtp_ctx->nalu_hdr.u5Type);
    if (rtp_ctx->last_frame_type != FRAME_IDR) {
        // drop frame ensures that the first frame is IDR-Slice
        if (rtp_ctx->nalu_hdr.u5Type == FRAME_SPS) {
            rtp_ctx->last_frame_type      = FRAME_SPS;
        } else if (rtp_ctx->nalu_hdr.u5Type == FRAME_PPS
            && rtp_ctx->last_frame_type     == FRAME_SPS) {
            rtp_ctx->last_frame_type      = FRAME_PPS;
            rtp_ctx->last_last_frame_type = FRAME_SPS;
        } else if (((rtp_ctx->nalu_hdr.u5Type  == FRAME_IDR)
            || (rtp_ctx->nalu_hdr.u5Type       == FRAME_FU_A))
            && rtp_ctx->last_frame_type        == FRAME_PPS
            && rtp_ctx->last_last_frame_type   == FRAME_SPS) {
            rtp_ctx->last_frame_type = FRAME_IDR;
        } else {
            rtp_ctx->last_frame_type = -1;
            rtp_ctx->last_last_frame_type = -1;
            printf("drop frame. u5Type:%d. \n", rtp_ctx->nalu_hdr.u5Type);
            return 0;
        }
    }

    if (rtp_ctx->nalu_type == NALU_SIGNEL) {
        offset = sizeof(RTP_FIXED_HEADER_T);

        // add 0x00000001 before NALU header
        memcpy((char *)buffer + offset - sizeof(h264_startcode)
            , &h264_startcode
            , sizeof(h264_startcode));

        // save stream to file
        rtp_save_stream(ctx, (char *)buffer + offset - 4, len - offset + 4);
    } else if (rtp_ctx->nalu_type == NALU_MUTIL) {
        // TODO: handle NALU_MUTIL
        printf("NALU_MUTIL not support. \n");
    } else if (rtp_ctx->nalu_type == NALU_SLICE) {
        rtp_ctx->fu_ind = 
            *(FU_INDICATOR_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T));
        rtp_ctx->fu_hdr = 
            *(FU_HEADER_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T) + 1);
        
        int SliceSta;
        if (rtp_ctx->fu_hdr.u1S == 1 && rtp_ctx->fu_hdr.u1E == 0) {
            SliceSta = SLICE_START;
        } else if (rtp_ctx->fu_hdr.u1S == 0 && rtp_ctx->fu_hdr.u1E == 0) {
            SliceSta = SLICE_MID;
        } else if (rtp_ctx->fu_hdr.u1S == 0 && rtp_ctx->fu_hdr.u1E == 1) {
            SliceSta = SLICE_END;
        }

        offset = sizeof(RTP_FIXED_HEADER_T)
                + sizeof(FU_INDICATOR_T)
                + sizeof(FU_HEADER_T);
        
        if (SliceSta == SLICE_START) {
            // add 0x00000001 before NALU header
            memcpy((char *)buffer + offset - sizeof(h264_startcode) - sizeof(NALU_HEADER_T)
                , &h264_startcode
                , sizeof(h264_startcode));
            *((char *)buffer + offset - 1) = rtp_ctx->fu_ind.u1F << 7
                                           | rtp_ctx->fu_ind.u2Nri << 5
                                           | (rtp_ctx->fu_hdr.u5Type & 0x1F);
            // save stream to file
            rtp_save_stream(ctx, (char *)buffer + offset - 5, len - offset + 5);
        } else if (SliceSta == SLICE_MID) {
            // save stream to file
            rtp_save_stream(ctx, (char *)buffer + offset, len - offset);
        } else if (SliceSta == SLICE_END) {
            // save stream to file
            rtp_save_stream(ctx, (char *)buffer + offset, len - offset);
        }
    }
    return 0;
}

int rtcp_pkg_parse(void *buffer, int len)
{
    return 0;
}

