#ifndef COMM_H
#define COMM_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>

#include "data.h"
#include "network.h"

#define BUFSIZE 1024
#define MSG_HEADER_LEN 13
#define MAX_BODY_LEN 512

#define MSG_START 0
#define MSG_SET_NBRS 1
#define MSG_STOP 3
#define MSG_ADD_ENTRY 4
#define MSG_REQ_DATA 5
#define MSG_REPLY_DATA 6
#define MSG_FLOOD_FOR_ENTRIES 7
#define MSG_ENTRIES_FOUND 8
#define MSG_REQ_ENTRIES 9
#define MSG_REPLY_ENTRIES 10

typedef struct Message {
    uint8_t type;
    uint32_t body_len;
    uint32_t req_num;
    uint32_t id;
    char body[MAX_BODY_LEN];
} Message;

void serialize_message(char *dest, Message *msgp);
void deserialize_message(char *src, Message *msgp);

int send_message_to(int sd, Message *msgp, struct sockaddr_in* to);
int send_message(int sd, Message *msgp);

int recv_message_from(int sd, Message *msgp, struct sockaddr_in* from);
int recv_message(int sd, Message *msgp);

#endif
