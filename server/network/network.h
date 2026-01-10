/**
 * @file network.h
 * @brief 网络层接口定义
 * 
 * 提供RTSP和RTP socket创建功能
 */

#ifndef NETWORK_H
#define NETWORK_H

/**
 * @brief 创建RTSP TCP监听socket
 * 
 * @param port RTSP监听端口
 * @return 成功返回socket文件描述符，失败返回-1
 */
int CreateRtspSocket(int port);

/**
 * @brief 创建RTP UDP socket
 * 
 * @param port 指定的RTP端口
 * @param rtpPortOut 输出实际分配的RTP端口
 * @param rtcpPortOut 输出实际分配的RTCP端口
 * @return 成功返回RTP socket文件描述符，失败返回-1
 */
int CreateRtpSocket(int port, int *rtpPortOut, int *rtcpPortOut);

/**
 * @brief 创建RTP TCP socket并连接到客户端
 * 
 * @param clientAddr 客户端地址和端口
 * @return 成功返回TCP socket文件描述符，失败返回-1
 */
int CreateRtpTcpSocket(const struct sockaddr_in *clientAddr);

#endif /* NETWORK_H */

