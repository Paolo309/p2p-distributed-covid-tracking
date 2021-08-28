#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>

#include "network.h"
#include "data.h"
#include "comm.h"
#include "commandline.h"
#include "graph.h"

/* comment out the next two defines to user the real date */
#define FAKE_DAY 10
#define FAKE_MONTH 2

#define REG_CLOSING_HOUR 23

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

typedef struct ThisPeer {
    struct sockaddr_in addr;
    fd_set sockets;
    int dserver, fdmax;
    int state;
    struct timeval timeout, *actual_timeout;
    Graph neighbors;
    EntryList entries;
} ThisPeer;

void init_peer(ThisPeer *peer, int host_port)
{
    memset(&peer->addr, 0, sizeof(peer->addr));
    peer->addr.sin_family = AF_INET;
    peer->addr.sin_port = htons(host_port);
    peer->addr.sin_addr.s_addr = INADDR_ANY;

    FD_ZERO(&peer->sockets);
    peer->fdmax = 0;
    
    peer->dserver = -1;
    peer->state = STATE_OFF;
    peer->actual_timeout = NULL;

    create_graph(&peer->neighbors);
}

void add_desc(ThisPeer *peer, int fd)
{
    FD_SET(fd, &peer->sockets);
    
    if (fd > peer->fdmax)
        peer->fdmax = fd;
}

void remove_desc(ThisPeer *peer, int fd)
{
    FD_CLR(fd, &peer->sockets);
    
    if (fd == peer->fdmax)
    {
        while (FD_ISSET(peer->fdmax, &peer->sockets) == false)
            peer->fdmax--;
    }
}

void set_timeout(ThisPeer *peer, int seconds)
{
    peer->timeout.tv_sec = seconds;
    peer->timeout.tv_usec = 0;
    peer->actual_timeout = &peer->timeout;
}

void clear_timeout(ThisPeer *peer)
{
    peer->actual_timeout = NULL;
}

void enable_user_input(ThisPeer *peer)
{
    add_desc(peer, STDIN_FILENO);
}

void disable_user_input(ThisPeer *peer)
{
    remove_desc(peer, STDIN_FILENO);
}



int send_start_msg_to_dserver(ThisPeer *peer)
{
    Message msg;
    int ret;

    msg.type = MSG_START;
    msg.body_len = 0;

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

int cmd_start(ThisPeer *peer, int argc, char **argv)
{
    struct sockaddr_in server_addr;
    int ret;

    if (peer->state != STATE_OFF)
    {
        printf("peer already started\n");
        return 0;
    }

    /* reading the arguments (address and port) */

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

    /* creating and connecting UDP socket */

    peer->dserver = socket(AF_INET, SOCK_DGRAM, 0);
    if (peer->dserver == -1)
    {
        perror("socket error");
        return -1;
    }

    /* binding the socket to the port assigned to this peer lets the server
    know the port assigned to this peer by the user, and allows an easier
    management of communication between server and peer, both server-side
    and client-side */
    /* N.B. a TCP and a UDP socket can be bound to the same port */
    ret = bind(peer->dserver, (struct sockaddr*)&peer->addr, sizeof(peer->addr));
    if (ret == -1)
    {
        perror("bind error");
    }

    ret = connect(peer->dserver, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (ret == -1)
    {
        perror("connect error");
        return -1;
    }

    return send_start_msg_to_dserver(peer);
}

#define ADD_TYPE_TAMPONI "t"
#define ADD_TYPE_NCASI "c"

int cmd_add(ThisPeer *peer, int argc, char **argv)
{
    time_t now;
    struct tm *timeinfo;
    /* char str_time[TIMESTAMP_STRLEN]; */
    Entry *tmp_entry;
    int32_t tmp_tamponi, tmp_ncasi;

    if (argc != 3)
    {
        printf("usage: %s <type> <quantity>\n", argv[0]);
        return -1;
    }

    tmp_tamponi = tmp_ncasi = 0;

    if (strcmp(argv[1], ADD_TYPE_TAMPONI) == 0)
    {
        tmp_tamponi = atoi(argv[2]);
    }
    else if (strcmp(argv[1], ADD_TYPE_NCASI) == 0)
    {
        tmp_ncasi = atoi(argv[2]);
    }
    else
    {
        printf("invalid type \"%s\"\n", argv[1]);
        return -1;
    }

    time(&now);
    timeinfo = localtime(&now);

    /* COMMENTED OUT FOR TESTING */
    if (timeinfo->tm_hour >= REG_CLOSING_HOUR)
        timeinfo->tm_mday++;

    timeinfo->tm_hour = timeinfo->tm_min = timeinfo->tm_sec = 0;

    #ifdef FAKE_DAY
    timeinfo->tm_mday = FAKE_DAY;
    #endif

    #ifdef FAKE_MONTH
    timeinfo->tm_mon = FAKE_MONTH;
    #endif

    /* TODO ugly: can directly pass time_t to create_entry */
    /* strftime(str_time, TIMESTAMP_STRLEN, "%Y-%m-%d", timeinfo); */
    
    tmp_entry = create_entry(mktime(timeinfo), tmp_tamponi, tmp_ncasi, 0);
    add_entry(&peer->entries, tmp_entry);

    print_entries_asc(&peer->entries);

    /* TODO remove memory leak in data when adding existing entry */

    return -1;
}

int cmd_get(ThisPeer *peer, int argc, char **argv)
{
    return -1;
}

int cmd_stop(ThisPeer *peer, int argc, char **argv)
{
    Message msg;
    int ret;

    msg.type = MSG_STOP;
    msg.body_len = 0;

    ret = send_message(peer->dserver, &msg);

    if (ret == -1)
    {
        printf("could not send stop message to discovery server\n");
        return -1;
    }

    /* TODO send all records to other peers */

    return 0;
}

#define NUM_CMDS 4

char *cmd_str[NUM_CMDS] = { "start", "add", "get", "stop" };

int (*cmd_func[NUM_CMDS])(ThisPeer*, int, char**) = 
    { &cmd_start, &cmd_add, &cmd_get, &cmd_stop };



/* ########## FUNCTIONS THAT HANDLE SERVER REQUESTS ########## */

void handle_failed_connection_attempt(ThisPeer *peer)
{
    printf("trying again in 3 seconds...\n");
    sleep(3);
    send_start_msg_to_dserver(peer);
}

void handle_set_neighbors_response(ThisPeer *peer, Message *msgp)
{
    /* int ret; */

    if (peer->state == STATE_OFF) return;

    if (peer->state == STATE_STARTING)
        printf("setting list of neighbors\n");
    else
        printf("refreshing list of neighbors\n");

    /* TODO free properly old list of peers */
    peer->neighbors.first = peer->neighbors.last = NULL;
    deserialize_peers(msgp->body, &peer->neighbors.first);
    print_peers(peer->neighbors.first);

    peer->state = STATE_STARTED;

    clear_timeout(peer);
    enable_user_input(peer);
}


/* ########## DEMULTIPLEXING ########## */

void demux_user_input(ThisPeer *peer)
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

void demux_peer_request(ThisPeer *peer, int sd)
{
    printf("demultiplexing peer request\n");
}

void demux_server_request(ThisPeer *peer)
{
    Message msg;
    int ret;

    printf("demultiplexing server request\n");

    ret = recv_message(peer->dserver, &msg);

    if (ret == -1)
    {
        if (peer->state == STATE_STARTING && errno == ECONNREFUSED)
            handle_failed_connection_attempt(peer);
        else
            printf("error while receiving message from server\n");

        return;
    }

    if (msg.type == MSG_SET_NBRS)
    {
        handle_set_neighbors_response(peer, &msg);
    }
}



int main(int argc, char** argv)
{
    ThisPeer peer;
    fd_set working_set;
    int ret, i, desc_ready;
    char register_file[30];

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

    /* TODO check if file was opened successfully */
    sprintf(register_file, "reg_%d.txt", ret);
    load_register_from_file(&peer.entries, register_file);
    print_entries_asc(&peer.entries);

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
