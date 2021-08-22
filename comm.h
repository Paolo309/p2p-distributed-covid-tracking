#ifndef COMM_H
#define COMM_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "data.h"
#include "network.h"

#define BUFF_SIZE 1024
#define MSG_HEADER_LEN 5
#define MAX_BODY_LEN 256

typedef struct Message {
    uint8_t type;
    uint32_t body_len;
    char body[MAX_BODY_LEN];
} Message;

void serialize_message(char *dest, Message *msgp);
void deserialize_message(char *src, Message *msgp);

int send_message_to(int sd, Message *msgp, struct sockaddr_in* to);
int send_message(int sd, Message *msgp);

int recv_message_from(int sd, Message *msgp, struct sockaddr_in* from);
int recv_message(int sd, Message *msgp);

#endif
