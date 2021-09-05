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


void tell_neighbors(int sd, struct sockaddr_in *addr, Peer *peer)
{
    int ret;
    Message msg;
    msg.body = malloc(512);

    printf("telling to %d\n", ntohs(addr->sin_port));

    msg.type = MSG_SET_NBRS;
    msg.body_len = serialize_peers(msg.body, peer->node->neighbors) - msg.body;

    ret = send_message_to(sd, &msg, addr);
    if (ret == -1)
    {
        printf("error on sending neighbors\n");
    }
}


int main(int argc, char** argv)
{
    struct sockaddr_in my_addr, client_addr;
    /* struct sockaddr_in *tmp_addr; */
    Message msg;
    int ret, sd;
    /* in_port_t tmp_port; */

    Graph peers;
    Peer *new_peer, *old_last;
    GraphNode *neighbors, *prec_node, *next_node, *node_p;

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

    create_graph(&peers);

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

            if (peers.last != NULL)
                old_last = peers.last->peer;
            else
                old_last = NULL;

            /* tmp_port = client_addr.sin_port;
            client_addr.sin_port = *(in_port_t*)msg.body; */

            new_peer = add_peer(&peers, &client_addr);
            
            /* client_addr.sin_port = tmp_port; */

            if (new_peer == NULL)
            {
                printf("peer already present\n");
                goto done;
            }

            /* only one peer present: nothing to connect */
            if (old_last == NULL)
            {
                printf("only one peer present\n");

                tell_neighbors(sd, &client_addr, new_peer);

                goto done;
            }

            /* more than two peers present: disconnect last from second-last */
            if (!peer_equals(peers.first->next->peer, old_last))
            {
                printf("more than two peers present\n");
                unset_neighbors(&peers, peers.first->peer->node, old_last->node);
            }

            /* connect new peer to second-last */
            set_neighbors(&peers, new_peer->node, old_last->node, PUSH_FRONT);
            /* connect new peer with first peer */
            set_neighbors(&peers, peers.first, new_peer->node, PUSH_FRONT);

            /* need to inform: old_last, new_peer and peers.first->peer */
            /* print_graph(&peers); */

            tell_neighbors(sd, &client_addr, new_peer);

            node_p = new_peer->node->neighbors;
            while (node_p)
            {
                tell_neighbors(sd, &node_p->peer->addr, node_p->peer);
                node_p = node_p->next;
            }
        }
        else if (msg.type == MSG_STOP)
        {
            /*
            add peer to graph
            get somehow list of modified neighbors lists
            send to each interested peer the new list of neighbors */
            
            /* save source port for later use */
            /* tmp_port = client_addr.sin_port; */
            /* set client addr port to the value used to identify the peer */
            /* client_addr.sin_port = *(in_port_t*)msg.body; */

            printf("removing node %d\n", ntohs(client_addr.sin_port));
            fflush(stdout);

            neighbors = remove_peer(&peers, &client_addr);

            /* reset source port */
            /* client_addr.sin_port = tmp_port; */

            if (neighbors== NULL)
            {
                printf("peer was not connected\n");
                fflush(stdout);
                goto done;
            }

            printf("peer's neighbors (%d): ", ntohs(client_addr.sin_port));
            fflush(stdout);
            print_peers(neighbors);
            
            if (peers.first != peers.last)
            {
                prec_node = neighbors;
                printf("%p\n", (void*)prec_node);
                fflush(stdout);
                next_node = neighbors->next;
                printf("%p\n", (void*)next_node);
                fflush(stdout);

                set_neighbors(&peers, prec_node->parent, next_node->parent, PUSH_FRONT);
            }
            else
            {
                printf("no more peers to connect\n");
            }     

            node_p = neighbors;
            while (node_p)
            {
                msg.type = MSG_STOP;
                msg.body_len = 0;

                ret = send_message_to(sd, &msg, &client_addr);
                if (ret == -1)
                {
                    printf("could not send stop acknowledgement to peer\n");
                }

                tell_neighbors(sd, &node_p->peer->addr, node_p->peer);
                node_p = node_p->next;
            }       

            /* print_graph(&peers); */
        }
        else {
            printf("request received is not supported\n");
        }

done:
        printf("DONE\n");
        print_graph(&peers);
    }
}

