#ifndef PLAYER_H
#define PLAYER_H

#include <stdio.h>
#include <stddef.h>

typedef struct player_s
{
    FILE *pipe;
    char cmd[256];
} player_t;

int player_open(player_t *player, const char *cmd);
int player_feed(player_t *player, const void *data, size_t len);
int player_close(player_t *player);

#endif /* PLAYER_H */
