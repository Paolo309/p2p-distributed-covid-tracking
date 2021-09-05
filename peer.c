#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include "network.h"
#include "data.h"
#include "comm.h"
#include "commandline.h"
#include "graph.h"

/* comment out the next two defines to user the real date */
#define FAKE_DAY 28
#define FAKE_MONTH 2
#define FAKE_YEAR 2021

#define REG_CLOSING_HOUR 23

#define REQ_SOMMA 0
#define REQ_VARIAZIONE 1
#define REQ_TAMPONI 2
#define REQ_NUOVI_CASI 3

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

/* in_port_t get_port(int sd)
{
    struct sockaddr_in addr;
    socklen_t slen = sizeof(addr);
    int ret;

    ret = getsockname(sd, (struct sockaddr *)&addr, &slen);

    if (ret == -1)
    {
        perror("getsockname error");
        return -1;
    }
    
    return ntohs(addr.sin_port);
} */

int32_t get_num_of_peers(Message *msgp)
{
    /* return *(int32_t*)msgp->body; */
    return msgp->body_len / sizeof(in_port_t);
}

void set_num_of_peers(Message *msgp, int32_t num)
{
    /* return *(int32_t*)msgp->body = num; */
    msgp->body_len = num * sizeof(in_port_t);
}

void inc_num_of_peers(Message *msgp, int32_t num)
{
    /* return *(int32_t*)msgp->body += num; */
    msgp->body_len += num * sizeof(in_port_t);
}

int32_t add_peer_to_msg(Message *msgp, in_port_t port)
{
    int32_t num_of_peers = get_num_of_peers(msgp);

    *((in_port_t*)msgp->body + num_of_peers) = port;
    inc_num_of_peers(msgp, 1);
    
    return num_of_peers + 1;
}

int32_t merge_peer_list_msgs(Message *dest, Message *src)
{
    char *dest_body;
    int32_t src_num_of_peers, dest_num_of_peers;

    src_num_of_peers = get_num_of_peers(src);
    dest_num_of_peers = get_num_of_peers(dest);

    dest_body = dest->body + dest_num_of_peers * sizeof(in_port_t);

    memcpy(dest_body, src->body, src_num_of_peers * sizeof(in_port_t));
    inc_num_of_peers(dest, src_num_of_peers);

    return src_num_of_peers + dest_num_of_peers;
}

in_port_t get_peer_port(Message *msgp, int n)
{
    return *((in_port_t*)msgp->body + n);
}

#define STATE_OFF 0
#define STATE_STARTING 1
#define STATE_STARTED 2
#define STATE_STARTING_FLOOD 3
#define STATE_HANDLING_FLOOD 4

#define NUM_LAST_REQUESTS 3
#define REQUESTER_SELF -1

struct ThisPeer;

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
    int last_request_nums[NUM_LAST_REQUESTS];
    FloodRequest last_requests[NUM_LAST_REQUESTS];
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
    peer->lr_tail = NUM_LAST_REQUESTS - 1;    

    create_graph(&peer->neighbors);
}

void _add_desc(fd_set *fdsetp, int *fdmax, int fd)
{
    FD_SET(fd, fdsetp);
    if (fdmax != NULL && fd > *fdmax)
        *fdmax = fd;
}

void _remove_desc(fd_set *fdsetp, int *fdmax, int fd)
{
    FD_CLR(fd, fdsetp);
    
    if (fdmax != NULL && fd == *fdmax)
    {
        while (FD_ISSET(*fdmax, fdsetp) == false && *fdmax >= 0)
            *fdmax -= 1;
    }
}

void add_desc(ThisPeer *peer, int fd)
{
    _add_desc(&peer->master_read_set, &peer->fdmax_r, fd);
}

void remove_desc(ThisPeer *peer, int fd)
{
    _remove_desc(&peer->master_read_set, &peer->fdmax_r, fd);
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

bool is_user_input_enabled(ThisPeer *peer)
{
    return FD_ISSET(STDIN_FILENO, &peer->master_read_set);
}

void request_serviced(ThisPeer *peer, int req_num)
{
    peer->last_request_nums[peer->lr_tail] = req_num;
    peer->lr_head = (peer->lr_head + 1) % NUM_LAST_REQUESTS;
    peer->lr_tail = (peer->lr_tail + 1) % NUM_LAST_REQUESTS;
}

bool valid_request(ThisPeer *peer, int req_num)
{
    int i;
    for (i = 0; i < NUM_LAST_REQUESTS; i++)
    {
        if (peer->last_request_nums[i] == req_num)
            return false;
    }
    return true;
}

FloodRequest *get_request_by_num(ThisPeer *peer, int req_num)
{
    int i;
    for (i = 0; i < NUM_LAST_REQUESTS; i++)
    {
        if (peer->last_request_nums[i] == req_num)
            return &peer->last_requests[i];
    }
    return NULL;
}

FloodRequest *get_request_by_sd(ThisPeer *peer, int sd)
{
    int i;
    for (i = 0; i < NUM_LAST_REQUESTS; i++)
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



/*----------------------------------------------
 |  UTILITY FUNCTIONS TO COMPUTE AGGREGATES
 *---------------------------------------------*/


/**
 * Sum the values of all the entries at put the result in entry_res.
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
            /* TODO free entry? */
        }
        entry = entry->prev;
    }
}

/**
 * Computes the variations between consecutive entries in the 
 * specified list and puts the results into entries_res.
 * Does not check if the entries are valid, nor if the timestamps
 * are consecutive. Only uses entries that have SCOPE_GLOBAL.
 * 
 * @param peer 
 * @param entries source list
 * @param entries_res result list
 */
void compute_aggr_var(ThisPeer *peer, EntryList *entries, EntryList *entries_res, int32_t flags)
{
    Entry *entry, *tmp;

    if (entries->last == NULL)
        return;

    entry = entries->last->prev;
    while (entry)
    {
        /* if ((entry->flags       & ENTRY_SCOPE) == SCOPE_GLOBAL &&
            (entry->next->flags & ENTRY_SCOPE) == SCOPE_GLOBAL) */
        if ((entry->flags       & ENTRY_SCOPE) == SCOPE_GLOBAL &&
            (entry->next->flags & ENTRY_SCOPE) == SCOPE_GLOBAL &&
            entry->timestamp == entry->next->timestamp - 86400)
        {
            tmp = create_entry(
                entry->timestamp,
                entry->next->tamponi - entry->tamponi,
                entry->next->nuovi_casi - entry->nuovi_casi,
                flags
            );
            tmp->period_len = 2;
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
 * nuovi_casi, and is put in the list `not_found`.
 * 
 * TODO OPTIMIZE
 * 
 * @param peer 
 * @param found 
 * @param not_found 
 * @param start 
 * @param end 
 * @param flags 
 * @return int 
 */
int search_needed_entries(ThisPeer *peer, EntryList *found, EntryList *not_found, time_t start, time_t end, int32_t period_len, int32_t flags)
{
    Entry *found_entry;
    Entry *new_entry;
    time_t t_day;
    struct tm *tm_day;
    int count = 0;

    t_day = end;
    tm_day = localtime(&t_day);
    
    /* for each day of the period */
    while (difftime(t_day, start) >= 0)
    {
        found_entry = search_entry(peer->entries.last, t_day, flags, period_len);

        if (found_entry != NULL && (found_entry->flags & ENTRY_SCOPE) == SCOPE_GLOBAL)
        {
            /* TODO create copy entry function */
            new_entry = create_entry(found_entry->timestamp, found_entry->tamponi, found_entry->nuovi_casi, found_entry->flags);
            new_entry->period_len = found_entry->period_len;
            add_entry(found, new_entry);
        }
        else
        {
            new_entry = create_entry(t_day, 0, 0, flags);
            new_entry->period_len = period_len;
            add_entry(not_found, new_entry);
            count++;
        }

        /* move back one day */
        tm_day->tm_mday--;
        t_day = mktime(tm_day);
        tm_day = localtime(&t_day);
    }

    return count;
}

/**
 * Removes from needed_totals the entries that are not needed to compute
 * the needed variations in needed_vars. An entry in needed_totals is
 * removed if there is no entry in needed_vars with same timestamp, or
 * timestamp relative to the day before.
 * E.g. an entry total of day x is needed only if you have to compute
 * a variation between x+1 and x, or between x and x-1.
 * 
 * @param needed_totals 
 * @param needed_vars 
 * @return int 
 */
int remove_not_needed_totals(EntryList *needed_totals, EntryList *needed_vars)
{   
    Entry *entry, *found_entry, *removed_entry; 
    int count = 0; /* removed entries */

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

        if (found_entry == NULL) /* entry not needed */
        {
            removed_entry = entry;
            remove_entry(needed_totals, entry);
            count++;
        }

        entry = entry->prev;
        free(removed_entry);
    }

    return count;
}



/*----------------------------------------------
 |  UTILITY FUNCTIONS HANDLE FLOODINGS
 *---------------------------------------------*/

/**
 * Connect to all the neighbors and put the created socket 
 * descriptors in master_write_set. If a request is specified,
 * the socket descriptors are also put in the request's 
 * peers_involved fd set.
 * 
 * TODO add except neighbor
 * 
 * @param peer 
 * @param request 
 */
void connect_to_neighbors(ThisPeer *peer, FloodRequest *request)
{
    int sd, ret;
    socklen_t slen;
    GraphNode *nbr;

    slen = sizeof(struct sockaddr_in);

    nbr = peer->neighbors.first;
    while (nbr)
    {
        sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd == -1)
        {
            perror("socket error");
            return;
        }

        ret = connect(sd, (struct sockaddr *)&nbr->peer->addr, slen);
        if (ret == -1)
        {
            perror("connect error");
            return;
        }

        _add_desc(&peer->master_write_set, &peer->fdmax_w, sd);
        if (request != NULL)
        {
            printf("adding involved sd %d\n", sd);
            _add_desc(&request->peers_involved, NULL, sd);
        }
        
        nbr = nbr->next;
    }
}

bool ask_aggr_to_neighbors_v2(ThisPeer *peer, EntryList *req_aggr, EntryList *recvd_aggr)
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
        msg.body_len = serialize_entries(msg.body, req_aggr) - msg.body;
        
        printf("asking aggr to %d\n", ntohs(nbr->peer->addr.sin_port));

        ret = send_message(sd, &msg);
        if (ret == -1)
        {
            printf("could not send REQ_DATA to peer %d\n", ntohs(nbr->peer->addr.sin_port));
            return false;
        }

        ret = recv_message(sd, &msg);
        if (ret == -1)
        {
            printf("error while receiving REPLY_DATA\n");
            return false;
        }

        close(sd);

        /* TODO check message type? */
        
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
                    /* free(found_entry); */ /* TODO needed? */
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

/* TODO can it be removed? */
bool ask_aggr_to_neighbors(ThisPeer *peer, EntryList *req_aggr)
{
    EntryList data_received;
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
        msg.body_len = serialize_entries(msg.body, req_aggr) - msg.body;
        
        printf("asking aggr to %d\n", ntohs(nbr->peer->addr.sin_port));

        ret = send_message(sd, &msg);
        if (ret == -1)
        {
            printf("could not send REQ_DATA to peer %d\n", ntohs(nbr->peer->addr.sin_port));
            return false;
        }

        ret = recv_message(sd, &msg);
        if (ret == -1)
        {
            printf("error while receiving REPLY_DATA\n");
            return false;
        }

        close(sd);

        /* TODO check message type? */
        
        init_entry_list(&data_received);
        deserialize_entries(msg.body, &data_received);

        if (!is_entry_list_empty(&data_received))
        {
            printf("aggregate found in neighbor %d\n", msg.id);
            merge_entry_lists(req_aggr, &data_received, COPY_SHALLOW);
            return true;
        }
        
        nbr = nbr->next;
    }

    return false;
}

void ask_aggr_to_peers(
    ThisPeer *peer, 
    EntryList *req_entries, EntryList *recvd_entries, 
    in_port_t peers[], int n)
{
    EntryList tmp_data_received;/* , total_data_received; */
    socklen_t slen;
    int i;
    Message msg;
    int ret, sd;
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr.s_addr);

    init_entry_list(recvd_entries);

    slen = sizeof(struct sockaddr_in);

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
        msg.body_len = serialize_entries(msg.body, req_entries) - msg.body;

        ret = send_message(sd, &msg);
        if (ret == -1)
        {
            printf("could not send MSG_REQ_DATA to peer %d\n", peers[i]);
            return;
        }

        ret = recv_message(sd, &msg);
        if (ret == -1)
        {
            printf("error while receiving MSG_REPLY_DATA\n");
            return;
        }

        close(sd);

        /* TODO check message type? */
        
        init_entry_list(&tmp_data_received);
        deserialize_entries(msg.body, &tmp_data_received);

        printf("received:\n");
        print_entries_asc(&tmp_data_received);

        if (!is_entry_list_empty(&tmp_data_received))
        {
            merge_entry_lists(recvd_entries, &tmp_data_received, COPY_SHALLOW);
        }
    }

    return;
}



/*----------------------------------------------
 |  FUNCTIONS TO HANDLE COMMAND 'get' and FLOODING
 *---------------------------------------------*/


/* called when a flooding for 'get sum' is finished */
void finalize_get_aggr_tot(ThisPeer *peer, in_port_t peers[], int n, int req_num);

void show_aggr_tot_result(Entry *result, int type)
{
    printf("+------------------------------------------\n");
    printf("|\tRESULT: totale ");
    if (type == REQ_TAMPONI)
        printf("tamponi = %d\n", result->tamponi);
    else
        printf("nuovi casi = %d\n", result->nuovi_casi);
    printf("+------------------------------------------\n");
}

void show_aggr_var_result(EntryList *result, int type)
{
    Entry *entry;
    time_t time;
    char time_str[TIMESTAMP_STRLEN];
    
    printf("+------------------------------------------\n");
    printf("|\tRESULT: variazioni di ");
    if (type == REQ_TAMPONI)
        printf("tamponi:\n");
    else
        printf("nuovi casi\n");
    printf("\t----------------------------n");
    
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
    printf("+------------------------------------------\n");
}

/**
 * Called when users executes the command 'get sum'. If it launches a
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
    EntryList found_entries, not_found_entries;
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
        SCOPE_GLOBAL  : the value of the entry must be common to all peers
        TYPE_TOTAL    : we're looking for a sum, not a variation */
    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL;

    /* period_len = period length in seconds / seconds in a day
       does not account for leap seconds
       includes the first and last days of the period, hence the +1 */
    period_len = (int)difftime(end_period, beg_period) / 86400 + 1;

    entry = search_entry(peer->entries.last, beg_period, flags, period_len);

    /* aggregate entry found in this peer */
    if (entry != NULL)
    {
        printf("sum found in local register\n");
        show_aggr_tot_result(entry, type);
        return;
    }

    printf("sum not found in local register\n");


    /*----------------------------------------------
    |  searching for entries needed to compute aggregate
    *---------------------------------------------*/

    /* flags that the entries used to query peers must have:
        AGGREG_DAILY : because we're looking for daily totals 
        SCOPE_LOCAL  : because search for local entries also also returns globals */
    /* TODO why SCOPE_LOCAL? search only returns GLOBALS */
    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;
    
    init_entry_list(&found_entries);
    init_entry_list(&not_found_entries);

    missing_entries = search_needed_entries(
        peer, 
        &found_entries, &not_found_entries, 
        beg_period, end_period, 0, /* period_len = 0 days */
        flags
    );

    /* now, not_found_entries, if not empty, contains the entries that
        are needed to compute the sum and are not present in this peer */

    /* this peer has all the entries needed to compute the aggregation */
    if (missing_entries == 0)
    {
        printf("found all needed entries to compute sum\n");

        /* entry which will contain the result of the aggregation */
        entry_res = create_entry(
            beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL
        );
        entry_res->period_len = period_len;

        compute_aggr_tot(peer, &found_entries, entry_res);

        show_aggr_tot_result(entry_res, type);

        /* add the found entry to the local register */
        add_entry(&peer->entries, entry_res);
        printf("register updated\n");
        /* print_entries_asc(&peer->entries); */

        return;
    }

    printf("missing some entries needed to compute the sum\n");

    printf("entries found:\n");
    print_entries_asc(&found_entries);

    printf("%d missing entries:\n", missing_entries);
    print_entries_asc(&not_found_entries);


    /*----------------------------------------------
    |  asking aggregate to neighbors
    *---------------------------------------------*/    

    init_entry_list(&aggr_requested);
    entry = create_entry(beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL);
    entry->period_len = period_len;
    add_entry(&aggr_requested, entry);

    data_found = ask_aggr_to_neighbors(peer, &aggr_requested);
    if (data_found)
    {
        printf("a neighbor has the requested sum\n");
        show_aggr_tot_result(aggr_requested.first, type);

        /* add the found entry to the local register */
        add_entry(&peer->entries, aggr_requested.first);
        printf("register updated\n");
        /* print_entries_asc(&peer->entries); */

        return;
    }

    printf("no neighbor has the requested sum\n");


    /*----------------------------------------------
    |  launching flood for entries
    *---------------------------------------------*/ 

    printf("flooding for entries\n");

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
    connect_to_neighbors(peer, request);

    request->required_entries = malloc(sizeof(EntryList));
    init_entry_list(request->required_entries);

    request->found_entries = malloc(sizeof(EntryList));
    init_entry_list(request->found_entries);

    request->required_entries->first = not_found_entries.first;
    request->required_entries->last = not_found_entries.last;

    /* initializing response message as empty */
    request->response_msg = malloc(sizeof(Message));
    set_num_of_peers(request->response_msg, 0);

    /* since the flooding is verbose, listening for user input is useless */
    disable_user_input(peer);

    /* TODO still needed? */
    set_timeout(peer, rand() % 4);

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
    EntryList found_var_entries, found_tot_entries;
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
        flags
    );

    if (missing_entries == 0)
    {
        printf("variations found in local register:\n");
        show_aggr_var_result(&found_var_entries, type);
        return;
    }

    printf("not all variations found in local register\n");

    printf("found var entries:\n");
    print_entries_asc(&found_var_entries);

    printf("not found var entries:\n");
    print_entries_asc(&not_found_var_entries);


    /*----------------------------------------------
    |  searching for entries needed to compute aggregate
    *---------------------------------------------*/

    /* flags that the entries used to query peers must have:
        AGGREG_DAILY : because we're looking for daily totals 
        SCOPE_LOCAL  : because search for local entries also also returns globals 
        TYPE_TOTAL   : we want daily totals to compute daily variations */
    /* TODO why SCOPE_LOCAL? search only returns GLOBALS */
    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;

    init_entry_list(&found_tot_entries); /* TODO free properly */
    init_entry_list(&not_found_tot_entries);

    missing_entries = search_needed_entries(
        peer, 
        &found_tot_entries, &not_found_tot_entries, 
        beg_period, end_period, 0, /* period_len = 0 days */
        flags
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

    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    if (missing_entries == 0)
    {
        printf("found all needed entries to compute all variations\n");

        init_entry_list(&entries_res);
        compute_aggr_var(peer, &found_tot_entries, &entries_res, flags);

        merge_entry_lists(&entries_res, &found_var_entries, COPY_STRICT);

        show_aggr_var_result(&entries_res, type);

        merge_entry_lists(&peer->entries, &entries_res, COPY_STRICT);
        printf("register updated\n");
        /* print_entries_asc(&peer->entries); */

        return;
    }

    printf("missing some entries needed to compute all variations\n");

    printf("totals found:\n");
    print_entries_asc(&found_tot_entries);

    printf("%d missing totals:\n", missing_entries);
    print_entries_asc(&not_found_tot_entries);


    /*----------------------------------------------
    |  asking aggregate to neighbors
    *---------------------------------------------*/ 

    init_entry_list(&found_var_entries); /* TODO free properly */
    
    all_data_found = ask_aggr_to_neighbors_v2(peer, &not_found_var_entries, &found_var_entries);

    if (all_data_found)
    {
        printf("the neighbors have all the requested variations:\n");

        show_aggr_var_result(&found_var_entries, type);

        /* add the found entries found to the local register */
        merge_entry_lists(&peer->entries, &found_var_entries, COPY_STRICT);
        printf("register updated\n");
        /* print_entries_asc(&peer->entries); */

        return;
    }

    printf("some variation is still missing:\n");
    print_entries_asc(&not_found_var_entries);

    /* TODO this must be done here? */
    /* add the aggr entries found to the local register */
    merge_entry_lists(&peer->entries, &found_var_entries, COPY_STRICT);

    /* TODO can this be reintroduced? */
    /* some of the variations that were needed before may now not be
        needed anymore (because retrieved from neighbors), so some
        needed total can be removed from not_found_tot_entries. */
    /* count -= remove_not_needed_totals(
        &not_found_tot_entries,
        &not_found_var_entries
    ); */

    printf("totals needed to compute the missing variations:\n");
    print_entries_asc(&not_found_tot_entries);

    /*----------------------------------------------
    |  launching flood for entries
    *---------------------------------------------*/ 

    printf("flooding for entries\n");

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
    connect_to_neighbors(peer, request);

    request->required_entries = malloc(sizeof(EntryList));
    init_entry_list(request->required_entries);

    request->found_entries = malloc(sizeof(EntryList));
    init_entry_list(request->found_entries);

    request->required_entries->first = not_found_tot_entries.first;
    request->required_entries->last = not_found_tot_entries.last;

    /* initializing response message as empty */
    request->response_msg = malloc(sizeof(Message));
    set_num_of_peers(request->response_msg, 0);

    /* since the flooding is verbose, listening for user input is useless */
    disable_user_input(peer);

    /* TODO still needed? */
    set_timeout(peer, rand() % 4);

    peer->state = STATE_STARTING_FLOOD;
}

/* TODO REFACTOR! */
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
    
    printf("asking this entries to peers:\n");
    print_entries_asc(request->required_entries);

    init_entry_list(&received_entries);

    ask_aggr_to_peers(peer, request->required_entries, &received_entries, peers, n);

    printf("entries received:\n");
    print_entries_asc(&received_entries);

    entry = received_entries.first;
    while (entry)
    {   
        entry->flags |= SCOPE_GLOBAL;
        entry = entry->next;
    }

    /* add the received entries to the local register */
    merge_entry_lists(&peer->entries, &received_entries, COPY_STRICT);

    /* printf("updated register:\n");
    print_entries_asc(&peer->entries); */

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
        flags
    );

    if (missing_entries != 0)
    {
        printf("entries still missing:\n");
        print_entries_asc(&not_found_entries);
        printf("adding them to local register\n");

        entry = not_found_entries.first;
        while (entry)
        {
            entry->flags |= SCOPE_GLOBAL;
            /* TODO can do merge() after loop? */
            add_entry(&peer->entries, copy_entry(entry));
            entry = entry->next;
        }
    }

    printf("updated register:\n");
    print_entries_asc(&peer->entries);

    /* TODO free the lists, even those in the request */

    get_aggr_tot(peer, request->beg_period, request->end_period, request->type);
    return;
}

/* TODO REFACTOR! */
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

    printf("asking this entries to peers:\n");
    print_entries_asc(request->required_entries);

    init_entry_list(&received_entries);

    ask_aggr_to_peers(peer, request->required_entries, &received_entries, peers, n);
    
    printf("entries received:\n");
    print_entries_asc(&received_entries);

    entry = received_entries.first;
    while (entry)
    {   
        entry->flags |= SCOPE_GLOBAL;
        entry = entry->next;
    }

    /* add the received entries to the local register */
    merge_entry_lists(&peer->entries, &received_entries, COPY_STRICT);

    /* printf("updated register:\n");
    print_entries_asc(&peer->entries); */

    /* some entry may still be missing: no peer has those entries so it's
        assumed that their values are zero and are created and added to this
        peer's register */

    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    init_entry_list(&found_var_entries);
    init_entry_list(&not_found_var_entries);

    /* fill not_found_var_entries to remove entries in  not_found_tot_entries
        that are not needed to compute the missing variations */
    search_needed_entries(
        peer, 
        &found_var_entries, &not_found_var_entries, 
        request->beg_period, request->end_period, 2,
        flags
    );

    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;
    
    init_entry_list(&found_tot_entries);
    init_entry_list(&not_found_tot_entries);

    missing_entries = search_needed_entries(
        peer, 
        &found_tot_entries, &not_found_tot_entries, 
        request->beg_period, request->end_period, 0,
        flags
    );

    missing_entries -= remove_not_needed_totals(
        &not_found_tot_entries,
        &not_found_var_entries
    );

    if (missing_entries != 0)
    {
        printf("entries still missing:\n");
        print_entries_asc(&not_found_tot_entries);
        printf("adding them to local register\n");

        entry = not_found_tot_entries.first;
        while (entry)
        {
            entry->flags |= SCOPE_GLOBAL;
            /* TODO can do merge() after loop? */
            add_entry(&peer->entries, copy_entry(entry));
            add_entry(&found_tot_entries, copy_entry(entry));
            entry = entry->next;
        }
    }

    printf("updated register:\n");
    print_entries_asc(&peer->entries);

    /* TODO free the lists, even those in the request */

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

#define ARG_TYPE_TAMPONI "t"
#define ARG_TYPE_NCASI "c"

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

    if (strcmp(argv[1], ARG_TYPE_TAMPONI) == 0)
    {
        tmp_tamponi = atoi(argv[2]);
    }
    else if (strcmp(argv[1], ARG_TYPE_NCASI) == 0)
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

    #ifdef FAKE_DAY
    timeinfo->tm_mday = FAKE_DAY;
    #endif

    #ifdef FAKE_MONTH
    timeinfo->tm_mon = FAKE_MONTH - 1;
    #endif

    #ifdef FAKE_YEAR
    timeinfo->tm_year = FAKE_YEAR - 1900;
    #endif

    timeinfo->tm_hour = timeinfo->tm_min = timeinfo->tm_sec = 0;
    timeinfo->tm_isdst = 0;

    /* TODO ugly: can directly pass time_t to create_entry */
    /* strftime(str_time, TIMESTAMP_STRLEN, "%Y-%m-%d", timeinfo); */
    
    tmp_entry = create_entry(mktime(timeinfo), tmp_tamponi, tmp_ncasi, 0);
    add_entry(&peer->entries, tmp_entry);

    print_entries_asc(&peer->entries);

    /* TODO remove memory leak in data when adding existing entry */

    return -1;
}

#define ARG_AGGREG_SUM "sum"
#define ARG_AGGREG_VAR "var"

int cmd_get(ThisPeer *peer, int argc, char **argv)
{
    char *token, *str;
    char str_time[TIMESTAMP_STRLEN];
    int count;
    time_t period[2];
    int32_t flags;
    struct tm *timeinfo;
    int aggr, type;

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
        count = 0;
        for (str = argv[3]; ; str = NULL)
        {
            token = strtok(str, "-");
            if (token == NULL)
                break;

            if (count < 2)
            {
                if (strcmp(token, "*") == 0)
                {
                    period[count] = 0;
                }
                else
                {
                    period[count] = str_to_time(token);
                    if (period[count] == -1)
                    {
                        printf("invalid date format: \"%s\"\n", token);
                        return -1;
                    }
                }
            }

            count++;
        }

        /* more or less than two arguments in period */
        if (count != 2)
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

    /* repalcing end period to yesterday if it was set to "*" */
    if (period[1] == 0)
    {
        period[1] = time(NULL);
        timeinfo = localtime(&period[1]);

        #ifdef FAKE_DAY
        timeinfo->tm_mday = FAKE_DAY;
        #endif

        #ifdef FAKE_MONTH
        timeinfo->tm_mon = FAKE_MONTH - 1;
        #endif

        #ifdef FAKE_YEAR
        timeinfo->tm_year = FAKE_YEAR - 1900;
        #endif

        timeinfo->tm_mday--;
        timeinfo->tm_hour = timeinfo->tm_min = timeinfo->tm_sec = 0;
        timeinfo->tm_isdst = 0;
        period[1] = mktime(timeinfo);
    }
    
    if (period[1] < period[0])
    {
        printf("invalid period\n");
        return -1;
    }


    if (strcmp(argv[1], ARG_AGGREG_SUM) == 0)
    {
        flags = TYPE_TOTAL;
        /* somma = true; */
        aggr = REQ_SOMMA;
    }
    else if (strcmp(argv[1], ARG_AGGREG_VAR) == 0)
    {
        flags = TYPE_VARIATION;
        /* somma = false; */
        aggr = REQ_VARIAZIONE;
    }
    else
    {
        printf("invalid aggr \"%s\"\n", argv[1]);
        return -1;
    }
    flags |= AGGREG_PERIOD | SCOPE_GLOBAL;

    if (strcmp(argv[2], ARG_TYPE_TAMPONI) == 0)
        type = REQ_TAMPONI;
        /* tamponi = true; */
    else if (strcmp(argv[2], ARG_TYPE_NCASI) == 0)
        type = REQ_NUOVI_CASI;
        /* tamponi = false; */
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
    printf("\n---\n");

    /* the aggregation is done "asynchronously" */

    if (aggr == REQ_SOMMA)
        get_aggr_tot(peer, period[0], period[1], type);
    else
        get_aggr_var(peer, period[0], period[1], type);

    return 0;
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



/*----------------------------------------------
 |  FUNCTIONS THAT HANDLE SERVER REQUESTS
 *---------------------------------------------*/


void handle_failed_connection_attempt(ThisPeer *peer)
{
    printf("trying again in 3 seconds...\n");
    sleep(3);
    send_start_msg_to_dserver(peer);
}

void handle_set_neighbors_response(ThisPeer *peer, Message *msgp)
{
    int sd, ret;
    int reuse = 1;

    if (peer->state == STATE_STARTING)
    {
        printf("setting list of neighbors\n");

        sd = socket(AF_INET, SOCK_STREAM, 0);
        if (sd == -1)
        {
            perror("socket error");
            printf("could not start listening for peers\n");
            return;
        }

        if (setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse)) < 0)
            perror("setsockopt(SO_REUSEADDR) failed");

        ret = bind(sd, (struct sockaddr *)&peer->addr, sizeof(peer->addr));
        if (ret == -1)
        {
            perror("bind error");
            printf("could not start listening for peers\n");
            return;
        }

        ret = listen(sd, 10);
        if (ret == -1)
        {
            perror("listen error");
            printf("could not start listening for peers\n");
            return;
        }
        
        peer->listening = sd;
        add_desc(peer, sd);
    }
    else
        printf("refreshing list of neighbors\n");
    
    if (peer->state == STATE_OFF) return;

    /* TODO free properly old list of peers */
    peer->neighbors.first = peer->neighbors.last = NULL;
    deserialize_peers(msgp->body, &peer->neighbors.first);
    print_peers(peer->neighbors.first);

    peer->state = STATE_STARTED;

    clear_timeout(peer);
    enable_user_input(peer);
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

    printf("peer %d requires entries\n", msgp->id);
    print_entries_asc(&req_entries);

    init_entry_list(&found_entries);
    
    req_entry = req_entries.last;
    while (req_entry)
    {
        /* TODO correct here!!! */
        /* printf("searching\n"); */
        found_entry = search_entry(
            peer->entries.last, 
            req_entry->timestamp,
            req_entry->flags,
            req_entry->period_len
        );

        removed_entry = NULL;

        if (found_entry != NULL)
        {
            found_entry = copy_entry(found_entry);
            add_entry(&found_entries, found_entry);
            remove_entry(&req_entries, req_entry);
            removed_entry = req_entry;
        }

        req_entry = req_entry->prev;
        if (req_entry == NULL)
            printf("last\n");
        free(removed_entry);
    }

    printf("entries available:\n");
    print_entries_asc(&found_entries);

    printf("entries NOT available:\n");
    print_entries_asc(&req_entries);

    msgp->type = MSG_REPLY_DATA;
    msgp->id = get_peer_id(peer);
    buff = serialize_entries(msgp->body, &found_entries);
    buff = serialize_entries(buff, &req_entries);
    msgp->body_len = buff - msgp->body;

    ret = send_message(sd, msgp);
    if (ret == -1)
    {
        printf("could not send entries to requester\n");
        return;
    }

    printf("entries sent to requester\n");
}

/* 


 */

void handle_flood_for_entries(ThisPeer *peer, Message *msgp, int sd)
{
    EntryList empty_list_response;
    Entry *req_entry, *found_entry, *removed_entry;
    int ret;
    FloodRequest *request;

    printf("handling flood for entries for %d (sd: %d)\n", msgp->id, sd);
    printf("req num: %d\n", msgp->req_num);

    /* avoid servicing requests more than once */
    /* cannot handle more than one request at once */
    if (!valid_request(peer, msgp->req_num) || (peer->state != STATE_STARTED))
    {
        printf("request already handled, sending empty response\n");
        
        /* TODO needed? */
        init_entry_list(&empty_list_response);

        msgp->type = MSG_ENTRIES_FOUND;
        msgp->id = get_peer_id(peer);
        
        set_num_of_peers(msgp, 0);
        
        ret = send_message(sd, msgp);
        if (ret == -1)
        {
            printf("could not send empty FLOOD response\n");
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

    printf("I've been asked to look for:\n");
    print_entries_asc(request->required_entries);

    /* TODO it's redauntant */
    request->found_entries = malloc(sizeof(EntryList));
    init_entry_list(request->found_entries);

    /* iterating through requested entries and searching them in the 
        register. The ones that are not found, or are found but have
        SCOPE_LOCAL, are put into req_entries to be asked to this
        peer's neighbors. */

    printf("register\n");
    print_entries_dsc(&peer->entries);

    req_entry = request->required_entries->first;
    while (req_entry != NULL) 
    {
        printf("checking %ld %d %d\n", req_entry->timestamp, req_entry->flags, req_entry->period_len);
        print_entry(req_entry);

        found_entry = search_entry(
            peer->entries.last, 
            req_entry->timestamp, 
            req_entry->flags, 
            req_entry->period_len
        );

        removed_entry = NULL;

        if (found_entry != NULL)
        {
            found_entry = copy_entry(found_entry);
            add_entry(request->found_entries, found_entry);

            /* global entries have the same value for all the peers.
                The neighbors wont be asked for global entries if this
                peer already has some of them */
            if ((found_entry->flags & ENTRY_SCOPE) == SCOPE_GLOBAL)
            {
                remove_entry(request->required_entries, req_entry);
                removed_entry = req_entry;
            }
        }
        else
            printf("entry NOT found\n");

        req_entry = req_entry->next;  
        free(removed_entry);  
    }
    
    printf("I've got:\n");
    print_entries_asc(request->found_entries);

    printf("Flooding for:\n");
    print_entries_asc(request->required_entries);

    /* establishing connections with neighbors */
    /* A request will be actually sent to a neighbor when it will
        accept the connection (when the socket is ready to write).
       When a socket is ready to write, do_flood_for_entries is called. */

    FD_ZERO(&request->peers_involved);
    connect_to_neighbors(peer, request);

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
    
    request->callback = NULL;

    set_num_of_peers(request->response_msg, 0);

    /* if this peer has some of the requested entries */
    if (!is_entry_list_empty(request->found_entries))
    {
        /* add this peer to the aggregated response message */
        printf("adding myself in\n");
        add_peer_to_msg(request->response_msg, ntohs(peer->addr.sin_port));
    }
    else
    {
        printf("NOT adding myself in\n");
    }
}

/* called for each socket write-ready */
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

    printf("actually sending FLOOD request to sd %d\n", sd);

    msg.type = MSG_FLOOD_FOR_ENTRIES;
    msg.req_num = request->number;
    msg.id = get_peer_id(peer);
    msg.body_len = serialize_entries(msg.body, request->required_entries) - msg.body;

    ret = send_message(sd, &msg);
    if (ret == -1) 
    {
        printf("could not send FLOOD request\n");
        return;
    }

    printf("sent request to neighbor (%d)\n", request->nbrs_remaining);

    /* moving the socket from the write set, to the read set */
    _remove_desc(&peer->master_write_set, &peer->fdmax_w, sd);
    _add_desc(&peer->master_read_set, &peer->fdmax_r, sd);

    /* TODO write a better condition */
    /* write set not completely empty -> other sockets remain to write */
    if (peer->fdmax_w != -1)
        set_timeout(peer, rand() % 4);
    else
    {
        printf("last request sent\n");
        clear_timeout(peer);
    }
    
    request->nbrs_remaining++;
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

    printf("received flood response from %d (sd: %d)\n", msgp->id, sd);


    if (request->requester_sd != REQUESTER_SELF)
    {
        printf("relaying flood response\n");

        /* TODO check if really needed */
        if (request->response_msg == NULL)
        {
            printf("flood response already relayed\n");
            return;
        }
    }

    printf("received num of peers: %d\n", get_num_of_peers(msgp));

    /* updating the number of peers that satisfy the request in 
        the aggregated response message */
    
    merge_peer_list_msgs(request->response_msg, msgp);
    
    printf("new num of peers: %d\n", get_num_of_peers(request->response_msg));
    
    /* this is the last response message */
    if (request->nbrs_remaining == 1)
    {
        if (request->requester_sd != REQUESTER_SELF)
        {
            /* sending back this peer's aggregated response */

            request->response_msg->type = MSG_ENTRIES_FOUND;
            request->response_msg->req_num = request->number;

            printf("last response: sending it to sd %d\n", request->requester_sd);
            
            ret = send_message(request->requester_sd, request->response_msg);
            if (ret == -1)
            {
                printf("could not send (relay) FLOOD response\n");
                return;
            }

            printf("closing %d\n", sd);
            close(sd);
            _remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
            _remove_desc(&request->peers_involved, NULL, sd);

            free(request->response_msg);
            request->response_msg = NULL;
        }
        else
        {   
            printf("closing %d\n", sd);
            close(sd);
            _remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
            _remove_desc(&request->peers_involved, NULL, sd);

            peers = (in_port_t*)request->response_msg->body;

            num_of_peers = get_num_of_peers(request->response_msg);

            printf("%d peers have entries for me:\n", num_of_peers);

            /* deserializing received peers addresses */
            while (num_of_peers > 0)
                printf("peer: %d\n", peers[--num_of_peers]);
            
            if (request->callback != NULL)
            {
                num_of_peers = get_num_of_peers(request->response_msg);
                printf("finalising the computation of aggregate\n");

                peers = calloc(num_of_peers, sizeof(in_port_t));

                if (peers != NULL)
                    memcpy(peers, request->response_msg->body, num_of_peers * sizeof(in_port_t));

                /* while (num_of_peers >= 0)
                    printf("> peer: %d\n", peers[--num_of_peers]);
                num_of_peers = get_num_of_peers(peer->req_resp_msg); */

                (*request->callback)(peer, peers, num_of_peers, request->number);
                /* TODO when to free msgp? */
            }
        }
    }
    else
    {
        printf("closing %d\n", sd);
        close(sd);
        _remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
        _remove_desc(&request->peers_involved, NULL, sd);
    }

    /* TODO check when to close the sockets (in demux? really?) */
    /* close(sd);
    _remove_desc(&peer->master_read_set, &peer->fdmax_r, sd); */

    request->nbrs_remaining--;

    printf("waiting for %d more responses\n", request->nbrs_remaining);

    /* other sockets remain to read */
    if (request->nbrs_remaining > 0)
    {
        set_timeout(peer, rand() % 4);
    }
    else
    {
        printf("all expected responses received\n");
    
        peer->state = STATE_STARTED;
        clear_timeout(peer);
        enable_user_input(peer);
    }

}


/* ########## DEMULTIPLEXING ########## */

void demux_user_input(ThisPeer *peer)
{
    int i, argc;
    char **argv;

    argv = get_command_line(&argc);

    if (argc == 0) return;
    
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
        add_desc(peer, ret);
        return;
    }

    if (FD_ISSET(sd, &peer->master_write_set))
    {
        printf("socket ready to write\n");
        if (peer->state == STATE_HANDLING_FLOOD ||
            peer->state == STATE_STARTING_FLOOD)
            do_flood_for_entries(peer, sd);
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
        remove_desc(peer, sd);
        /* if (peer->state == STATE_HANDLING_FLOOD)
            peer->nbrs_remaining--; */

        return;
    }

    if (msg.type == MSG_REQ_DATA)
    {
        handle_req_data(peer, &msg, sd);
        close(sd);
        remove_desc(peer, sd);
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
}



int main(int argc, char** argv)
{
    ThisPeer peer;
    fd_set working_read_set, working_write_set;
    int ret, i, fdmax, desc_ready;
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

    srand(time(NULL) + peer.addr.sin_port);

    /* TODO check if file was opened successfully */
    sprintf(register_file, "reg_%d.txt", ret);
    load_register_from_file(&peer.entries, register_file);
    print_entries_asc(&peer.entries);

    add_desc(&peer, STDIN_FILENO);

    for (;;)
    {
        working_read_set = peer.master_read_set;
        working_write_set = peer.master_write_set;

        fdmax = peer.fdmax_r;
        if (peer.fdmax_w > fdmax)
            fdmax = peer.fdmax_w;

        if (is_user_input_enabled(&peer))
        {
            printf("peer@%d$ ", ntohs(peer.addr.sin_port));
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
            if (peer.state == STATE_HANDLING_FLOOD ||
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

/*
types of messages:
START               connect to ds (register peer)
SET_NBRS            set nbrs list
STOP                unconnect from ds (unregister peer)
ADD_ENTRY           tell peer to add entries in data in its register
REQ_DATA            ask peer for aggr data
REPLY_DATA          reply with aggr data or null
FLOOD_FOR_ENTRIES   ask peer to look for entries owners
ENTRIES_FOUND       tell peer entries owners
REQ_ENTRIES         ask entries to entries owner
REPLY_ENTRIES       tell peer owned entries


--> x   : comando inviato a x
x <--   : comando ricevuto da x
o       : comando inserito da terminale con effetto solo locale

peer:
START             --> ds
ADD         p <--o--> p
STOP              --> ds
REQ_DATA    p <--o--> p     (get aggr)
REPLY_DATA  p <-- --> p
FLOOD       p <-- --> p

on start(addr, port):
    send START to ds(addr, port)

    put udp socket in set
    while true:
        set timeout
        select(set)
        if received from sd
            set peers
            break

on add(entry):
    store entry

on stop:
    for each nbr:
        send entries to nbr
    close

on get(aggr):
    if aggr present:
        show aggr
    elif all data available:
        compute aggr
        store aggr
        show aggr
    elif data missing:
        for each nbr:
            send REQ_DATA(aggr) to nbr
        wait for all REPLY_DATA(aggr) responses
        if exists non empty response(aggr):
            store aggr
            show aggr
        else:
            for each nbr:
                for each missing data:
                    send FLOOD(data) to nbr
            wait for all FLOOD(peer) responses
            for each FLOOD(peer) response:
                send REQ_ENTRIES(data) to peer
                wait for REQ_ENTRIES(data) response from peer
                store data
            if not data missing:
                compute aggr
                store aggr
                show aggr
            else:
                show "data not available"

on REQ_DATA(aggr) from peer:
    if aggr present:
        respond REPLY_DATA(aggr) to peer
    else:
        respond REPLY_DATA(empty) to peer

on FLOOD(data) from peer:
    if data present:
        respond FLOOD(me)
    else:
        for each nbr:
            send FLOOD(data) to nbr
        wait for all FLOOD(peer2) responses
        for each FLOOD(peer2) response:
            if peer2 has data:
                send FLOOD(peer2) to peer



*/
