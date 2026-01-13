/**
 * @file h265_parser_sample.c
 * @brief H.265 码流解析示例程序
 * 
 * 演示如何使用 h265_parser 解析码流文件并通过 ringbuf 进行数据传输
 * 
 * 功能：
 * 1. 一个线程：使用 h265_parser 解析码流文件，将帧推送到 ringbuf
 * 2. 另一个线程：从 ringbuf 中读取帧，并保存到输出文件
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/prctl.h>
#include "../h265_parser/h265_parser.h"
#include "../ringbuf/ringbuf.h"
#include "../log.h"

// 全局变量
static RingBuffer_t *g_ringbuf = NULL;
static H265Parser_t *g_parser = NULL;
static pthread_t g_parse_thread = 0;
static pthread_t g_write_thread = 0;
static int g_running = 0;
static FILE *g_output_file = NULL;

// 线程参数结构体
typedef struct {
    const char *input_file;
    const char *output_file;
    size_t ringbuf_size;
    int loop_enabled;
} ThreadParams_t;

/**
 * @brief 推送帧数据到 ringbuf
 * 
 * @param rb ringbuf 指针
 * @param frameData 帧数据指针
 * @param frameSize 帧大小
 * @return 成功返回0，失败返回-1
 */
static int PushFrameToRingbuf(RingBuffer_t *rb, const unsigned char *frameData, size_t frameSize)
{
    if (rb == NULL || frameData == NULL || frameSize == 0)
    {
        return -1;
    }

    // 先推送4字节的长度字段
    unsigned int frameSizeTmp = (unsigned int)frameSize;
    int ret = RingBufferPush(rb, &frameSizeTmp, sizeof(frameSizeTmp));
    if (ret < 0)
    {
        LOG_ERR("[H265ParserThread] Failed to push frame size to ringbuf\n");
        return -1;
    }

    // 再推送帧数据
    ret = RingBufferPush(rb, frameData, frameSize);
    if (ret < 0)
    {
        LOG_ERR("[H265ParserThread] Failed to push frame data to ringbuf\n");
        return -1;
    }

    LOG_DEBUG("[H265ParserThread] Frame pushed: size=%zu, ringbuf usage: %zu/%zu\n",
        frameSize, RingBufferGetUsedSize(rb), RingBufferGetUsedSize(rb) + RingBufferGetFreeSize(rb));

    return 0;
}

/**
 * @brief 解析线程函数
 * 
 * 使用 h265_parser 解析码流文件，将帧推送到 ringbuf
 * 
 * @param arg 线程参数（ThreadParams_t*）
 * @return NULL
 */
static void* ParseThread(void *arg)
{
    ThreadParams_t *params = (ThreadParams_t *)arg;
    int ret = prctl(PR_SET_NAME, "H265Parser", 0, 0, 0);
    if (ret != 0)
    {
        LOG_WARN("[H265ParserThread] Failed to set thread name\n");
    }

    LOG_INFO("[H265ParserThread] Thread started, parsing H.265 file: %s\n", params->input_file);

    // 创建 H265 解析器
    g_parser = H265ParserCreate(params->input_file, 0, params->loop_enabled);
    if (g_parser == NULL)
    {
        LOG_ERR("[H265ParserThread] Failed to create H.265 parser\n");
        g_running = 0;
        return NULL;
    }

    // 分配帧缓冲区（用于拷贝帧数据，避免内部缓冲区被覆盖）
    unsigned char *frameBuf = (unsigned char *)malloc(2 * 1024 * 1024); // 2MB
    if (frameBuf == NULL)
    {
        LOG_ERR("[H265ParserThread] Failed to allocate frame buffer\n");
        H265ParserDestroy(g_parser);
        g_parser = NULL;
        g_running = 0;
        return NULL;
    }

    H265Frame_t frame;
    int frame_count = 0;

    // 循环解析帧
    while (g_running)
    {
        // 获取下一帧
        int result = H265ParserNextFrame(g_parser, &frame);
        if (result < 0)
        {
            LOG_ERR("[H265ParserThread] Failed to get next frame\n");
            break;
        }
        else if (result > 0)
        {
            // EOF（仅在 loop_enabled=0 时）
            LOG_INFO("[H265ParserThread] End of file reached, total frames: %d\n", frame_count);
            break;
        }

        // 检查帧大小
        if (frame.size > 2 * 1024 * 1024)
        {
            LOG_ERR("[H265ParserThread] Frame too large: %zu bytes\n", frame.size);
            continue;
        }
        
        // 立即拷贝帧数据到缓冲区（因为 frame.data 指向内部缓冲区，可能被覆盖）
        memcpy(frameBuf, frame.data, frame.size);
        
        // 推送到 ringbuf（使用拷贝后的数据）
        if (PushFrameToRingbuf(g_ringbuf, frameBuf, frame.size) < 0)
        {
            LOG_ERR("[H265ParserThread] Failed to push frame to ringbuf, stopping\n");
            break;
        }

        frame_count++;
        if (frame_count % 10000 == 0)
        {
            LOG_INFO("[H265ParserThread] Parsed %d H.265 frames\n", frame_count);
        }
    }

    free(frameBuf);
    H265ParserDestroy(g_parser);
    g_parser = NULL;

    LOG_INFO("[H265ParserThread] Thread exited, total H.265 frames parsed: %d\n", frame_count);
    return NULL;
}

/**
 * @brief 写入线程函数
 * 
 * 从 ringbuf 中读取帧，并保存到输出文件
 * 
 * @param arg 线程参数（ThreadParams_t*）
 * @return NULL
 */
static void* WriteThread(void *arg)
{
    ThreadParams_t *params = (ThreadParams_t *)arg;
    int ret = prctl(PR_SET_NAME, "FrameWriter", 0, 0, 0);
    if (ret != 0)
    {
        LOG_WARN("[WriteThread] Failed to set thread name\n");
    }

    LOG_INFO("[WriteThread] Thread started, writing to file: %s\n", params->output_file);

    // 打开输出文件
    g_output_file = fopen(params->output_file, "wb");
    if (g_output_file == NULL)
    {
        LOG_ERR("[WriteThread] Failed to open output file: %s\n", params->output_file);
        g_running = 0;
        return NULL;
    }

    // 分配缓冲区
    unsigned char *frameBuf = (unsigned char *)malloc(2 * 1024 * 1024); // 2MB
    unsigned char *sizeBuf = (unsigned char *)malloc(sizeof(unsigned int));
    if (frameBuf == NULL || sizeBuf == NULL)
    {
        LOG_ERR("[WriteThread] Failed to allocate buffer\n");
        fclose(g_output_file);
        g_output_file = NULL;
        g_running = 0;
        return NULL;
    }

    int frame_count = 0;

    // 循环读取帧
    while (g_running || RingBufferGetUsedSize(g_ringbuf) > 0)
    {
        // 先读取4字节的长度字段
        int result = RingBufferPop(g_ringbuf, sizeBuf, sizeof(unsigned int), 100);
        if (result < 0)
        {
            // ringbuf 已关闭
            LOG_DEBUG("[WriteThread] Ringbuf closed\n");
            break;
        }
        else if (result > 0)
        {
            // 超时，继续等待
            continue;
        }

        // 读取帧大小
        unsigned int frameSize = *(unsigned int *)sizeBuf;
        if (frameSize == 0 || frameSize > 2 * 1024 * 1024)
        {
            LOG_ERR("[WriteThread] Invalid frame size: %u bytes\n", frameSize);
            continue;
        }

        // 读取帧数据（指定确切长度）
        result = RingBufferPop(g_ringbuf, frameBuf, (size_t)frameSize, 100);
        if (result < 0)
        {
            // ringbuf 已关闭
            LOG_DEBUG("[WriteThread] Ringbuf closed while reading frame data\n");
            break;
        }
        else if (result > 0)
        {
            // 超时
            LOG_WARN("[WriteThread] Timeout waiting for frame data\n");
            continue;
        }

        // 写入文件
        size_t written = fwrite(frameBuf, 1, frameSize, g_output_file);
        if (written != (size_t)frameSize)
        {
            LOG_ERR("[WriteThread] Failed to write frame to file, expected %u bytes, wrote %zu bytes\n",
                frameSize, written);
            break;
        }

        fflush(g_output_file);

        frame_count++;
        if (frame_count % 10000 == 0)
        {
            LOG_INFO("[WriteThread] Wrote %d frames\n", frame_count);
        }
    }

    free(frameBuf);
    free(sizeBuf);
    fclose(g_output_file);
    g_output_file = NULL;

    LOG_INFO("[WriteThread] Thread exited, total frames written: %d\n", frame_count);
    return NULL;
}

/**
 * @brief 信号处理函数
 * 
 * @param sig 信号编号
 */
static void SignalHandler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
    {
        LOG_INFO("[Main] Received signal %d, shutting down...\n", sig);
        g_running = 0;
        if (g_ringbuf != NULL)
        {
            RingBufferClose(g_ringbuf);
        }
    }
}

/**
 * @brief 主函数
 * 
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 0=成功，-1=失败
 */
int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        printf("Usage: %s <input_file> <output_file> [ringbuf_size] [loop_enabled] [debug]\n", argv[0]);
        printf("  input_file:    H.265 stream file path\n");
        printf("  output_file:   Output file path\n");
        printf("  ringbuf_size:  Ring buffer size in bytes (default: 2MB)\n");
        printf("  loop_enabled:  Enable loop reading (1=enabled, 0=disabled, default: 0)\n");
        printf("  debug:         Enable debug log (pass 'debug' to enable, default: disabled)\n");
        return -1;
    }

    const char *input_file = argv[1];
    const char *output_file = argv[2];
    size_t ringbuf_size = (argc > 3) ? (size_t)atoi(argv[3]) : (2 * 1024 * 1024); // 默认2MB
    int loop_enabled = (argc > 4) ? atoi(argv[4]) : 0;
    int debug_enabled = 0;

    if (ringbuf_size == 0)
    {
        ringbuf_size = 2 * 1024 * 1024; // 默认2MB
    }

    // 检查是否有 debug 参数
    for (int i = 3; i < argc; i++)
    {
        if (strcmp(argv[i], "debug") == 0)
        {
            debug_enabled = 1;
            break;
        }
    }

    // 开启/关闭 DEBUG 日志
    SetLogDebugEnabled(debug_enabled);

    // 注册信号处理
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);

    LOG_INFO("[Main] Starting H.265 file parser sample\n");
    LOG_INFO("[Main] Input file: %s\n", input_file);
    LOG_INFO("[Main] Output file: %s\n", output_file);
    LOG_INFO("[Main] Ringbuf size: %zu bytes\n", ringbuf_size);
    LOG_INFO("[Main] Loop enabled: %d\n", loop_enabled);
    LOG_INFO("[Main] Debug enabled: %d\n", debug_enabled);

    // 创建 ringbuf
    g_ringbuf = RingBufferCreate(ringbuf_size);
    if (g_ringbuf == NULL)
    {
        LOG_ERR("[Main] Failed to create ringbuf\n");
        return -1;
    }

    // 准备线程参数
    ThreadParams_t params;
    params.input_file = input_file;
    params.output_file = output_file;
    params.ringbuf_size = ringbuf_size;
    params.loop_enabled = loop_enabled;

    // 启动线程
    g_running = 1;

    if (pthread_create(&g_parse_thread, NULL, ParseThread, &params) != 0)
    {
        LOG_ERR("[Main] Failed to create parse thread\n");
        RingBufferDestroy(g_ringbuf);
        g_ringbuf = NULL;
        return -1;
    }

    if (pthread_create(&g_write_thread, NULL, WriteThread, &params) != 0)
    {
        LOG_ERR("[Main] Failed to create write thread\n");
        g_running = 0;
        RingBufferClose(g_ringbuf);
        pthread_join(g_parse_thread, NULL);
        RingBufferDestroy(g_ringbuf);
        g_ringbuf = NULL;
        return -1;
    }

    // 等待线程结束
    pthread_join(g_parse_thread, NULL);
    pthread_join(g_write_thread, NULL);

    // 清理资源
    if (g_ringbuf != NULL)
    {
        RingBufferDestroy(g_ringbuf);
        g_ringbuf = NULL;
    }

    LOG_INFO("[Main] Program exited\n");
    return 0;
}

