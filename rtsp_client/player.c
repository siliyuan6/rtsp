#include "player.h"

#include <string.h>

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
