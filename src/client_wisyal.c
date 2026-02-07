//wisyal
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>

#include "../include/common.h"

static void make_fifo_path(char *out, size_t outsz, const char *prefix, pid_t pid) {
    snprintf(out, outsz, "/tmp/%s_%d.fifo", prefix, (int)pid);
}

static int make_fifo(const char *path) {
    if (mkfifo(path, 0666) == -1) {
        if (errno != EEXIST) return -1;
    }
    return 0;
}

int main(int argc, char **argv) {
    const char *name = (argc >= 2) ? argv[1] : "player";
    pid_t pid = getpid();

    char req_fifo[FIFO_PATH];
    char reply_fifo[FIFO_PATH];
    make_fifo_path(req_fifo, sizeof(req_fifo), "snakes_req", pid);
    make_fifo_path(reply_fifo, sizeof(reply_fifo), "snakes_reply", pid);

    if (make_fifo(req_fifo) == -1) { perror("mkfifo(req)"); return 1; }
    if (make_fifo(reply_fifo) == -1) { perror("mkfifo(reply)"); unlink(req_fifo); return 1; }

    // Send JOIN to server
    msg_t join;
    memset(&join, 0, sizeof(join));
    join.type = MSG_JOIN;
    join.pid = pid;
    snprintf(join.name, sizeof(join.name), "%s", name);
    snprintf(join.req_fifo, sizeof(join.req_fifo), "%s", req_fifo);
    snprintf(join.reply_fifo, sizeof(join.reply_fifo), "%s", reply_fifo);

    int fd_srv = open(SERVER_FIFO, O_WRONLY);
    if (fd_srv == -1) {
        perror("open(SERVER_FIFO)");
        fprintf(stderr, "Is the server running?\n");
        unlink(req_fifo);
        unlink(reply_fifo);
        return 1;
    }
    if (write(fd_srv, &join, sizeof(join)) != sizeof(join)) {
        perror("write(JOIN)");
        close(fd_srv);
        unlink(req_fifo);
        unlink(reply_fifo);
        return 1;
    }
    close(fd_srv);

    printf("[client] Joined as '%s' pid=%d\n", name, (int)pid);
    printf("[client] req_fifo=%s\n", req_fifo);
    printf("[client] reply_fifo=%s\n", reply_fifo);
    printf("Type: roll | quit\n");

    // Open reply FIFO
    int fd_reply = open(reply_fifo, O_RDWR);
    if (fd_reply == -1) {
        perror("open(reply_fifo)");
        unlink(req_fifo);
        unlink(reply_fifo);
    return 1;
    }

    // Open request FIFO
    int fd_req = open(req_fifo, O_RDWR);
    if (fd_req == -1) {
        perror("open(req_fifo)");
        close(fd_reply);
        unlink(req_fifo);
        unlink(reply_fifo);
    return 1;
    }

// Read initial welcome message (sent by server on JOIN)
msg_t r;
ssize_t n = read(fd_reply, &r, sizeof(r));
if (n == sizeof(r)) {
    printf("[server] %s\n", r.text);
}


    char line[64];
    while (1) {
        printf("> ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;
        line[strcspn(line, "\n")] = 0;

        msg_t m;
        memset(&m, 0, sizeof(m));
        m.pid = pid;

        if (strcmp(line, "roll") == 0) {
            m.type = MSG_ROLL;
        } else if (strcmp(line, "quit") == 0) {
            m.type = MSG_QUIT;
        } else {
            printf("Commands: roll | quit\n");
            continue;
        }

        if (write(fd_req, &m, sizeof(m)) != sizeof(m)) {
            perror("write(req)");
            break;
        }

        // Read one reply from server
        msg_t r;
        ssize_t n = read(fd_reply, &r, sizeof(r));
        if (n <= 0) {
            printf("[client] server closed reply channel\n");
            break;
        }
        if (n == sizeof(r)) {
            printf("[server] %s\n", r.text);
        }
        if (m.type == MSG_QUIT) break;
    }

    close(fd_req);
    close(fd_reply);
    unlink(req_fifo);
    unlink(reply_fifo);
    return 0;
}
