#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include "../include/board.h"

#define N 10
#define CELL_W 7  // fits board into one msg

static int tile_number(int row, int col) {
    int level = (N - 1) - row;      // 0 bottom .. 9 top
    int base  = level * 10 + 1;
    if (level % 2 == 0) return base + col;            // L->R
    else                return base + (N - 1 - col);  // R->L
}

static void appendf(char *out, size_t outsz, size_t *len, const char *fmt, ...) {
    if (*len >= outsz) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *len, outsz - *len, fmt, ap);
    va_end(ap);
    if (n > 0) {
        size_t nn = (size_t)n;
        *len += (nn < outsz - *len) ? nn : (outsz - *len);
    }
}

static int find_jump(const jump_t *jumps, int num_jumps, int tile,
                     int *to_out, char *kind_out, int *id_out) {
    int lid = 0, sid = 0;
    for (int i = 0; i < num_jumps; i++) {
        char kind = (jumps[i].from < jumps[i].to) ? 'L' : 'S';
        if (kind == 'L') lid++; else sid++;

        if (jumps[i].from == tile) {
            if (to_out) *to_out = jumps[i].to;
            if (kind_out) *kind_out = kind;
            if (id_out) *id_out = (kind == 'L') ? lid : sid;
            return 1;
        }
    }
    return 0;
}

static void pad7(char b[CELL_W + 1]) {
    size_t k = strlen(b);
    while (k < CELL_W) b[k++] = ' ';
    b[CELL_W] = '\0';
}

static void cell1(char b[CELL_W + 1], int tile, const jump_t *j, int nj) {
    int to, id; char kind;
    if (find_jump(j, nj, tile, &to, &kind, &id)) {
        snprintf(b, CELL_W + 1, "%3d %c%d", tile, kind, id); // " 20 S1"
        pad7(b);
    } else {
        snprintf(b, CELL_W + 1, "%3d", tile);
        pad7(b);
    }
}

static void cell2(char b[CELL_W + 1], int tile, const int *pos, int np) {
    char p[6] = {0};
    int idx = 0;
    for (int i = 0; i < np && idx < 5; i++)
        if (pos[i] == tile) p[idx++] = (char)('1' + i);
    p[idx] = '\0';
    snprintf(b, CELL_W + 1, "%-7s", p);
}

static void cell3(char b[CELL_W + 1], int tile, const jump_t *j, int nj) {
    int to, id; char kind;
    if (find_jump(j, nj, tile, &to, &kind, &id)) {
        snprintf(b, CELL_W + 1, "->%3d", to); // "-> 84"
        pad7(b);
    } else {
        memset(b, ' ', CELL_W);
        b[CELL_W] = '\0';
    }
}

void render_board_10x10(
    char *out, size_t outsz,
    const int *players_pos, int num_players,
    const jump_t *jumps, int num_jumps
) {
    size_t len = 0;
    if (!out || outsz == 0) return;
    out[0] = '\0';

    // top border
    for (int c = 0; c < N; c++) {
        appendf(out, outsz, &len, "+");
        for (int k = 0; k < CELL_W; k++) appendf(out, outsz, &len, "-");
    }
    appendf(out, outsz, &len, "+\n");

    for (int r = 0; r < N; r++) {
        for (int inner = 0; inner < 3; inner++) {
            for (int c = 0; c < N; c++) {
                int tile = tile_number(r, c);
                char cell[CELL_W + 1];
                if (inner == 0) cell1(cell, tile, jumps, num_jumps);
                else if (inner == 1) cell2(cell, tile, players_pos, num_players);
                else cell3(cell, tile, jumps, num_jumps);
                appendf(out, outsz, &len, "|%s", cell);
            }
            appendf(out, outsz, &len, "|\n");
        }

        for (int c = 0; c < N; c++) {
            appendf(out, outsz, &len, "+");
            for (int k = 0; k < CELL_W; k++) appendf(out, outsz, &len, "-");
        }
        appendf(out, outsz, &len, "+\n");
    }

    appendf(out, outsz, &len, "Players: 1=P1 2=P2 3=P3 4=P4 5=P5\n");
}
