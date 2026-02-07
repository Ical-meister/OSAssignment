//afiq
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <time.h>

#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <semaphore.h>
#include <stdarg.h>

#include "../include/common.h"
#include "../include/board.h"


#define MAX_PLAYERS   5
#define SHM_NAME     "/snl_shm_v1"

#define LOG_Q_SIZE    256
#define LOG_LINE_MAX  256
#define SCORE_MAX     128

#define BOARD_SIZE    100
#define NUM_JUMPS     8

static const char *LOG_FILE    = "game.log";
static const char *SCORES_FILE = "scores.txt";


static const jump_t jumps[NUM_JUMPS] = {
    {4, 14}, {9, 31}, {17, 7}, {20, 38},
    {28, 84}, {40, 59}, {51, 67}, {89, 26}
};

static int apply_snake_or_ladder(int pos)
{
    for (int i = 0; i < NUM_JUMPS; i++)
        if (jumps[i].from == pos) return jumps[i].to;
    return pos;
}

typedef struct {
    int connected;              // 1 = active, 0 = inactive
    char name[MAX_NAME];
} PlayerSlot;

typedef struct {
    // ===== turn control =====
    int game_active;
    int current_turn;
    int turn_done;

    PlayerSlot slot[MAX_PLAYERS];

    // ===== board state =====
    int positions[MAX_PLAYERS]; // 0..100
    int winner_id;              // -1 if none

    // ===== process-shared sync (Member 3) =====
    pthread_mutex_t game_mtx;
    pthread_cond_t  turn_cv;
    pthread_cond_t  turn_done_cv;

    // ===== logger queue (Member 4) =====
    pthread_mutex_t log_mtx;
    int log_head;
    int log_tail;
    char log_q[LOG_Q_SIZE][LOG_LINE_MAX];
    sem_t log_slots;
    sem_t log_items;

    // ===== scores (Member 4) =====
    pthread_mutex_t scores_mtx;
    struct { char name[MAX_NAME]; int wins; } scores[SCORE_MAX];
    int score_count;

    // ===== last dice (used by your previous winner logic; keep if you want) =====
    int last_roll[MAX_PLAYERS];

    volatile sig_atomic_t shutting_down;
} SharedState;

// ===== globals =====
static int g_fd_r = -1;
static int g_fd_w_dummy = -1;

static int g_shm_fd = -1;
static SharedState *g_st = NULL;
static volatile sig_atomic_t g_shutdown = 0;

static pthread_t g_logger_tid;
static int g_logger_running = 0;

// ===== cleanup =====
static void cleanup(void)
{
    if (g_fd_r != -1) close(g_fd_r);
    if (g_fd_w_dummy != -1) close(g_fd_w_dummy);

    if (g_st) munmap(g_st, sizeof(*g_st));
    if (g_shm_fd != -1) close(g_shm_fd);

    // For grading: many lecturers prefer unlinking only at end. Optional:
    // shm_unlink(SHM_NAME);

    unlink(SERVER_FIFO);
}

// ===== shared memory create (Linux POSIX) =====
static SharedState *shm_create(void)
{
    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd < 0) {
        perror("shm_open");
        return NULL;
    }

    if (ftruncate(g_shm_fd, sizeof(SharedState)) < 0) {
        perror("ftruncate");
        return NULL;
    }

    void *p = mmap(NULL, sizeof(SharedState),
                   PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (p == MAP_FAILED) {
        perror("mmap");
        return NULL;
    }

    return (SharedState *)p;
}

static void init_pshared_sync(SharedState *st)
{
    pthread_mutexattr_t ma;
    pthread_condattr_t  ca;

    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&st->game_mtx, &ma);

    pthread_condattr_init(&ca);
    pthread_condattr_setpshared(&ca, PTHREAD_PROCESS_SHARED);
    pthread_cond_init(&st->turn_cv, &ca);
    pthread_cond_init(&st->turn_done_cv, &ca);

    pthread_mutexattr_destroy(&ma);
    pthread_condattr_destroy(&ca);
}

static void init_aux(SharedState *st)
{
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setpshared(&ma, PTHREAD_PROCESS_SHARED);

    pthread_mutex_init(&st->log_mtx, &ma);
    pthread_mutex_init(&st->scores_mtx, &ma);

    pthread_mutexattr_destroy(&ma);

    sem_init(&st->log_slots, 1, LOG_Q_SIZE);
    sem_init(&st->log_items, 1, 0);

    st->log_head = 0;
    st->log_tail = 0;

    st->score_count = 0;
    st->shutting_down = 0;

    for (int i = 0; i < MAX_PLAYERS; i++)
        st->last_roll[i] = 0;
}

static void init_state(SharedState *st)
{
    st->game_active = 0;
    st->current_turn = MAX_PLAYERS - 1; // so player0 starts first RR
    st->turn_done = 0;

    st->winner_id = -1;

    for (int i = 0; i < MAX_PLAYERS; i++) {
        st->slot[i].connected = 0;
        st->slot[i].name[0] = '\0';
        st->positions[i] = 0;
    }
}

static int active_players(SharedState *st)
{
    int c = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (st->slot[i].connected) c++;
    return c;
}

static int next_connected(SharedState *st, int cur)
{
    for (int k = 1; k <= MAX_PLAYERS; k++) {
        int idx = (cur + k) % MAX_PLAYERS;
        if (st->slot[idx].connected) return idx;
    }
    return -1;
}

static int alloc_slot(SharedState *st, const char *name)
{
    for (int i = 0; i < MAX_PLAYERS; i++) {
        if (!st->slot[i].connected) {
            st->slot[i].connected = 1;
            snprintf(st->slot[i].name, sizeof(st->slot[i].name), "%s", name);
            st->positions[i] = 0;
            return i;
        }
    }
    return -1;
}

// ===== logger =====
static void now_str(char *buf, size_t n)
{
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(buf, n, "%Y-%m-%d %H:%M:%S", &tm);
}

static void log_enqueue(SharedState *st, const char *line)
{
    if (!st || st->shutting_down) return;

    if (sem_trywait(&st->log_slots) != 0)
        return; // non-blocking: drop if full

    pthread_mutex_lock(&st->log_mtx);
    strncpy(st->log_q[st->log_tail], line, LOG_LINE_MAX);
    st->log_q[st->log_tail][LOG_LINE_MAX - 1] = '\0';
    st->log_tail = (st->log_tail + 1) % LOG_Q_SIZE;
    pthread_mutex_unlock(&st->log_mtx);

    sem_post(&st->log_items);
}

static void log_event(SharedState *st, const char *fmt, ...)
{
    char ts[32];
    now_str(ts, sizeof(ts));

    char body[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    char line[LOG_LINE_MAX];
    snprintf(line, sizeof(line), "[%s] (pid=%ld) %s", ts, (long)getpid(), body);
    log_enqueue(st, line);
}

static void *logger_thread(void *arg)
{
    SharedState *st = (SharedState *)arg;

    FILE *fp = fopen(LOG_FILE, "a");
    if (!fp) return NULL;

    setvbuf(fp, NULL, _IOLBF, 0);

    while (1) {
        if (st->shutting_down) {
            while (sem_trywait(&st->log_items) == 0) {
                char line[LOG_LINE_MAX];

                pthread_mutex_lock(&st->log_mtx);
                strncpy(line, st->log_q[st->log_head], sizeof(line));
                line[LOG_LINE_MAX - 1] = '\0';
                st->log_head = (st->log_head + 1) % LOG_Q_SIZE;
                pthread_mutex_unlock(&st->log_mtx);

                sem_post(&st->log_slots);
                fprintf(fp, "%s\n", line);
            }
            break;
        }

        if (sem_wait(&st->log_items) != 0) {
            if (errno == EINTR) continue;
            break;
        }

        char line[LOG_LINE_MAX];

        pthread_mutex_lock(&st->log_mtx);
        strncpy(line, st->log_q[st->log_head], sizeof(line));
        line[LOG_LINE_MAX - 1] = '\0';
        st->log_head = (st->log_head + 1) % LOG_Q_SIZE;
        pthread_mutex_unlock(&st->log_mtx);

        sem_post(&st->log_slots);
        fprintf(fp, "%s\n", line);
    }

    fclose(fp);
    return NULL;
}

static int logger_start(SharedState *st)
{
    g_logger_running = 1;
    if (pthread_create(&g_logger_tid, NULL, logger_thread, st) != 0) {
        g_logger_running = 0;
        return -1;
    }
    return 0;
}

static void logger_stop(SharedState *st)
{
    if (!st) return;

    st->shutting_down = 1;
    sem_post(&st->log_items);

    if (g_logger_running) {
        pthread_join(g_logger_tid, NULL);
        g_logger_running = 0;
    }
}

// ===== scores =====
static int scores_load(SharedState *st, const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        fp = fopen(path, "w");
        if (!fp) return -1;
        fclose(fp);
        return 0;
    }

    pthread_mutex_lock(&st->scores_mtx);
    st->score_count = 0;

    char name[MAX_NAME];
    int wins;
    while (st->score_count < SCORE_MAX && fscanf(fp, "%31s %d", name, &wins) == 2) {
        strncpy(st->scores[st->score_count].name, name, MAX_NAME);
        st->scores[st->score_count].name[MAX_NAME - 1] = '\0';
        st->scores[st->score_count].wins = wins;
        st->score_count++;
    }

    pthread_mutex_unlock(&st->scores_mtx);
    fclose(fp);
    return 0;
}

static int scores_save(SharedState *st, const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *fp = fopen(tmp, "w");
    if (!fp) return -1;

    pthread_mutex_lock(&st->scores_mtx);
    for (int i = 0; i < st->score_count; i++)
        fprintf(fp, "%s %d\n", st->scores[i].name, st->scores[i].wins);
    pthread_mutex_unlock(&st->scores_mtx);

    fclose(fp);
    return (rename(tmp, path) == 0) ? 0 : -1;
}

static void scores_inc(SharedState *st, const char *winner)
{
    if (!winner || !winner[0]) return;

    pthread_mutex_lock(&st->scores_mtx);

    int idx = -1;
    for (int i = 0; i < st->score_count; i++) {
        if (strncmp(st->scores[i].name, winner, MAX_NAME) == 0) {
            idx = i;
            break;
        }
    }

    if (idx >= 0) {
        st->scores[idx].wins++;
    } else if (st->score_count < SCORE_MAX) {
        idx = st->score_count++;
        strncpy(st->scores[idx].name, winner, MAX_NAME);
        st->scores[idx].name[MAX_NAME - 1] = '\0';
        st->scores[idx].wins = 1;
    }

    pthread_mutex_unlock(&st->scores_mtx);

    scores_save(st, SCORES_FILE);
}

// ===== scheduler (turn control) =====
static void *scheduler_thread(void *arg)
{
    SharedState *st = (SharedState *)arg;

    while (!g_shutdown) {
        pthread_mutex_lock(&st->game_mtx);

        while (!g_shutdown && (st->game_active == 0 || active_players(st) < 3)) {
            pthread_mutex_unlock(&st->game_mtx);
            usleep(20000);
            pthread_mutex_lock(&st->game_mtx);
        }

        if (g_shutdown) {
            pthread_mutex_unlock(&st->game_mtx);
            break;
        }

        int next = next_connected(st, st->current_turn);
        if (next < 0) {
            st->game_active = 0;
            pthread_cond_broadcast(&st->turn_cv);
            pthread_cond_broadcast(&st->turn_done_cv);
            pthread_mutex_unlock(&st->game_mtx);
            continue;
        }

        st->current_turn = next;
        st->turn_done = 0;

        pthread_cond_broadcast(&st->turn_cv);

        while (!g_shutdown && st->game_active == 1 && st->turn_done == 0) {
            pthread_cond_wait(&st->turn_done_cv, &st->game_mtx);
        }

        pthread_mutex_unlock(&st->game_mtx);
        usleep(1000);
    }

    return NULL;
}

// ===== signals / reaping =====
static void reap_children(int sig)
{
    (void)sig;
    int status = 0;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        (void)pid;
        (void)status;
    }
}

static void handle_sigint(int sig)
{
    (void)sig;
    const char msg[] = "\n[server] shutting down (SIGINT)\n";
    (void)write(STDOUT_FILENO, msg, sizeof(msg) - 1);

    g_shutdown = 1;
    if (g_st) g_st->shutting_down = 1;
}

static void send_reply(const char *reply_fifo, const char *fmt, ...)
{
    msg_t r;
    memset(&r, 0, sizeof(r));
    r.type = MSG_TEXT;

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r.text, sizeof(r.text), fmt, ap);
    va_end(ap);

    int fd = open(reply_fifo, O_WRONLY);
    if (fd == -1) return;

    size_t total = 0;
    const char *p = (const char *)&r;
    while (total < sizeof(r)) {
        ssize_t n = write(fd, p + total, sizeof(r) - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        total += (size_t)n;
    }
    close(fd);
}

static int read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    char *p = buf;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n == 0) return 0;
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 1;
}

// ===== game move (uses your SharedState) =====
static int handle_player_move_locked(SharedState *st, int player_id, int dice,
                                    int *old_pos_out, int *new_pos_out, int *jumped_out)
{
    int old_pos = st->positions[player_id];
    int new_pos = old_pos + dice;

    // exact-roll rule: overshoot -> stay
    if (new_pos > BOARD_SIZE) new_pos = old_pos;

    int jumped = apply_snake_or_ladder(new_pos);
    st->positions[player_id] = jumped;

    if (old_pos_out) *old_pos_out = old_pos;
    if (new_pos_out) *new_pos_out = new_pos;
    if (jumped_out)  *jumped_out  = jumped;

    if (jumped == BOARD_SIZE) {
        st->winner_id = player_id;
        st->game_active = 0;
        return 1; // won
    }
    return 0;
}

// ===== child: session loop =====
static void session_loop(const msg_t *join, int my_id)
{
    printf("[child %d] session for '%s' req='%s' reply='%s'\n",
           (int)getpid(), join->name, join->req_fifo, join->reply_fifo);

    int fd_req = open(join->req_fifo, O_RDONLY);
    if (fd_req == -1) {
        perror("[child] open(req_fifo)");
        exit(1);
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    send_reply(join->reply_fifo, "JOIN accepted. Type 'roll' or 'quit'.");

    while (1) {
        msg_t m;
        int rr = read_full(fd_req, &m, sizeof(m));
        
        if (rr == 0) {
            log_event(g_st, "DISCONNECT name='%s' slot=%d", join->name, my_id);

            pthread_mutex_lock(&g_st->game_mtx);
            g_st->slot[my_id].connected = 0;
            g_st->slot[my_id].name[0] = '\0';

            if (g_st->current_turn == my_id) {
                g_st->turn_done = 1;
                pthread_cond_signal(&g_st->turn_done_cv);
            }

            if (active_players(g_st) < 3) {
                g_st->game_active = 0;
                pthread_cond_broadcast(&g_st->turn_cv);
                pthread_cond_signal(&g_st->turn_done_cv);
            }

            pthread_mutex_unlock(&g_st->game_mtx);
            break;
        }
            if (rr < 0) {
                if (errno == EINTR) continue;
                perror("[child] read");
                break;
                        }

        if (m.type == MSG_ROLL) {
            pthread_mutex_lock(&g_st->game_mtx);

            if (g_st->game_active == 0) {
                pthread_mutex_unlock(&g_st->game_mtx);
                send_reply(join->reply_fifo, "WAIT (need 3 players)");
                continue;
            }
            if (my_id != g_st->current_turn) {
                pthread_mutex_unlock(&g_st->game_mtx);
                send_reply(join->reply_fifo, "NOT YOUR TURN");
                continue;
            }

            int dice = (rand() % 6) + 1;

            int oldp, newp, jumped;
            int won = handle_player_move_locked(g_st, my_id, dice, &oldp, &newp, &jumped);

            g_st->last_roll[my_id] = dice;

            g_st->turn_done = 1;
            pthread_cond_signal(&g_st->turn_done_cv);

            pthread_mutex_unlock(&g_st->game_mtx);

            char buf[128];
            if (jumped != newp) {
                snprintf(buf, sizeof(buf),
                         "Rolled %d. %d -> %d, jump to %d.",
                         dice, oldp, newp, jumped);
            } else {
                snprintf(buf, sizeof(buf),
                         "Rolled %d. %d -> %d.",
                         dice, oldp, jumped);
            }

            log_event(g_st, "ROLL name='%s' slot=%d dice=%d pos=%d",
                      join->name, my_id, dice, jumped);

            char board[3600];
            render_board_10x10(board, sizeof(board),
                   g_st->positions, MAX_PLAYERS,
                   jumps, NUM_JUMPS);

            if (won) {
        log_event(g_st, "WIN name='%s' slot=%d", join->name, my_id);
        scores_inc(g_st, join->name);
        send_reply(join->reply_fifo, "%s\n\n%s\n\n%s", buf, board, "You reached 100! You win!");
        } else  {
            send_reply(join->reply_fifo, "%s\n\n%s", buf, board);
        }
        }
        else if (m.type == MSG_QUIT) {
            log_event(g_st, "QUIT name='%s' slot=%d", join->name, my_id);

            send_reply(join->reply_fifo, "Goodbye!");

            pthread_mutex_lock(&g_st->game_mtx);
            g_st->slot[my_id].connected = 0;
            g_st->slot[my_id].name[0] = '\0';

            if (g_st->current_turn == my_id) {
                g_st->turn_done = 1;
                pthread_cond_signal(&g_st->turn_done_cv);
            }

            if (active_players(g_st) < 3) {
                g_st->game_active = 0;
                pthread_cond_broadcast(&g_st->turn_cv);
                pthread_cond_signal(&g_st->turn_done_cv);
            }

            pthread_mutex_unlock(&g_st->game_mtx);
            break;
        }
        else {
            send_reply(join->reply_fifo, "Unknown command.");
        }
    }

    close(fd_req);
    exit(0);
}

// ===== main =====
int main(void)
{
    atexit(cleanup);

    struct sigaction si;
    memset(&si, 0, sizeof(si));
    si.sa_handler = handle_sigint;
    sigemptyset(&si.sa_mask);
    si.sa_flags = 0;
    sigaction(SIGINT, &si, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    if (mkfifo(SERVER_FIFO, 0666) == -1) {
        if (errno != EEXIST) {
            perror("mkfifo(SERVER_FIFO)");
            return 1;
        }
    }

    printf("[server] listening on %s\n", SERVER_FIFO);
    
    g_fd_r = open(SERVER_FIFO, O_RDONLY);
    if (g_fd_r == -1) {
        perror("open(SERVER_FIFO O_RDONLY)");
        return 1;
    }

    g_fd_w_dummy = open(SERVER_FIFO, O_WRONLY);
    (void)g_fd_w_dummy;

    g_st = shm_create();
    if (!g_st) return 1;

    // first run init
    memset(g_st, 0, sizeof(*g_st));
    init_pshared_sync(g_st);
    init_aux(g_st);
    init_state(g_st);

    scores_load(g_st, SCORES_FILE);
    logger_start(g_st);
    log_event(g_st, "[server] started fifo=%s", SERVER_FIFO);

    static pthread_t g_sched_tid;
    if (pthread_create(&g_sched_tid, NULL, scheduler_thread, g_st) != 0) {
        perror("pthread_create(scheduler)");
        return 1;
    }

    while (!g_shutdown) {
        msg_t join;

        int rr = read_full(g_fd_r, &join, sizeof(join));
        if (rr == 0) continue; // no writers / EOF style, keep listening
        if (rr < 0) { perror("read(server_fifo)"); 
            break; }
    

    if (join.type != MSG_JOIN) continue;

        int my_id = -1;

        pthread_mutex_lock(&g_st->game_mtx);
        my_id = alloc_slot(g_st, join.name);

        if (my_id >= 0) {
            log_event(g_st, "JOIN name='%s' pid=%d slot=%d",
                      join.name, (int)join.pid, my_id);
        }

        // if game was inactive and now we have 3 players, start (reset board)
        if (my_id >= 0 && active_players(g_st) >= 3) {
            if (g_st->game_active == 0) {
                for (int i = 0; i < MAX_PLAYERS; i++) g_st->positions[i] = 0;
                g_st->winner_id = -1;
            }
            g_st->game_active = 1;
            pthread_cond_broadcast(&g_st->turn_cv);
        }

        pthread_mutex_unlock(&g_st->game_mtx);

        if (my_id < 0) {
            send_reply(join.reply_fifo, "Server full (max 5 players).");
            continue;
        }

        pid_t child = fork();
        if (child == -1) {
            perror("fork");
            send_reply(join.reply_fifo, "Server error: fork failed.");

            pthread_mutex_lock(&g_st->game_mtx);
            g_st->slot[my_id].connected = 0;
            g_st->slot[my_id].name[0] = '\0';
            pthread_mutex_unlock(&g_st->game_mtx);
            continue;
        }

        if (child == 0) {
            close(g_fd_r);
            if (g_fd_w_dummy != -1) close(g_fd_w_dummy);
            session_loop(&join, my_id);
        }

        printf("[server] spawned child pid=%d for '%s'\n", (int)child, join.name);
    }

    log_event(g_st, "[server] shutdown");
    scores_save(g_st, SCORES_FILE);
    logger_stop(g_st);

    


    return 0;
}
