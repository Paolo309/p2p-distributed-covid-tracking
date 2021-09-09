#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "constants.h"
#include "common_utils.h"
#include "network.h"
#include "comm.h"
#include "graph.h"
#include "commandline.h"

#define MAX_NBRS_MSG_SIZE 64

#define ACK_SEND true
#define ACK_DONT_SEND true

/**
 * Tell the a peer who its neighbors are.
 * 
 * @param sd 
 * @param addr address of the peer to which send the list of neighbors
 * @param peer pointer to peer which containst the list of neighbors
 */
void tell_new_neighbors(int sd, struct sockaddr_in *addr, Peer *peer)
{
    int ret;
    Message msg;
    msg.body = malloc(MAX_NBRS_MSG_SIZE);

    printf("telling neighbors to %hd\n", ntohs(addr->sin_port));

    msg.type = MSG_SET_NBRS;
    msg.body_len = serialize_peers(msg.body, peer->node->neighbors) - msg.body;

    ret = send_message_to(sd, &msg, addr);
    if (ret == -1)
    {
        printf("error on sending neighbors\n");
    }

    free(msg.body);
}



/*----------------------------------------------
 |  FUNCTIONS THAT HANDLE USER COMMANDS 
 *---------------------------------------------*/


int cmd_help(Graph *peers, int argc, char **argv)
{
    printf("1) help --> mostra i dettagli dei comandi\n");
    printf("2) showpeers --> mostra un elenco dei peer connessi\n");
    printf("3) showneighbor <peer> --> mostra i neighbor di un peer\n");
    printf("4) esc --> chiude il DSDettaglio comandi\n");

    return 0;
}

int cmd_show_peers(Graph *peers, int argc, char **argv)
{
    print_graph(peers);

    return 0;
}

int cmd_show_neighbor(Graph *peers, int argc, char **argv)
{
    in_port_t port;
    GraphNode *node;

    if (argc != 2)
    {
        printf("usage: %s <peer>\n", argv[0]);
        return -1;
    }

    port = validate_port(argv[1]);

    if (port == -1) return -1;

    node = search_peer_node_by_port(peers->first, port);

    if (node == NULL)
    {
        printf("peer %hd not present\n", port);
        return 0;
    }

    print_peers(node->neighbors);

    return 0;
}

int cmd_esc(Graph *peers, int argc, char **argv)
{
    GraphNode *node;
    Message msg;
    int ret, sd;

    sd = atoi(argv[1]);

    msg.type = MSG_STOP;
    msg.body_len = 0;
    msg.body = NULL;

    node = peers->first;
    while (node)
    {
        ret = send_message_to(sd, &msg, &node->peer->addr);
        if (ret == -1)
        {
            printf(
                "could not send stop message to peer %hd\n", 
                ntohs(node->peer->addr.sin_port)
            );
        }
        node = node->next;
    }

    close(sd);
    exit(EXIT_SUCCESS);

    return 0;
}

#define NUM_CMDS 4

char *cmd_str[NUM_CMDS] = { "help", "showpeers", "showneighbor", "esc" };

int (*cmd_func[NUM_CMDS])(Graph*, int, char**) = 
    { &cmd_help, &cmd_show_peers, &cmd_show_neighbor, &cmd_esc };



/*----------------------------------------------
 |  FUNCTIONS THAT HANDLE PEER REQUESTS
 *---------------------------------------------*/

/* connects the new peer mantaining a circular structure */
void peer_started(Graph *peers, int sd, struct sockaddr_in *client_addr)
{
    Peer *new_peer, *old_last;
    GraphNode *node_p;

    /* get the current last peer in list of peers */
    if (peers->last != NULL)
        old_last = peers->last->peer;
    else
        old_last = NULL;

    new_peer = add_peer(peers, client_addr);

    if (new_peer == NULL)
    {
        printf("peer already present\n");
        return;
    }

    /* only one peer present: no neighbors to assign */
    if (old_last == NULL)
    {
        printf("only one peer present\n");

        tell_new_neighbors(sd, client_addr, new_peer);

        return;
    }

    /* more than two peers present: disconnect last one from second-last */
    if (!cmp_peers(peers->first->next->peer, old_last))
    {
        /* printf("more than two peers present\n"); */
        unset_neighbors(peers, peers->first->peer->node, old_last->node);
    }

    /* connect new peer to second-last */
    set_neighbors(peers, new_peer->node, old_last->node, PUSH_FRONT);
    /* connect new peer with first peer */
    set_neighbors(peers, peers->first, new_peer->node, PUSH_FRONT);


    /* tell new neighbors to new peer and to all its neighbors */

    tell_new_neighbors(sd, client_addr, new_peer);

    node_p = new_peer->node->neighbors;
    while (node_p)
    {
        tell_new_neighbors(sd, &node_p->peer->addr, node_p->peer);
        node_p = node_p->next;
    }
}

/* remove the stopped peer mantaining a circular structure */
void peer_stopped(Graph *peers, int sd, struct sockaddr_in *client_addr, bool send_ack)
{
    int ret;
    GraphNode *neighbors, *prec_node, *next_node, *node_p;
    Message msg;

    printf("removing node %d\n", ntohs(client_addr->sin_port));

    neighbors = remove_peer(peers, client_addr);

    if (neighbors== NULL)
    {
        printf("peer was not connected\n");
        return;
    }

    printf("peer's neighbors (%d): ", ntohs(client_addr->sin_port));
    print_peers(neighbors);
    
    /* more than one peer remaining */
    if (peers->first != peers->last)
    {
        /* connect together the two neighbors of the stopped peer */
        prec_node = neighbors;
        next_node = neighbors->next;
        set_neighbors(peers, prec_node->parent, next_node->parent, PUSH_FRONT);
    }
    else
    {
        printf("no more peers to connect\n");
    }     

    /* tell to all neighbors of stopped peer which are its new neighbors */
    node_p = neighbors;
    while (node_p)
    {
        tell_new_neighbors(sd, &node_p->peer->addr, node_p->peer);
        node_p = node_p->next;
    }

    if (send_ack)
    {
        printf("sending ACK to stopped peer\n");

        msg.type = MSG_STOP;
        msg.body_len = 0;
        msg.body = NULL;

        ret = send_message_to(sd, &msg, client_addr);
        if (ret == -1)
        {
            printf("could not send acknowledgement to stopped peer\n");
        }
    }
}



/*----------------------------------------------
 |  I/O DEMULTIPLEXING
 *---------------------------------------------*/


void demux_user_input(Graph *peers, int sd)
{
    int i, argc;
    char **argv;

    argv = scan_command_line(&argc);

    if (argv == NULL || argc == 0) 
    {
        return;   
    }
    
    /* hack: needed to pass socket descriptor to cmd_esc() */
    if (strcmp(argv[0], "esc") == 0)
    {
        argv = realloc(argv, 2 * sizeof(char*));
        argv[1] = malloc(7);
        argc++;
        sprintf(argv[1], "%hd", sd);
    }

    for (i = 0; i < NUM_CMDS; i++)
    {
        if (strcmp(argv[0], cmd_str[i]) == 0)
        {
            cmd_func[i](peers, argc, argv);
            return;
        }
    }
    
    printf("unknown command \"%s\"\n", argv[0]);

    free_command_line(argc, argv);
}

void demux_peer_request(Graph *peers, int sd)
{
    Message msg;
    int ret;
    struct sockaddr_in client_addr;

    printf("\n");

    ret = recv_message_from(sd, &msg, &client_addr);
    if (ret == -1)
    {
        printf("error receiving peer request\n");
        return;
    }

    if (ret == 0)
    {
        printf("peer disconnected (sd: %d)\n", sd);
        peer_stopped(peers, sd, &client_addr, ACK_DONT_SEND);
        return;
    }

    if (msg.type == MSG_START)
    {
        peer_started(peers, sd, &client_addr);
    }
    else if (msg.type == MSG_STOP)
    {
        peer_stopped(peers, sd, &client_addr, ACK_SEND);
    }

    printf("PEERS\n");
    print_graph(peers);
}


int main(int argc, char** argv)
{
    int ret, sd, fdmax, desc_ready, i;
    struct sockaddr_in my_addr;
    Graph peers;
    fd_set master_set, working_set;

    if (argc != 2)
    {
        printf("usage: ./peer <porta>\n");
        exit(EXIT_FAILURE);
    }

    /* parsing port from argument */
    ret = validate_port(argv[1]);
    if (ret == -1) 
        exit(EXIT_FAILURE);

    printf("*********** DISCOVERY SERVER %d ***********\n", ret);

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(ret);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd == -1)
    {
        perror("socket error");
        exit(EXIT_FAILURE);
    }

    ret = bind(sd, (struct sockaddr*)&my_addr, sizeof(my_addr));
    if (ret == -1)
    {
        perror("bind error");
        exit(EXIT_FAILURE);
    }


    create_graph(&peers);

    FD_ZERO(&master_set);
    FD_ZERO(&working_set);

    fdmax = -1;
    add_desc(&master_set, &fdmax, STDIN_FILENO);
    add_desc(&master_set, &fdmax, sd);

    for (;;)
    {
        working_set = master_set;

        if (FD_ISSET(STDIN_FILENO, &master_set))
        {
            printf("dserver@%d >>> ", ntohs(my_addr.sin_port));
            fflush(stdout);
        }

        desc_ready = select(fdmax + 1, &working_set, NULL, NULL, NULL);

        if (desc_ready == -1)
        {
            perror("select error");
            exit(EXIT_FAILURE);
        }
        
        for (i = 0; i <= fdmax && desc_ready > 0; i++)
        {
            if (!FD_ISSET(i, &working_set)) continue;
            
            desc_ready--;

            if (i == STDIN_FILENO) 
                demux_user_input(&peers, sd);
            else 
                demux_peer_request(&peers, sd);
        }
    }
}
