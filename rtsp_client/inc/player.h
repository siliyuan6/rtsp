/**
 * @file player.h
 * @brief 视频播放器接口头文件
 * 
 * 提供通过管道调用外部播放器（如ffplay）播放H.264视频流的接口。
 */

#ifndef PLAYER_H
#define PLAYER_H

#include <stdio.h>
#include <stddef.h>

/**
 * @brief 播放器上下文结构体
 */
typedef struct player_s
{
    FILE *pipe;          // 管道文件指针
    char cmd[256];       // 播放器命令字符串
} player_t;

int player_open(player_t *player, const char *cmd);
int player_feed(player_t *player, const void *data, size_t len);
int player_close(player_t *player);

#endif /* PLAYER_H */
