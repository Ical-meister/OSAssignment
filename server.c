#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define MAX_PLAYERS 5
#define BOARD_SIZE 100

typedef enum {
    GAME_WAITING,
    GAME_RUNNING,
    GAME_OVER
} game_state_t;

typedef struct {
    int positions[MAX_PLAYERS];
    int active[MAX_PLAYERS];
    int current_turn;
    int winner_id;
    game_state_t state;
    pthread_mutex_t game_mutex;
} game_t;

typedef struct {
    int from;
    int to;
} jump_t;

#define NUM_JUMPS 8

static const jump_t jumps[NUM_JUMPS] = {
    {4, 14}, {9, 31}, {17, 7}, {20, 38},
    {28, 84}, {40, 59}, {51, 67}, {89, 26}
};

int roll_dice() {
    return (rand() % 6) + 1;
}

int apply_snake_or_ladder(int pos) {
    for (int i = 0; i < NUM_JUMPS; i++) {
        if (jumps[i].from == pos)
            return jumps[i].to;
    }
    return pos;
}

void handle_player_move(game_t *game, int player_id) {
    pthread_mutex_lock(&game->game_mutex);

    if (game->state != GAME_RUNNING ||
        game->current_turn != player_id ||
        !game->active[player_id]) {
        pthread_mutex_unlock(&game->game_mutex);
        return;
    }

    int roll = roll_dice();
    int old_pos = game->positions[player_id];
    int new_pos = old_pos + roll;

    if (new_pos > BOARD_SIZE) {
        new_pos = old_pos;
    } else {
        new_pos = apply_snake_or_ladder(new_pos);
    }

    game->positions[player_id] = new_pos;

    if (new_pos == BOARD_SIZE) {
        game->winner_id = player_id;
        game->state = GAME_OVER;
    } else {
        game->current_turn = (game->current_turn + 1) % MAX_PLAYERS;
    }

    pthread_mutex_unlock(&game->game_mutex);
}

void reset_game(game_t *game) {
    pthread_mutex_lock(&game->game_mutex);

    for (int i = 0; i < MAX_PLAYERS; i++) {
        game->positions[i] = 0;
        game->active[i] = 0;  //im not sure if players have to register again or not...
    }
    game->winner_id = -1;
    game->current_turn = 0;
    game->state = GAME_WAITING;

    pthread_mutex_unlock(&game->game_mutex);
}

void init_game(game_t *game) {
    srand(time(NULL) ^ getpid());

    pthread_mutex_init(&game->game_mutex, NULL);
    reset_game(game);
}
