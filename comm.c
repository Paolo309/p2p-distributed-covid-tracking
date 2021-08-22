/*
types of messages:
name                | body                    | description
--------------------|-------------------------|-----------------------------
START                addr and port             connect to ds (register peer)
SET_NBRS             list of nbrs              set nbrs list
STOP                 addr and port (?)         unconnect from ds (unregister peer)
ADD_ENTRY            list: entry key, value    tell peer to add specified entries
REQ_DATA             aggr data req             ask peer for aggr data
REPLY_DATA           aggr data res             reply with aggr data or null
FLOOD_FOR_ENTRIES    list: entry key           ask peer to look for entries owners
ENTRIES_FOUND        list: entry key, owner    tell peer entries owners
REQ_ENTRIES          list: entry key           ask for entries to owner peer
REPLY_ENTRIES        list: entry key, value    tell peer owned entries 

protocols for sending lists:

*/

#include "comm.h"

char _buffer[BUFF_SIZE];

void serialize_message(char *dest, Message *msgp)
{
    *(uint8_t*)(dest) = msgp->type;
    *(uint32_t*)(dest + 1) = htonl(msgp->body_len);
    memcpy(dest + MSG_HEADER_LEN, msgp->body, msgp->body_len);
}

void deserialize_message(char *src, Message *msgp)
{
    msgp->type = *(uint8_t*)src;
    msgp->body_len = ntohl(*(uint32_t*)(src + 1));
    memcpy(msgp->body, src + MSG_HEADER_LEN, msgp->body_len);
}


int send_message_to(int sd, Message *msgp, struct sockaddr_in* to)
{
    int ret, len;
    
    if (msgp == NULL) return -1;
    
    serialize_message(_buffer, msgp);
    
    len = MSG_HEADER_LEN + msgp->body_len;
    
    ret = sendto(sd, _buffer, len, 0, (struct sockaddr*)to, sizeof(*to));
    
    if (ret < len)
    {
        if (ret == -1) perror("sendto error: ");
        else printf("sendto error: sent %d bytes instead of %d\n", ret, len);
        return -1;
    }
    
    return ret;
}

int send_message(int sd, Message *msgp)
{
    return send_message_to(sd, msgp, NULL);
}

int recv_message_from(int sd, Message *msgp, struct sockaddr_in* from)
{
    int ret;
    socklen_t socklen, *lenp;
    
    if (msgp == NULL) return -1;
    
    socklen = sizeof(*from);
    
    if (from != NULL) lenp = &socklen;
    else lenp = NULL;
    
    ret = recvfrom(sd, _buffer, BUFF_SIZE, 0, (struct sockaddr*)from, lenp);
    
    if (ret == -1) 
    {
        perror("recvfrom error: ");
        return -1;
    }
    
    if (ret < MSG_HEADER_LEN)
    {
        if (ret == -1) perror("recvfrom error: ");
        else printf("recvfrom error: received %d bytes instead of %d (header length)\n", ret, MSG_HEADER_LEN);
        return -1;
    }
    
    deserialize_message(_buffer, msgp);
    
    /* post length-check */
    
    if (ret < MSG_HEADER_LEN + msgp->body_len)
    {
        printf("recvfrom error: received BODY length %d bytes instead of %d\n", ret - MSG_HEADER_LEN, msgp->body_len);
        return -1;
    }
    
    return ret;
}

int recv_message(int sd, Message *msgp)
{
    return recv_message_from(sd, msgp, NULL);
}
