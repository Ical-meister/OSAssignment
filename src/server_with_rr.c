//afiq with rr
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

#include "../include/common.h"
////////////
#define MAX_PLAYERS 5
#define SHM_NAME "/snl_shm_v1"

typedef struct
{
    int connected; // 1 = active, 0 = inactive
} PlayerSlot;

typedef struct
{
    // Member 3: turn control
    int game_active;
    int current_turn;
    int turn_done;

    PlayerSlot slot[MAX_PLAYERS];

    // Member 3: process-shared synchronization
    pthread_mutex_t game_mtx;
    pthread_cond_t turn_cv;
    pthread_cond_t turn_done_cv;
} SharedState;
/////////////#

static int g_fd_r = -1;
static int g_fd_w_dummy = -1;

//////////////
static int g_shm_fd = -1;
static SharedState *g_st = NULL;
static volatile sig_atomic_t g_shutdown = 0;
/////////////#

////////////
static void cleanup(void)
{
    if (g_fd_r != -1)
        close(g_fd_r);
    if (g_fd_w_dummy != -1)
        close(g_fd_w_dummy);

    if (g_st)
        munmap(g_st, sizeof(*g_st));
    if (g_shm_fd != -1)
        close(g_shm_fd);

    // shm_unlink(SHM_NAME);

    unlink(SERVER_FIFO);
}

static SharedState *shm_create(void)
{
    g_shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (g_shm_fd < 0)
    {
        perror("shm_open");
        return NULL;
    }

    if (ftruncate(g_shm_fd, sizeof(SharedState)) < 0)
    {
        perror("ftruncate");
        return NULL;
    }

    void *p = mmap(NULL, sizeof(SharedState),
                   PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (p == MAP_FAILED)
    {
        perror("mmap");
        return NULL;
    }

    return (SharedState *)p;
}

static void init_pshared_sync(SharedState *st)
{
    pthread_mutexattr_t ma;
    pthread_condattr_t ca;

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
static void init_state(SharedState *st)
{
    st->game_active = 0;
    st->current_turn = MAX_PLAYERS - 1; /////edited for player 0 to start
    st->turn_done = 0;

    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        st->slot[i].connected = 0;
    }
}


static int active_players(SharedState *st)
{
    int c = 0;
    for (int i = 0; i < MAX_PLAYERS; i++)
        if (st->slot[i].connected)
            c++;
    return c;
}

static int next_connected(SharedState *st, int cur)
{
    for (int k = 1; k <= MAX_PLAYERS; k++)
    {
        int idx = (cur + k) % MAX_PLAYERS;
        if (st->slot[idx].connected)
            return idx;
    }
    return -1;
}

static int alloc_slot(SharedState *st)
{
    for (int i = 0; i < MAX_PLAYERS; i++)
    {
        if (!st->slot[i].connected)
        {
            st->slot[i].connected = 1;
            return i;
        }
    }
    return -1;
}

static void *scheduler_thread(void *arg)
{
    SharedState *st = (SharedState *)arg;

    while (!g_shutdown)
    {

        pthread_mutex_lock(&st->game_mtx);

        // 1) Wait until game is active and at least 3 players connected
        while (!g_shutdown && (st->game_active == 0 || active_players(st) < 3))
        {
            pthread_mutex_unlock(&st->game_mtx);
            usleep(20000); // simple wait (no extra condvar needed)
            pthread_mutex_lock(&st->game_mtx);
        }

        if (g_shutdown)
        {
            pthread_mutex_unlock(&st->game_mtx);
            break;
        }

        // 2) Pick next connected player (RR)
        int next = next_connected(st, st->current_turn);

        if (next < 0)
        {
            st->game_active = 0;
            pthread_cond_broadcast(&st->turn_cv);
            pthread_cond_broadcast(&st->turn_done_cv);
            pthread_mutex_unlock(&st->game_mtx);
            continue;
        }

        // 3) Update shared turn state
        st->current_turn = next;
        st->turn_done = 0;

        // 4) Notify all players: new turn started
        pthread_cond_broadcast(&st->turn_cv);

        // 5) Wait until current player finishes turn
        while (!g_shutdown && st->game_active == 1 && st->turn_done == 0)
        {
            pthread_cond_wait(&st->turn_done_cv, &st->game_mtx);
        }

        pthread_mutex_unlock(&st->game_mtx);
        usleep(1000);
    }

    return NULL;
}

//////////////#

static void reap_children(int sig)
{
    (void)sig;
    int status = 0;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
    {
        if (WIFEXITED(status))
        {
            printf("[server] reaped child %d (exit=%d)\n", (int)pid, WEXITSTATUS(status));
        }
        else if (WIFSIGNALED(status))
        {
            printf("[server] reaped child %d (signal=%d)\n", (int)pid, WTERMSIG(status));
        }
        else
        {
            printf("[server] reaped child %d\n", (int)pid);
        }
        fflush(stdout);
    }
}

static void handle_sigint(int sig)
{
    (void)sig;
    const char msg[] = "\n[server] shutting down (SIGINT)\n";
    ssize_t ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    g_shutdown = 1; ///////edited
    exit(0);
}

static void send_reply(const char *reply_fifo, const char *text)
{
    msg_t r;
    memset(&r, 0, sizeof(r));
    r.type = MSG_TEXT;
    snprintf(r.text, sizeof(r.text), "%s", text);

    int fd = open(reply_fifo, O_WRONLY);
    if (fd == -1)
        return;

    size_t total = 0;
    const char *p = (const char *)&r;

    while (total < sizeof(r))
    {
        ssize_t n = write(fd, p + total, sizeof(r) - total);
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        total += (size_t)n;
    }

    close(fd);
}

// Child: handle one client's session from their req_fifo
////////edited
static void session_loop(const msg_t *join, int my_id)
{
    printf("[child %d] session for '%s' req='%s' reply='%s'\n",
           (int)getpid(), join->name, join->req_fifo, join->reply_fifo);

    int fd_req = open(join->req_fifo, O_RDONLY);
    if (fd_req == -1)
    {
        perror("[child] open(req_fifo)");
        exit(1);
    }

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    send_reply(join->reply_fifo, "JOIN accepted. Type 'roll' or 'quit'.");

    while (1)
    {
        msg_t m;
        ssize_t n = read(fd_req, &m, sizeof(m));
        if (n == 0)
        {
            printf("[child %d] client '%s' disconnected (EOF on req fifo)\n", (int)getpid(), join->name);
            send_reply(join->reply_fifo, "Client disconnected.");
            /////////////
            pthread_mutex_lock(&g_st->game_mtx);
            g_st->slot[my_id].connected = 0;

            // If they disconnected on their own turn, release scheduler
            if (g_st->current_turn == my_id)
            {
                g_st->turn_done = 1;
                pthread_cond_signal(&g_st->turn_done_cv);
            }

            // If fewer than 3 active players remain, stop game and wake everyone
            if (active_players(g_st) < 3)
            {
                g_st->game_active = 0;
                pthread_cond_broadcast(&g_st->turn_cv);
                pthread_cond_signal(&g_st->turn_done_cv);
            }
            pthread_mutex_unlock(&g_st->game_mtx);
            ////////////////
            break;
        }
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            perror("[child] read");
            break;
        }
        if (n != sizeof(m))
            continue;

        /////////////
        if (m.type == MSG_ROLL)
        {
            // (A) Turn gate: only allow if it's my turn
            pthread_mutex_lock(&g_st->game_mtx);

            if (g_st->game_active == 0)
            {
                pthread_mutex_unlock(&g_st->game_mtx);
                send_reply(join->reply_fifo, "WAIT");
                continue;
            }

            if (my_id != g_st->current_turn)
            {
                pthread_mutex_unlock(&g_st->game_mtx);
                send_reply(join->reply_fifo, "NOT YOUR TURN");
                continue;
            }

            pthread_mutex_unlock(&g_st->game_mtx);

            // (B) Allowed action (your current dice logic)
            int dice = (rand() % 6) + 1;
            char buf[128];
            snprintf(buf, sizeof(buf), "You rolled a %d.", dice);
            send_reply(join->reply_fifo, buf);

            // (C) Handshake: tell scheduler this turn finished
            pthread_mutex_lock(&g_st->game_mtx);
            g_st->turn_done = 1;
            pthread_cond_signal(&g_st->turn_done_cv);
            pthread_mutex_unlock(&g_st->game_mtx);
        } //////////////

        else if (m.type == MSG_QUIT)
        {
            printf("[child %d] client '%s' requested quit\n", (int)getpid(), join->name);
            send_reply(join->reply_fifo, "Goodbye!");
            /////////////
            pthread_mutex_lock(&g_st->game_mtx);
            g_st->slot[my_id].connected = 0;

            if (g_st->current_turn == my_id)
            {
                g_st->turn_done = 1;
                pthread_cond_signal(&g_st->turn_done_cv);
            }

            if (active_players(g_st) < 3)
            {
                g_st->game_active = 0;
                pthread_cond_broadcast(&g_st->turn_cv);
                pthread_cond_signal(&g_st->turn_done_cv);
            }
            pthread_mutex_unlock(&g_st->game_mtx);

            ////////////////

            break;
        }
        else
        {
            send_reply(join->reply_fifo, "Unknown command.");
        }
    }

    close(fd_req);
    exit(0);
}
//////////#

int main(void)
{
    atexit(cleanup);

    struct sigaction si;
    memset(&si, 0, sizeof(si));
    si.sa_handler = handle_sigint;
    sigemptyset(&si.sa_mask);
    si.sa_flags = SA_RESTART;
    sigaction(SIGINT, &si, NULL);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = reap_children;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
    {
        perror("sigaction(SIGCHLD)");
        return 1;
    }

    if (mkfifo(SERVER_FIFO, 0666) == -1)
    {
        if (errno != EEXIST)
        {
            perror("mkfifo(SERVER_FIFO)");
            return 1;
        }
    }

    printf("[server] listening on %s\n", SERVER_FIFO);

    g_fd_r = open(SERVER_FIFO, O_RDONLY);
    if (g_fd_r == -1)
    {
        perror("open(SERVER_FIFO O_RDONLY)");
        return 1;
    }

    g_fd_w_dummy = open(SERVER_FIFO, O_WRONLY);
    (void)g_fd_w_dummy;

    ////////////

    g_st = shm_create();
    if (!g_st)
        return 1;

    // For first run only; later you can guard with an init flag
    memset(g_st, 0, sizeof(*g_st));
    init_pshared_sync(g_st);
    init_state(g_st);

    pthread_t sched_tid;
    if (pthread_create(&sched_tid, NULL, scheduler_thread, g_st) != 0)
    {
        perror("pthread_create(scheduler)");
        return 1;
    }

    ///////////#

    while (1)
    {
        msg_t join;
        ssize_t n = read(g_fd_r, &join, sizeof(join));
        if (n == 0)
            continue;
        if (n < 0)
        {
            if (errno == EINTR)
                continue;
            perror("read(server_fifo)");
            break;
        }
        if (n != sizeof(join))
            continue;

        if (join.type != MSG_JOIN)
        {
            printf("[server] ignoring non-JOIN on main fifo\n");
            continue;
        }

        printf("[server] JOIN from pid=%d name='%s'\n", (int)join.pid, join.name);

        ///////////

        int my_id = -1;
        pthread_mutex_lock(&g_st->game_mtx);
        my_id = alloc_slot(g_st);
        if (my_id >= 0 && active_players(g_st) >= 3)
        {
            g_st->game_active = 1;
            pthread_cond_broadcast(&g_st->turn_cv);
        }
        pthread_mutex_unlock(&g_st->game_mtx);
        if (my_id < 0)
        {
            send_reply(join.reply_fifo, "Server full (max 5 players).");
            continue;
        }

        ///////////////#
        pid_t child = fork();
        if (child == -1)
        {
            perror("fork");
            send_reply(join.reply_fifo, "Server error: fork failed.");
            //////////
            pthread_mutex_lock(&g_st->game_mtx);
            g_st->slot[my_id].connected = 0;
            pthread_mutex_unlock(&g_st->game_mtx);
            //////////#
            continue;
        }
        if (child == 0)
        {
            // Child: handle this client
            close(g_fd_r);
            if (g_fd_w_dummy != -1)
                close(g_fd_w_dummy);
            session_loop(&join, my_id); ////////edited
        }

        // Parent continues accepting JOINs
        printf("[server] spawned child pid=%d for '%s'\n", (int)child, join.name);
    }

    return 0;
}
