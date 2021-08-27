#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "network.h"
#include "comm.h"
#include "graph.h"

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


void tell_neighbors(Graph *graph, Peer *peer)
{
    
}


int main(int argc, char** argv)
{
    struct sockaddr_in my_addr, client_addr;
    struct sockaddr_in *tmp_addr;
    Message msg;
    int ret, i, sd;

    Graph peers;
    Peer *new_peer, *old_last;
    GraphNode *removed_node, *prec_node, *next_node;

    if (argc != 2)
    {
        printf("usage: ./peer <porta>\n");
        exit(EXIT_FAILURE);
    }

    ret = validate_port(argv[1]);
    if (ret == -1) 
        exit(EXIT_FAILURE);

    printf("DISCOVERY SERVER %d\n", ret);

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

    for (;;)
    {
        ret = recv_message_from(sd, &msg, &client_addr);
        if (ret == -1)
        {
            printf("Error on receiving message\n");
            continue;
        }

        if (msg.type == MSG_START)
        {
            /*
            add peer to graph
            get somehow list of modified neighbors lists
            send to each interested peer the new list of neighbors */


            tmp_addr = malloc(sizeof(struct sockaddr_in));
            *tmp_addr = client_addr;
            /* TODO put new port */

            old_last = NULL;
            if (peers.last != NULL)
                old_last = peers.last->peer;
            
            new_peer = add_peer(&peers, tmp_addr);

            /* the peer was already present */
            if (new_peer == NULL) 
            {
                printf("peer already present");
                free(tmp_addr);
                goto done;
            }

            /* only one peer present: nothing to connect */
            if (old_last == NULL)
            {
                printf("only one peer present\n");
                goto done;
            }

            /* more than two peers present: disconnect last from second-last */
            if (!peer_equals(peers.first->next->peer, old_last))
            {
                printf("more than two peers present\n");
                unset_neighbors(&peers, peers.first->peer, old_last);
            }

            /* connect new peer to second-last */
            set_neighbors(&peers, new_peer, old_last, PUSH_FRONT);
            /* connect new peer with first peer */
            set_neighbors(&peers, peers.first->peer, new_peer, PUSH_FRONT);
            /* set_neighbors(&peers, peers.first->peer, peers.last->peer, false); */

            /* need to inform: old_last, new_peer and peers.first->peer */
            print_graph(&peers);
        }
        else if (msg.type == MSG_STOP)
        {
            /*
            add peer to graph
            get somehow list of modified neighbors lists
            send to each interested peer the new list of neighbors */

            removed_node = remove_peer(&peers, &client_addr);

            if (removed_node == NULL)
            {
                printf("peer was not connected\n");
                goto done;
            }

            printf("peer's neighbors: ");
            print_peers(removed_node->neighbors);
            
            prec_node = removed_node->neighbors->parent;
            next_node = removed_node->neighbors->next->parent;
            
            set_neighbors_nodes(&peers, prec_node, next_node, PUSH_FRONT);

            print_graph(&peers);
        }
        else {
            printf("request received is not supported\n");
        }

done:

    }
}








int fake_main(int argc, char** argv)
{
    int ret, sd;

    struct sockaddr_in my_addr, client_addr;
    in_port_t my_port;

    Message msg;

    if (argc != 2)
    {
        printf("usage: ./peer <porta>\n");
        exit(1);
    }

    my_port = atoi(argv[1]);

    memset(&my_addr, 0, sizeof(my_addr));
    my_addr.sin_family = AF_INET;
    my_addr.sin_port = htons(my_port);
    my_addr.sin_addr.s_addr = INADDR_ANY;

    sd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sd == -1)
    {
        perror("main() -> socket() FAIL");
        exit(EXIT_FAILURE);
    }

    ret = bind(sd, (struct sockaddr*)&my_addr, sizeof(my_addr));
    if (ret == -1)
    {
        perror("main() -> bind() FAIL");
        exit(EXIT_FAILURE);
    }

    printf("DISCOVERY SERVER %d\n", my_port);
    
    for (;;)
    {
        ret = recv_message_from(sd, &msg, &client_addr);

        if (ret == -1)
        {
            printf("Error on receiving message\n");
            continue;
        }

        msg.type = MSG_SET_NBRS;
        sprintf(msg.body, "Hi %d, this is %d", client_addr.sin_port, my_addr.sin_port);
        msg.body_len = strlen(msg.body) + 1;

        sleep(4);

        ret = send_message_to(sd, &msg, &client_addr);

        if (ret == -1)
        {
            printf("Error on sending message\n");
            continue;
        }
    }

    return 0;
}
