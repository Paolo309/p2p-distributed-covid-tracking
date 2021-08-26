#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "network.h"
#include "data.h"
#include "comm.h"
#include "commandline.h"


int validate_port(const char *str_port)
{
    char *eptr;
    int raw_port = strtol(str_port, &eptr, 10);
    
    /* checking if input is not a number */
    if (eptr == str_port)
    {
        printf("invalid port value \"%s\" (not a number)\n", str_port);
        return -1;
    }
    
    /* checking if port is inside valid range of 0-65535 */
    if (raw_port < 0 || raw_port > UINT16_MAX)
    {
        printf("invalid port number %d (\"%s\")\n", raw_port, str_port);
        printf("port must be a valid value between 0 and %d\n", UINT16_MAX);
        return -1;
    }

    return raw_port;
}


#define STATE_OFF 0
#define STATE_STARTING 1
#define STATE_STARTED 2

typedef struct Peer {
    struct sockaddr_in addr;
    fd_set sockets;
    int dserver, fdmax;
    int state;
    struct timeval timeout, *actual_timeout;
} Peer;

void init_peer(Peer *peer, int host_port)
{
    memset(&peer->addr, 0, sizeof(peer->addr));
    peer->addr.sin_family = AF_INET;
    peer->addr.sin_port = htons(host_port);
    peer->addr.sin_addr.s_addr = INADDR_ANY;

    FD_ZERO(&peer->sockets);
    peer->fdmax = 0;
    peer->dserver = -1;
    peer->state = STATE_OFF;
}

void add_desc(Peer *peer, int fd)
{
    FD_SET(fd, &peer->sockets);
    
    if (fd > peer->fdmax)
        peer->fdmax = fd;
}

void remove_desc(Peer *peer, int fd)
{
    FD_CLR(fd, &peer->sockets);
    
    if (fd == peer->fdmax)
    {
        while (FD_ISSET(peer->fdmax, &peer->sockets) == false)
            peer->fdmax--;
    }
}

void set_timeout(Peer *peer, int seconds)
{
    peer->timeout.tv_sec = seconds;
    peer->timeout.tv_usec = 0;
    peer->actual_timeout = &peer->timeout;
}

void clear_timeout(Peer *peer)
{
    peer->actual_timeout = NULL;
}

void enable_user_input(Peer *peer)
{
    add_desc(peer, STDIN_FILENO);
}

void disable_user_input(Peer *peer)
{
    remove_desc(peer, STDIN_FILENO);
}



int send_start_msg_to_dserver(Peer *peer)
{
    Message msg;
    int ret;

    msg.type = MSG_START;
    sprintf(msg.body, "Hi from %d", peer->addr.sin_port);
    msg.body_len = strlen(msg.body) + 1;

    ret = send_message(peer->dserver, &msg);
    if (ret == -1)
    {
        printf("could not send start message to discover server\n");
        return -1;
    }

    peer->state = STATE_STARTING;

    set_timeout(peer, 3);
    disable_user_input(peer);
    add_desc(peer, peer->dserver);

    printf("waiting for server response...\n");

    return 0;
}





/* ########## FUNCTIONS THAT HANDLE USER COMMANDS ########## */

int cmd_start(Peer *peer, int argc, char **argv)
{
    struct sockaddr_in server_addr;
    int ret;

    if (peer->state != STATE_OFF)
    {
        printf("peer already started\n");
        return 0;
    }

    if (argc != 3)
    {
        printf("usage: %s <address> <port>\n", argv[0]);
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;

    ret = inet_pton(AF_INET, argv[1], &server_addr.sin_addr);
    if (ret == 0)
    {
        printf("%s: invalid address \"%s\"\n", argv[0], argv[1]);
        return -1;
    }

    ret = validate_port(argv[2]);
    if (ret == -1)
        return -1;

    server_addr.sin_port = htons(ret);

    peer->dserver = socket(AF_INET, SOCK_DGRAM, 0);

    if (peer->dserver == -1)
    {
        perror("socket error");
        return -1;
    }

    ret = connect(peer->dserver, (struct sockaddr *)&server_addr, sizeof(server_addr));

    if (ret == -1)
    {
        perror("connect error");
        return -1;
    }

    return send_start_msg_to_dserver(peer);
}

int cmd_add(Peer *peer, int argc, char **argv)
{
    return -1;
}

int cmd_get(Peer *peer, int argc, char **argv)
{
    return -1;
}

int cmd_stop(Peer *peer, int argc, char **argv)
{
    return -1;
}

#define NUM_CMDS 4

char *cmd_str[NUM_CMDS] = { "start", "add", "get", "stop" };

int (*cmd_func[NUM_CMDS])(Peer*, int, char**) = 
    { &cmd_start, &cmd_add, &cmd_get, &cmd_stop };



/* ########## DEMULTIPLEXING ########## */

void demux_user_input(Peer *peer)
{
    int i, argc;
    char **argv;

    argv = get_command_line(&argc);

    if (argv == NULL)
    {
        /* TODO what to do? */
        printf("cmd line error?\n");
        return;
    }
    
    for (i = 0; i < NUM_CMDS; i++)
    {
        if (strcmp(argv[0], cmd_str[i]) == 0)
        {
            cmd_func[i](peer, argc, argv);
            return;
        }
    }
    
    printf("unknown command \"%s\"\n", argv[0]);

    free_command_line(argc, argv);
}

void demux_peer_request(Peer *peer, int sd)
{
    printf("demultiplexing peer request\n");
}


void demux_server_request(Peer *peer)
{
    /* TODO actually demultiplex */

    Message msg;
    int ret;

    printf("demultiplexing server request\n");

    ret = recv_message(peer->dserver, &msg);

    if (ret == -1)
    {
        if (peer->state == STATE_STARTING && errno == ECONNREFUSED)
        {
            printf("trying again in 3 seconds...\n");
            sleep(3);
            send_start_msg_to_dserver(peer);
            return;
        }

        printf("error while receiving message from server\n");
        return;
    }

    printf("received type %d\nbody length %d\n%s\n", msg.type, msg.body_len, msg.body);

    if (msg.type == MSG_SET_NBRS)
    {
        if (peer->state != STATE_STARTING) return;

        printf("SETTING NEIGHBORS\n");

        peer->state = STATE_STARTED;

        clear_timeout(peer);
        enable_user_input(peer);
    }
}



int main(int argc, char** argv)
{
    Peer peer;
    fd_set working_set;
    int ret, i, desc_ready;

    if (argc != 2)
    {
        printf("usage: ./peer <porta>\n");
        exit(EXIT_FAILURE);
    }

    ret = validate_port(argv[1]);
    if (ret == -1) 
        exit(EXIT_FAILURE);

    printf("PEER %d\n", ret);

    init_peer(&peer, ret);

    add_desc(&peer, STDIN_FILENO);

    for (;;)
    {
        working_set = peer.sockets;

        desc_ready = select(peer.fdmax + 1, &working_set, NULL, NULL, peer.actual_timeout);

        if (desc_ready == -1)
        {
            perror("select error");
            exit(EXIT_FAILURE);
        }
        
        if (desc_ready == 0)
        {
            if (peer.state == STATE_STARTING)
                send_start_msg_to_dserver(&peer);
        }

        for (i = 0; i <= peer.fdmax && desc_ready > 0; i++)
        {
            if (!FD_ISSET(i, &working_set)) continue;

            desc_ready--;

            if      (i == STDIN_FILENO) demux_user_input(&peer);
            else if (i == peer.dserver) demux_server_request(&peer);
            else                        demux_peer_request(&peer, i);
        }
    }
}
