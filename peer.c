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
#define FAKE_DAY 10
#define FAKE_MONTH 2

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
    fd_set involved_peers;
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
        if (FD_ISSET(sd, &peer->last_requests[i].involved_peers))
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

/* query: start period, end period, TOT or VAR 

TOT:
    search single entry
    if found:
        stop
    
    LT = create list of GLOBAL TOTS needed
    for each day in period:
        add empty entry to LT with day, flags = TOT
    
    res = empty entry
    for each entry in LT:
        if entry present in register:
            res += entry
            remove entry from LT

    if LT is empty:
        stop
    
    REQ_DATA(aggr)
    wait REPLY_DATA(aggr)

    if REPLY_DATA(aggr) not empty:
        stop

    FLOOD_FOR_ENTRIES(LT)
    wait FLOOD_FOR_ENTRIES(peers)

    for each peer in peers:
        ask peer for entry
        wait for entry
        update entry in LT

    // N.B. entries in LT not update are not present in
    // any register, so the value is assumed to be zero

    for each entry in LT:
        save entry in register
        res += entry
        remove entry from LT?
    
    stop

VAR:
    VF = create list of GLOBAL VARS found
    VNF = create list of GLOBAL VARS not found
    for each day in period:
        if register contains correct entry:
            put it in VF
        else:
            put it in VNF
    
    if VNF empty:
        stop
    
    LT = create list of GLOBAL TOTS needed
    for each day from start period to end period-1:
        add empty entry to LT with day, flags = TOT
    
    TF = create list of GLOBAL TOTS possessed
    for each entry in LT:
        if entry present in register:
            add entry to TF
            remove entry from LT

    if LT is empty:
        compute aggr
        stop
    
    VFP = create list of GLOBAL VARS found in nbrs

    REQ_DATA(VNF)
    wait REPLY_DATA(VFP)

    for each entry in VFP:
        remove entry from VNF

    if VNF is empty:
        stop

    clear LT and TF

    LT = create list of GLOBAL TOTS needed
    for each entry in VNF:
        add empty entry to LT with day, flags = TOT
        add empty entry to LT with prev day, flags = TOT

    TF = create list of GLOBAL TOTS possessed
    for each entry in LT:
        if entry present in register:
            add entry to TF
            remove entry from LT

    if LT is empty:
        compute aggr
        stop
    
    FLOOD_FOR_ENTRIES(LT)
    wait FLOOD_FOR_ENTRIES(peers)

    for each peer in peers:
        ask peer for entry
        wait for entry
        update entry in LT

    // N.B. entries in LT not update are not present in
    // any register, so the value is assumed to be zero

    compute aggr
    stop

*/

/**
 * Sum all entries in list entries. Does not check if entries are valid.
 * Only sums entry that have SCOPE_GLOBAL.
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
        if ((entry->flags       & ENTRY_SCOPE) == SCOPE_GLOBAL &&
            (entry->next->flags & ENTRY_SCOPE) == SCOPE_GLOBAL)
        {
            tmp = copy_entry(entry);
            tmp->tamponi = entry->next->tamponi - tmp->tamponi;
            tmp->nuovi_casi = entry->next->nuovi_casi - tmp->nuovi_casi;
            tmp->flags = flags;
            tmp->period_len = 2;
            add_entry(entries_res, tmp);
            /* TODO free entry? */
        }
        entry = entry->prev;
    }
}

/**
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
        /* create empty entry */
        /* entry = create_entry(t_day, 0, 0, flags); */
        /* add_entry(entries, entry); */

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

int remove_not_needed_totals(EntryList *needed_totals, EntryList *needed_vars)
{   
    Entry *entry, *found_entry, *removed_entry; 
    int count = 0;

    entry = needed_totals->last;
    while (entry)
    {
        found_entry = search_entry(
            needed_vars->last,
            entry->timestamp,
            AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION,
            2
        );

        if (found_entry == NULL)
        {
            found_entry = search_entry(
                needed_vars->last,
                entry->timestamp - 86400,
                AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION,
                2
            );
        }

        removed_entry = NULL;

        if (found_entry == NULL)
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
            /* free_flood_request(peer->curr_req);
            peer->curr_req = NULL; */
            return;
        }

        ret = connect(sd, (struct sockaddr *)&nbr->peer->addr, slen);
        if (ret == -1)
        {
            perror("connect error");
            /* free_flood_request(peer->curr_req);
            peer->curr_req = NULL; */
            return;
        }

        _add_desc(&peer->master_write_set, &peer->fdmax_w, sd);
        if (request != NULL)
        {
            printf("adding involved sd %d\n", sd);
            _add_desc(&request->involved_peers, NULL, sd);
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
            printf("entries found in neighbor %d\n", msg.id);
            merge_entry_lists(recvd_entries, &tmp_data_received, COPY_SHALLOW);
        }
    }

    printf("all entries received:\n");
    print_entries_asc(recvd_entries);

    return;
}



void finalize_get_aggr_tot(ThisPeer *peer, in_port_t peers[], int n, int req_num)
{
    EntryList received_entries, found_entries, not_found_entries;
    Entry *entry;
    int32_t flags;
    int count;
    int period_len;

    FloodRequest *request = get_request_by_num(peer, req_num);
    
    printf("asking this entries to peers:\n");
    print_entries_asc(request->required_entries);

    init_entry_list(&received_entries);
    ask_aggr_to_peers(peer, request->required_entries, &received_entries, peers, n);

    printf("RECVD_ENTRIES obtained\n");
    print_entries_asc(&received_entries);

    entry = received_entries.first;
    while (entry)
    {   
        entry->flags |= SCOPE_GLOBAL;
        entry = entry->next;
    }

    printf("RECVD_ENTRIES before merge\n");
    print_entries_asc(&received_entries);

    merge_entry_lists(&peer->entries, &received_entries, COPY_STRICT);

    printf("REGISTER AFTER MERGE\n");
    print_entries_asc(&peer->entries);

    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;

    init_entry_list(&found_entries);
    init_entry_list(&not_found_entries);

    count = search_needed_entries(
        peer, 
        &found_entries, &not_found_entries, 
        request->beg_period, request->end_period, 0,
        flags
    );

    if (count != 0)
    {
        printf("missing entries\n");
        entry = not_found_entries.first;
        while (entry)
        {
            entry->flags |= SCOPE_GLOBAL;
            printf("adding:\n");
            print_entry(entry);
            add_entry(&peer->entries, copy_entry(entry));
            entry = entry->next;
        }
    }

    period_len = (int)difftime(request->end_period, request->beg_period) / 86400 + 1;

    entry = create_entry(
            request->beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL
        );
    entry->period_len = period_len;
    compute_aggr_tot(peer, &found_entries, entry);

    printf("RESULT ");
    if (request->type == REQ_TAMPONI)
    {
        printf("tamponi = %d\n", entry->tamponi);
    }
    else
    {
        printf("nuovi casi = %d\n", entry->nuovi_casi);
    }

    add_entry(&peer->entries, entry);

    /* TODO be less verbose */
}

void finalize_get_aggr_var(ThisPeer *peer, in_port_t peers[], int n, int req_num)
{
    EntryList found_tot_entries, not_found_tot_entries;
    EntryList found_var_entries, not_found_var_entries;
    EntryList received_entries, entries_res;
    int32_t flags;
    Entry* entry;
    int count;
    time_t tmp_time;
    char str_time[TIMESTAMP_STRLEN];

    FloodRequest *request = get_request_by_num(peer, req_num);

    /* finalize_get_aggr_tot(peer, peers, n, req_num); */

    printf("asking this entries to peers:\n");
    print_entries_asc(request->required_entries);

    init_entry_list(&received_entries);
    ask_aggr_to_peers(peer, request->required_entries, &received_entries, peers, n);
    
    printf("RECVD_ENTRIES obtained\n");
    print_entries_asc(&received_entries);

    entry = received_entries.first;
    while (entry)
    {   
        entry->flags |= SCOPE_GLOBAL;
        entry = entry->next;
    }

    printf("RECVD_ENTRIES before merge\n");
    print_entries_asc(&received_entries);

    merge_entry_lists(&peer->entries, &received_entries, COPY_STRICT);

    printf("REGISTER AFTER MERGE\n");
    print_entries_asc(&peer->entries);

    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    init_entry_list(&found_var_entries);
    init_entry_list(&not_found_var_entries);

    count = search_needed_entries(
        peer, 
        &found_var_entries, &not_found_var_entries, 
        request->beg_period, request->end_period, 2, /* period_len = 0 days */
        flags
    );

    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;
    
    init_entry_list(&found_tot_entries);
    init_entry_list(&not_found_tot_entries);

    count = search_needed_entries(
        peer, 
        &found_tot_entries, &not_found_tot_entries, 
        request->beg_period, request->end_period, 0, /* period_len = 0 days */
        flags
    );

    count -= remove_not_needed_totals(
        &not_found_tot_entries,
        &not_found_var_entries
    );

    printf("MISSING ENTRIES:\n");
    print_entries_asc(&not_found_tot_entries); 

    if (count != 0)
    {
        printf("missing entries\n");
        entry = not_found_tot_entries.first;
        while (entry)
        {
            entry->flags |= SCOPE_GLOBAL;
            printf("adding:\n");
            print_entry(entry);
            add_entry(&peer->entries, copy_entry(entry));
            entry = entry->next;
        }
    }

    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    init_entry_list(&entries_res);

    /* TODO non va bene! Alcune variazioni sono gia` presenti, altre 
    vanno calcolate*/
    compute_aggr_var(peer, &peer->entries, &entries_res, flags);

    printf("RESULTS variazione giornaliera di ");
    if (request->type == REQ_TAMPONI)
        printf("tamponi:\n");
    else
        printf("nuovi casi:\n");
    
    entry = entries_res.first;
    while (entry)
    {
        tmp_time = entry->timestamp + 86400;
        time_to_str(str_time, &tmp_time);
        printf("%s ", str_time);
        if (request->type == REQ_TAMPONI)
            printf("%d\n", entry->tamponi);
        else
            printf("%d\n", entry->nuovi_casi);
        entry = entry->next;
    }

    merge_entry_lists(&peer->entries, &entries_res, COPY_STRICT);

    printf("REGISTER AFTER UPDATE:\n");
    print_entries_asc(&peer->entries);
}

void get_aggr_tot(ThisPeer *peer, time_t beg_period, time_t end_period, int type)
{
    int period_len;
    Entry *entry, *req_entry, *entry_res;
    int32_t flags;
    EntryList req_aggr;
    EntryList found_entries, not_found_entries;
    int count = 0;
    int req_num;
    bool data_found;
    FloodRequest *request;

    /* 2021:02:13-2021:02:16 (4 days) (flag: 5) TOTALE GLOBALE t: 320  c: 59 */

    /* ##### searching for the pre-computed aggregate ##### */

    /* flags that the entry we're looking for must have:
        AGGREG_PERIOD : we're looking for an entry that covers a period
        SCOPE_GLOBAL  : the value of the entry must be common to all peers
        TYPE_TOTAL    : we're looking for a sum, not a variation */
    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL;

    /* period_len = period length in seconds / seconds in a day */
    /* does not account for leap seconds */
    /* includes the first and last days of the period, hence the +1 */
    period_len = (int)difftime(end_period, beg_period) / 86400 + 1;

    entry = search_entry(peer->entries.last, beg_period, flags, period_len);

    if (entry != NULL)
    {
        printf("sum found in local register\n");
        print_entry(entry);
        return;
    }

    printf("sum not found in local register\n");

    /* ##### searching for entries needed to compute aggregate ##### */

    /* flags that the entries used to query peers must have:
        AGGREG_DAILY : because we're looking for daily totals 
        SCOPE_LOCAL  : because search for local entries also also returns globals */
    /* TODO why SCOPE_LOCAL? search only returns GLOBALS */
    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;
    
    init_entry_list(&found_entries);
    init_entry_list(&not_found_entries);

    count = search_needed_entries(
        peer, 
        &found_entries, &not_found_entries, 
        beg_period, end_period, 0, /* period_len = 0 days */
        flags
    );

    /* Now, not_found_entries, if not empty, contains the entries that
        need to be asked to neighbors */

    /* zero entries still needed to compute the aggregation */
    if (count == 0)
    {
        printf("found all needed entries\n");

        /* entry which will contain the result of the aggregation */
        entry_res = create_entry(
            beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL
        );
        entry_res->period_len = period_len;

        compute_aggr_tot(peer, &found_entries, entry_res);

        printf("result computed\n");
        print_entry(entry_res);

        add_entry(&peer->entries, entry_res);

        printf("register updated\n");
        print_entries_asc(&peer->entries);

        return;
    }

    printf("entries found:\n");
    print_entries_asc(&found_entries);

    printf("%d remaining entries:\n", count);
    print_entries_asc(&not_found_entries);

    /* ##### asking for aggregate to neighbors ##### */
    
    /* TODO continue from here */

    init_entry_list(&req_aggr);
    req_entry = create_entry(beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL);
    req_entry->period_len = period_len;
    add_entry(&req_aggr, req_entry);

    data_found = ask_aggr_to_neighbors(peer, &req_aggr);
    if (data_found)
    {
        printf("result:\n");
        print_entries_asc(&req_aggr);
        return;
    }

    printf("no neighbor has the requested sum\n");


    /* flooding */

    /* peer->curr_req = request = malloc(sizeof(FloodRequest)); */
    req_num = rand();
    request_serviced(peer, req_num);
    request = get_request_by_num(peer, req_num);
    request->number = req_num;

    FD_ZERO(&request->involved_peers);
    connect_to_neighbors(peer, request);

    request->required_entries = malloc(sizeof(EntryList));
    init_entry_list(request->required_entries);

    request->found_entries = malloc(sizeof(EntryList));
    init_entry_list(request->found_entries);

    /* merge_entry_lists(peer->req_entries, &totals_needed); */
    request->required_entries->first = not_found_entries.first;
    request->required_entries->last = not_found_entries.last;

    peer->state = STATE_STARTING_FLOOD;
    request->requester_sd = REQUESTER_SELF;
    request->nbrs_remaining = 0;
    request->beg_period = beg_period;
    request->end_period = end_period;
    request->type = type;
    request->callback = &finalize_get_aggr_tot;

    request->response_msg = malloc(sizeof(Message));
    set_num_of_peers(request->response_msg, 0);

    disable_user_input(peer);
    set_timeout(peer, rand() % 4);
}

void get_aggr_var(ThisPeer *peer, time_t beg_period, time_t end_period, int type)
{
    EntryList found_var_entries, found_tot_entries;
    EntryList not_found_var_entries, not_found_tot_entries;
    EntryList entries_res;
    int count, incl_end_period;
    bool all_data_found;
    int32_t flags;
    int req_num;
    FloodRequest *request;

    /* get var t 2021:02:20-2021:02:25 */

    /* flags that the entry we're looking for must have:
        AGGREG_PERIOD  : we're looking for an entry that covers a period
        SCOPE_GLOBAL   : the value of the entry must be common to all peers
        TYPE_VARIATION : we're looking for a variation, not a sum */
    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    init_entry_list(&found_var_entries);
    init_entry_list(&not_found_var_entries);

    /* the next search includes the extremities of the periods, so search
        until one the day before end_period */
    incl_end_period = end_period - 86400;

    /* each aggreg entry representing a variation is saved with timestamp
        set to the first day, period_len = 2 and flag TYPE_VARIATION
       e.g. variation between 25/11 and 26/11 is -136:
        timestamp = 25/11
        tamponi = -136 
        period_len = 2 */

    count = search_needed_entries(
        peer,
        &found_var_entries, &not_found_var_entries,
        beg_period, incl_end_period, 2, /* period_len = 2 days */
        flags
    );

    if (count == 0)
    {
        printf("variations found in local register:\n");
        print_entries_asc(&found_var_entries);
        return;
    }

    printf("not all variations found in local register\n");

    printf("found var entries:\n");
    print_entries_asc(&found_var_entries);

    printf("NOT found var entries:\n");
    print_entries_asc(&not_found_var_entries);

    /* flags that the entries used to query peers must have:
        AGGREG_DAILY : because we're looking for daily totals 
        SCOPE_LOCAL  : because search for local entries also also returns globals 
        TYPE_TOTAL   : we want daily totals to compute daily variations */
    /* TODO why SCOPE_LOCAL? search only returns GLOBALS */
    flags = AGGREG_DAILY | SCOPE_LOCAL | TYPE_TOTAL;

    init_entry_list(&found_tot_entries); /* TODO free properly */
    init_entry_list(&not_found_tot_entries);

    count = search_needed_entries(
        peer, 
        &found_tot_entries, &not_found_tot_entries, 
        beg_period, end_period, 0, /* period_len = 0 days */
        flags
    );

    printf("ann not found entries before subtraction:\n");
    print_entries_asc(&not_found_tot_entries);

    /* not_found_tot_entries now contains all entries of TYPE_TOTAL
        which are needed to compute each variation in the specified
        period.
       some of the variations may be already present in the register
        (those in found_var_entries), this means that some entry
        in not_found_tot_entries is not needed.
       this removes those entries from not_found_tot_entries:
        for each of the tot entries not present in this peer, 
        remove the entry if this entry is not present in the list
        of var entries that are not in this peer (and need to be
        computed or asked to other peers) */
    count -= remove_not_needed_totals(
        &not_found_tot_entries,
        &not_found_var_entries
    );

    flags = AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_VARIATION;

    if (count == 0)
    {
        printf("found all needed entries\n");

        printf("final results:\n");
        print_entries_asc(&entries_res);

        init_entry_list(&entries_res);
        compute_aggr_var(peer, &found_tot_entries, &entries_res, flags);
        merge_entry_lists(&peer->entries, &entries_res, COPY_STRICT);

        return;
    }

    printf("found tot entries:\n");
    print_entries_asc(&found_tot_entries);

    printf("NOT found tot entries:\n");
    print_entries_asc(&not_found_tot_entries);

    init_entry_list(&found_var_entries); /* TODO free properly */
    
    all_data_found = ask_aggr_to_neighbors_v2(peer, &not_found_var_entries, &found_var_entries);

    if (all_data_found)
    {
        printf("neighbors have all missing aggr entries:\n");

        print_entries_asc(&found_var_entries);

        /* add the aggr entries found to the local register */
        merge_entry_lists(&peer->entries, &found_var_entries, COPY_STRICT);

        printf("UPDATED REGISTER:\n");
        print_entries_asc(&peer->entries);

        return;
    }

    /* add the aggr entries found to the local register */
    merge_entry_lists(&peer->entries, &found_var_entries, COPY_STRICT);

    printf("UPDATED REGISTER:\n");
    print_entries_asc(&peer->entries);

    printf("entries still missing:\n");
    print_entries_asc(&not_found_var_entries);

    /* some of the variations that were needed before may now not be
        needed anymore (because retrieved from neighbors), so some
        needed total can be removed from not_found_tot_entries. */
    /* count -= remove_not_needed_totals(
        &not_found_tot_entries,
        &not_found_var_entries
    ); */

    printf("needed tot entries:\n");
    print_entries_asc(&not_found_tot_entries);

    /* flooding */

    req_num = rand();
    request_serviced(peer, req_num);
    request = get_request_by_num(peer, req_num);
    request->number = req_num;

    FD_ZERO(&request->involved_peers);
    connect_to_neighbors(peer, request);

    request->required_entries = malloc(sizeof(EntryList));
    init_entry_list(request->required_entries);

    request->found_entries = malloc(sizeof(EntryList));
    init_entry_list(request->found_entries);

    /* merge_entry_lists(peer->req_entries, &totals_needed); */
    request->required_entries->first = not_found_tot_entries.first;
    request->required_entries->last = not_found_tot_entries.last;

    peer->state = STATE_STARTING_FLOOD;
    request->requester_sd = REQUESTER_SELF;
    request->nbrs_remaining = 0;
    request->beg_period = beg_period;
    request->end_period = end_period;
    request->type = type;
    request->callback = &finalize_get_aggr_var;

    request->response_msg = malloc(sizeof(Message));
    set_num_of_peers(request->response_msg, 0);

    disable_user_input(peer);
    set_timeout(peer, rand() % 4);

    /* 
2021:02:20 1 10 20
2021:02:21 1 12 18
2021:02:22 1 14 17
2021:02:23 1 16 15
2021:02:24 1 18 14
2021:02:25 1 30 5
    
2021:02:20 7 23 -4 2
2021:02:21 7 20 -6 2
2021:02:22 7 17 -8 2
2021:02:23 7 23 -10 2
2021:02:24 7 29 -12 2
     */
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

#define ARG_AGGREG_SUM "sum"
#define ARG_AGGREG_VAR "var"

int cmd_get(ThisPeer *peer, int argc, char **argv)
{
    char *token, *str;
    char str_time[TIMESTAMP_STRLEN];
    int count;
    time_t period[2];
    int32_t flags;
    /* bool somma, tamponi; */
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
        period[1] = time(NULL) - 86400;
    
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



/* ########## FUNCTIONS THAT HANDLE SERVER REQUESTS ########## */

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

/* ########## FUNCTIONS THAT HANDLE PEER REQUESTS ########## */

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

    FD_ZERO(&request->involved_peers);
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
            _remove_desc(&request->involved_peers, NULL, sd);

            free(request->response_msg);
            request->response_msg = NULL;
        }
        else
        {   
            printf("closing %d\n", sd);
            close(sd);
            _remove_desc(&peer->master_read_set, &peer->fdmax_r, sd);
            _remove_desc(&request->involved_peers, NULL, sd);

            peers = (in_port_t*)request->response_msg->body;

            num_of_peers = get_num_of_peers(request->response_msg);

            printf("%d peers have entries for me:\n", num_of_peers);

            /* deserializing received peers addresses */
            while (num_of_peers >= 0)
                printf("peer: %d\n", peers[--num_of_peers]);
            
            if (request->callback != NULL)
            {
                num_of_peers = get_num_of_peers(request->response_msg);
                printf("finalising the aggr request\n");
                printf("%d peers responded\n", num_of_peers);

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
        _remove_desc(&request->involved_peers, NULL, sd);
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
