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

/* ==================== 常量定义 ==================== */

#define RTSP_PORT 8554          // RTSP 服务端口
#define RTP_PORT 5000           // RTP 数据端口
#define RTCP_PORT 5001          // RTCP 控制端口
#define MAX_REQUEST_SIZE 2048   // RTSP 请求最大长度
#define H264_PAYLOAD_TYPE 96    // H.264 RTP 负载类型
#define MAX_RTP_PACKET_SIZE 1400 // RTP 包最大大小
#define RTP_HEADER_SIZE 12      // RTP 固定头大小
#define H264_FU_HEADER_SIZE 2   // H.264 FU-A 分片头大小
#define H264_FILE_PATH "../resource/704x576_pal_baseLine.h264" // H.264 文件路径
#define FRAME_RATE 25           // 视频帧率（fps）
#define RTP_CLOCK_RATE 90000    // RTP 时钟频率（Hz）
#define TIMESTAMP_INCREMENT (RTP_CLOCK_RATE / FRAME_RATE) // 每帧时间戳增量

/* ==================== 数据结构定义 ==================== */

/**
 * @brief RTP 固定头结构体
 */
typedef struct
{
	unsigned char version:2;      // RTP 版本号（固定为 2）
	unsigned char padding:1;       // 填充标志
	unsigned char extension:1;     // 扩展标志
	unsigned char csrc_count:4;    // CSRC 计数
	unsigned char marker:1;        // 标记位（帧结束标记）
	unsigned char payload_type:7;  // 负载类型
	unsigned short sequence;       // 序列号
	unsigned int timestamp;        // 时间戳
	unsigned int ssrc;             // 同步源标识符
} rtp_header_t;

/**
 * @brief 客户端会话信息结构体
 */
typedef struct
{
	int client_sock;                    // RTSP 控制套接字
	int rtp_sock;                       // RTP 数据套接字
	char session_id[32];                // 会话 ID
	struct sockaddr_in client_rtp_addr; // 客户端 RTP 地址
	unsigned short rtp_sequence;        // RTP 序列号
	unsigned int rtp_timestamp;         // RTP 时间戳
} client_session_t;

/* ==================== 全局变量 ==================== */

static unsigned char* g_h264_data = NULL;    // H.264 文件数据缓冲区
static size_t g_h264_data_size = 0;          // H.264 文件大小
static size_t g_h264_data_pos = 0;           // 当前读取位置

/* ==================== 函数声明 ==================== */

static int load_h264_file(const char* filename);
static size_t find_next_nalu(size_t start_pos);
static void send_single_nalu_rtp(int rtp_sock, struct sockaddr_in* client_addr,
		unsigned char* nalu_data, size_t nalu_size,
		unsigned short* sequence, unsigned int* timestamp);
static void send_fragmented_nalu_rtp(int rtp_sock, struct sockaddr_in* client_addr,
		unsigned char* nalu_data, size_t nalu_size,
		unsigned short* sequence, unsigned int* timestamp);
static void send_nalu_rtp(int rtp_sock, struct sockaddr_in* client_addr,
		unsigned char* nalu_data, size_t nalu_size,
		unsigned short* sequence, unsigned int* timestamp);
static int create_rtsp_socket(void);
static int create_rtp_socket(void);
static int parse_transport_header(const char* buffer, struct sockaddr_in* client_rtp_addr);
static int parse_cseq(const char* buffer);
static void handle_options(int client_sock, int cseq);
static void handle_describe(int client_sock, int cseq);
static int handle_setup(int client_sock, int cseq, client_session_t* session);
static void handle_play(int client_sock, int cseq, client_session_t* session);
static void handle_teardown(int client_sock, int cseq, client_session_t* session);
static void send_h264_stream(client_session_t* session);
static void* handle_client(void* arg);

/* ==================== H.264 文件处理函数 ==================== */

/**
 * @brief 加载 H.264 文件到内存
 * @param filename 文件路径
 * @return 成功返回 0，失败返回 -1
 */
static int load_h264_file(const char* filename)
{
	FILE* fp = fopen(filename, "rb");
	if (fp == NULL)
	{
		perror("fopen");
		return -1;
	}

	// 获取文件大小
	fseek(fp, 0, SEEK_END);
	g_h264_data_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);

	// 分配内存
	g_h264_data = (unsigned char*)malloc(g_h264_data_size);
	if (g_h264_data == NULL)
	{
		fclose(fp);
		return -1;
	}

	// 读取文件内容
	size_t read_size = fread(g_h264_data, 1, g_h264_data_size, fp);
	fclose(fp);

	if (read_size != g_h264_data_size)
	{
		free(g_h264_data);
		g_h264_data = NULL;
		return -1;
	}

	printf("已加载 H.264 文件: %zu 字节\n", g_h264_data_size);
	return 0;
}

/**
 * @brief 查找下一个 NALU 起始码位置
 * @param start_pos 起始搜索位置
 * @return 找到的 NALU 起始位置（跳过起始码），未找到返回文件大小
 * 
 * 支持两种起始码格式：
 * - 0x00000001 (4 字节)
 * - 0x000001 (3 字节)
 */
static size_t find_next_nalu(size_t start_pos)
{
	if (start_pos >= g_h264_data_size)
	{
		return g_h264_data_size;
	}

	for (size_t i = start_pos; i < g_h264_data_size - 3; i++)
	{
		if (g_h264_data[i] == 0 && g_h264_data[i + 1] == 0)
		{
			if (g_h264_data[i + 2] == 1)
			{
				return i + 3; // 找到 0x000001
			}
			else if (i < g_h264_data_size - 4 && 
					g_h264_data[i + 2] == 0 && g_h264_data[i + 3] == 1)
			{
				return i + 4; // 找到 0x00000001
			}
		}
	}
	return g_h264_data_size;
}

/* ==================== RTP 打包和发送函数 ==================== */

/**
 * @brief 发送单个 NALU（不分片）
 * @param rtp_sock RTP 套接字
 * @param client_addr 客户端地址
 * @param nalu_data NALU 数据（不包含起始码）
 * @param nalu_size NALU 数据大小
 * @param sequence RTP 序列号指针（会被更新）
 * @param timestamp RTP 时间戳指针（会被更新）
 */
static void send_single_nalu_rtp(int rtp_sock, struct sockaddr_in* client_addr,
		unsigned char* nalu_data, size_t nalu_size,
		unsigned short* sequence, unsigned int* timestamp)
{
	unsigned char packet[MAX_RTP_PACKET_SIZE];
	rtp_header_t* rtp_hdr = (rtp_header_t*)packet;

	// 填充 RTP 头
	rtp_hdr->version = 2;
	rtp_hdr->padding = 0;
	rtp_hdr->extension = 0;
	rtp_hdr->csrc_count = 0;
	rtp_hdr->marker = ((nalu_data[0] & 0x1F) == 5) ? 1 : 0; // IDR 帧标记
	rtp_hdr->payload_type = H264_PAYLOAD_TYPE;
	rtp_hdr->sequence = htons(*sequence);
	rtp_hdr->timestamp = htonl(*timestamp);
	rtp_hdr->ssrc = htonl(0x12345678);

	// 复制 NALU 数据
	memcpy(packet + RTP_HEADER_SIZE, nalu_data, nalu_size);

	// 发送 RTP 包
	sendto(rtp_sock, packet, RTP_HEADER_SIZE + nalu_size, 0,
			(struct sockaddr*)client_addr, sizeof(*client_addr));

	(*sequence)++;
}

/**
 * @brief 发送分片的 NALU（FU-A 格式）
 * @param rtp_sock RTP 套接字
 * @param client_addr 客户端地址
 * @param nalu_data NALU 数据（不包含起始码）
 * @param nalu_size NALU 数据大小
 * @param sequence RTP 序列号指针（会被更新）
 * @param timestamp RTP 时间戳指针（会被更新）
 */
static void send_fragmented_nalu_rtp(int rtp_sock, struct sockaddr_in* client_addr,
		unsigned char* nalu_data, size_t nalu_size,
		unsigned short* sequence, unsigned int* timestamp)
{
	// 提取 NALU 头信息
	unsigned char nalu_type = nalu_data[0] & 0x1F;
	unsigned char nalu_f = nalu_data[0] & 0x80;
	unsigned char nalu_nri = nalu_data[0] & 0x60;

	size_t offset = 1; // 跳过 NALU 头
	size_t remaining = nalu_size - 1;
	size_t max_payload_size = MAX_RTP_PACKET_SIZE - RTP_HEADER_SIZE - H264_FU_HEADER_SIZE;

	while (remaining > 0)
	{
		unsigned char packet[MAX_RTP_PACKET_SIZE];
		rtp_header_t* rtp_hdr = (rtp_header_t*)packet;

		// 计算当前分片的负载大小
		size_t payload_size = (remaining > max_payload_size) ? max_payload_size : remaining;

		// 填充 RTP 头
		rtp_hdr->version = 2;
		rtp_hdr->padding = 0;
		rtp_hdr->extension = 0;
		rtp_hdr->csrc_count = 0;
		rtp_hdr->marker = (remaining == payload_size) ? 1 : 0; // 最后一个分片标记
		rtp_hdr->payload_type = H264_PAYLOAD_TYPE;
		rtp_hdr->sequence = htons(*sequence);
		rtp_hdr->timestamp = htonl(*timestamp);
		rtp_hdr->ssrc = htonl(0x12345678);

		// FU indicator (F + NRI + Type=28 for FU-A)
		packet[RTP_HEADER_SIZE] = (nalu_f | nalu_nri | 28);

		// FU header (Type + S + E + R)
		packet[RTP_HEADER_SIZE + 1] = nalu_type;
		if (offset == 1)
		{
			packet[RTP_HEADER_SIZE + 1] |= 0x80; // Start bit
		}
		if (remaining == payload_size)
		{
			packet[RTP_HEADER_SIZE + 1] |= 0x40; // End bit
		}

		// 复制 NALU 数据
		memcpy(packet + RTP_HEADER_SIZE + H264_FU_HEADER_SIZE,
				nalu_data + offset, payload_size);

		// 发送 RTP 包
		sendto(rtp_sock, packet, RTP_HEADER_SIZE + H264_FU_HEADER_SIZE + payload_size, 0,
				(struct sockaddr*)client_addr, sizeof(*client_addr));

		offset += payload_size;
		remaining -= payload_size;
		(*sequence)++;
	}
}

/**
 * @brief 发送 NALU（自动选择单包或分片）
 * @param rtp_sock RTP 套接字
 * @param client_addr 客户端地址
 * @param nalu_data NALU 数据（不包含起始码）
 * @param nalu_size NALU 数据大小
 * @param sequence RTP 序列号指针（会被更新）
 * @param timestamp RTP 时间戳指针（会被更新）
 */
static void send_nalu_rtp(int rtp_sock, struct sockaddr_in* client_addr,
		unsigned char* nalu_data, size_t nalu_size,
		unsigned short* sequence, unsigned int* timestamp)
{
	size_t max_single_packet_size = MAX_RTP_PACKET_SIZE - RTP_HEADER_SIZE - 1;

	if (nalu_size <= max_single_packet_size)
	{
		// 单个 RTP 包
		send_single_nalu_rtp(rtp_sock, client_addr, nalu_data, nalu_size, sequence, timestamp);
	}
	else
	{
		// FU-A 分片
		send_fragmented_nalu_rtp(rtp_sock, client_addr, nalu_data, nalu_size, sequence, timestamp);
	}

	// 更新时间戳（每帧递增）
	*timestamp += TIMESTAMP_INCREMENT;
}

/* ==================== 套接字创建函数 ==================== */

/**
 * @brief 创建 RTSP 监听套接字
 * @return 成功返回套接字描述符，失败返回 -1
 */
static int create_rtsp_socket(void)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
	{
		perror("socket");
		return -1;
	}

	// 设置地址重用选项
	int opt = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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
	if (listen(sock, 5) < 0)
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
static int create_rtp_socket(void)
{
	int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
	{
		perror("socket RTP");
		return -1;
	}

	struct sockaddr_in rtp_addr;
	memset(&rtp_addr, 0, sizeof(rtp_addr));
	rtp_addr.sin_family = AF_INET;
	rtp_addr.sin_port = htons(RTP_PORT);
	rtp_addr.sin_addr.s_addr = INADDR_ANY;

	if (bind(sock, (struct sockaddr*)&rtp_addr, sizeof(rtp_addr)) < 0)
	{
		perror("bind RTP");
		close(sock);
		return -1;
	}

	return sock;
}

/* ==================== RTSP 请求解析函数 ==================== */

/**
 * @brief 解析 RTSP 请求中的 CSeq 字段
 * @param buffer RTSP 请求缓冲区
 * @return CSeq 值，未找到返回 0
 */
static int parse_cseq(const char* buffer)
{
	char* cseq_start = strstr(buffer, "CSeq:");
	if (cseq_start != NULL)
	{
		int cseq = 0;
		sscanf(cseq_start, "CSeq: %d", &cseq);
		return cseq;
	}
	return 0;
}

/**
 * @brief 解析 Transport 头，获取客户端 RTP 地址和端口
 * @param buffer RTSP 请求缓冲区
 * @param client_rtp_addr 输出的客户端 RTP 地址结构
 * @return 成功返回 0，失败返回 -1
 */
static int parse_transport_header(const char* buffer, struct sockaddr_in* client_rtp_addr)
{
	char* transport = strstr(buffer, "Transport:");
	if (transport == NULL)
	{
		return -1;
	}

	int client_rtp_port = 0;
	int client_rtcp_port = 0;
	char client_ip[64] = {0};

	// 解析 Transport 头中的端口信息
	if (sscanf(transport, "Transport: RTP/AVP;unicast;client_port=%d-%d",
			&client_rtp_port, &client_rtcp_port) != 2)
	{
		return -1;
	}

	// 尝试从 Host 头获取客户端 IP
	char* host_line = strstr(buffer, "Host:");
	if (host_line != NULL)
	{
		sscanf(host_line, "Host: %63s", client_ip);
		char* colon = strchr(client_ip, ':');
		if (colon != NULL)
		{
			*colon = '\0';
		}
	}
	else
	{
		strcpy(client_ip, "127.0.0.1");
	}

	// 填充客户端地址结构
	memset(client_rtp_addr, 0, sizeof(*client_rtp_addr));
	client_rtp_addr->sin_family = AF_INET;
	client_rtp_addr->sin_port = htons(client_rtp_port);
	client_rtp_addr->sin_addr.s_addr = inet_addr(client_ip);

	printf("客户端 RTP 地址: %s:%d\n", client_ip, client_rtp_port);
	return 0;
}

/* ==================== RTSP 方法处理函数 ==================== */

/**
 * @brief 处理 OPTIONS 请求
 * @param client_sock 客户端套接字
 * @param cseq 请求序列号
 */
static void handle_options(int client_sock, int cseq)
{
	char response[512];
	snprintf(response, sizeof(response),
			"RTSP/1.0 200 OK\r\n"
			"CSeq: %d\r\n"
			"Server: RTSP-Server/1.0\r\n"
			"Public: OPTIONS,DESCRIBE,SETUP,PLAY,TEARDOWN\r\n"
			"\r\n", cseq);
	send(client_sock, response, strlen(response), 0);
}

/**
 * @brief 处理 DESCRIBE 请求，返回 SDP 描述
 * @param client_sock 客户端套接字
 * @param cseq 请求序列号
 */
static void handle_describe(int client_sock, int cseq)
{
	// 生成 SDP 描述体
	char sdp_body[512];
	int sdp_len = snprintf(sdp_body, sizeof(sdp_body),
			"v=0\r\n"
			"o=- 0 0 IN IP4 127.0.0.1\r\n"
			"s=H.264 Stream\r\n"
			"t=0 0\r\n"
			"m=video 0 RTP/AVP %d\r\n"
			"a=rtpmap:%d H264/90000\r\n"
			"a=fmtp:%d packetization-mode=1\r\n"
			"a=control:streamid=0\r\n",
			H264_PAYLOAD_TYPE, H264_PAYLOAD_TYPE, H264_PAYLOAD_TYPE);

	// 生成 RTSP 响应
	char response[2048];
	snprintf(response, sizeof(response),
			"RTSP/1.0 200 OK\r\n"
			"CSeq: %d\r\n"
			"Content-Type: application/sdp\r\n"
			"Content-Length: %d\r\n"
			"\r\n"
			"%s",
			cseq, sdp_len, sdp_body);
	send(client_sock, response, strlen(response), 0);
}

/**
 * @brief 处理 SETUP 请求，建立 RTP 传输通道
 * @param client_sock 客户端套接字
 * @param cseq 请求序列号
 * @param session 客户端会话信息
 * @return 成功返回 0，失败返回 -1
 */
/**
 * @brief 处理 SETUP 请求，建立 RTP 传输通道
 * @param client_sock 客户端套接字
 * @param cseq 请求序列号
 * @param session 客户端会话信息（client_rtp_addr 应该已经填充）
 * @return 成功返回 0，失败返回 -1
 */
static int handle_setup(int client_sock, int cseq, client_session_t* session)
{
	// 创建 RTP 套接字（如果尚未创建）
	if (session->rtp_sock < 0)
	{
		session->rtp_sock = create_rtp_socket();
		if (session->rtp_sock < 0)
		{
			return -1;
		}
	}

	// 生成会话 ID
	pthread_t tid = pthread_self();
	snprintf(session->session_id, sizeof(session->session_id), "%lu", (unsigned long)tid);

	// 生成 RTSP 响应
	char response[512];
	snprintf(response, sizeof(response),
			"RTSP/1.0 200 OK\r\n"
			"CSeq: %d\r\n"
			"Transport: RTP/AVP;unicast;server_port=%d-%d\r\n"
			"Session: %s\r\n"
			"\r\n",
			cseq, RTP_PORT, RTCP_PORT, session->session_id);
	send(client_sock, response, strlen(response), 0);

	return 0;
}

/**
 * @brief 处理 PLAY 请求，开始推送视频流
 * @param client_sock 客户端套接字
 * @param cseq 请求序列号
 * @param session 客户端会话信息
 */
static void handle_play(int client_sock, int cseq, client_session_t* session)
{
	// 发送 PLAY 响应
	char response[512];
	snprintf(response, sizeof(response),
			"RTSP/1.0 200 OK\r\n"
			"CSeq: %d\r\n"
			"Session: %s\r\n"
			"Range: npt=0.000-\r\n"
			"\r\n", cseq, session->session_id);
	send(client_sock, response, strlen(response), 0);

	// 开始发送 H.264 视频流
	if (session->rtp_sock >= 0 && g_h264_data != NULL)
	{
		send_h264_stream(session);
	}
}

/**
 * @brief 处理 TEARDOWN 请求，结束会话
 * @param client_sock 客户端套接字
 * @param cseq 请求序列号
 * @param session 客户端会话信息
 */
static void handle_teardown(int client_sock, int cseq, client_session_t* session)
{
	char response[512];
	snprintf(response, sizeof(response),
			"RTSP/1.0 200 OK\r\n"
			"CSeq: %d\r\n"
			"Session: %s\r\n"
			"\r\n", cseq, session->session_id);
	send(client_sock, response, strlen(response), 0);
}

/* ==================== 视频流发送函数 ==================== */

/**
 * @brief 发送 H.264 视频流
 * @param session 客户端会话信息
 */
static void send_h264_stream(client_session_t* session)
{
	printf("开始发送 H.264 视频流...\n");

	// 初始化流状态
	g_h264_data_pos = 0;
	session->rtp_sequence = 0;
	session->rtp_timestamp = 0;

	while (g_h264_data_pos < g_h264_data_size)
	{
		// 查找下一个 NALU 起始位置
		size_t nalu_start = find_next_nalu(g_h264_data_pos);
		if (nalu_start >= g_h264_data_size)
		{
			// 到达文件末尾，循环播放
			g_h264_data_pos = 0;
			continue;
		}

		// 查找当前 NALU 的结束位置
		size_t nalu_end = find_next_nalu(nalu_start);
		if (nalu_end > nalu_start)
		{
			size_t nalu_size = nalu_end - nalu_start;

			// 发送 NALU
			send_nalu_rtp(session->rtp_sock, &session->client_rtp_addr,
					g_h264_data + nalu_start, nalu_size,
					&session->rtp_sequence, &session->rtp_timestamp);

			// 控制发送速率（约 25fps，每帧 40ms）
			usleep(40000);
		}

		g_h264_data_pos = nalu_end;
	}

	printf("H.264 视频流发送完成\n");
}

/* ==================== 客户端处理函数 ==================== */

/**
 * @brief 处理单个 RTSP 客户端连接
 * @param arg 客户端套接字（转换为 long 再转回 int）
 * @return NULL
 */
static void* handle_client(void* arg)
{
	int client_sock = (int)(long)arg;
	char buffer[MAX_REQUEST_SIZE];
	client_session_t session = {0};

	session.client_sock = client_sock;
	session.rtp_sock = -1;

	pid_t pid = getpid();
	pthread_t tid = pthread_self();
	printf("[PID:%d][TID:%lu] 客户端处理线程启动\n", pid, (unsigned long)tid);

	while (1)
	{
		// 接收 RTSP 请求
		int len = recv(client_sock, buffer, MAX_REQUEST_SIZE - 1, 0);
		if (len <= 0)
		{
			break;
		}
		buffer[len] = '\0';

		// 解析 CSeq
		int cseq = parse_cseq(buffer);

		printf("\r\n<<<<<<<<<< 请求:\r\n%s\n", buffer);

		// 根据请求方法分发处理
		if (strstr(buffer, "OPTIONS") != NULL)
		{
			handle_options(client_sock, cseq);
		}
		else if (strstr(buffer, "DESCRIBE") != NULL)
		{
			handle_describe(client_sock, cseq);
		}
		else if (strstr(buffer, "SETUP") != NULL)
		{
			// 需要从 buffer 中解析 Transport 头
			if (parse_transport_header(buffer, &session.client_rtp_addr) < 0)
			{
				memset(&session.client_rtp_addr, 0, sizeof(session.client_rtp_addr));
				session.client_rtp_addr.sin_family = AF_INET;
				session.client_rtp_addr.sin_port = htons(5000);
				session.client_rtp_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
			}

			if (handle_setup(client_sock, cseq, &session) < 0)
			{
				break;
			}
		}
		else if (strstr(buffer, "PLAY") != NULL)
		{
			handle_play(client_sock, cseq, &session);
		}
		else if (strstr(buffer, "TEARDOWN") != NULL)
		{
			handle_teardown(client_sock, cseq, &session);
			break;
		}
	}

	// 清理资源
	close(client_sock);
	if (session.rtp_sock >= 0)
	{
		close(session.rtp_sock);
	}

	printf("[PID:%d][TID:%lu] 客户端处理线程结束\n", pid, (unsigned long)tid);
	return NULL;
}

/* ==================== 主函数 ==================== */

/**
 * @brief 程序入口：启动 RTSP 服务器并等待客户端连接
 * @return 成功返回 0，失败返回 1
 */
int main(void)
{
	// 加载 H.264 文件
	if (load_h264_file(H264_FILE_PATH) < 0)
	{
		fprintf(stderr, "加载 H.264 文件失败: %s\n", H264_FILE_PATH);
		return 1;
	}

	printf("主进程 PID: %d\n", getpid());

	// 创建 RTSP 监听套接字
	int rtsp_sock = create_rtsp_socket();
	if (rtsp_sock < 0)
	{
		fprintf(stderr, "创建 RTSP 套接字失败\n");
		if (g_h264_data != NULL)
		{
			free(g_h264_data);
		}
		return 1;
	}

	printf("RTSP 服务器监听端口 %d\n", RTSP_PORT);
	printf("RTSP 客户端连接地址: rtsp://localhost:8554/\n");

	// 主循环：接受客户端连接
	while (1)
	{
		struct sockaddr_in client_addr;
		socklen_t client_len = sizeof(client_addr);
		int client = accept(rtsp_sock, (struct sockaddr*)&client_addr, &client_len);
		if (client < 0)
		{
			perror("accept");
			continue;
		}

		printf("新客户端连接: %s:%d\n",
				inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));

		// 为每个客户端创建独立线程
		pthread_t thread;
		if (pthread_create(&thread, NULL, handle_client, (void*)(long)client) != 0)
		{
			perror("pthread_create");
			close(client);
		}
		else
		{
			pthread_detach(thread);
		}
	}

	// 清理资源（正常情况下不会执行到这里）
	close(rtsp_sock);
	if (g_h264_data != NULL)
	{
		free(g_h264_data);
	}
	return 0;
}