#include <stdio.h>
#include <pthread.h>

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

void handle_player_move(game_t *game, int player_id);
void init_game(game_t *game);


void print_board(game_t *game) {
    printf("\nPlayer positions:\n");
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (game->active[i]) {
            printf("Player %d: %d\n", i, game->positions[i]);
        }
    }
    printf("\n");
}

void player_prompt(game_t *game, int player_id) {
    printf("Player %d's turn. Press ENTER to roll.\n", player_id);
    getchar();

    handle_player_move(game, player_id);
}

int main() {
    game_t game;
    init_game(&game);

    game.active[0] = 1;
    game.active[1] = 1;
    game.state = GAME_RUNNING;

    while (game.state != GAME_OVER) {
        print_board(&game);
        player_prompt(&game, game.current_turn);
    }

    printf("\n player %d wins!\n", game.winner_id);
    return 0;
}
