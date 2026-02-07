//wisyal
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

#include "../include/common.h"

static int g_fd_r = -1;
static int g_fd_w_dummy = -1;


static void cleanup(void) {
    if (g_fd_r != -1) close(g_fd_r);
    if (g_fd_w_dummy != -1) close(g_fd_w_dummy);
    unlink(SERVER_FIFO);
}

static void reap_children(int sig) {
    (void)sig;
    int status = 0;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFEXITED(status)) {
            printf("[server] reaped child %d (exit=%d)\n", (int)pid, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[server] reaped child %d (signal=%d)\n", (int)pid, WTERMSIG(status));
        } else {
            printf("[server] reaped child %d\n", (int)pid);
        }
        fflush(stdout);
    }
}

static void handle_sigint(int sig) {
    (void)sig;
    const char msg[] = "\n[server] shutting down (SIGINT)\n";
    ssize_t ignored = write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    (void)ignored;
    exit(0);
}


static void send_reply(const char *reply_fifo, const char *text) {
    msg_t r;
    memset(&r, 0, sizeof(r));
    r.type = MSG_TEXT;
    snprintf(r.text, sizeof(r.text), "%s", text);

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


// Child: handle one client's session from their req_fifo
static void session_loop(const msg_t *join) {
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
        ssize_t n = read(fd_req, &m, sizeof(m));
        if (n == 0) {
            printf("[child %d] client '%s' disconnected (EOF on req fifo)\n", (int)getpid(), join->name);
            send_reply(join->reply_fifo, "Client disconnected.");
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("[child] read");
            break;
        }
        if (n != sizeof(m)) continue;

        if (m.type == MSG_ROLL) {
            int dice = (rand() % 6) + 1;
            char buf[128];
            snprintf(buf, sizeof(buf), "You rolled a %d.", dice);
            send_reply(join->reply_fifo, buf);
        } else if (m.type == MSG_QUIT) {
            printf("[child %d] client '%s' requested quit\n", (int)getpid(), join->name);
            send_reply(join->reply_fifo, "Goodbye!");
            break;
        } else {
            send_reply(join->reply_fifo, "Unknown command.");
        }
    }

    close(fd_req);
    exit(0);
}

int main(void) {
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
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction(SIGCHLD)");
        return 1;
    }

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


    while (1) {
        msg_t join;
        ssize_t n = read(g_fd_r, &join, sizeof(join));
        if (n == 0) continue;
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("read(server_fifo)");
            break;
        }
        if (n != sizeof(join)) continue;

        if (join.type != MSG_JOIN) {
            printf("[server] ignoring non-JOIN on main fifo\n");
            continue;
        }

        printf("[server] JOIN from pid=%d name='%s'\n", (int)join.pid, join.name);

        pid_t child = fork();
        if (child == -1) {
            perror("fork");
            send_reply(join.reply_fifo, "Server error: fork failed.");
            continue;
        }
        if (child == 0) {
            // Child: handle this client
            close(g_fd_r);
            if (g_fd_w_dummy != -1) close(g_fd_w_dummy);
            session_loop(&join);
        }


        // Parent continues accepting JOINs
        printf("[server] spawned child pid=%d for '%s'\n", (int)child, join.name);
    }

    
    return 0;
}
