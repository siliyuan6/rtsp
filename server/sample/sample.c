/**
 * @file sample.c
 * @brief RTSP服务器使用示例
 * 
 * 演示如何使用RTSP服务器API创建H.264流服务器
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include "../api/rtsp_api.h"
#include "../../common/log.h"
#include "../../common/ringbuf/ringbuf.h"

static RTSPHandle_t *g_handle = NULL;
static RingBuffer_t *g_ringbuf = NULL;
static pthread_t g_read_thread = 0;
static int g_thread_running = 0;
static char *g_stream_file = NULL;


/**
 * @brief 信号处理函数
 * 
 * @param sig 信号编号
 */
static void SignalHandler(int sig)
{
	if (sig == SIGINT || sig == SIGTERM)
	{
		LOG("Received signal %d, shutting down...\n", sig);
		g_thread_running = 0;
		if (NULL != g_ringbuf)
		{
			RingBufferClose(g_ringbuf);
		}
		if (NULL != g_handle)
		{
			RTSPDestroy(g_handle);
			g_handle = NULL;
		}
		exit(0);
	}
}

/**
 * @brief 查找H.264起始码
 * 
 * @param data 数据缓冲区
 * @param size 缓冲区大小
 * @param offset 起始偏移量
 * @param startcode_len 输出参数，返回起始码长度（3或4），可为NULL
 * @return 找到起始码返回起始码的开始位置，未找到返回-1
 */
static int FindStartCode(const unsigned char *data, int size, int offset, int *startcode_len)
{
	int i;
	for (i = offset; i < size - 3; i++)
	{
		// 查找 0x00 0x00 0x00 0x01 或 0x00 0x00 0x01
		if (data[i] == 0x00 && data[i+1] == 0x00)
		{
			if (i + 3 < size && data[i+2] == 0x00 && data[i+3] == 0x01)
			{
				if (startcode_len != NULL)
				{
					*startcode_len = 4;
				}
				return i; // 返回起始码的开始位置
			}
			else if (i + 2 < size && data[i+2] == 0x01)
			{
				if (startcode_len != NULL)
				{
					*startcode_len = 3;
				}
				return i; // 返回起始码的开始位置
			}
		}
	}
	return -1;
}

/**
 * @brief 读取码流文件线程函数
 * 
 * 不断读取码流文件，按帧（通过起始码分隔）推送到循环队列
 * 
 * @param arg 线程参数（未使用）
 * @return 线程返回值
 */
static void* StreamReadThread(void *arg)
{
	(void) arg;
	FILE *fp = NULL;
	unsigned char *read_buf = NULL;
	int read_buf_size = 1024 * 1024; // 1MB读取缓冲区
	int frame_buf_size = 512 * 1024; // 512KB帧缓冲区
	unsigned char *frame_buf = NULL;
	int ret = 0;
	int start_pos = 0;
	int next_frame_start = 0;
	int frame_size = 0;

	if (NULL == g_stream_file)
	{
		LOG_ERR("Stream file path is NULL\n");
		return NULL;
	}

	// 打开码流文件
	fp = fopen(g_stream_file, "rb");
	if (NULL == fp)
	{
		LOG_ERR("Failed to open stream file: %s\n", g_stream_file);
		return NULL;
	}

	// 分配读取缓冲区
	read_buf = (unsigned char *)malloc(read_buf_size);
	if (NULL == read_buf)
	{
		LOG_ERR("Failed to allocate read buffer\n");
		fclose(fp);
		return NULL;
	}

	// 分配帧缓冲区
	frame_buf = (unsigned char *)malloc(frame_buf_size);
	if (NULL == frame_buf)
	{
		LOG_ERR("Failed to allocate frame buffer\n");
		free(read_buf);
		fclose(fp);
		return NULL;
	}

	LOG("Stream read thread started, reading from: %s\n", g_stream_file);

	// 读取文件数据到缓冲区
	int data_in_buf = 0;

	while (g_thread_running)
	{
		// 如果缓冲区数据不足，从文件读取更多数据
		if (data_in_buf < 1024)
		{
			int read_size = fread(read_buf + data_in_buf, 1, 
				read_buf_size - data_in_buf, fp);
			if (read_size <= 0)
			{
				// 文件读取完毕，重新开始（循环播放）
				fseek(fp, 0, SEEK_SET);
				data_in_buf = 0;
				start_pos = 0;
				LOG("Stream file read completed, restarting...\n");
				continue;
			}
			data_in_buf += read_size;
		}

		// 查找第一个起始码
		if (start_pos == 0)
		{
			int startcode_len = 0;
			start_pos = FindStartCode(read_buf, data_in_buf, 0, &startcode_len);
			if (start_pos < 0)
			{
				// 未找到起始码，清空缓冲区，继续读取
				data_in_buf = 0;
				start_pos = 0;
				usleep(10000); // 等待10ms
				continue;
			}
			// start_pos 已经是起始码的开始位置，可以直接使用
		}

		// 查找下一个起始码（确定帧边界）
		// 从当前帧的起始码后开始查找
		int search_start = start_pos + 4; // 至少跳过当前起始码
		int next_startcode_len = 0;
		next_frame_start = FindStartCode(read_buf, data_in_buf, search_start, &next_startcode_len);
		if (next_frame_start < 0)
		{
			// 未找到下一个起始码，可能需要读取更多数据
			if (data_in_buf >= read_buf_size - 1024)
			{
				// 缓冲区快满了，将剩余数据作为一帧
				frame_size = data_in_buf - start_pos;
			}
			else
			{
				// 继续读取
				usleep(10000);
				continue;
			}
		}
		else
		{
			// 找到下一个起始码，next_frame_start 已经是起始码的开始位置
			// 计算当前帧的大小（包含起始码）
			frame_size = next_frame_start - start_pos;
		}

		// 检查帧大小是否合理
		if (frame_size <= 0 || frame_size > frame_buf_size)
		{
			LOG_ERR("Invalid frame size: %d\n", frame_size);
			data_in_buf = 0;
			start_pos = 0;
			continue;
		}

		// 复制帧数据到帧缓冲区
		memcpy(frame_buf, read_buf + start_pos, frame_size);

		// 推送到循环队列（先推送长度字段，再推送数据）
		// LOG("Push frame to ring buffer, size: %d\n", frame_size);
		// 先推送4字节的长度字段（网络字节序）
		unsigned int frame_size_net = htonl((unsigned int)frame_size);
		ret = RingBufferPush(g_ringbuf, &frame_size_net, sizeof(frame_size_net));
		if (ret < 0)
		{
			LOG_ERR("Failed to push frame size to ring buffer (closed?)\n");
			break;
		}
		// 再推送帧数据
		ret = RingBufferPush(g_ringbuf, frame_buf, frame_size);
		if (ret < 0)
		{
			LOG_ERR("Failed to push frame data to ring buffer (closed?)\n");
			break;
		}

		// 更新缓冲区状态
		if (next_frame_start > 0)
		{
			// next_frame_start 是下一个帧的起始位置
			// 移动剩余数据到缓冲区开头
			int remaining = data_in_buf - next_frame_start;
			if (remaining > 0)
			{
				memmove(read_buf, read_buf + next_frame_start, remaining);
			}
			data_in_buf = remaining;
			start_pos = 0;
		}
		else
		{
			// 已处理完所有数据
			data_in_buf = 0;
			start_pos = 0;
		}

		// 控制读取速度（根据FPS，这里假设25fps）
		usleep(40000); // 40ms = 25fps
	}

	// 清理资源
	if (read_buf != NULL)
	{
		free(read_buf);
	}
	if (frame_buf != NULL)
	{
		free(frame_buf);
	}
	if (fp != NULL)
	{
		fclose(fp);
	}

	LOG("Stream read thread exited\n");
	return NULL;
}

/**
 * @brief 初始化码流读取功能
 * 
 * 创建循环队列并启动读取线程
 * 
 * @param stream_file 码流文件路径
 * @param ringbuf_size 循环队列大小（字节），默认2MB
 * @return 成功返回0，失败返回-1
 */
static int InitStreamReader(const char *stream_file, size_t ringbuf_size)
{
	int ret = 0;

	if (NULL == stream_file)
	{
		LOG_ERR("Stream file path is NULL\n");
		return -1;
	}

	// 分配文件路径字符串
	g_stream_file = strdup(stream_file);
	if (NULL == g_stream_file)
	{
		LOG_ERR("Failed to allocate memory for stream file path\n");
		return -1;
	}

	// 创建循环队列
	if (ringbuf_size == 0)
	{
		ringbuf_size = 2 * 1024 * 1024; // 默认2MB
	}
	g_ringbuf = RingBufferCreate(ringbuf_size);
	if (NULL == g_ringbuf)
	{
		LOG_ERR("Failed to create ring buffer\n");
		free(g_stream_file);
		g_stream_file = NULL;
		return -1;
	}

	LOG("Ring buffer created, size: %zu bytes\n", ringbuf_size);

	// 启动读取线程
	g_thread_running = 1;
	ret = pthread_create(&g_read_thread, NULL, StreamReadThread, NULL);
	if (ret != 0)
	{
		LOG_ERR("Failed to create read thread\n");
		RingBufferDestroy(g_ringbuf);
		g_ringbuf = NULL;
		free(g_stream_file);
		g_stream_file = NULL;
		g_thread_running = 0;
		return -1;
	}

	LOG("Stream reader initialized successfully\n");
	return 0;
}

/**
 * @brief 清理码流读取功能
 */
static void CleanupStreamReader(void)
{
	// 停止线程
	g_thread_running = 0;
	if (g_ringbuf != NULL)
	{
		RingBufferClose(g_ringbuf);
	}

	// 等待线程结束
	if (g_read_thread != 0)
	{
		pthread_join(g_read_thread, NULL);
		g_read_thread = 0;
	}


	// 销毁循环队列
	if (g_ringbuf != NULL)
	{
		RingBufferDestroy(g_ringbuf);
		g_ringbuf = NULL;
	}

	// 释放文件路径
	if (g_stream_file != NULL)
	{
		free(g_stream_file);
		g_stream_file = NULL;
	}

	LOG("Stream reader cleaned up\n");
}

/**
 * @brief 数据回调函数
 * 
 * 从循环队列中读取H.264帧数据（读取完整一帧）
 * 根据push规则：先读取4字节长度字段，再读取对应长度的帧数据
 * 
 * @param buf 数据缓冲区
 * @param bufSize 缓冲区大小
 * @return 成功返回数据长度，失败返回-1，无数据返回0
 */
static int GetDataCallback(unsigned char *buf, unsigned int bufSize)
{
	size_t actualSize = 0;
	int ret = 0;
	unsigned int frame_size_net = 0;
	unsigned int frame_size = 0;

	if (NULL == buf || bufSize <= 0)
	{
		return -1;
	}

	// 如果循环队列未初始化，返回0（无数据）
	if (NULL == g_ringbuf)
	{
		return 0;
	}

	// 第一步：从ringbuf读取4字节长度字段
	ret = RingBufferPop(g_ringbuf, &frame_size_net, sizeof(frame_size_net), 
		&actualSize, 100);
	if (ret != 0)
	{
		// 超时或错误
		return (ret == 1) ? 0 : -1;
	}
	
	if (actualSize != sizeof(frame_size_net))
	{
		LOG_ERR("Failed to read frame size, got %zu bytes\n", actualSize);
		return -1;
	}
	
	// 转换网络字节序为主机字节序，得到帧长度
	frame_size = ntohl(frame_size_net);
	
	// 检查帧大小是否合理
	if (frame_size == 0 || frame_size > 10 * 1024 * 1024)
	{
		LOG_ERR("Invalid frame size: %u\n", frame_size);
		return -1;
	}
	
	// 检查缓冲区是否足够
	if (bufSize < frame_size)
	{
		LOG_ERR("Buffer too small: need %u bytes, got %u bytes\n", frame_size, bufSize);
		return -1;
	}
	
	// 第二步：从ringbuf读取完整帧数据
	ret = RingBufferPop(g_ringbuf, buf, frame_size, &actualSize, 100);
	if (ret != 0 || actualSize != frame_size)
	{
		LOG_ERR("Failed to read complete frame, expected %u, got %zu\n", 
			frame_size, actualSize);
		return (ret == 1) ? 0 : -1;
	}
	
	// 返回完整帧长度
	return (int)frame_size;
}

/**
 * @brief 主函数
 * 
 * @param argc 参数个数
 * @param argv 参数数组
 * @return 成功返回0，失败返回-1
 */
int main(int argc, char *argv[])
{
	RTSPConfig_t config;
	RTSPStatus_t status;
	int ret = 0;
	const char *stream_file = NULL;

	// 解析命令行参数
	if (argc < 2)
	{
		LOG_ERR("Usage: %s <stream_file>\n", argv[0]);
		return -1;
	}
	stream_file = argv[1];

	// 注册信号处理
	signal(SIGINT, SignalHandler);
	signal(SIGTERM, SignalHandler);

	// 初始化码流读取功能（如果文件存在）
	ret = InitStreamReader(stream_file, 2 * 1024 * 1024); // 2MB队列
	if (ret < 0)
	{
		LOG_ERR("Failed to initialize stream reader\n");
		return -1;
	}
	
	// 配置RTSP服务器
	memset(&config, 0, sizeof(config));
	config.rtspPort = 8554; // RTSP监听端口
	config.rtpPort = 5000;  // RTP端口
	config.format = RTSP_FORMAT_H264; // H.264格式
	config.fps = 25; // 25帧/秒

	LOG("RTSP Server Sample\n");
	LOG("RTSP Port: %d\n", config.rtspPort);
	LOG("RTP Port: %d\n", config.rtpPort);
	LOG("Format: H.264\n");
	LOG("FPS: %d\n", config.fps);

	// 创建RTSP服务器
	ret = RTSPCreate(&g_handle, &config, GetDataCallback);
	if (ret < 0)
	{
		LOG_ERR("RTSPCreate failed\n");
		CleanupStreamReader();
		return -1;
	}

	LOG("RTSP server started successfully\n");
	LOG("You can connect using: rtsp://localhost:%d/live\n",
		config.rtspPort);
	LOG("Press Ctrl+C to stop\n");

	// 主循环：定期检查状态
	while (1)
	{
		sleep(1);

		// 检查状态
		if (NULL != g_handle)
		{
			ret = RTSPGetStatus(g_handle, &status);
			if (ret == 0)
			{
				if (status == RTSP_STATUS_STOPPED)
				{
					LOG("RTSP server stopped\n");
					break;
				}
			}
		}
		else
		{
			break;
		}
	}

	// 清理
	if (NULL != g_handle)
	{
		RTSPDestroy(g_handle);
		g_handle = NULL;
	}

	// 清理码流读取功能
	CleanupStreamReader();

	LOG("RTSP server sample exited\n");
	return 0;
}

