#ifndef BOARD_H
#define BOARD_H

#include <stddef.h>

typedef struct { int from; int to; } jump_t;

void render_board_10x10(
    char *out, size_t outsz,
    const int *players_pos, int num_players,
    const jump_t *jumps, int num_jumps
);

#endif
