#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <winsock2.h>
#include <windows.h>

#define RTSP_PORT 8554
#define RTP_PORT 5000
#define RTCP_PORT 5001
#define MAX_REQUEST_SIZE 2048

SOCKET create_rtsp_socket() {
	SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(RTSP_PORT);
	addr.sin_addr.s_addr = htons(INADDR_ANY);
	bind(sock, (struct sockaddr*)&addr, sizeof(addr));
	listen(sock, 5);
	return sock;
}

void send_rtp_packet(SOCKET rtp_sock, struct sockaddr_in* client_addr) {
	static unsigned short sequence = 0;
	static unsigned int timestamp = 0;

	char packet[12 + 160]; // RTP头+假数据
	memset(packet, 0, sizeof(packet));

	// RTP头
	packet[0] = 0x80; // V=2, P=0, X=0, CC=0
	packet[1] = 0x0A; // M=0, PT=10 (PCMU)
	sequence++;
	packet[2] = (sequence >> 8) & 0xFF;
	packet[3] = sequence & 0xFF;

	timestamp += 160; // 假设8kHz采样率，20ms数据
	packet[4] = (timestamp >> 24) & 0xFF;
	packet[5] = (timestamp >> 16) & 0xFF;
	packet[6] = (timestamp >> 8) & 0xFF;
	packet[7] = timestamp & 0xFF;
	packet[8] = 0x12; // SSRC（伪随机）
	packet[9] = 0x34;
	packet[10] = 0x56;
	packet[11] = 0x78;

	sendto(rtp_sock, packet, sizeof(packet), 0, 
			(struct sockaddr*)client_addr, sizeof(*client_addr));
}

int get_sdp_descr()
{
	char sdp_time[128] = {0};
	int32_t len = 0;

	rtsp_msg_get_sdp_id(sdp_time);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "v=0\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "o=- %s 0 IN IP4 %s\r\n", sdp_time, session->localip);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "s=session\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "i=N/A\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "c=IN IP4 %s\r\n", session->client_info.ip);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "t=0 0\r\n");
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=recvonly\r\n");

	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "m=video 0 RTP/AVP %d\r\n", H264_PAYLOAD_TYPE);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=rtpmap:%d H264/90000\r\n", H264_PAYLOAD_TYPE);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=fmtp:%d packetization-mode=1;\r\n", H264_PAYLOAD_TYPE);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=control:%s%d\r\n", RTSP_STREAM_URL, FRAME_TYPE_VIDEO);

	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "m=audio 0 RTP/AVP %d\r\n", PCM_PAYLOAD_TYPE);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=rtpmap:%d L16/%d\r\n", PCM_PAYLOAD_TYPE, media_info->audio_sample_rate);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=fmtp:%d\r\n", PCM_PAYLOAD_TYPE);
	len += snprintf(msg + len, RTSP_BUFFERSIZE - len, "a=control:%s%d\r\n", RTSP_STREAM_URL, FRAME_TYPE_AUDIO);

}

DWORD WINAPI handle_client(LPVOID lpParam) {
	SOCKET client_sock = (SOCKET)lpParam;
	char buffer[MAX_REQUEST_SIZE];
	int cseq = 0;
	char session_id[32] = {0};
	struct sockaddr_in client_rtp_addr;
	SOCKET rtp_sock = INVALID_SOCKET;

	// 添加线程信息打印
	DWORD pid = GetCurrentProcessId();
	DWORD tid = GetCurrentThreadId();
	printf("[PID:%lu][TID:%lu] Client handler started\n", pid, tid);
	while (1) {
		char response[256];
		int len = recv(client_sock, buffer, MAX_REQUEST_SIZE, 0);
		if (len <= 0) break;

		// 解析CSeq
		char* cseq_start = strstr(buffer, "CSeq:");
		if (cseq_start) sscanf(cseq_start, "CSeq: %d", &cseq);

		printf("\r\n<<<<<<<<<<request:\r\n%s\n", buffer);

		// 解析方法
		if (strstr(buffer, "OPTIONS")) {
			sprintf(response, 
				"RTSP/1.0 200 \r\n "
				"CSeq: %d \r\n "
				"Server: cc/1.0 \r\n"
				"Public: OPTIONS,DESCRIBE,GET_PARAMETER,SETUP,PLAY,TEARDOWN \r\n"
				"\r\n", cseq);
			send(client_sock, response, strlen(response), 0);
		}
		else if (strstr(buffer, "DESCRIBE")) {
			sprintf(response, 
				"Response: RTSP/1.0 200 OK\r\n"
				"CSeq: %d\r\n"
				"Date: Thu, Jan 01 1970 00:03:29 GMT\r\n"
				"Content-Base: rtsp://localhost:8554/\r\n"
				"Content-type: application/sdp"
				"Content-length: 374"
				"\r\n", 
				"m=video 0 RTP/AVP 96\r\n"
				"a=control:streamid=0\r\n"
				"a=range:npt=0-7.741000\r\n"
				"a=length:npt=7.741000\r\n"
				"a=rtpmap:96 MP4V-ES/5544\r\n"
				"a=mimetype:string;"video/MP4V-ES"\r\n"
				"a=AvgBitRate:integer;304018"
				"a=StreamName:string;"hinted video track""
				"m=audio 0 RTP/AVP 97"
				"a=control:streamid=1"
				"a=range:npt=0-7.712000"
				"a=length:npt=7.712000"
				"a=rtpmap:97 mpeg4-generic/32000/2"
				"a=mimetype:string;"audio/mpeg4-generic""
				"a=AvgBitRate:integer;65790"
				"a=StreamName:string;"hinted audio track""

				
				
				cseq);
			send(client_sock, response, strlen(response), 0);
		}
		else if (strstr(buffer, "SETUP")) {
			// 创建RTP Socket
			if (rtp_sock == INVALID_SOCKET) {
				rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
				struct sockaddr_in rtp_addr = {0};
				rtp_addr.sin_family = AF_INET;
				rtp_addr.sin_port = htons(RTP_PORT);
				rtp_addr.sin_addr.s_addr = INADDR_ANY;
				bind(rtp_sock, (struct sockaddr*)&rtp_addr, sizeof(rtp_addr));
			}

			// 获取客户端地址
			char* transport = strstr(buffer, "Transport:");
			int client_rtp_port;
			sscanf(transport, "Transport: RTP/AVP;unicast;client_port=%d", &client_rtp_port);
			
			memset(&client_rtp_addr, 0, sizeof(client_rtp_addr));
			client_rtp_addr.sin_family = AF_INET;
			client_rtp_addr.sin_port = htons(client_rtp_port);
			client_rtp_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
			
			sprintf(session_id, "%lu", GetCurrentThreadId());
			
			sprintf(response, "RTSP/1.0 200 OK\r\n"
					"CSeq: %d\r\n"
					"Transport: RTP/AVP;unicast;server_port=%d-%d\r\n"
					"Session: %s\r\n", 
					cseq, RTP_PORT, RTCP_PORT, session_id);
			send(client_sock, response, strlen(response), 0);
		}
		else if (strstr(buffer, "PLAY")) {
			sprintf(response, "RTSP/1.0 200 OK\r\n"
					"CSeq: %d\r\n"
					"Session: %s\r\n", cseq, session_id);
			send(client_sock, response, strlen(response), 0);
			
			// 开始发送RTP数据
			for (int i = 0; i < 100; ++i) {
				send_rtp_packet(rtp_sock, &client_rtp_addr);
				Sleep(20); // 模拟实时传输
			}
		}
		else if (strstr(buffer, "TEARDOWN")) {
			break;
		}
		printf("\r\n>>>>>>>>>>response:\r\n%s", response);
	}

	closesocket(client_sock);
	if (rtp_sock != INVALID_SOCKET) closesocket(rtp_sock);
	return 0;
}

int main() {
	WSADATA wsa;
	SOCKET client = 0;
	WSAStartup(MAKEWORD(2,2), &wsa);

	printf("Main process PID: %lu\n", GetCurrentProcessId());

	SOCKET rtsp_sock = create_rtsp_socket();
	printf("RTSP Server listening on port %d\n", RTSP_PORT);
	printf("RTSP Client Link: rtsp://localhost:8554/ \n");

	while (1) {
		if (!client) {
			client = accept(rtsp_sock, NULL, NULL);
			CreateThread(NULL, 0, handle_client, (LPVOID)client, 0, NULL);
		} else {
			Sleep(1000);
		}
	}

	closesocket(rtsp_sock);
	WSACleanup();
	return 0;
}