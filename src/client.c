//afiq
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

static void make_fifo_path(char *out, size_t outsz, const char *prefix, pid_t pid)
{
    snprintf(out, outsz, "/tmp/%s_%d.fifo", prefix, (int)pid);
}

static int make_fifo(const char *path)
{
    if (mkfifo(path, 0666) == -1) {
        if (errno != EEXIST) return -1;
    }
    return 0;
}

static void read_name(char *out, size_t n)
{
    printf("Enter name: ");
    fflush(stdout);

    if (!fgets(out, (int)n, stdin)) {
        snprintf(out, n, "player");
        return;
    }

    out[strcspn(out, "\n")] = '\0'; // remove newline
    if (out[0] == '\0')
        snprintf(out, n, "player");
}

static int read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    char *p = buf;

    while (got < len) {
        ssize_t n = read(fd, p + got, len - got);
        if (n == 0) return 0;              // EOF
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)n;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t len)
{
    size_t sent = 0;
    const char *p = buf;

    while (sent < len) {
        ssize_t n = write(fd, p + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 1;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    char name_buf[MAX_NAME];
    read_name(name_buf, sizeof(name_buf));

    pid_t pid = getpid();

    char req_fifo[FIFO_PATH];
    char reply_fifo[FIFO_PATH];
    make_fifo_path(req_fifo, sizeof(req_fifo), "snakes_req", pid);
    make_fifo_path(reply_fifo, sizeof(reply_fifo), "snakes_reply", pid);

    if (make_fifo(req_fifo) == -1) {
        perror("mkfifo(req)");
        return 1;
    }
    if (make_fifo(reply_fifo) == -1) {
        perror("mkfifo(reply)");
        unlink(req_fifo);
        return 1;
    }

    // Build JOIN
    msg_t join;
    memset(&join, 0, sizeof(join));
    join.type = MSG_JOIN;
    join.pid  = pid;
    snprintf(join.name, sizeof(join.name), "%s", name_buf);
    snprintf(join.req_fifo, sizeof(join.req_fifo), "%s", req_fifo);
    snprintf(join.reply_fifo, sizeof(join.reply_fifo), "%s", reply_fifo);

    // Send JOIN to server
    int fd_srv = open(SERVER_FIFO, O_WRONLY);
    if (fd_srv == -1) {
        perror("open(SERVER_FIFO)");
        fprintf(stderr, "Is the server running?\n");
        unlink(req_fifo);
        unlink(reply_fifo);
        return 1;
    }

    if (write_full(fd_srv, &join, sizeof(join)) != 1) {
        perror("write(JOIN)");
        close(fd_srv);
        unlink(req_fifo);
        unlink(reply_fifo);
        return 1;
    }
    close(fd_srv);

    printf("[client] Joined as '%s' pid=%d\n", name_buf, (int)pid);
    printf("[client] req_fifo=%s\n", req_fifo);
    printf("[client] reply_fifo=%s\n", reply_fifo);
    printf("Controls: ENTER=roll, q + ENTER=quit\n");

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

    // Read initial welcome message
    msg_t r;
    int rr = read_full(fd_reply, &r, sizeof(r));
    if (rr <= 0) {
    printf("[client] server closed reply channel\n");
    close(fd_req);
    close(fd_reply);
    unlink(req_fifo);
    unlink(reply_fifo);
    return 1;
    }
    printf("[server] %s\n", r.text);

    char line[64];
    while (1) {
        printf("Press ENTER to roll (or type q then ENTER to quit): ");
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) break;

        msg_t m;
        memset(&m, 0, sizeof(m));
        m.pid = pid;

        // if user typed q/Q -> quit; otherwise roll
        if (line[0] == 'q' || line[0] == 'Q') {
            m.type = MSG_QUIT;
        } else {
            m.type = MSG_ROLL;
        }

        if (write_full(fd_req, &m, sizeof(m)) != 1) {
            perror("write(req)");
            break;
        }

        // Read reply from server
        msg_t reply;
        int rrc = read_full(fd_reply, &reply, sizeof(reply));
        if (rrc <= 0) {
        printf("[client] server closed reply channel\n");
        break;
        }
        printf("[server] %s\n", reply.text);

        if (m.type == MSG_QUIT) break;
    }

    close(fd_req);
    close(fd_reply);
    unlink(req_fifo);
    unlink(reply_fifo);
    return 0;
}
