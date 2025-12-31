#include <stdio.h>
#include <string.h>
#include <time.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include "rtsp.h"
#include "rtp.h"

volatile sig_atomic_t running_flag = TRUE;

// 从SETUP响应中解析Session ID
int rtsp_get_session_id(const char *response, char *session_id)
{
    // 从SETUP响应中提取Session ID
    const char *session_id_str = strstr(response, "Session: ");
    if (session_id_str == NULL)
    {
        printf("Failed to extract session ID from SETUP response\n");
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
        printf("Failed to extract session ID\n");
        return -1;
    }

    return 0;
}

// 发送RTSP请求报文
int rtsp_request(rtsp_clinet_t *ctx)
{
    printf("==========Request start==========\n");
    printf("%s", ctx->rtsp_send_buf);
    printf("\n==========end==========\n");
    int ret = send(ctx->rtsp_fd, ctx->rtsp_send_buf
                , strlen(ctx->rtsp_send_buf), 0);
    if (ret < 0)
    {
        printf("Send failed. ret:%d, fd:%d, len:%d. \n"
            , ret
            , ctx->rtsp_fd
            , strlen(ctx->rtsp_send_buf));
        return -1;
    }
    return 0;
}

// 接收RTSP响应报文
int rtsp_receive(rtsp_clinet_t *ctx)
{
    int recv_len = recv(ctx->rtsp_fd
                        , ctx->rtsp_recv_buf
                        , sizeof(ctx->rtsp_recv_buf) - 1
                        , 0);
    if (recv_len > 0)
    {
        ctx->rtsp_recv_buf[recv_len] = '\0';
        printf("==========Response start==========\n");
        printf("%s", ctx->rtsp_recv_buf);
        printf("\n==========end==========\n");
        return 0;
    }
    else
    {
        printf("Receive failed\n");
        return -1;
    }
}

// 创建RTSP客户端上下文并连接服务器
int rtsp_create(void **ctx)
{
    int ret = 0;
    WSADATA wsaData;
    rtsp_clinet_t *rtsp_ctx = NULL;

    if (ctx == NULL)
    {
        printf("ctx == null\n");
        return -1;
    }

    rtsp_ctx = (rtsp_clinet_t *)malloc(sizeof(rtsp_clinet_t));
    if (rtsp_ctx == NULL)
    {
        printf("malloc failed\n");
        return -1;
    }
    memset(rtsp_ctx, 0, sizeof(rtsp_clinet_t));

    // 初始化Winsock
    ret = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (ret != 0)
    {
        printf("WSAStartup failed\n");
        return -1;
    }
    rtsp_ctx->rtsp_wsa_flag = 1;

    // 创建TCP套接字
    rtsp_ctx->rtsp_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (rtsp_ctx->rtsp_fd == INVALID_SOCKET)
    {
        printf("Socket creation failed\n");
        WSACleanup();
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
        printf("Connect failed. ret:%d.\n", ret);
        closesocket(rtsp_ctx->rtsp_fd);
        WSACleanup();
        return -1;
    }

    printf("RTSP create success. rtsp_fd:%d. \n", rtsp_ctx->rtsp_fd);

    *ctx = rtsp_ctx;

    return 0;
}

// 释放RTSP客户端资源
int rtsp_destroy(void *ctx)
{
    rtsp_clinet_t *rtsp_ctx = (rtsp_clinet_t *)ctx;

    if (rtsp_ctx->rtsp_fd)
    {
        closesocket(rtsp_ctx->rtsp_fd);
    }

    if (rtsp_ctx->rtsp_wsa_flag)
    {
        WSACleanup();
        rtsp_ctx->rtsp_wsa_flag = 0;
    }

    if (rtsp_ctx)
    {
        free(rtsp_ctx);
    }

    printf("RTSP destroy success.\n");

    return 0;
}

// RTSP工作线程：完成建连、收包与RTP解析
void * rtsp_work(void *args)
{
    int ret = 0;
    char str_port0[16];
    char str_port1[16];
    rtsp_clinet_t *ctx = (rtsp_clinet_t *)args;
    
    ret = rtp_create((void *)&ctx->rtp_ctx);
    if (ret < 0)
    {
        printf("RTP create failed\n");
        return (void *)-1;
    }

    // 监听端口转换为字符串
    snprintf(str_port0, sizeof(str_port0), "%d"
        , ctx->rtp_ctx->rtp_listen_port[0]);
    snprintf(str_port1, sizeof(str_port1), "%d"
        , ctx->rtp_ctx->rtp_listen_port[1]);

    // 发送RTSP请求
    memset(ctx->rtsp_send_buf, 0, sizeof(ctx->rtsp_send_buf));
    strncpy(ctx->rtsp_send_buf, RTSP_REQUEST_OPTION
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
    strncpy(ctx->rtsp_send_buf, RTSP_REQUEST_DESCRIBE
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
                printf("RTSP disconnect!\n");
                goto EXIT_FAIL;
            }
            printf("RTSP keepalive\n");
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
        int nsel = select(0, &readfds, NULL, NULL, &tv);
        if (nsel > 0 && FD_ISSET(rtp_ctx->rtp_fd[0], &readfds))
        {
            int len = recvfrom(rtp_ctx->rtp_fd[0]
                                , rtp_ctx->rtp_recv_buf
                                , rtp_ctx->rtp_recv_len
                                , 0, NULL, NULL);
            if (len <= 0)
            {
                printf("RTP recv len:%d\n", len);
            }
            rtp_pkg_parse((void *)rtp_ctx, rtp_ctx->rtp_recv_buf, len);
        }
        else if (nsel > 0 && FD_ISSET(rtp_ctx->rtp_fd[1], &readfds))
        {
            int len = recvfrom(rtp_ctx->rtp_fd[1]
                                , rtp_ctx->rtp_recv_buf
                                , rtp_ctx->rtp_recv_len
                                , 0, NULL, NULL);
            if (len <= 0)
            {
                printf("RTCP recv len:%d\n", len);
            }
            rtcp_pkg_parse(rtp_ctx->rtp_recv_buf, len);
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

// SIGINT信号处理：通知线程退出
void handle_sigint(int sig)
{
    running_flag = FALSE;
}

// 程序入口：创建RTSP客户端并启动工作线程
int main(void)
{
    signal(SIGINT, handle_sigint);
    setvbuf(stdout, NULL, _IONBF, 0);

    int ret = 0;
    rtsp_clinet_t *ctx = NULL;

    ret = rtsp_create((void **)&ctx);
    if (ret < 0)
    {
        printf("RTSP create failed\n");
        return -1;
    }

    // 启动RTSP客户端线程
    ctx->thread_status = TRUE;
    ret = pthread_create(&ctx->thread_id, NULL, rtsp_work, (void *)ctx);
    if (ret != 0)
    {
        printf("pthread_create failed\n");
        rtsp_destroy(ctx);
        return -1;
    }

    pthread_join(ctx->thread_id, NULL);

    rtsp_destroy(ctx);

    return 0;
}
