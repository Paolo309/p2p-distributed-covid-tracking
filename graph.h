#ifndef GRAPH_H
#define GRAPH_H

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <netinet/in.h>

#define PUSH_FRONT false
#define PUSH_BACK true

typedef struct Peer {
    struct sockaddr_in addr;
    struct sockaddr_in comm_addr;
    struct GraphNode *node;
} Peer;

typedef struct GraphNode {
    Peer *peer;
    struct GraphNode *neighbors;
    struct GraphNode *next;
    struct GraphNode *parent;
} GraphNode;

typedef struct Graph
{
    GraphNode *first;
    GraphNode *last;
} Graph;


Peer *create_peer(struct sockaddr_in *addr);
GraphNode *create_node(Peer *peer, GraphNode *next);
Graph *create_graph(Graph *graph);
void free_graph(Graph *graph);

bool addr_equals(struct sockaddr_in *a, struct sockaddr_in *b);
bool peer_equals(Peer *a, Peer *b);

GraphNode *search_peer_node_by_addr(GraphNode *nodes, struct sockaddr_in *addr, GraphNode **prec);

Peer *add_peer(Graph *graph, struct sockaddr_in *addr);

void add_neighbor_back(GraphNode **nbr_list, Peer *peer, GraphNode *parent);
void add_neighbor_front(GraphNode **nbr_list, Peer *peer, GraphNode *parent);

void remove_node_from_list(GraphNode **nodes, GraphNode *node, GraphNode *prec);
GraphNode *remove_peer_from_list(GraphNode **nodes, struct sockaddr_in *addr, GraphNode **prec);

void remove_neighbor(GraphNode *node, Peer *peer);
GraphNode *remove_peer(Graph *nodes, struct sockaddr_in *addr);

void set_neighbors(Graph *graph, GraphNode *a, GraphNode *b, bool back);
void unset_neighbors(Graph *graph, GraphNode *a, GraphNode *b);

void print_peers(GraphNode *nodes);
void print_graph(Graph *graph);

char *serialize_peers(char *buffer, GraphNode *nodes);
void deserialize_peers(char *buffer, GraphNode **nodes);

#endif
