#define _POSIX_C_SOURCE 200809L
#include "player.h"

#include <string.h>

/**
 * @brief 打开播放器进程
 * 
 * 通过管道启动外部播放器（如ffplay）进程
 * @param player 播放器上下文指针
 * @param cmd 播放器命令字符串
 * @return 成功返回0，失败返回-1
 */
int player_open(player_t *player, const char *cmd)
{
    if (player == NULL || cmd == NULL)
    {
        return -1;
    }

    memset(player, 0, sizeof(*player));
    strncpy(player->cmd, cmd, sizeof(player->cmd) - 1);

#ifdef _WIN32
    player->pipe = _popen(player->cmd, "wb");
#else
    player->pipe = popen(player->cmd, "w");
#endif

    return player->pipe ? 0 : -1;
}

/**
 * @brief 向播放器发送数据
 * 
 * 将H.264码流数据通过管道发送给播放器
 * @param player 播放器上下文指针
 * @param data 要发送的数据缓冲区
 * @param len 数据长度
 * @return 成功返回0，失败返回-1
 */
int player_feed(player_t *player, const void *data, size_t len)
{
    if (player == NULL || player->pipe == NULL || data == NULL || len == 0)
    {
        return -1;
    }

    size_t written = fwrite(data, 1, len, player->pipe);
    fflush(player->pipe);
    return written == len ? 0 : -1;
}

/**
 * @brief 关闭播放器进程
 * @param player 播放器上下文指针
 * @return 关闭操作的返回值
 */
int player_close(player_t *player)
{
    if (player == NULL)
    {
        return 0;
    }

    int ret = 0;
    if (player->pipe != NULL)
    {
#ifdef _WIN32
        ret = _pclose(player->pipe);
#else
        ret = pclose(player->pipe);
#endif
        player->pipe = NULL;
    }

    return ret;
}
