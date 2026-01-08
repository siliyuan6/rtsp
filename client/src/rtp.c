#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <io.h>
#define access _access
#else
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define closesocket close
#define SOCKET int
#define INVALID_SOCKET (-1)
#endif

#include "rtp.h"
#include "log.h"

#define PLAYER_COMMAND "ffplay -loglevel warning -fflags nobuffer -framedrop -f h264 -i -"

#define RTP_RECV_LEN 5*1024*1024
#define RTP_STREAM_FILENAME "tmp_stream.h264"
// #define RTP_DEBUG_ENABLE  // 打开调试打印

#define SLICE_NORMAL    0	// 常规单包
#define SLICE_START     1   // 多包开始
#define SLICE_MID       2   // 多包中包
#define SLICE_END       3   // 多包尾包

/**
 * @brief 创建UDP套接字并绑定到指定端口
 * @param rtp_ctx RTP上下文指针（未使用，保留接口一致性）
 * @param port 要绑定的端口号
 * @return 成功返回套接字文件描述符，失败返回-1
 */
static int rtp_udp_create(int port)
{
    int ret = 0;

    // 创建UDP套接字
    // AF_INET - IPv4 Internet protocols
    // AF_INET6 - IPv6 Internet protocols
    // SOCK_STREAM - 提供面向连接的字节流服务的套接字类型，用于TCP
    // SOCK_DGRAM - 提供数据报服务的套接字类型，用于UDP
    // SOCK_RAW - 提供原始网络协议访问的套接字类型
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if ((SOCKET)sock == INVALID_SOCKET)
    {
        LOG_ERR("Socket creation failed\n");
        return -1;
    }

    // 设置套接字选项，允许地址重用
    // SOL_SOCKET - 套接字级别选项
    // SO_REUSEADDR - 允许重用本地地址和端口
    int reuse = 1;
    ret = setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));
    if (ret != 0)
    {
        LOG_ERR("setsockopt failed\n");
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
        LOG_ERR("bind failed\n");
        closesocket(sock);
        return -1;
    }

    return sock;
}

/**
 * @brief 关闭UDP套接字
 * @param sock 要关闭的套接字文件描述符
 */
static void rtp_udp_destroy(int sock)
{
    if (sock)
    {
        closesocket(sock);
    }
}

/**
 * @brief 创建RTP上下文并初始化RTP/RTCP套接字
 * 
 * 该函数会：
 * 1. 初始化Winsock（Windows平台）
 * 2. 自动查找可用端口并绑定RTP/RTCP套接字
 * 3. 分配接收缓冲区
 * 4. 初始化播放器
 * 
 * @param ctx 输出参数，返回创建的RTP上下文指针
 * @return 成功返回0，失败返回-1
 */
int rtp_create(void **ctx)
{
    rtp_t *rtp_ctx = NULL;

#ifdef _WIN32
    WSADATA wsaData;
#endif

    rtp_ctx = (rtp_t*)malloc(sizeof(rtp_t));
    if (rtp_ctx == NULL)
    {
        LOG_ERR("malloc failed\n");
        return -1;
    }

#ifdef _WIN32
    // 初始化Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        LOG_ERR("WSAStartup failed\n");
        return -1;
    }
    rtp_ctx->rtp_wsa_flag = 1;
#else
    rtp_ctx->rtp_wsa_flag = 0;
#endif

    for (int port = 1025; port < 65535; port++)
    {
        rtp_ctx->rtp_fd[0] = 0;
        rtp_ctx->rtp_fd[1] = 0;
        rtp_ctx->rtp_listen_port[0] = 0;
        rtp_ctx->rtp_listen_port[1] = 0;

        int rtp_fd0 = rtp_udp_create(port);
        if (rtp_fd0 < 0)
        {
            continue;
        }
        
        int rtp_fd1 = rtp_udp_create(port + 1);
        if (rtp_fd1 < 0)
        {
            rtp_udp_destroy(rtp_fd0);
            continue;
        }
        
        rtp_ctx->rtp_fd[0] = rtp_fd0;
        rtp_ctx->rtp_fd[1] = rtp_fd1;
        rtp_ctx->rtp_listen_port[0] = port;
        rtp_ctx->rtp_listen_port[1] = port + 1;
        LOG_INFO("bind port to %d and %d. \n", port, port + 1);
        break;
    }

    rtp_ctx->rtp_recv_len = RTP_RECV_LEN;
    rtp_ctx->rtp_recv_buf = malloc(RTP_RECV_LEN);
    if (rtp_ctx->rtp_recv_buf == NULL)
    {
        LOG_ERR("malloc failed\n");
        return -1;
    }

    snprintf(rtp_ctx->stream_filename
        , sizeof(rtp_ctx->stream_filename)
        , "%s"
        , RTP_STREAM_FILENAME);

    rtp_ctx->last_frame_type = FRAME_IDR; // 不确保首帧为IDR
    // rtp_ctx->last_frame_type = -1; // 确保首帧为IDR
    rtp_ctx->last_last_frame_type = -1;
    rtp_ctx->total_written = 0;

    // 初始化时删除旧文件，重新开始记录
    remove(rtp_ctx->stream_filename);
    LOG_INFO("Stream file will be saved to: %s\n", rtp_ctx->stream_filename);

    LOG_INFO("RTP create success.\n");

    *ctx = rtp_ctx;

    return 0;
}

/**
 * @brief 释放RTP上下文并关闭所有套接字
 * @param ctx RTP上下文指针
 * @return 成功返回0
 */
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
        rtp_udp_destroy(rtp_ctx->rtp_fd[0]);
    }

    if (rtp_ctx->rtp_fd[1])
    {
        rtp_udp_destroy(rtp_ctx->rtp_fd[1]);
    }
    
    if (rtp_ctx->total_written > 0)
    {
        LOG_INFO("Total %zu bytes written to %s\n", rtp_ctx->total_written, rtp_ctx->stream_filename);
    }
    else
    {
        LOG_ERR("No data written to stream file: %s\n", rtp_ctx->stream_filename);
    }
    
#ifdef _WIN32
    if (rtp_ctx->rtp_wsa_flag)
    {
        WSACleanup();
    }
#endif

    if (ctx)
    {
        free(ctx);
    }

    LOG_INFO("RTP destroy success.\n");

    return 0;
}

#ifdef RTP_DEBUG_ENABLE
// 打印内存内容用于调试RTP包
void rtp_print_memory(const void *addr, size_t size)
{
    const unsigned char *p = (const unsigned char *)addr;
    for (size_t i = 0; i < size; i++)
    {
        // 每16个字节换行
        if (i % 16 == 0 && i != 0)
        {
            LOG_INFO("\n");
        }
        // 打印每个字节的16进制
        LOG_INFO("%02x ", p[i]);
    }
    LOG_INFO("\n");
}
#endif

/**
 * @brief 处理接收到的H.264码流数据
 * 
 * 将重组后的NALU单元数据发送给播放器进行播放
 * @param ctx RTP上下文指针
 * @param buffer H.264码流数据缓冲区
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int rtp_stream_process(void *ctx, const void *buffer, int len)
{
    rtp_t *rtp_ctx = (rtp_t *)ctx;

#ifdef RTP_DEBUG_ENABLE
    LOG_INFO("==========start==========\n");
    LOG_INFO("rtp_stream_process. len:%d.\n", len);
    rtp_print_memory(buffer, len);
    LOG_INFO("==========done==========\n");
#endif

    rtp_ctx->fd = fopen(rtp_ctx->stream_filename, "ab");
    if (rtp_ctx->fd == NULL)
    {
        LOG_ERR("fopen failed. filename:%s. \n"
            , rtp_ctx->stream_filename);
        return -1;
    }

    size_t written = fwrite(buffer, 1, len, rtp_ctx->fd);
    if (written != (size_t)len)
    {
        LOG_ERR("fwrite failed. written:%zu, expected:%d\n", written, len);
    }
    else
    {
        rtp_ctx->total_written += written;
    }
    fflush(rtp_ctx->fd);
    fclose(rtp_ctx->fd);
    rtp_ctx->fd = NULL;

    return 0;
}

/**
 * @brief 解析RTP包并根据NALU类型重组H.264码流
 * 
 * 支持三种NALU类型：
 * - NALU_SIGNEL: 单个NALU包
 * - NALU_MUTIL: 多个NALU打包（暂未实现）
 * - NALU_SLICE: FU-A分片NALU包
 * 
 * @param ctx RTP上下文指针
 * @param buffer RTP数据包缓冲区
 * @param len RTP数据包长度
 * @return 成功返回0，失败返回-1
 */
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
        LOG_ERR("NALU type no support. u5Type:%d. \n", rtp_ctx->nalu_hdr.u5Type);
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
            LOG_ERR("drop frame. u5Type:%d, last_frame_type:%d. \n", 
                    rtp_ctx->nalu_hdr.u5Type, rtp_ctx->last_frame_type);
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
        LOG_ERR("NALU_MUTIL not support. \n");
    }
    else if (rtp_ctx->nalu_type == NALU_SLICE)
    {
        rtp_ctx->fu_ind = 
            *(FU_INDICATOR_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T));
        rtp_ctx->fu_hdr = 
            *(FU_HEADER_T*)((char *)buffer + sizeof(RTP_FIXED_HEADER_T) + 1);
        
        int SliceSta = SLICE_NORMAL;  // 初始化为默认值
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
        else
        {
            // 无效的FU-A分片状态，跳过
            LOG_ERR("Invalid FU-A slice state. S:%d E:%d\n", 
                   rtp_ctx->fu_hdr.u1S, rtp_ctx->fu_hdr.u1E);
            return 0;
        }
    }
    return 0;
}

/**
 * @brief 解析RTCP包
 * 
 * 当前为空实现，预留接口用于未来扩展
 * @param buffer RTCP数据包缓冲区
 * @param len RTCP数据包长度
 * @return 成功返回0
 */
int rtcp_pkg_parse(void *buffer, int len)
{
    (void)buffer;  // 未使用的参数，避免警告
    (void)len;     // 未使用的参数，避免警告
    return 0;
}
