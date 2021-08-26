#include "graph.h"

#include <string.h>

/**
 * @brief Create a peer object
 * 
 * @param addr peer address
 * @return pointer to created peer object 
 */
Peer *create_peer(struct sockaddr_in *addr)
{
    Peer *peer = malloc(sizeof(Peer));

    peer->addr = addr;

    return peer;
}

/**
 * @brief Create a graph node object
 * 
 * @param peer node's peer
 * @param next next node in list
 * @return pointer to created node
 */
GraphNode *create_node(Peer *peer, GraphNode *next)
{
    GraphNode *node = malloc(sizeof(GraphNode));

    node->peer = peer;
    node->neighbors = NULL;
    node->next = next;
    node->parent = NULL;

    return node;
}

/**
 * @brief Create a graph object.
 * 
 * If graph is NULL, a new graph will be created.
 * 
 * @param graph an already existing graph (optional)
 * @return pointer to created graph
 */
Graph *create_graph(Graph *graph)
{
    if (graph == NULL)
        graph = malloc(sizeof(Graph));

    graph->first = NULL;
    graph->last = NULL;

    return graph;
}

/**
 * @brief Compare two peers
 * 
 * @param a 
 * @param b 
 * @return true if a and b have same address and port
 */
bool peer_equals(Peer *a, Peer *b) /* NEW */
{
    if (a == NULL || b == NULL) return false;

    return a->addr->sin_port == b->addr->sin_port &&
           a->addr->sin_addr.s_addr == b->addr->sin_addr.s_addr;
}

/**
 * @brief Search node in list given peer.
 * 
 * Linearly search through peer list, returing the pointer to the node in the
 * list if it's present, NULL otherwise. If prec is not NULL, it will be set
 * to the preceding node or to NULL if the node found is the first or no node
 * is found.
 * 
 * @param nodes list of nodes to search through
 * @param peer peer to search
 * @param prec pointer to where to store pointer to preceding node
 * @return graph node if peer is found, NULL otherwise
 */
/* GraphNode *search_peer_node(GraphNode *nodes, Peer *peer, GraphNode **prec)
{
    if (nodes == NULL)
        return NULL;

    if (peer_equals(nodes->peer, peer))
        return nodes;

    if (prec != NULL)
        *prec = nodes;

    return search_peer_node(nodes->next, peer, prec);
} */

/**
 * Search node in list given peer's port.
 * 
 * Like search_peer_node.
 * 
 * @param nodes list of nodes to search through
 * @param port port to search
 * @param prec pointer to where to store pointer to preceding node
 * @return graph node if peer is found, NULL otherwise
 */
/* GraphNode *search_peer_node_by_port(GraphNode *nodes, in_port_t port, GraphNode **prec)
{
    if (nodes == NULL)
        return NULL;

    if (nodes->peer->addr->sin_port == port)
        return nodes;

    if (prec != NULL)
        *prec = nodes;

    return search_peer_node_by_port(nodes->next, port, prec);
} */

/**
 * Search node in list given peer's address and port.
 * 
 * Like search_peer_node.
 * 
 * @param nodes list of nodes to search through
 * @param port port to search
 * @param prec pointer to where to store pointer to preceding node
 * @return graph node if peer is found, NULL otherwise
 */
GraphNode *search_peer_node_by_addr(GraphNode *nodes, struct sockaddr_in *addr, GraphNode **prec)
{
    if (nodes == NULL)
        return NULL;

    if (nodes->peer->addr->sin_addr.s_addr == addr->sin_addr.s_addr &&
        nodes->peer->addr->sin_port == addr->sin_port)
        return nodes;

    if (prec != NULL)
        *prec = nodes;

    return search_peer_node_by_addr(nodes->next, addr, prec);
}


/** Create a new peer and add it to the graph. If the peer already exists, 
 * it is not added and NULL is returned.
 * 
 * @param graph 
 * @param addr address of the peer used to uniquely identify the peer
 * @return a pointer to the newly created peer
 */
Peer *add_peer(Graph *graph, struct sockaddr_in *addr) 
{
    GraphNode *node;
    Peer *peer;
    
    node = search_peer_node_by_addr(graph->first, addr, NULL);

    /* the peer already exists */
    if (node != NULL)
        return NULL;

    peer = create_peer(addr);
    node = create_node(peer, NULL);

    if (graph->first == NULL)
        graph->first = node;
        
    if (graph->last != NULL)
        graph->last->next = node;

    graph->last = node;

    return peer;
}

/**
 * Add neighbor at the back of the neighbors list.
 * 
 * @param nbr_list 
 * @param peer 
 * @param parent 
 */
void add_neighbor_back(GraphNode **nbr_list, Peer *peer, GraphNode *parent) /* MODIFIED */
{
    if (nbr_list == NULL)
        return;

    if (*nbr_list == NULL)
    {
        *nbr_list = create_node(peer, NULL);
        (*nbr_list)->parent = parent;
        return;
    }

    if (peer_equals((*nbr_list)->peer, peer))
        return;

    add_neighbor_back(&(*nbr_list)->next, peer, parent);
}

/**
 * Add neighbor at the front of the neighbors list.
 * 
 * @param nbr_list 
 * @param peer 
 * @param parent 
 */
void add_neighbor_front(GraphNode **nbr_list, Peer *peer, GraphNode *parent)
{
    if (nbr_list == NULL)
        return;

    if (*nbr_list == NULL || search_peer_node_by_addr(*nbr_list, peer->addr, NULL) == NULL)
    {
        *nbr_list = create_node(peer, *nbr_list);
        (*nbr_list)->parent = parent;
    }
}


void remove_node_from_list(GraphNode **nodes, GraphNode *node, GraphNode *prec)
{
    if (prec == NULL) /* first element in the list */
        *nodes = node->next;
    else
        prec->next = node->next;
}

/**
 * Remove peer from specified list. If list does not contain peer,
 * returns NULL.
 * 
 * @param nodes Pointer to pointer to the first node in the list
 * @param peer 
 * @param prec if not NULL is set to the preceding node
 * @return graph node removed from list
 */
GraphNode *remove_peer_from_list(GraphNode **nodes, struct sockaddr_in *addr, GraphNode **prec)
{
    GraphNode *node, *tmp_prec;

    tmp_prec = NULL;
    node = search_peer_node_by_addr(*nodes, addr, &tmp_prec);

    /* the list *nodes does not contain the peer */
    if (node == NULL)
        return NULL;

    remove_node_from_list(nodes, node, tmp_prec);

    /* return to the caller the peer preceding the one removed */
    if (prec != NULL)
        *prec = tmp_prec;

    return node;
}

/**
 * Remove specified peer from node's list of neighbors.
 * 
 * @param node node from which to remove the neighbor 
 * @param peer peer that is neighbor of peer pointed by node
 */
void remove_neighbor(GraphNode *node, Peer *peer)
{
    GraphNode *tmp = remove_peer_from_list(&node->neighbors, peer->addr, NULL);
    if (tmp != NULL)
        free(tmp);
}

/**
 * Remove peer from the list of peers.
 * 
 * @param nodes list of peers
 * @param peer peer to remove
 * @return the list of the peer's neighbors if the peer exists, NULL otherwise
 */
GraphNode *remove_peer(Graph *nodes, struct sockaddr_in *addr)
{
    GraphNode *node, *nbr, *tmp, *prec;

    /* remove peer from list of peers */
    /* node = remove_peer_from_list(&nodes->first, peer, &prec); */
    node = search_peer_node_by_addr(
        nodes->first, addr, &prec
    );

    if (node == NULL)
        return NULL;

    remove_node_from_list(&nodes->first, node, prec);

    if (node == nodes->last)
        nodes->last = prec;

    nbr = node->neighbors;

    /* for each neighbor nbr of the peer that's being removed */
    while (nbr)
    {
        /* remove the peer from nbr's list of neighbors */
        /* tmp = remove_peer_from_list(&nbr->parent->neighbors, peer, NULL); */
        tmp = remove_peer_from_list(&nbr->parent->neighbors, node->peer->addr, NULL);

        if (tmp != NULL)
            free(tmp);
        
        nbr = nbr->next;
    }

    nbr = node->neighbors;

    free(node);

    return nbr;
}


void set_neighbors_nodes(Graph *graph, GraphNode *a, GraphNode *b, bool back)
{
    if (back)
    {
        add_neighbor_back(&a->neighbors, b->peer, b);
        add_neighbor_back(&b->neighbors, a->peer, a);
    }
    else
    {
        add_neighbor_front(&a->neighbors, b->peer, b);
        add_neighbor_front(&b->neighbors, a->peer, a);
    }
}

void unset_neighbors_nodes(Graph *graph, GraphNode *a, GraphNode *b)
{
    remove_neighbor(a, b->peer);
    remove_neighbor(b, a->peer);
}


/**
 * Set two peers as neighbors.
 * 
 * @param graph 
 * @param a 
 * @param b 
 * @param back if true, each peer's node will be placed at the back
 * of the other peer's list of neighbors
 */
void set_neighbors(Graph *graph, Peer *a, Peer *b, bool back)
{
    GraphNode *node_a, *node_b;

    node_a = search_peer_node_by_addr(graph->first, a->addr, NULL);
    if (node_a == NULL)
        return;

    node_b = search_peer_node_by_addr(graph->first, b->addr, NULL);
    if (node_b == NULL)
        return;

    set_neighbors_nodes(graph, node_a, node_b, back);

    /* if (back)
    {
        add_neighbor_back(&node_a->neighbors, b, node_b);
        add_neighbor_back(&node_b->neighbors, a, node_a);
    }
    else
    {
        add_neighbor_front(&node_a->neighbors, b, node_b);
        add_neighbor_front(&node_b->neighbors, a, node_a);
    } */
}

/**
 * Unset neighborhood for the specified peers.
 * 
 * @param graph 
 * @param a 
 * @param b 
 */
void unset_neighbors(Graph *graph, Peer *a, Peer *b)
{
    GraphNode *node_a, *node_b;

    node_a = search_peer_node_by_addr(graph->first, a->addr, NULL);
    if (node_a == NULL)
        return;

    node_b = search_peer_node_by_addr(graph->first, b->addr, NULL);
    if (node_b == NULL)
        return;
    
    unset_neighbors_nodes(graph, node_a, node_b);

    /* remove_neighbor(node_a, b);
    remove_neighbor(node_b, a); */
}

/**
 * Print list of peers.
 * 
 * @param nodes 
 */
void print_peers(GraphNode *nodes)
{
    printf("[");
    while (nodes)
    {
        printf("%d", nodes->peer->addr->sin_port);
        if ((nodes = nodes->next) != NULL)
            printf(", ");
    }
    puts("]");
}

/**
 * Print list of peers, and for each peer, the list of its neighbors.
 * 
 * @param graph 
 */
void print_graph(Graph *graph)
{
    GraphNode *node = graph->first;
    printf("{\n");
    while (node)
    {
        printf("peer: %d; nbrs: ", node->peer->addr->sin_port);
        print_peers(node->neighbors);
        node = node->next;
    }
    printf("}\n");
}
