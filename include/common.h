#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <sys/types.h>

#define SERVER_FIFO "/tmp/snakes_server_fifo"
#define MAX_NAME    32
#define MAX_TEXT    8192
#define FIFO_PATH   64

typedef enum {
    MSG_JOIN = 1,
    MSG_ROLL = 2,
    MSG_QUIT = 3,
    MSG_TEXT = 4,
    MSG_ERR  = 5
} msg_type_t;

typedef struct {
    msg_type_t type;
    pid_t      pid;
    char       name[MAX_NAME];
    char       req_fifo[FIFO_PATH];
    char       reply_fifo[FIFO_PATH];
    int32_t    value;
    char       text[MAX_TEXT];
} msg_t;

#endif
