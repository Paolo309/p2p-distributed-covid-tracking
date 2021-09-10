#include "comm.h"

char _buffer[BUFSIZE];

/**
 * Serialize message into a buffer. If msgp->len is 0, then
 * msgp->body must be NULL.
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

    if (msgp->body != NULL)
        memcpy(dest + MSG_HEADER_LEN, msgp->body, msgp->body_len);
}

/**
 * Deserialize message from a buffer. Allocates message body's buffer with
 * size specified by body_len.
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

    msgp->body = malloc(msgp->body_len);
    if (msgp->body != NULL)
        memcpy(msgp->body, src + MSG_HEADER_LEN, msgp->body_len);
}

/**
 * Send message to destination, which is implicit in case of a connected socket,
 * or specified by *to otherwise. Leave to = NULL if the socket is connected.
 * In case of of error, errno is maintained.
 * 
 * @param sd socket descriptor
 * @param msgp message to send
 * @param to (optional) destination address
 * @return -1 on error, the number of bytes sent otherwise
 */
int send_message_to(int sd, Message *msgp, struct sockaddr_in* to)
{
    int ret, len;
    int saved_errno;
    int tot_sent;
    
    if (msgp == NULL) return -1;
    
    len = MSG_HEADER_LEN + msgp->body_len;

    if (len > BUFSIZE)
    {
        printf("cannot send message: message too large ()\n");
        exit(EXIT_FAILURE);
        return -1;
    }

    serialize_message(_buffer, msgp);
        
    tot_sent = 0;
    do
    {
        ret = sendto(sd, _buffer + tot_sent, len - tot_sent, 0, (struct sockaddr*)to, sizeof(*to));
        saved_errno = errno;
        tot_sent += ret;

    } while (ret > 0 && tot_sent < len);

    if (ret != 0 && ret < len)
    {
        if (ret == -1) perror("sendto error");
        else printf("sendto error: sent %d bytes instead of %d\n", ret, len);
        errno = saved_errno;
        return -1;
    }

    if (ret == 0)
    {
        printf("sendto: connection closed\n");
        return 0;
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
 * In case of error, errno is maintained.
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
    int saved_errno;
    int expected_len, tot_recvd;
    
    if (msgp == NULL) return -1;
    
    if (from != NULL) 
    {
        socklen = sizeof(*from);
        lenp = &socklen;
    }
    else 
        lenp = NULL;


    expected_len = 0;
    tot_recvd = 0;
    do
    {
        ret = recvfrom(sd, _buffer + tot_recvd, BUFSIZE - tot_recvd, 0, (struct sockaddr*)from, lenp);
        saved_errno = errno;
        tot_recvd += ret;
        
        if (expected_len == 0 && ret > MSG_HEADER_LEN)
        {
            expected_len = ntohl(*(uint32_t*)(_buffer + 1)) + MSG_HEADER_LEN;
        }

    } while (ret > 0 && tot_recvd < expected_len && tot_recvd < BUFSIZE);
    

    if (ret != 0 && tot_recvd < expected_len)
    {
        if (ret == -1) perror("recvfrom error");
        else printf("recvfrom error: received %d bytes instead of %d (header length)\n", ret, MSG_HEADER_LEN);
        errno = saved_errno;
        return -1;
    }

    if (ret == 0)
    {
        printf("recvfrom: connection closed\n");
        return 0;
    }

    deserialize_message(_buffer, msgp);
    
    if (ret < MSG_HEADER_LEN + msgp->body_len)
    {
        printf("recvfrom error: received BODY length %d bytes instead of %d\n", ret - MSG_HEADER_LEN, msgp->body_len);
        errno = saved_errno;
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
