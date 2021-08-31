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

char _buffer[BUFSIZE];

/**
 * Serialize message into a buffer.
 * 
 * @param dest pointer to the destination buffer
 * @param msgp pointer to the message to serialize
 */
void serialize_message(char *dest, Message *msgp)
{
    *(uint8_t*)(dest) = msgp->type;
    *(uint32_t*)(dest + 1) = htonl(msgp->body_len);
    *(uint32_t*)(dest + 5) = htonl(msgp->req_num);
    *(uint32_t*)(dest + 9) = htonl(msgp->id);
    memcpy(dest + MSG_HEADER_LEN, msgp->body, msgp->body_len);
}

/**
 * Deserialize message from a buffer.
 * 
 * @param src pointer to the source buffer
 * @param msgp pointer to where to put the message
 */
void deserialize_message(char *src, Message *msgp)
{
    msgp->type = *(uint8_t*)src;
    msgp->body_len = ntohl(*(uint32_t*)(src + 1));
    msgp->req_num  = ntohl(*(uint32_t*)(src + 5));
    msgp->id       = ntohl(*(uint32_t*)(src + 9));
    memcpy(msgp->body, src + MSG_HEADER_LEN, msgp->body_len);
}

/**
 * Send message to destination, which is implicit in case of a connected socket,
 * or specified by *to otherwise. Leave to as NULL if the socket is connected.
 * In case of of error, errno is undefined.
 * 
 * @param sd socket descriptor
 * @param msgp message to send
 * @param to (optional) destination address
 * @return -1 on error, the number of bytes sent otherwise
 */
int send_message_to(int sd, Message *msgp, struct sockaddr_in* to)
{
    int ret, len;
    int save_errno;
    
    if (msgp == NULL) return -1;
    
    serialize_message(_buffer, msgp);
    
    len = MSG_HEADER_LEN + msgp->body_len;
    
    ret = sendto(sd, _buffer, len, 0, (struct sockaddr*)to, sizeof(*to));
    save_errno = errno;    

    if (ret < len)
    {
        if (ret == -1) perror("sendto error");
        else printf("sendto error: sent %d bytes instead of %d\n", ret, len);
        errno = save_errno;
        return -1;
    }
    
    return ret;
}

/**
 * Send message to host connected to socket sd.
 * 
 * @param sd connected socket descriptor
 * @param msgp message to send 
 * @return -1 on error, the number of bytes sent otherwise 
 */
int send_message(int sd, Message *msgp)
{
    return send_message_to(sd, msgp, NULL);
}

/**
 * Receive a message from source, which is implicit in case of a connected socket,
 * or specified by *from otherwise. Leave from as NULL if the socket is connected.
 * In case of error, errno is undefined.
 * 
 * @param sd socket descriptor
 * @param msgp where to put the received message
 * @param from (optional) source address
 * @return -1 on error, the number of bytes received otherwise
 */
int recv_message_from(int sd, Message *msgp, struct sockaddr_in* from)
{
    int ret;
    socklen_t socklen, *lenp;
    int save_errno;
    
    if (msgp == NULL) return -1;
    
    if (from != NULL) 
    {
        socklen = sizeof(*from);
        lenp = &socklen;
    }
    else 
        lenp = NULL;
    
    ret = recvfrom(sd, _buffer, BUFSIZE, 0, (struct sockaddr*)from, lenp);
    save_errno = errno;
    
    if (ret < MSG_HEADER_LEN)
    {
        if (ret == -1) perror("recvfrom error");
        else printf("recvfrom error: received %d bytes instead of %d (header length)\n", ret, MSG_HEADER_LEN);
        errno = save_errno;
        return -1;
    }
    
    deserialize_message(_buffer, msgp);
    
    if (ret < MSG_HEADER_LEN + msgp->body_len)
    {
        printf("recvfrom error: received BODY length %d bytes instead of %d\n", ret - MSG_HEADER_LEN, msgp->body_len);
        errno = save_errno;
        return -1;
    }
    
    return ret;
}

/**
 * Receive message from host connected to socket sd.
 * 
 * @param sd connected socket descriptor
 * @param msgp where to put the received message 
 * @return -1 on error, the number of bytes received otherwise 
 */
int recv_message(int sd, Message *msgp)
{
    return recv_message_from(sd, msgp, NULL);
}


void server()
{
    int ret, sd, count;
    struct sockaddr_in server_addr, client_addr;
    Message msg;
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(4242);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    /* inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr); */
    
    /* SERVE??? */
    memset(&client_addr, 0, sizeof(client_addr));
    
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    
    ret = bind(sd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret == -1)
    {
        perror("bind error");
        exit(EXIT_FAILURE);
    }
    
    /* ret = listen(sd, 1);
    if (ret == -1)
    {
        perror("listen error");
        exit(EXIT_FAILURE);
    }
    
    len = sizeof(client_addr);
    conn = accept(sd, (struct sockaddr*)&client_addr, &len);
    if (conn == -1)
    {
        perror("accept error");
        exit(EXIT_FAILURE);
    } */
        
    for (count = 0; count < 5; count++)
    {
        recv_message_from(sd, &msg, &client_addr);
        
        printf("received header (%d, %d)\nbody: \"%s\"\n", msg.type, msg.body_len, msg.body);
        
        memset(&msg, 'a', sizeof(msg));
        
        sprintf(msg.body, "Greeting from server %d", count);
        msg.body_len = strlen(msg.body) + 1;
        
        send_message_to(sd, &msg, &client_addr);
    }
}

void client()
{
    int ret, sd, count;
    struct sockaddr_in server_addr;
    Message msg;
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(4242);
    inet_pton(AF_INET, "127.0.0.1", &server_addr.sin_addr);
    
    sd = socket(AF_INET, SOCK_DGRAM, 0);
    
    ret = connect(sd, (struct sockaddr*)&server_addr, sizeof(server_addr));
    if (ret == -1)
    {
        perror("connect error");
        exit(EXIT_FAILURE);
    }
    
    for (count = 0; count < 5; count++)
    {
        msg.type = 3;
        sprintf(msg.body, "Hi from client %d", count);
        msg.body_len = strlen(msg.body) + 1;
        
        send_message(sd, &msg);
        
        printf("send %s\n", msg.body);
        
        memset(&msg, 'a', sizeof(msg));
        
        recv_message(sd, &msg);
        
        printf("received header (%d, %d)\nbody: \"%s\"\n", msg.type, msg.body_len, msg.body);
    }
}

int main_test_comm(int argc, char **argv)
{
    if (argc != 2)
    {
        printf("usage: comm <C|S>\n");
        exit(EXIT_FAILURE);
    }
    
    if      (argv[1][0] == 'C') client();
    else if (argv[1][0] == 'S') server();
    else
    {
        printf("unknown %c\n", argv[1][0]);
        exit(EXIT_FAILURE);
    }
    
    return 0;
}
