#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <pthread.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#define closesocket close
#define SOCKET int
#define INVALID_SOCKET (-1)
#endif

#include "client.h"
#include "rtp.h"
#include "log.h"

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

volatile sig_atomic_t running_flag = TRUE;

/**
 * @brief 从SETUP响应中解析Session ID
 * @param response RTSP响应字符串
 * @param session_id 输出参数，用于存储解析得到的Session ID
 * @return 成功返回0，失败返回-1
 */
int rtsp_get_session_id(const char *response, char *session_id)
{
    // 从SETUP响应中提取Session ID
    const char *session_id_str = strstr(response, "Session: ");
    if (session_id_str == NULL)
    {
        LOG_ERR("Failed to extract session ID from SETUP response\n");
        return -1;
    }
    
    session_id_str += strlen("Session: ");
    char *end = strchr(session_id_str, '\n');
    if (end)
    {
        size_t length = end - session_id_str;
        strncpy(session_id, session_id_str, length);
        session_id[length] = '\0';
    }
    else
    {
        LOG_ERR("Failed to extract session ID\n");
        return -1;
    }

    return 0;
}

/**
 * @brief 发送RTSP请求报文到服务器
 * @param ctx RTSP客户端上下文指针
 * @return 成功返回0，失败返回-1
 */
int rtsp_request(rtsp_client_t *ctx)
{
    LOG_INFO(">>>>>>>>>> request:\r\n");
    LOG_INFO("%s", ctx->rtsp_send_buf);

    int ret = send(ctx->rtsp_fd, ctx->rtsp_send_buf
                , strlen(ctx->rtsp_send_buf), 0);
    if (ret < 0)
    {
        LOG_ERR("Send failed. ret:%d, fd:%d, len:%zu. \n"
            , ret
            , ctx->rtsp_fd
            , strlen(ctx->rtsp_send_buf));
        return -1;
    }
    return 0;
}

/**
 * @brief 接收RTSP服务器响应报文
 * @param ctx RTSP客户端上下文指针
 * @return 成功返回0，失败返回-1
 */
int rtsp_receive(rtsp_client_t *ctx)
{
    int recv_len = recv(ctx->rtsp_fd
                        , ctx->rtsp_recv_buf
                        , sizeof(ctx->rtsp_recv_buf) - 1
                        , 0);
    if (recv_len > 0)
    {
        ctx->rtsp_recv_buf[recv_len] = '\0';
        LOG_INFO(">>>>>>>>>> response:\r\n");
        LOG_INFO("%s", ctx->rtsp_recv_buf);
        return 0;
    }
    else
    {
        LOG_ERR("Receive failed\n");
        return -1;
    }
}

/**
 * @brief 创建RTSP客户端上下文并连接到服务器
 * 
 * 该函数会：
 * 1. 初始化Winsock（Windows平台）
 * 2. 创建TCP套接字
 * 3. 连接到RTSP服务器
 * 
 * @param ctx 输出参数，返回创建的RTSP客户端上下文指针
 * @return 成功返回0，失败返回-1
 */
int rtsp_create(void **ctx)
{
    int ret = 0;
    rtsp_client_t *rtsp_ctx = NULL;

#ifdef _WIN32
    WSADATA wsaData;
#endif

    if (ctx == NULL)
    {
        LOG_ERR("ctx == null\n");
        return -1;
    }

    rtsp_ctx = (rtsp_client_t *)malloc(sizeof(rtsp_client_t));
    if (rtsp_ctx == NULL)
    {
        LOG_ERR("malloc failed\n");
        return -1;
    }
    memset(rtsp_ctx, 0, sizeof(rtsp_client_t));

#ifdef _WIN32
    // 初始化Winsock
    ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        LOG_ERR("WSAStartup failed\n");
        return -1;
    }
    rtsp_ctx->rtsp_wsa_flag = 1;
#endif

    // 创建TCP套接字
    rtsp_ctx->rtsp_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if ((SOCKET)rtsp_ctx->rtsp_fd == INVALID_SOCKET)
    {
        LOG_ERR("Socket creation failed\n");
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    // 设置服务器地址
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(RTSP_SERVER_PORT);
    inet_pton(AF_INET, RTSP_SERVER_IP, &server_addr.sin_addr);

    // 连接RTSP服务器
    ret = connect(rtsp_ctx->rtsp_fd
        , (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret < 0)
    {
        LOG_ERR("Connect failed. ret:%d.\n", ret);
        closesocket(rtsp_ctx->rtsp_fd);
#ifdef _WIN32
        WSACleanup();
#endif
        return -1;
    }

    LOG_INFO("RTSP create success. rtsp_fd:%d. \n", rtsp_ctx->rtsp_fd);

    *ctx = rtsp_ctx;

    return 0;
}

/**
 * @brief 释放RTSP客户端资源并关闭连接
 * @param ctx RTSP客户端上下文指针
 * @return 成功返回0
 */
int rtsp_destroy(void *ctx)
{
    rtsp_client_t *rtsp_ctx = (rtsp_client_t *)ctx;

    if (rtsp_ctx->rtsp_fd)
    {
        closesocket(rtsp_ctx->rtsp_fd);
    }

#ifdef _WIN32
    if (rtsp_ctx->rtsp_wsa_flag)
    {
        WSACleanup();
        rtsp_ctx->rtsp_wsa_flag = 0;
    }
#endif

    if (rtsp_ctx)
    {
        free(rtsp_ctx);
    }

    LOG_INFO("RTSP destroy success.\n");

    return 0;
}

/**
 * @brief RTSP工作线程主函数
 * 
 * 该线程负责：
 * 1. 执行完整的RTSP协议流程（OPTIONS -> DESCRIBE -> SETUP -> PLAY）
 * 2. 接收和解析RTP/RTCP数据包
 * 3. 定期发送保活请求
 * 4. 优雅关闭连接（TEARDOWN）
 * 
 * @param args RTSP客户端上下文指针
 * @return 成功返回(void*)0，失败返回(void*)-1
 */
void *rtsp_work(void *args)
{
    int ret = 0;
    char str_port0[16];
    char str_port1[16];
    rtsp_client_t *ctx = (rtsp_client_t *)args;
    
    ret = rtp_create((void *)&ctx->rtp_ctx);
    if (ret < 0)
    {
        LOG_ERR("RTP create failed\n");
        return (void *)-1;
    }
    LOG_INFO("RTP create success. rtp_fd[0]:%d, rtp_fd[1]:%d. \n"
        , ctx->rtp_ctx->rtp_fd[0]
        , ctx->rtp_ctx->rtp_fd[1]);

    // 监听端口转换为字符串
    snprintf(str_port0, sizeof(str_port0), "%d"
        , ctx->rtp_ctx->rtp_listen_port[0]);
    snprintf(str_port1, sizeof(str_port1), "%d"
        , ctx->rtp_ctx->rtp_listen_port[1]);

    // 发送RTSP请求
    memset(ctx->rtsp_send_buf, 0, sizeof(ctx->rtsp_send_buf));
    memcpy(ctx->rtsp_send_buf, RTSP_REQUEST_OPTION
            , strlen(RTSP_REQUEST_OPTION));
    if (rtsp_request(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    if (rtsp_receive(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }

    memset(ctx->rtsp_send_buf, 0, sizeof(ctx->rtsp_send_buf));
    memcpy(ctx->rtsp_send_buf, RTSP_REQUEST_DESCRIBE
            , strlen(RTSP_REQUEST_DESCRIBE));
    if (rtsp_request(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    if (rtsp_receive(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }

    memset(ctx->rtsp_send_buf, 0, sizeof(ctx->rtsp_send_buf));
    snprintf(ctx->rtsp_send_buf
            , sizeof(ctx->rtsp_send_buf)
            , RTSP_REQUEST_SETUP_TRACK1
            , str_port0
            , str_port1);
    LOG_INFO("Sending SETUP request (track0) with client_port=%s-%s\n", str_port0, str_port1);
    if (rtsp_request(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    if (rtsp_receive(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    ret = rtsp_get_session_id(ctx->rtsp_recv_buf, ctx->session_id);
    if (ret != 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }

    memset(ctx->rtsp_send_buf, 0, sizeof(ctx->rtsp_send_buf));
    snprintf(ctx->rtsp_send_buf
        , sizeof(ctx->rtsp_send_buf)
        , RTSP_REQUEST_SETUP_TRACK2
        , str_port0
        , str_port1
        , ctx->session_id);
    LOG_INFO("Sending SETUP request (track1) with client_port=%s-%s\n", str_port0, str_port1);
    if (rtsp_request(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    if (rtsp_receive(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }

    memset(ctx->rtsp_send_buf, 0, sizeof(ctx->rtsp_send_buf));
    snprintf(ctx->rtsp_send_buf, sizeof(ctx->rtsp_send_buf)
            , RTSP_REQUEST_PLAY, ctx->session_id);
    if (rtsp_request(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    if (rtsp_receive(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    
    time_t last_time = time(NULL);
    while (ctx->thread_status == TRUE)
    {
        ctx->thread_status = running_flag;
        // 超过5秒则发送OPTIONS保活
        if (time(NULL) - last_time > 5)
        {
            if (rtsp_request(ctx) < 0)
            {
                LOG_ERR("RTSP disconnect!\n");
                goto EXIT_FAIL;
            }
            LOG_INFO("RTSP keepalive\n");
            last_time = time(NULL);
        }

        // 默认使用UDP接收RTP/RTCP
        rtp_t *rtp_ctx = ctx->rtp_ctx;
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 10 * 1000;
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(rtp_ctx->rtp_fd[0], &readfds);
        FD_SET(rtp_ctx->rtp_fd[1], &readfds);
#ifdef _WIN32
        int nsel = select(0, &readfds, NULL, NULL, &tv);
#else
        // 使用两个文件描述符中较大的那个 + 1
        int max_fd = (rtp_ctx->rtp_fd[0] > rtp_ctx->rtp_fd[1]) ? 
                     rtp_ctx->rtp_fd[0] : rtp_ctx->rtp_fd[1];
        int nsel = select(max_fd + 1, &readfds, NULL, NULL, &tv);
#endif
        if (nsel < 0)
        {
            LOG_ERR("select failed, errno:%d\n", errno);
            continue;
        }
        
        // 检查 RTP socket
        if ((nsel > 0) && FD_ISSET(rtp_ctx->rtp_fd[0], &readfds))
        {
            int len = recvfrom(rtp_ctx->rtp_fd[0]
                                , rtp_ctx->rtp_recv_buf
                                , rtp_ctx->rtp_recv_len
                                , 0, NULL, NULL);
            if (len > 0)
            {
                static int packet_count = 0;
                packet_count++;
                if (packet_count <= 10 || packet_count % 100 == 0)
                {
                    LOG_INFO("Received RTP packet #%d, len=%d\n", packet_count, len);
                }
                rtp_pkg_parse((void *)rtp_ctx, rtp_ctx->rtp_recv_buf, len);
            }
            else if (len < 0)
            {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINTR)
                {
                    LOG_ERR("RTP recvfrom failed, WSAGetLastError:%d\n", err);
                }
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    LOG_ERR("RTP recvfrom failed, errno:%d\n", errno);
                }
#endif
            }
        }
        
        // 检查 RTCP socket
        if ((nsel > 0) && FD_ISSET(rtp_ctx->rtp_fd[1], &readfds))
        {
            int len = recvfrom(rtp_ctx->rtp_fd[1]
                                , rtp_ctx->rtp_recv_buf
                                , rtp_ctx->rtp_recv_len
                                , 0, NULL, NULL);
            if (len > 0)
            {
                rtcp_pkg_parse(rtp_ctx->rtp_recv_buf, len);
            }
            else if (len < 0)
            {
#ifdef _WIN32
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK && err != WSAEINTR)
                {
                    LOG_ERR("RTCP recvfrom failed, WSAGetLastError:%d\n", err);
                }
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    LOG_ERR("RTCP recvfrom failed, errno:%d\n", errno);
                }
#endif
            }
        }
    }

    memset(ctx->rtsp_send_buf, 0, sizeof(ctx->rtsp_send_buf));
    snprintf(ctx->rtsp_send_buf, sizeof(ctx->rtsp_send_buf)
            , RTSP_REQUEST_TEARDOWN, ctx->session_id);
    if (rtsp_request(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    if (rtsp_receive(ctx) < 0)
    {
        goto EXIT_FAIL;
        return (void *)-1;
    }
    
    rtp_destroy(ctx->rtp_ctx);
    return (void *)0;

EXIT_FAIL:
    rtp_destroy(ctx->rtp_ctx);
    return (void *)-1;
}

/**
 * @brief SIGINT信号处理函数
 * 
 * 当用户按下Ctrl+C时，设置运行标志为FALSE，通知工作线程退出
 * @param sig 信号编号（未使用）
 */
void handle_sigint(int sig)
{
    (void)sig;  // 未使用的参数，避免警告
    running_flag = FALSE;
}

/**
 * @brief 程序主入口
 * 
 * 创建RTSP客户端，启动工作线程，等待线程结束并清理资源
 * @return 成功返回0，失败返回-1
 */
int main(void)
{
    signal(SIGINT, handle_sigint);
    setvbuf(stdout, NULL, _IONBF, 0);

    int ret = 0;
    rtsp_client_t *ctx = NULL;

    ret = rtsp_create((void **)&ctx);
    if (ret < 0)
    {
        LOG_ERR("RTSP create failed\n");
        return -1;
    }

    // 启动RTSP客户端线程
    ctx->thread_status = TRUE;
    ret = pthread_create(&ctx->thread_id, NULL, rtsp_work, (void *)ctx);
    if (ret != 0)
    {
        LOG_ERR("pthread_create failed\n");
        rtsp_destroy(ctx);
        return -1;
    }

    pthread_join(ctx->thread_id, NULL);

    rtsp_destroy(ctx);

    return 0;
}
