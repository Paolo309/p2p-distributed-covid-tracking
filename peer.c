#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "constants.h"
#include "common_utils.h"
#include "network.h"
#include "data.h"
#include "comm.h"
#include "commandline.h"
#include "graph.h"

/*----------------------------------------------
 |  STRUCTS AND FUNCTIONS TO HANDLE REQUESTS AND THIS PEER'S DATA
 *---------------------------------------------*/

struct ThisPeer;

/* used to maintain request state during request processing */
typedef struct FloodRequest {
    int32_t number;
    EntryList *required_entries, *found_entries;
    int requester_sd;
    fd_set peers_involved;
    int nbrs_remaining;
    Message *response_msg;
    time_t beg_period, end_period;
    int type; /* somma o variazione */
    void (*callback)(struct ThisPeer*, in_port_t[], int, int);
} FloodRequest;


typedef struct ThisPeer {
    /* address bound to this peer */
    struct sockaddr_in addr;

    /* main IO multiplexing */
    fd_set master_read_set, master_write_set;
    int dserver, listening, fdmax_r, fdmax_w;
    struct timeval timeout, *actual_timeout;

    /* state variables */
    int state;
    Graph neighbors; 
    EntryList entries;

    /* handling flood requests */
    int last_request_nums[NUM_MAX_REQUESTS];
    FloodRequest last_requests[NUM_MAX_REQUESTS];
    int lr_head, lr_tail;

} ThisPeer;

void init_peer(ThisPeer *peer, int host_port)
{
    memset(&peer->addr, 0, sizeof(peer->addr));
    peer->addr.sin_family = AF_INET;
    peer->addr.sin_port = htons(host_port);
    peer->addr.sin_addr.s_addr = INADDR_ANY;

    FD_ZERO(&peer->master_read_set);
    FD_ZERO(&peer->master_write_set);
    peer->fdmax_r = peer->fdmax_w = 0;
    
    peer->dserver = -1;
    peer->listening = -1;
    peer->state = STATE_OFF;
    peer->actual_timeout = NULL;

    memset(peer->last_request_nums, 0, sizeof(peer->last_request_nums));
    memset(peer->last_requests, 0, sizeof(peer->last_requests));
    peer->lr_head = 0;
    peer->lr_tail = NUM_MAX_REQUESTS - 1;    

    create_graph(&peer->neighbors);
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
    add_desc(&peer->master_read_set, &peer->fdmax_r, STDIN_FILENO);
}

void disable_user_input(ThisPeer *peer)
{
    remove_desc(&peer->master_read_set, &peer->fdmax_r, STDIN_FILENO);
}

bool is_user_input_enabled(ThisPeer *peer)
{
    return FD_ISSET(STDIN_FILENO, &peer->master_read_set);
}

void terminate_peer(ThisPeer *peer)
{
    int fdmax;

    if (peer->state != STATE_STOPPED)
    {
        printf("cannot stop peer now\n");
        return;
    }
    
    printf("shutting down peer...\n");

    fdmax = peer->fdmax_r;
    while (fdmax > 0) {
        if (FD_ISSET(fdmax, &peer->master_read_set))
            close(fdmax);
        remove_desc(&peer->master_read_set, &fdmax, fdmax);
    }
    
    fdmax = peer->fdmax_w;
    while (fdmax > 0) {
        if (FD_ISSET(fdmax, &peer->master_write_set))
            close(fdmax);
        remove_desc(&peer->master_write_set, &fdmax, fdmax);
    }

    free_entry_list(&peer->entries);

    printf("stopped\n");
}

/**
 * Set req_num has "serviced". Future requests with the same
 * number will be answered with an empty reponse.
 * 
 * @param peer 
 * @param req_num 
 */
void request_serviced(ThisPeer *peer, int req_num)
{
    peer->last_request_nums[peer->lr_tail] = req_num;
    peer->lr_head = (peer->lr_head + 1) % NUM_MAX_REQUESTS;
    peer->lr_tail = (peer->lr_tail + 1) % NUM_MAX_REQUESTS;
}

/**
 * Check if req_num was not already serviced
 * 
 * @param peer 
 * @param req_num 
 * @return true if the request was not serviced
 */
bool valid_request(ThisPeer *peer, int req_num)
{
    int i;
    for (i = 0; i < NUM_MAX_REQUESTS; i++)
    {
        if (peer->last_request_nums[i] == req_num)
            return false;
    }
    return true;
}

/**
 * Given a request number, returns an instance of FloodRequest
 * associated with an already serviced request.
 * 
 * @param peer 
 * @param req_num 
 * @return FloodRequest* 
 */
FloodRequest *get_request_by_num(ThisPeer *peer, int req_num)
{
    int i;
    for (i = 0; i < NUM_MAX_REQUESTS; i++)
    {
        if (peer->last_request_nums[i] == req_num)
            return &peer->last_requests[i];
    }
    return NULL;
}

/**
 * Given a socket descriptor, return the most recent request that 
 * used that socket.
 * 
 * @param peer 
 * @param sd 
 * @return FloodRequest* 
 */
FloodRequest *get_request_by_sd(ThisPeer *peer, int sd)
{
    int i;
    for (i = 0; i < NUM_MAX_REQUESTS; i++)
    {
        if (FD_ISSET(sd, &peer->last_requests[i].peers_involved))
            return &peer->last_requests[i];
    }
    return NULL;
}

uint32_t get_peer_id(ThisPeer *peer)
{
    return ntohs(peer->addr.sin_port);
}



/*----------------------------------------------
 |  FUNCTIONS TO START AND STOP THE PEER
 *---------------------------------------------*/


int send_start_msg_to_dserver(ThisPeer *peer)
{
    Message msg;
    int ret;

    msg.type = MSG_START;
    msg.body_len = 0;
    msg.body = NULL;

    ret = send_message(peer->dserver, &msg);
    if (ret == -1)
    {
        printf("could not send start message to discover server\n");
        return -1;
    }

    peer->state = STATE_STARTING;

    /* timeout used by select(): start msg will be sent again on timeout */
    set_timeout(peer, START_MSG_TIMEOUT);
    disable_user_input(peer);
    add_desc(&peer->master_read_set, &peer->fdmax_r, peer->dserver);

    printf("waiting for server response...\n");

    return 0;
}

int send_stop_msg_to_dserver(ThisPeer *peer)
{
    Message msg;
    int ret;

    msg.type = MSG_STOP;
    msg.body_len = 0;
    msg.body = NULL;

    ret = send_message(peer->dserver, &msg);
    if (ret == -1)
    {
        printf("could not send start message to discover server\n");
        return -1;
    }

    peer->state = STATE_STOPPING;

    set_timeout(peer, 3);
    disable_user_input(peer);

    printf("waiting for server response...\n");

    return 0;
}



/*----------------------------------------------
 |  UTILITY FUNCTIONS TO COMPUTE AGGREGATES
 *---------------------------------------------*/


/**
 * Sum the values of all the entries and put the result in entry_res.
 * Does not check if the entries are valid. Only sums entries that 
 * have SCOPE_GLOBAL.
 * 
 * @param peer 
 * @param entries source list
 * @param entry_res result entry
 */
void compute_aggr_tot(ThisPeer *peer, EntryList *entries, Entry *entry_res)
{
    Entry *entry;

    entry = entries->last;
    while (entry)
    {
        if ((entry->flags & ENTRY_SCOPE) == SCOPE_GLOBAL)
        {
            entry_res->tamponi += entry->tamponi;
            entry_res->nuovi_casi += entry->nuovi_casi;
        }
        entry = entry->prev;
    }
}

/**
 * Computes the variations between consecutive entries in the 
 * specified list and puts the results into entries_res.
 * Does not check if the entries are valid. Only sums
 * consecutive entries. Uses only entries that have SCOPE_GLOBAL.
 * 
 * @param peer 
 * @param entries source list
 * @param entries_res result list
 * @param flags flags put into the new entries in entries_res
 */
void compute_aggr_var(ThisPeer *peer, EntryList *entries, EntryList *entries_res, int32_t flags)
{
    Entry *entry, *tmp;

    if (entries->last == NULL)
        return;

    entry = entries->last->prev;
    while (entry)
    {
        /* 1. both current and next entries must be GLOBAL */
        /* 2. next entry must be the day before the current one */
        if ((entry->flags       & ENTRY_SCOPE) == SCOPE_GLOBAL &&
            (entry->next->flags & ENTRY_SCOPE) == SCOPE_GLOBAL &&
            entry->timestamp == add_days(entry->next->timestamp, -1))
        {
            /* compute variation */
            tmp = create_entry(
                entry->timestamp,
                entry->next->tamponi - entry->tamponi,
                entry->next->nuovi_casi - entry->nuovi_casi,
                flags,
                2
            );
            add_entry(entries_res, tmp);
        }
        entry = entry->prev;
    }
}

/**
 * For each day in the given period, search this peer's register for
 * entries that satisfy the specified conditions (period and flags).
 * The entries found are put in the list `found`. For each missing
 * entry, a new entry is created with values zero for tamponi and
 * nuovi_casi, and is put in the list `not_found`, with flags `flags`.
 * If scope is SCOPE_GLOBAL, only global entries are used. If scope is
 * SCOPE_LOCAL, both global and local entries are used.
 * Entries in `found` are copies of entries in register.
 * 
 * TODO OPTIMIZE
 * 
 * @param peer 
 * @param found 
 * @param not_found 
 * @param start 
 * @param end 
 * @param flags flags used for searching, and put in create entries 
 * @param scope either SCOPE_LOCAL or SCOPE_GLOBAL
 * @return number of entries not found
 */
int search_needed_entries(
    ThisPeer *peer, 
    EntryList *found, EntryList *not_found, 
    time_t start, time_t end, 
    int32_t period_len, 
    int32_t flags, 
    int32_t scope
)
{
    Entry *found_entry;
    Entry *new_entry;
    Entry *entry;
    time_t t_day;
    int count_not_found = 0;

    t_day = end;

    /* BEGINNING_OF_TIME is used for queries without lower bound */
    if (start == BEGINNING_OF_TIME)
    {
        if (is_entry_list_empty(&peer->entries))
        {
            start = str_to_time(DEFAULT_PERIOD_LOWER_BOUND);
        }
        else
        {
            /* set start to the value of the oldest timestamp in the register,
                excluding the entries with timestamp equal to BEGINNING_OF_TIME */

            entry = peer->entries.first;
            while (entry && entry->timestamp == BEGINNING_OF_TIME)
                entry = entry->next;
            
            if (entry)
                start = entry->timestamp;
        }

    }
    
    /* for each day of the period */
    while (difftime(t_day, start) >= 0)
    {
        found_entry = search_entry(peer->entries.last, t_day, flags, period_len);

        /* if (found_entry != NULL && (found_entry->flags & ENTRY_SCOPE) == SCOPE_GLOBAL) */
        if (found_entry != NULL && (((found_entry->flags & ENTRY_SCOPE) >= scope) || ((scope == -1) && (found_entry->flags & ENTRY_SCOPE) == SCOPE_GLOBAL)))
        {
            new_entry = copy_entry(found_entry);
            add_entry(found, new_entry);
        }
        else
        {
            new_entry = create_entry(t_day, 0, 0, flags, period_len);
            add_entry(not_found, new_entry);
            count_not_found++;
        }

        t_day = add_days(t_day, -1);
    }

    return count_not_found;
}

/**
 * Removes from needed_totals the entries that are not needed to compute
 * the needed variations in needed_vars. An entry in needed_totals is
 * removed if there is no entry in needed_vars with same timestamp, or
 * timestamp equal to the day before of the entry in needed_totals.
 * E.g. an entry total of day x is needed only if you have to compute
 * a variation between x+1 and x, or between x and x-1.
 * 
 * @param needed_totals 
 * @param needed_vars 
 * @return number of removed entries
 */
int remove_not_needed_totals(EntryList *needed_totals, EntryList *needed_vars)
{   
    Entry *entry, *found_entry, *removed_entry; 
    int count_removed = 0;

    entry = needed_totals->last;
    while (entry)
    {
        /* variation between entry->timestamp and day after */
        found_entry = search_entry(
            needed_vars->last,
            entry->timestamp,
            AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION,
            2
        );

        if (found_entry == NULL)
        {
            /* variation between entry->timestamp and day before */
            found_entry = search_entry(
                needed_vars->last,
                entry->timestamp - 86400,
                AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION,
                2
            );
        }

        removed_entry = NULL;

        if (found_entry == NULL) /* entry is not needed */
        {
            removed_entry = entry;
            remove_entry(needed_totals, entry);
            count_removed++;
        }

        entry = entry->prev;
        free(removed_entry);
    }

    return count_removed;
}



/*----------------------------------------------
 |  UTILITY FUNCTIONS TO HANDLE FLOODINGS
 *---------------------------------------------*/

/**
 * Connect to all the neighbors and put the created socket 
 * descriptors in master_write_set. If a request is specified,
 * the socket descriptors are also put in the request's 
 * peers_involved fd set.
 * 
 * @param peer 
 * @param request 
 * @return number of neighbors connected to    
 */
int connect_to_neighbors(ThisPeer *peer, FloodRequest *request, in_port_t except)
{
    int sd, ret, count_connected;
    socklen_t slen;
    GraphNode *nbr;

    slen = sizeof(struct sockaddr_in);
    count_connected = 0;

    nbr = peer->neighbors.first;
    while (nbr)
    {
        if (nbr->peer->addr.sin_port == htons(except))
        {
            nbr = nbr->next;
            continue;
        }

        sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd == -1)
        {
            perror("socket error");
            return count_connected;
        }

        ret = connect(sd, (struct sockaddr *)&nbr->peer->addr, slen);
        if (ret == -1)
        {
            perror("connect error");
            return count_connected;
        }

        printf("connected to peer %hd (sd: %d)\n", ntohs(nbr->peer->addr.sin_port), sd);

        add_desc(&peer->master_write_set, &peer->fdmax_w, sd);
        
        if (request != NULL)
        {
            add_desc(&request->peers_involved, NULL, sd);
        }
        
        count_connected++;
        nbr = nbr->next;
    }

    return count_connected;
}

/**
 * Ask the aggregates in teq_aggr to this peer's neighbors.
 * The aggregates that are found are removed from req_aggr
 * and place in recvd_aggr.
 * 
 * @param peer 
 * @param req_aggr 
 * @param recvd_aggr 
 * @return true if at least one aggregate was found
 */
bool ask_aggr_to_neighbors(ThisPeer *peer, EntryList *req_aggr, EntryList *recvd_aggr)
{
    EntryList data_received;
    Entry *entry, *found_entry;
    socklen_t slen;
    GraphNode *nbr;
    Message msg;
    int ret, sd;
    
    slen = sizeof(struct sockaddr_in);

    nbr = peer->neighbors.first;
    while (nbr)
    {
        sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd == -1)
        {
            perror("socket error");
            return false;
        }

        ret = connect(sd, (struct sockaddr *)&nbr->peer->addr, slen);
        if (ret == -1)
        {
            perror("connect error");
            return false;
        }

        msg.type = MSG_REQ_DATA;
        msg.id = get_peer_id(peer);
        msg.body = allocate_entry_list_buffer(req_aggr->length);
        msg.body_len = serialize_entries(msg.body, req_aggr) - msg.body;
        
        printf("asking aggregates to %d\n", ntohs(nbr->peer->addr.sin_port));

        ret = send_message(sd, &msg);
        if (ret == -1)
        {
            printf("could not send REQ_DATA to peer %d\n", ntohs(nbr->peer->addr.sin_port));
            free(msg.body);
            return false;
        }

        free(msg.body);

        ret = recv_message(sd, &msg);
        if (ret == -1)
        {
            printf("error while receiving REPLY_DATA\n");
            return false;
        }

        close(sd);
        
        init_entry_list(&data_received);
        deserialize_entries(msg.body, &data_received);

        if (!is_entry_list_empty(&data_received))
        {
            printf("aggregate found in neighbor %d\n", msg.id);
            merge_entry_lists(recvd_aggr, &data_received, COPY_SHALLOW);

            entry = data_received.last;
            while (entry)
            {
                found_entry = search_entry(
                    req_aggr->last,
                    entry->timestamp,
                    entry->flags,
                    entry->period_len
                );

                if (found_entry != NULL)
                {
                    remove_entry(req_aggr, found_entry);
                    free(found_entry);
                }
                
                entry = entry->prev;
            }

            if (is_entry_list_empty(req_aggr))
                return true;
        }
        
        nbr = nbr->next;
    }

    return false;
}

/**
 * Ask the entries in `req_entries` to the peers in array `peers`.
 * The received entries are strictly aggregated and put in rcvd_entries.
 * 
 * @param peer 
 * @param req_entries entries to ask for
 * @param recvd_entries list where to put the received entries
 * @param peers peers to ask for entries
 * @param n how many peers
 */
void ask_aggr_to_peers(
    ThisPeer *peer, 
    EntryList *req_entries, EntryList *recvd_entries, 
    in_port_t peers[], int n)
{
    EntryList tmp_data_received;
    socklen_t slen;
    int i;
    Message msg;
    int ret, sd;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    /* assuming peer on same host as this peer */
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr.s_addr);

    init_entry_list(recvd_entries);

    slen = sizeof(struct sockaddr_in);

    /* for each peer */
    for (i = 0; i < n; i++)
    {
        sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd == -1)
        {
            perror("socket error");
            return;
        }

        printf("asking entries to %d (sd: %d)\n", peers[i], sd);

        addr.sin_port = htons(peers[i]);

        ret = connect(sd, (struct sockaddr *)&addr, slen);
        if (ret == -1)
        {
            perror("connect error");
            return;
        }

        msg.type = MSG_REQ_DATA;
        msg.id = get_peer_id(peer);
        msg.req_num = rand();
        msg.body = allocate_entry_list_buffer(req_entries->length);
        msg.body_len = serialize_entries(msg.body, req_entries) - msg.body;

        ret = send_message(sd, &msg);
        if (ret == -1)
        {
            printf("could not send MSG_REQ_DATA to peer %d\n", peers[i]);
            free(msg.body);
            return;
        }

        free(msg.body);

        ret = recv_message(sd, &msg);
        if (ret == -1)
        {
            printf("error while receiving MSG_REPLY_DATA\n");
            return;
        }

        close(sd);
        
        init_entry_list(&tmp_data_received);
        deserialize_entries(msg.body, &tmp_data_received);

        /* print_entries_asc(&tmp_data_received, "peer sent"); */

        if (!is_entry_list_empty(&tmp_data_received))
        {
            merge_entry_lists(recvd_entries, &tmp_data_received, COPY_STRICT);
        }
    }

    return;
}



/*----------------------------------------------
 |  FUNCTIONS TO HANDLE COMMAND 'get' and FLOODING
 *---------------------------------------------*/


void show_aggr_tot_result(Entry *result, int type)
{
    printf("\n+------------------------------------------\n");
    printf("|\tRESULT: totale ");
    if (type == REQ_TAMPONI)
        printf("tamponi = %d\n", result->tamponi);
    else
        printf("nuovi casi = %d\n", result->nuovi_casi);
    printf("+------------------------------------------\n\n");
}

void show_aggr_var_result(EntryList *result, int type)
{
    Entry *entry;
    time_t time;
    char time_str[TIMESTAMP_STRLEN];
    
    printf("\n+------------------------------------------\n");
    printf("|\tRESULT: variazioni di ");
    if (type == REQ_TAMPONI)
        printf("tamponi:\n");
    else
        printf("nuovi casi\n");
    printf("|\t----------------------------\n");
    
    entry = result->first;
    while (entry)
    {
        time = get_enf_of_period(entry);
        time_to_str(time_str, &time);
        printf("|\t%s ", time_str);

        if (type == REQ_TAMPONI)
            printf("%d\n", entry->tamponi);
        else
            printf("%d\n", entry->nuovi_casi);

        entry = entry->next;
    }
    printf("+------------------------------------------\n\n");
}

/* called when a flooding for 'get sum' is finished */
void finalize_get_aggr_tot(ThisPeer *peer, in_port_t peers[], int n, int req_num);

/**
 * Called when users executes the command 'get sum'. If it starts a
 * flooding, then this is called again when the flooding is finished.
 * 
 * @param peer 
 * @param beg_period 
 * @param end_period 
 * @param type either REQ_TAMPONI or REQ_NUOVI_CASI
 */
void get_aggr_tot(ThisPeer *peer, time_t beg_period, time_t end_period, int type)
{
    /* used to handle entries */
    int32_t flags, period_len;
    Entry *entry;
    
    /* used to search entries and compute aggregations */
    EntryList found_entries, not_found_entries, found_aggr;
    int missing_entries = 0;
    Entry *entry_res; /* result of the aggregation */
    
    /* used when contacting neighbors asking for computed aggregates */
    EntryList aggr_requested;
    bool data_found;

    /* used to prepare and setup a flooding request */
    FloodRequest *request;
    int req_num;


    /*----------------------------------------------
    |  searching for the pre-computed aggregate in this peer
    *---------------------------------------------*/

    /* flags that the entry we're looking for must have:
        AGGREG_PERIOD : we're looking for an entry that covers a period
        TYPE_TOTAL    : we're looking for a sum, not a variation
        SCOPE_GLOBAL  : the value of the entry must be common to all peers */
    flags = AGGREG_PERIOD | TYPE_TOTAL | SCOPE_GLOBAL;

    period_len = diff_days(end_period, beg_period) + 1;

    entry = search_entry(peer->entries.last, beg_period, flags, period_len);

    /* aggregate entry found in this peer */
    if (entry != NULL)
    {
        printf("[GET_SUM] sum found in local register\n");
        show_aggr_tot_result(entry, type);
        return;
    }

    printf("[GET_SUM] sum not found in local register\n");


    /*----------------------------------------------
    |  searching for entries needed to compute aggregate
    *---------------------------------------------*/

    /* flags that the entries used to query peers must have:
        AGGREG_DAILY : because we're looking for daily totals 
        TYPE_TOTAL   : we're looking for a sum, not a variation
        SCOPE_LOCAL  : the enties in not_found_entries cannot be global */
    flags = AGGREG_DAILY | TYPE_TOTAL | SCOPE_LOCAL;
    
    init_entry_list(&found_entries);
    init_entry_list(&not_found_entries);

    missing_entries = search_needed_entries(
        peer, 
        &found_entries, &not_found_entries, 
        beg_period, end_period, 0, /* period_len = 0 days */
        flags,
        SCOPE_GLOBAL
    );

    /* now, not_found_entries, if not empty, contains the entries that
        are needed to compute the sum and are not present in this peer */

    /* this peer has all the entries needed to compute the aggregation */
    if (missing_entries == 0)
    {
        printf("[GET_SUM] found all entries needed to compute sum\n");

        /* entry which will contain the result of the aggregation */
        entry_res = create_entry(
            beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL, period_len
        );

        compute_aggr_tot(peer, &found_entries, entry_res);

        show_aggr_tot_result(entry_res, type);

        /* add the found entry to the local register */
        add_entry(&peer->entries, entry_res);

        printf("[GET_SUM] register updated\n");
        /* print_entries_asc(&peer->entries); */

        free_entry_list(&found_entries);
        free_entry_list(&not_found_entries);

        return;
    }

    printf("[GET_SUM] missing some entries needed to compute the sum\n");

    print_entries_asc(&found_entries, "entries found");
    print_entries_asc(&not_found_entries, "missing entries");

    /*----------------------------------------------
    |  asking aggregate to neighbors
    *---------------------------------------------*/    

    init_entry_list(&aggr_requested);
    entry = create_entry(beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL, period_len);
    add_entry(&aggr_requested, entry);

    print_entries_asc(&aggr_requested, "REQUESTED");

    init_entry_list(&found_aggr);

    data_found = ask_aggr_to_neighbors(peer, &aggr_requested, &found_aggr);
    if (data_found)
    {
        printf("[GET_SUM] a neighbor has the requested sum\n");
        show_aggr_tot_result(found_aggr.first, type);

        /* add the found entry to the local register */
        add_entry(&peer->entries, copy_entry(found_aggr.first));
        printf("[GET_SUM] register updated\n");
        /* print_entries_asc(&peer->entries, "REGISTER"); */

        free_entry_list(&found_entries);
        free_entry_list(&not_found_entries);
        free_entry_list(&found_aggr);

        return;
    }

    printf("[GET_SUM] no neighbor has the requested sum\n");

    free_entry_list(&aggr_requested);
    free_entry_list(&found_aggr);


    /*----------------------------------------------
    |  launching flood for entries
    *---------------------------------------------*/ 

    printf("[GET_SUM] flooding for entries\n");

    req_num = rand();
    
    /* set the new request as handled, so that when receiving flood
        requests with same number, those requests are "ignored" */
    request_serviced(peer, req_num);

    /* get object which will contain the request status and data */
    request = get_request_by_num(peer, req_num);

    request->number = req_num;
    request->requester_sd = REQUESTER_SELF;

    request->beg_period = beg_period;
    request->end_period = end_period;
    request->type = type;

    request->nbrs_remaining = 0;
    request->callback = &finalize_get_aggr_tot;

    FD_ZERO(&request->peers_involved);
    connect_to_neighbors(peer, request, 0);

    request->required_entries = malloc(sizeof(EntryList));
    init_entry_list(request->required_entries);

    request->found_entries = malloc(sizeof(EntryList));
    init_entry_list(request->found_entries);

    request->required_entries->first = not_found_entries.first;
    request->required_entries->last = not_found_entries.last;
    request->required_entries->length = not_found_entries.length;

    request->found_entries->first = found_entries.first;
    request->found_entries->last = found_entries.last;
    request->found_entries->length = found_entries.length;

    /* initializing response message as empty */
    request->response_msg = malloc(sizeof(Message));
    request->response_msg->body = malloc(256);
    set_num_of_peers(request->response_msg, 0);

    /* since the flooding is verbose, listening for user input is useless */
    disable_user_input(peer);

    /* set_timeout(peer, rand() % 4); */

    peer->state = STATE_STARTING_FLOOD;
}

/* called when a flooding for 'get var' is finished */
void finalize_get_aggr_var(ThisPeer *peer, in_port_t peers[], int n, int req_num);

/**
 * Called when users executes the command 'get var'. If it launches a
 * flooding, then this is called again when the flooding is finished.
 * 
 * @param peer 
 * @param beg_period 
 * @param end_period 
 * @param type either REQ_TAMPONI or REQ_NUOVI_CASI
 */
void get_aggr_var(ThisPeer *peer, time_t beg_period, time_t end_period, int type)
{
    /* used to handle entries */
    int32_t flags, act_end_period;
    
    /* used to search entries and compute aggregations */
    EntryList found_var_entries, found_var_entries_nbrs, found_tot_entries;
    int missing_entries = 0;
    EntryList entries_res; /* result of the aggregation */

    /* used when contacting neighbors asking for computed aggregates */
    EntryList not_found_var_entries, not_found_tot_entries;
    bool all_data_found;
    
    /* used to prepare and setup a flooding request */
    int req_num;
    FloodRequest *request;


    /*----------------------------------------------
    |  searching for the pre-computed aggregate in this peer
    *---------------------------------------------*/

    /* flags that the entry we're looking for must have:
        AGGREG_PERIOD  : we're looking for an entry that covers a period
        SCOPE_GLOBAL   : the value of the entry must be common to all peers
        TYPE_VARIATION : we're looking for a variation, not a sum */
    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    init_entry_list(&found_var_entries);
    init_entry_list(&not_found_var_entries);

    /* limit the search to a day before end_period */
    act_end_period = end_period - 86400;

    /* each aggreg entry representing a variation is saved with timestamp
        set to the first day, period_len = 2 and flag TYPE_VARIATION
       e.g. variation between 25/11 and 26/11 is -136:
        timestamp = 25/11
        tamponi = -136 
        period_len = 2 */

    missing_entries = search_needed_entries(
        peer,
        &found_var_entries, &not_found_var_entries,
        beg_period, act_end_period, 2, /* period_len = 2 days */
        flags,
        SCOPE_GLOBAL
    );

    if (missing_entries == 0)
    {
        printf("[GET VAR] variations found in local register:\n");
        show_aggr_var_result(&found_var_entries, type);

        free_entry_list(&found_var_entries);
        free_entry_list(&not_found_var_entries);

        return;
    }

    printf("[GET VAR] not all variations found in local register\n");

    print_entries_asc(&found_var_entries, "var entries found");
    print_entries_asc(&not_found_var_entries, "var entries not found");


    /*----------------------------------------------
    |  searching for entries needed to compute aggregate
    *---------------------------------------------*/

    /* flags that the entries used to query peers must have:
        AGGREG_DAILY : because we're looking for daily totals 
        SCOPE_LOCAL  : not found entries cannot be global 
        TYPE_TOTAL   : we want daily totals to compute daily variations */
    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;

    init_entry_list(&found_tot_entries);
    init_entry_list(&not_found_tot_entries);

    missing_entries = search_needed_entries(
        peer, 
        &found_tot_entries, &not_found_tot_entries, 
        beg_period, end_period, 0, /* period_len = 0 days */
        flags,
        SCOPE_GLOBAL
    );

    /* not_found_tot_entries now contains all the entries of TYPE_TOTAL
        which are needed to compute all the variations in the specified
        period.
       for the variations already present in this peer there's no need
        to keep the data needed to compute them.
       the next function call removes from not_found_tot_entries the 
        entries that wouldn't be use to compute any variation, so that
        only really needed entries are asked to other peers. */
    missing_entries -= remove_not_needed_totals(
        &not_found_tot_entries,
        &not_found_var_entries
    );

    print_entries_asc(&found_tot_entries, "found totals");

    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    if (missing_entries == 0)
    {
        printf("[GET VAR] found all entries needed to compute all variations\n");

        init_entry_list(&entries_res);
        compute_aggr_var(peer, &found_tot_entries, &entries_res, flags);

        merge_entry_lists(&entries_res, &found_var_entries, COPY_STRICT);

        show_aggr_var_result(&entries_res, type);

        merge_entry_lists(&peer->entries, &entries_res, COPY_STRICT);
        printf("[GET VAR] register updated\n");
        /* print_entries_asc(&peer->entries, "REGISTER"); */

        free_entry_list(&found_tot_entries);
        free_entry_list(&not_found_tot_entries);
        free_entry_list(&not_found_var_entries);

        return;
    }

    printf("[GET VAR] missing some entries needed to compute all variations\n");

    print_entries_asc(&found_tot_entries, "totals found");
    print_entries_asc(&not_found_tot_entries, "totals missing");


    /*----------------------------------------------
    |  asking aggregate to neighbors
    *---------------------------------------------*/ 

    /* free_entry_list(&found_var_entries); */

    init_entry_list(&found_var_entries_nbrs);
    
    all_data_found = ask_aggr_to_neighbors(peer, &not_found_var_entries, &found_var_entries_nbrs);

    if (all_data_found)
    {
        printf("[GET VAR] the neighbors have all the requested variations:\n");

        merge_entry_lists(&found_var_entries, &found_var_entries_nbrs, COPY_STRICT);

        show_aggr_var_result(&found_var_entries, type);

        /* add the found entries found to the local register */
        merge_entry_lists(&peer->entries, &found_var_entries, COPY_STRICT);
        printf("[GET VAR] register updated\n");
        /* print_entries_asc(&peer->entries, "REGISTER"); */

        free_entry_list(&found_tot_entries);
        free_entry_list(&not_found_tot_entries);
        free_entry_list(&not_found_var_entries);

        return;
    }

    free_entry_list(&found_var_entries); 

    printf("[GET VAR] some variation is still missing\n");
    print_entries_asc(&not_found_var_entries, "variations missing");

    /* add the aggr entries found to the local register */
    merge_entry_lists(&peer->entries, &found_var_entries_nbrs, COPY_STRICT);

    printf("[GET VAR] totals needed to compute the missing variations:\n");
    print_entries_asc(&not_found_tot_entries, NULL);

    /*----------------------------------------------
    |  launching flood for entries
    *---------------------------------------------*/ 

    printf("[GET VAR] flooding for entries\n");

    req_num = rand();

    /* set the new request as handled, so that when receiving flood
        requests with same number, those requests are "ignored" */
    request_serviced(peer, req_num);

    /* get object which will contain the request status and data */
    request = get_request_by_num(peer, req_num);

    request->number = req_num;
    request->requester_sd = REQUESTER_SELF;
    request->beg_period = beg_period;
    request->end_period = end_period;
    request->type = type;

    request->nbrs_remaining = 0;
    request->callback = &finalize_get_aggr_var;

    FD_ZERO(&request->peers_involved);
    connect_to_neighbors(peer, request, 0);

    request->required_entries = malloc(sizeof(EntryList));
    init_entry_list(request->required_entries);

    request->found_entries = malloc(sizeof(EntryList));
    init_entry_list(request->found_entries);

    request->required_entries->first = not_found_tot_entries.first;
    request->required_entries->last = not_found_tot_entries.last;
    request->required_entries->length = not_found_tot_entries.length;

    request->found_entries->first = found_tot_entries.first;
    request->found_entries->last = found_tot_entries.last;
    request->found_entries->length = found_tot_entries.length;

    free_entry_list(&not_found_var_entries);

    /* initializing response message as empty */
    request->response_msg = malloc(sizeof(Message));
    request->response_msg->body = malloc(256);
    set_num_of_peers(request->response_msg, 0);

    /* since the flooding is verbose, listening for user input is useless */
    disable_user_input(peer);

    /* set_timeout(peer, rand() % 4); */

    peer->state = STATE_STARTING_FLOOD;
}

/**
 * Called after flood for entries. Ask entries to the peers received
 * as response from the flooding and finished to compute the aggregate.
 *
 * @param peer 
 * @param peers peer ports
 * @param n number of peers
 * @param req_num request number
 */
void finalize_get_aggr_tot(ThisPeer *peer, in_port_t peers[], int n, int req_num)
{
    EntryList received_entries, found_entries, not_found_entries;
    Entry *entry;
    int32_t flags;
    int missing_entries;

    FloodRequest *request = get_request_by_num(peer, req_num);

    printf("[GET TOT FIN] finalising get sum command\n");
    
    print_entries_asc(request->required_entries, "asking this entries to peers");

    init_entry_list(&received_entries);

    ask_aggr_to_peers(peer, request->required_entries, &received_entries, peers, n);

    print_entries_asc(&received_entries, "entries received");

    /* add the received entries to the local register */
    merge_entry_lists(&peer->entries, &received_entries, COPY_STRICT);

    /* some entry may still be missing: no peer has those entries so it's
        assumed that their values are zero and are created and added to this
        peer's register */

    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;

    init_entry_list(&found_entries);
    init_entry_list(&not_found_entries);

    missing_entries = search_needed_entries(
        peer, 
        &found_entries, &not_found_entries, 
        request->beg_period, request->end_period, 0,
        flags,
        SCOPE_LOCAL
    );

    /* create the totals that were not received */
    if (missing_entries != 0)
    {
        print_entries_asc(&not_found_entries, "entries still missing");
        printf("[GET TOT FIN] creating missing entries\n");
        merge_entry_lists(&found_entries, &not_found_entries, COPY_STRICT);
    }

    /* there are now entires in this peer that have SCOPE_LOCAL because:
        1. they existed has SCOPE_LOCAL, and now their value has been
            updated to the global value using other peer's local values
        2. they did not exist, but have been retrieved from peers al local values
       those entries need to be set as SCOPE_GLOBAL */
    entry = found_entries.first;
    while (entry)
    {
        entry->flags |= SCOPE_GLOBAL;
        entry = entry->next;
    }

    merge_entry_lists(&peer->entries, &found_entries, COPY_STRICT);

    printf("[GET TOT FIN] register updated\n");
    /* print_entries_asc(&peer->entries, "REGISTER"); */

    free_entry_list(request->required_entries);
    free_entry_list(request->found_entries);

    get_aggr_tot(peer, request->beg_period, request->end_period, request->type);
    return;
}

/**
 * Called after flood for entries. Ask entries to the peers received
 * as response from the flooding and finished to compute the aggregate.
 *
 * @param peer 
 * @param peers peer ports
 * @param n number of peers
 * @param req_num request number
 */
void finalize_get_aggr_var(ThisPeer *peer, in_port_t peers[], int n, int req_num)
{
    EntryList found_tot_entries, not_found_tot_entries;
    EntryList found_var_entries, not_found_var_entries;
    EntryList received_entries;
    Entry* entry;
    int missing_entries;
    int32_t flags;

    FloodRequest *request = get_request_by_num(peer, req_num);

    printf("[GET VAR FIN] finalising get var command\n");

    print_entries_asc(request->required_entries, "asking this entries to peers");

    init_entry_list(&received_entries);

    ask_aggr_to_peers(peer, request->required_entries, &received_entries, peers, n);
    
    print_entries_asc(&received_entries, "entries received");

    /* add the received entries to the local register */
    merge_entry_lists(&peer->entries, &received_entries, COPY_STRICT);

    /* print_entries_asc(request->found_entries, "OLD REQUIRED ENTRIES"); */

    /* some entry may still be missing: no peer has those entries so it's
        assumed that their values are zero and are created and added to this
        peer's register */

    flags = AGGREG_PERIOD | SCOPE_LOCAL | TYPE_VARIATION;

    init_entry_list(&found_var_entries);
    init_entry_list(&not_found_var_entries);

    /* fill not_found_var_entries to remove entries in not_found_tot_entries
        that are not needed to compute the missing variations */
    search_needed_entries(
        peer, 
        &found_var_entries, &not_found_var_entries, 
        request->beg_period, request->end_period, 2,
        flags,
        SCOPE_GLOBAL
    );

    /* setting not found entries to SCOPE_GLOBAL because those entries
        are definetively missing in all peers */
    flags = AGGREG_DAILY | SCOPE_GLOBAL | TYPE_TOTAL;
    
    init_entry_list(&found_tot_entries);
    init_entry_list(&not_found_tot_entries);

    /* find the totals needed to compute the variaton.
       An entry may be local if it's present only in this peer:
        in this case, that entry is set to SCOPE_GLOBAL an put
        in found_tot_entries. */
    missing_entries = search_needed_entries(
        peer, 
        &found_tot_entries, &not_found_tot_entries, 
        request->beg_period, request->end_period, 0,
        flags,
        SCOPE_LOCAL
    );

    /* remove the totals not actually needed to compute the remaining variations */
    missing_entries -= remove_not_needed_totals(
        &not_found_tot_entries,
        &not_found_var_entries
    );

    /* create the totals that were not received */
    if (missing_entries != 0)
    {
        print_entries_asc(&not_found_tot_entries, "entries still missing");
        printf("[GET VAR FIN] creating missing entries\n");

        merge_entry_lists(&found_tot_entries, &not_found_tot_entries, COPY_STRICT);
    }

    /* setting as global all received and created entries */
    entry = found_tot_entries.first;
    while (entry)
    {
        entry->flags |= SCOPE_GLOBAL;
        entry = entry->next;
    }

    merge_entry_lists(&peer->entries, &found_tot_entries, COPY_STRICT);

    printf("[GET VAR FIN] register updated\n");
    /* print_entries_asc(&peer->entries, "REGISTER"); */

    free_entry_list(&found_var_entries);
    free_entry_list(&found_var_entries);
    free_entry_list(request->required_entries);
    free_entry_list(request->found_entries);

    get_aggr_var(peer, request->beg_period, request->end_period, request->type);
    return;
}



/*----------------------------------------------
 |  FUNCTIONS THAT HANDLE USER COMMANDS 
 *---------------------------------------------*/


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

    /* creating, binding and "connecting" UDP socket to discovery server */

    peer->dserver = socket(AF_INET, SOCK_DGRAM, 0);
    if (peer->dserver == -1)
    {
        perror("socket error");
        return -1;
    }

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

int cmd_add(ThisPeer *peer, int argc, char **argv)
{
    Entry *entry;
    int32_t tamponi, ncasi;

    if (argc != 3)
    {
        printf("usage: %s <type> <quantity>\n", argv[0]);
        return -1;
    }

    tamponi = ncasi = 0;

    if (strcmp(argv[1], ARG_TYPE_TAMPONI) == 0)
    {
        tamponi = atoi(argv[2]);
    }
    else if (strcmp(argv[1], ARG_TYPE_NCASI) == 0)
    {
        ncasi = atoi(argv[2]);
    }
    else
    {
        printf("invalid type \"%s\"\n", argv[1]);
        return -1;
    }

    entry = create_entry(
        get_today_adjusted(), 
        tamponi, ncasi, 
        SCOPE_LOCAL | TYPE_TOTAL | AGGREG_DAILY, 
        0
    );
    add_entry(&peer->entries, entry);

    print_entries_asc(&peer->entries, "REGISTER");

    return -1;
}

int cmd_get(ThisPeer *peer, int argc, char **argv)
{
    /* used to parse the period */
    char *token, *str;

    /* used to print the period */
    char str_time[TIMESTAMP_STRLEN];

    /* get command arguments */
    time_t period[2];
    int aggr, type;

    int count_dates;
    time_t now;
    int32_t flags;
    

    if (argc < 3 || argc > 4)
    {
        printf("usage: %s <aggr> <type> <period>\n", argv[0]);
        return -1;
    }

    period[0] = period[1] = 0;

    /* if period is specified */
    if (argc == 4)
    {
        /* iterate through strings divided by "-" to read the period */
        /* put start of period in period[0], end of period in period[1] */
        /* 0 is used as placeholder for asterisks */

        count_dates = 0;
        for (str = argv[3]; ; str = NULL)
        {
            token = strtok(str, "-");
            if (token == NULL)
                break;

            if (count_dates < 2)
            {
                if (strcmp(token, "*") == 0)
                {
                    period[count_dates] = 0;
                }
                else
                {
                    period[count_dates] = str_to_time(token);
                    if (period[count_dates] == -1)
                    {
                        printf("invalid date format: \"%s\"\n", token);
                        return -1;
                    }
                }
            }

            count_dates++;
        }

        /* more or less than two arguments in period */
        if (count_dates != 2)
        {
            printf("invalid period format\n");
            return -1;
        }

        /* both dates are asterisks */
        if (period[1] == 0 && period[0] == 0)
        {
            printf("invalid period format\n");
            return -1;   
        }
    }

    /* if after REG_CLOSING_HOUR, `now` is tomorrow */
    now = get_today_adjusted();

    /* start of period is "*" */
    if (period[0] == 0)
    {
        period[0] = BEGINNING_OF_TIME;
    }

    /* repalcing end period to yesterday if it was set to "*" */
    if (period[1] == 0)
    {
        period[1] = add_days(now, -1);
    }
    /* if end of period is in the future */
    else if (difftime(period[1], now) >= 0)
    {
        printf("invalid end period date\n");
        return -1;
    }
    
    /* if end date is before start date */
    if (period[1] < period[0])
    {
        printf("invalid period\n");
        return -1;
    }

    if (strcmp(argv[1], ARG_AGGREG_SUM) == 0)
    {
        flags = TYPE_TOTAL;
        aggr = REQ_SOMMA;
    }
    else if (strcmp(argv[1], ARG_AGGREG_VAR) == 0)
    {
        flags = TYPE_VARIATION;
        aggr = REQ_VARIAZIONE;
    }
    else
    {
        printf("invalid aggr \"%s\"\n", argv[1]);
        return -1;
    }
    flags |= AGGREG_PERIOD | SCOPE_GLOBAL;

    if (strcmp(argv[2], ARG_TYPE_TAMPONI) == 0)
    {
        type = REQ_TAMPONI;
    }
    else if (strcmp(argv[2], ARG_TYPE_NCASI) == 0)
    {
        type = REQ_NUOVI_CASI;
    }
    else
    {
        printf("invalid type \"%s\"\n", argv[2]);
        return -1;
    }

    
    printf("\n---\n");
    printf("CALCOLO ");
    if (aggr == REQ_SOMMA) printf("TOTALE ");
    else                   printf("VARIAZIONI ");
    if (type == REQ_TAMPONI) printf("TAMPONI ");
    else                     printf("NUOVI CASI ");

    if (period[0] == 0)
        printf("FRA INIZIO REGISTER ");
    else
    {
        time_to_str(str_time, &period[0]);
        printf("FRA %s ", str_time);
    }
    time_to_str(str_time, &period[1]);
    printf("E %s", str_time);
    printf("\n---\n\n");

    /* the aggregation is done "asynchronously" */

    if (aggr == REQ_SOMMA)
        get_aggr_tot(peer, period[0], period[1], type);
    else
        get_aggr_var(peer, period[0], period[1], type);

    return 0;
}

int cmd_stop(ThisPeer *peer, int argc, char **argv)
{
    if (peer->state == STATE_OFF)
    {
        printf("peer not started\n");
        return 0;
    }

    if (peer->state != STATE_STARTED)
    {
        printf("cannot stop peer now\n");
        return 0;
    }

    return send_stop_msg_to_dserver(peer);
}

/* print the register */
int cmd_reg(ThisPeer* peer, int argc, char** argv)
{
    printf("\n");
    print_entries_asc(&peer->entries, "REGISTER");
    return 0;
}

/* load register from file */
int cmd_load(ThisPeer* peer, int argc, char** argv)
{
    if (argc != 2)
    {
        printf("usage: %s <file>\n", argv[0]);
        return -1;
    }

    load_register_from_file(&peer->entries, argv[1]);

    print_entries_asc(&peer->entries, "REGISTER");

    return 0;
}

int cmd_help(ThisPeer *peer, int argc, char **argv)
{
    printf("1) start <address> <port> --> richiede al DS la connessione al network\n");
    printf("2) add <type> <quantity> --> aggiunge una nuova entry\n");
    printf("3) get <aggr> <type> <period> --> esegue la query specificata\n");
    printf("4) stop --> chiude il DSDettaglio comandi\n");
    printf("5) reg --> mostra le entry nel register\n");
    printf("6) load <file> --> carica entry da un file\n");
    printf("7) help --> mostra i dettagli dei comandi\n");

    return 0;
}

#define NUM_CMDS 7

char *cmd_str[NUM_CMDS] = { "start", "add", "get", "stop", "reg", "load", "help" };

int (*cmd_func[NUM_CMDS])(ThisPeer*, int, char**) = 
    { &cmd_start, &cmd_add, &cmd_get, &cmd_stop, &cmd_reg, &cmd_load, &cmd_help };



/*----------------------------------------------
 |  FUNCTIONS THAT HANDLE SERVER REQUESTS
 *---------------------------------------------*/

/* called if the server has not received the start message */
void handle_failed_connection_attempt(ThisPeer *peer)
{
    printf("trying again in 3 seconds...\n");
    sleep(3);
    send_start_msg_to_dserver(peer);
}

/* called when server sends a list of neighbors to the peer */
void handle_set_neighbors_response(ThisPeer *peer, Message *msgp)
{
    int sd, ret;
    int reuse = 1;

    if (peer->state == STATE_STARTING)
    {
        printf("[SET NBRS] setting list of neighbors\n");

        sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd == -1)
        {
            perror("[SET NBRS]socket error");
            printf("[SET NBRS]could not start listening for peers\n");
            return;
        }

        if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
            perror("[SET NBRS] setsockopt(SO_REUSEADDR) failed");

        ret = bind(sd, (struct sockaddr *)&peer->addr, sizeof(peer->addr));
        if (ret == -1)
        {
            perror("[SET NBRS] bind error");
            printf("[SET NBRS] could not start listening for peers\n");
            return;
        }

        ret = listen(sd, 10);
        if (ret == -1)
        {
            perror("[SET NBRS] listen error");
            printf("[SET NBRS] could not start listening for peers\n");
            return;
        }
        
        peer->listening = sd;
        add_desc(&peer->master_read_set, &peer->fdmax_r, sd);
    }
    else
        printf("[SET NBRS] refreshing list of neighbors\n");
    
    if (peer->state == STATE_OFF) return;

    free_graph(&peer->neighbors);
    peer->neighbors.first = peer->neighbors.last = NULL;
    deserialize_peers(msgp->body, &peer->neighbors.first);
    print_peers(peer->neighbors.first);

    peer->state = STATE_STARTED;

    clear_timeout(peer);
    enable_user_input(peer);
}

/* called when servers responds to a "stop" command sent by this peer */
void handle_stop_response(ThisPeer *peer, Message *msgp)
{
    int req_num;
    FloodRequest *request;

    if (msgp->type != MSG_STOP)
        return;

    if (peer->state == STATE_STOPPING)
    {
        print_entries_asc(&peer->entries, "[STOP] sending this entries to neighbors");

        req_num = rand();
        request_serviced(peer, req_num);
        request = get_request_by_num(peer, req_num);
        FD_ZERO(&request->peers_involved);

        request->nbrs_remaining = connect_to_neighbors(peer, request, 0);

        peer->state = STATE_STOPPED;
    }
    else if (peer->state == STATE_STARTED)
    {
        printf("[STOP] server stopped\n");
        peer->state = STATE_STOPPED;
        terminate_peer(peer);
        exit(EXIT_SUCCESS);
    }
}



/*----------------------------------------------
 |  FUNCTIONS THAT HANDLE PEER REQUESTS
 *---------------------------------------------*/


void handle_req_data(ThisPeer *peer, Message *msgp, int sd)
{
    EntryList req_entries;
    EntryList found_entries;
    Entry *req_entry, *found_entry, *removed_entry;
    char *buff;
    int ret;

    init_entry_list(&req_entries);
    deserialize_entries(msgp->body, &req_entries);

    printf("[REQ DATA] peer %d requires entries:\n", msgp->id);
    print_entries_asc(&req_entries, NULL);

    init_entry_list(&found_entries);
    
    /* find the requested entries in local register */
    /* put a copies of found entries in found_entries */
    /* remove found entries from req_entries */

    req_entry = req_entries.last;
    while (req_entry)
    {
        found_entry = search_entry(
            peer->entries.last, 
            req_entry->timestamp,
            req_entry->flags,
            req_entry->period_len
        );

        removed_entry = NULL;

        if (found_entry != NULL)
        {
            add_entry(&found_entries, copy_entry(found_entry));
            remove_entry(&req_entries, req_entry);
            removed_entry = req_entry;
        }

        req_entry = req_entry->prev;
        free(removed_entry);
    }

    print_entries_asc(&found_entries, "available entries");
    print_entries_asc(&req_entries, "missing entries");

    msgp->type = MSG_REPLY_DATA;
    msgp->id = get_peer_id(peer);
    msgp->body = allocate_entry_list_buffer(found_entries.length + req_entries.length);
    buff = serialize_entries(msgp->body, &found_entries);
    buff = serialize_entries(buff, &req_entries);
    msgp->body_len = buff - msgp->body;

    ret = send_message(sd, msgp);
    if (ret == -1)
    {
        printf("[REQ DATA] could not send entries to requester\n");
        return;
    }

    free(msgp->body);

    printf("[REQ DATA] entries sent to requester\n");
}

/* called if a FLOOD_FOR_ENTRIES request is received */
void handle_flood_for_entries(ThisPeer *peer, Message *msgp, int sd)
{
    EntryList found_entries;
    Entry *req_entry, *found_entry, *removed_entry;
    int ret;
    FloodRequest *request;

    printf("[FLOOD REQ] handling flood for entries for %d (sd: %d)\n", msgp->id, sd);
    printf("[FLOOD REQ] req num: %d\n", msgp->req_num);

    /* avoid servicing requests more than once */
    /* cannot handle more than one request at once */
    if (!valid_request(peer, msgp->req_num) || (peer->state != STATE_STARTED))
    {
        printf("[FLOOD REQ] request already handled, sending empty response\n");

        msgp->type = MSG_ENTRIES_FOUND;
        msgp->id = get_peer_id(peer);
        
        set_num_of_peers(msgp, 0);
        
        ret = send_message(sd, msgp);
        if (ret == -1)
        {
            printf("[FLOOD REQ] could not send empty FLOOD response\n");
            return;
        }
        return;
    }
    request_serviced(peer, msgp->req_num);
    request = get_request_by_num(peer, msgp->req_num);
    request->number = msgp->req_num;

    /* reading requested entries */
    
    request->required_entries = malloc(sizeof(EntryList));
    init_entry_list(request->required_entries);
    deserialize_entries(msgp->body, request->required_entries);

    printf("[FLOOD REQ] I've been asked to look for:\n");
    print_entries_asc(request->required_entries, NULL);

    init_entry_list(&found_entries);

    /* iterating through requested entries and searching them in the 
        register. The ones that are not found, or are found but have
        SCOPE_LOCAL, are put into req_entries to be asked to this
        peer's neighbors. */

    req_entry = request->required_entries->first;
    while (req_entry != NULL) 
    {
        found_entry = search_entry(
            peer->entries.last, 
            req_entry->timestamp, 
            req_entry->flags, 
            req_entry->period_len
        );

        removed_entry = NULL;

        if (found_entry != NULL)
        {
            add_entry(&found_entries, copy_entry(found_entry));

            /* global entries have the same value for all the peers.
                The neighbors wont be asked for global entries if this
                peer already has some of them */
            if ((found_entry->flags & ENTRY_SCOPE) == SCOPE_GLOBAL)
            {
                remove_entry(request->required_entries, req_entry);
                removed_entry = req_entry;
            }
        }
        req_entry = req_entry->next;  
        free(removed_entry);  
    }

    print_entries_asc(&found_entries, "entries I've got");

    print_entries_asc(request->required_entries, "flooding for");
    
    /* if this peer has all the required entries, stop the flooding */
    if (is_entry_list_empty(request->required_entries))
    {
        printf("[FLOOD REQ] I have all the entries, stopping the flooding\n");

        request->response_msg = malloc(sizeof(Message));

        request->response_msg->type = MSG_ENTRIES_FOUND;
        request->response_msg->req_num = msgp->req_num;
        request->response_msg->id = get_peer_id(peer);
        request->response_msg->body = malloc(256);

        set_num_of_peers(request->response_msg, 0);
        add_peer_to_msg(request->response_msg, ntohs(peer->addr.sin_port));

        ret = send_message(sd, request->response_msg);
        if (ret == -1)
        {
            printf("[FLOOD REQ] could not send (immediate) FLOOD response\n");
            return;
        }

        free_entry_list(&found_entries);
        free_entry_list(request->required_entries);

        return;
    }

    /* establishing connections with neighbors */
    /* A request will be actually sent to a neighbor when it will
        accept the connection (when the socket is ready to write).
       When a socket is ready to write, do_flood_for_entries is called. */

    FD_ZERO(&request->peers_involved);
    connect_to_neighbors(peer, request, msgp->id);

    peer->state = STATE_HANDLING_FLOOD;

    /* configuring main IO multiplexing */
    disable_user_input(peer);
    set_timeout(peer, rand() % 4);

    /* configuring request-handling data */
    /* request->number = msgp->req_num; */
    request->requester_sd = sd;
    request->nbrs_remaining = 0; /* how many nbrs accepted the connection */

    /* creating aggregated response message */
    /* each nbr response is added to this message's body */
    request->response_msg = malloc(sizeof(Message));
    request->response_msg->type = MSG_ENTRIES_FOUND;
    request->response_msg->req_num = msgp->req_num;
    request->response_msg->id = get_peer_id(peer); /* for debugging */
    request->response_msg->body = malloc(256);
    
    request->callback = NULL;

    set_num_of_peers(request->response_msg, 0);

    /* if this peer has some of the requested entries */
    if (!is_entry_list_empty(&found_entries))
    {
        /* add this peer to the aggregated response message */
        printf("[FLOOD REQ] adding my address to aggregated response\n");
        add_peer_to_msg(request->response_msg, ntohs(peer->addr.sin_port));
    }
    else
    {
        printf("[FLOOD REQ] NOT adding my address to aggregated response\n");
    }

    free_entry_list(&found_entries);
}

/* called for each socket that is ready to be written during a flooding */
/**
 * Send a FLOOD_FOR_ENTRIES message using the specified socket, with
 * entries previously in peer->req_entries.
 * 
 * @param peer 
 * @param sd socket ready to write
 */
void do_flood_for_entries(ThisPeer *peer, int sd)
{
    Message msg;
    int ret;

    FloodRequest *request = get_request_by_sd(peer, sd);

    printf("[FLOOD] sending FLOOD request to sd %d\n", sd);

    msg.type = MSG_FLOOD_FOR_ENTRIES;
    msg.req_num = request->number;
    msg.id = get_peer_id(peer);
    msg.body = allocate_entry_list_buffer(request->required_entries->length);
    msg.body_len = serialize_entries(msg.body, request->required_entries) - msg.body;

    ret = send_message(sd, &msg);
    if (ret == -1) 
    {
        printf("[FLOOD] could not send FLOOD request\n");
        free(msg.body);
        return;
    }

    free(msg.body);

    printf("[FLOOD] sent request to neighbor (%d)\n", request->nbrs_remaining);

    /* moving the socket from the write set, to the read set */
    remove_desc(&peer->master_write_set, &peer->fdmax_w, sd);
    add_desc(&peer->master_read_set, &peer->fdmax_r, sd);

    /* write set not completely empty -> other sockets remain to write */
    if (peer->fdmax_w != -1)
        set_timeout(peer, rand() % 4);
    else
    {
        printf("[FLOOD] last request sent\n");
        clear_timeout(peer);
    }
    
    request->nbrs_remaining++;
    
    printf("[FLOOD] waiting for %d responses\n", request->nbrs_remaining);
}

/**
 * Handle an ENTRIES_FOUND response message (response to FLOOD_FOR_ENTRIES).
 * 
 * @param peer 
 * @param msgp message received
 * @param sd socket from which the message was received
 */
void handle_flood_response(ThisPeer *peer, Message *msgp, int sd)
{
    int ret, num_of_peers;
    in_port_t *peers;

    FloodRequest *request = get_request_by_num(peer, msgp->req_num);

    printf("[FLOOD RSP] received flood response from %d (sd: %d)\n", msgp->id, sd);

    if (request->requester_sd != REQUESTER_SELF)
    {
        printf("[FLOOD RSP] relaying flood response\n");

        if (request->response_msg == NULL)
        {
            printf("[FLOOD RSP] flood response already relayed\n");
            return;
        }
    }

    printf("[FLOOD RSP] received num of peers: %d\n", get_num_of_peers(msgp));

    /* updating the number of peers that satisfy the request in 
        the aggregated response message */
    
    merge_peer_list_msgs(request->response_msg, msgp);
    
    printf("[FLOOD RSP] new num of peers: %d\n", get_num_of_peers(request->response_msg));
    
    /* this is the last response message */
    if (request->nbrs_remaining == 1)
    {
        printf("[FLOOD RSP] last flood response message\n");

        if (request->requester_sd != REQUESTER_SELF)
        {
            /* sending back this peer's aggregated response */

            request->response_msg->type = MSG_ENTRIES_FOUND;
            request->response_msg->req_num = request->number;

            printf("[FLOOD RSP] sending flood response to sd %d\n", request->requester_sd);
            
            ret = send_message(request->requester_sd, request->response_msg);
            if (ret == -1)
            {
                printf("[FLOOD RSP] could not send (relay) FLOOD response\n");
                return;
            }

            printf("[FLOOD RSP] closing %d\n", sd);
            close(sd);
            remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
            remove_desc(&request->peers_involved, NULL, sd);

            free(request->response_msg);
            request->response_msg = NULL;
        }
        else
        {   
            printf("[FLOOD RSP] closing %d\n", sd);
            close(sd);
            remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
            remove_desc(&request->peers_involved, NULL, sd);

            peers = (in_port_t*)request->response_msg->body;

            num_of_peers = get_num_of_peers(request->response_msg);

            printf("\n[FLOOD RSP] %d peers have entries for me:\n", num_of_peers);

            /* deserializing received peers addresses */
            while (num_of_peers > 0)
                printf("[FLOOD RSP] peer: %d\n", peers[--num_of_peers]);
            
            if (request->callback != NULL)
            {
                num_of_peers = get_num_of_peers(request->response_msg);
                printf("[FLOOD RSP] finalising the computation of aggregate\n");

                peers = calloc(num_of_peers, sizeof(in_port_t));

                if (peers != NULL)
                    memcpy(peers, request->response_msg->body, num_of_peers * sizeof(in_port_t));

                /* TODO move callback at end of function? */
                (*request->callback)(peer, peers, num_of_peers, request->number);
                /* TODO when to free msgp? */
            }
        }
    }
    else
    {
        printf("[FLOOD RSP] closing %d\n", sd);
        close(sd);
        remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
        remove_desc(&request->peers_involved, NULL, sd);
    }

    /* TODO check when to close the sockets (in demux? really?) */
    /* close(sd);
    remove_desc(&peer->master_read_set, &peer->fdmax_r, sd); */

    request->nbrs_remaining--;

    printf("[FLOOD RSP] waiting for %d more responses\n", request->nbrs_remaining);

    /* other sockets remain to read */
    if (request->nbrs_remaining > 0)
    {
        set_timeout(peer, rand() % 4);
    }
    else
    {
        printf("[FLOOD RSP] all expected responses received\n");
    
        peer->state = STATE_STARTED;
        clear_timeout(peer);
        enable_user_input(peer);
    }

}

/* called when a socket conencted to a peer is ready to be written during stop */
void do_share_register(ThisPeer *peer, int sd)
{
    EntryList shared_entries;
    Entry *entry, *next_entry;
    Message msg;
    int how_many, ret;

    FloodRequest *request = get_request_by_sd(peer, sd);
    
    printf("[SHARE] sending entries to neighbors (sd: %d)\n", sd);

    /* divide register in equal parts among neighbors */
    how_many = peer->entries.length / request->nbrs_remaining;

    init_entry_list(&shared_entries);
    
    entry = peer->entries.first;
    while (entry && how_many > 0)
    {
        next_entry = entry->next;
        
        remove_entry(&peer->entries, entry);
        entry->next = entry->prev = NULL;
        add_entry(&shared_entries, entry);

        how_many--;
        entry = next_entry;
    }

    msg.type = MSG_ADD_ENTRY;
    msg.id = get_peer_id(peer);
    msg.body = allocate_entry_list_buffer(shared_entries.length);
    is_entry_list_empty(&shared_entries);
    msg.body_len = serialize_entries(msg.body, &shared_entries) - msg.body;

    ret = send_message(sd, &msg); /* sending entries */
    if (ret == -1)
    {
        printf("[SHARE] could not send entries to neighbor\n");
        return;
    }
    
    close(sd);
    remove_desc(&peer->master_write_set, &peer->fdmax_w, sd);

    free_entry_list(&shared_entries);

    request->nbrs_remaining--;

    if (request->nbrs_remaining == 0) 
    {
        printf("[SHARE] entries sent to all neighbors\n");
        
        terminate_peer(peer);
        exit(EXIT_SUCCESS);

        return;
    }
}

/* called when a peers sends entries to this peer in any moment */
void handle_add_entry(ThisPeer *peer, Message *msgp)
{
    EntryList new_entries;

    printf("[ADD] received new entries from peer %hd\n", msgp->id);

    init_entry_list(&new_entries);
    deserialize_entries(msgp->body, &new_entries);
    print_entry(new_entries.first);

    print_entries_asc(&new_entries, "new entries");

    merge_entry_lists(&peer->entries, &new_entries, COPY_STRICT);

    print_entries_asc(&peer->entries, "REGISTER");
}



/*----------------------------------------------
 |  I/O DEMULTIPLEXING
 *---------------------------------------------*/


void demux_user_input(ThisPeer *peer)
{
    int i, argc;
    char **argv;

    argv = scan_command_line(&argc);

    if (argv == NULL || argc == 0) 
    {
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
    Message msg;
    int ret;
    struct sockaddr_in addr;
    socklen_t slen;

    printf("\n");
    /* printf("\n[demultiplexing peer request]\n"); */

    if (sd == peer->listening)
    {
        slen = sizeof(addr);
        ret = accept(sd, (struct sockaddr*)&addr, &slen);
        if (ret == -1)
        {
            perror("accept error");
            printf("could not accept peer conenction\n");
            return;
        }
        printf("accepted connection to peer (sd: %d)\n", ret);
        add_desc(&peer->master_read_set, &peer->fdmax_r, ret);
        return;
    }

    if (FD_ISSET(sd, &peer->master_write_set))
    {
        printf("socket ready to write\n");
        
        if (peer->state == STATE_HANDLING_FLOOD ||
            peer->state == STATE_STARTING_FLOOD)
            do_flood_for_entries(peer, sd);
        
        else if (peer->state == STATE_STOPPED)
            do_share_register(peer, sd);
        else {
            printf("error sd: %d, state: %d\n", sd, peer->state);
            exit(EXIT_FAILURE);
        }

        return;
    }

    ret = recv_message(sd, &msg);
    if (ret == -1)
    {
        printf("error receiving peer request\n");
        return;
    }

    if (ret == 0)
    {
        printf("peer disconnected (sd: %d)\n", sd);
        remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
        /* if (peer->state == STATE_HANDLING_FLOOD)
            peer->nbrs_remaining--; */

        return;
    }

    if (msg.type == MSG_REQ_DATA)
    {
        handle_req_data(peer, &msg, sd);
        close(sd);
        remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
    }
    else if (msg.type == MSG_FLOOD_FOR_ENTRIES)
    {
        handle_flood_for_entries(peer, &msg, sd);
        /* close(sd);
        remove_desc(peer, sd); */
    }
    else if (msg.type == MSG_ENTRIES_FOUND)
    {
        handle_flood_response(peer, &msg, sd);
        /* close(sd);
        remove_desc(peer, sd); */
    }
    else if (msg.type == MSG_ADD_ENTRY)
    {
        handle_add_entry(peer, &msg);
    }
}

void demux_server_request(ThisPeer *peer)
{
    Message msg;
    int ret;

    printf("\n");
    /* printf("\n[demultiplexing server request]\n"); */

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
    else if (msg.type == MSG_STOP)
    {
        handle_stop_response(peer, &msg);
    }
}



int main(int argc, char** argv)
{
    ThisPeer peer;
    fd_set working_read_set, working_write_set;
    int ret, i, fdmax, desc_ready;

    /* used to send fake commands */
    char str_cmd[64];
    int argc2;
    char **argv2;

    if (argc != 2)
    {
        printf("usage: ./peer <porta>\n");
        exit(EXIT_FAILURE);
    }

    ret = validate_port(argv[1]);
    if (ret == -1) 
        exit(EXIT_FAILURE);

    printf("*********** PEER %d ***********\n", ret);

    init_peer(&peer, ret);

    srand(time(NULL) + peer.addr.sin_port);

    add_desc(&peer.master_read_set, &peer.fdmax_r, STDIN_FILENO);

    sprintf(str_cmd, "load reg%c.in", argv[1][strlen(argv[1]) - 1]);
    argv2 = get_command_line(str_cmd, &argc2);
    cmd_load(&peer, argc2, argv2);
    free_command_line(argc, argv2);

    argv2 = get_command_line("start 127.0.0.1 4242", &argc2);
    cmd_start(&peer, argc2, argv2);
    free_command_line(argc, argv2);

    for (;;)
    {
        working_read_set = peer.master_read_set;
        working_write_set = peer.master_write_set;

        fdmax = peer.fdmax_r;
        if (peer.fdmax_w > fdmax)
            fdmax = peer.fdmax_w;

        if (is_user_input_enabled(&peer))
        {
            printf("peer@%d >>> ", ntohs(peer.addr.sin_port));
            fflush(stdout);
        }

        desc_ready = select(
            fdmax + 1, 
            &working_read_set, 
            &working_write_set, 
            NULL, 
            peer.actual_timeout
        );

        if (desc_ready == -1)
        {
            perror("select error");
            exit(EXIT_FAILURE);
        }
        
        if (desc_ready == 0)
        {
            if (peer.state == STATE_STARTING)
                send_start_msg_to_dserver(&peer);

            else if (peer.state == STATE_STARTING)
                send_stop_msg_to_dserver(&peer);

            else if (peer.state == STATE_HANDLING_FLOOD ||
                     peer.state == STATE_STARTING_FLOOD)
                set_timeout(&peer, rand() % 4);
        }

        for (i = 0; i <= fdmax && desc_ready > 0; i++)
        {
            if (!FD_ISSET(i, &working_read_set) && 
                !FD_ISSET(i, &working_write_set)) continue;

            desc_ready--;

            if      (i == STDIN_FILENO) demux_user_input(&peer);
            else if (i == peer.dserver) demux_server_request(&peer);
            else                        demux_peer_request(&peer, i);
        }
    }
}
