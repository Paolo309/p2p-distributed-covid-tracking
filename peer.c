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
void get_aggr_tot(ThisPeer *peer, time_t beg_period, time_t end_period)
{
    int period_len;
    Entry *entry, *reg_entry, *entry_res, *removed_entry;
    int32_t flags;
    EntryList totals_needed;
    struct tm *tm_day;
    /* struct tm *tm_beg_period; */
    time_t t_day;
    int count = 0;

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
        printf("entry found in local register\n");
        print_entry(entry);
        return;
    }

    printf("entry not found in local register\n");

    /* totals_needed is a list that will contain empty entries, one
        for each day of the period, with timestamp and flags properly
        set to match already existsing entries (if there are any) */

    init_entry_list(&totals_needed);

    /* AGGREG_DAILY : because we're looking for daily totals */
    flags = AGGREG_DAILY | SCOPE_GLOBAL | TYPE_TOTAL;

    /* time values used to iterate through the days of the period */
    t_day = end_period;
    tm_day = localtime(&t_day);
    
    /* for each day of the period */
    while (difftime(t_day, beg_period) >= 0)
    {
        /* create empty entry */
        entry = create_entry(t_day, 0, 0, flags);
        add_entry(&totals_needed, entry);

        /* move back one day */
        tm_day->tm_mday--;
        t_day = mktime(tm_day);
        tm_day = localtime(&t_day);

        count++;
    }

    printf("created %d entries\n", count);

    print_entries_asc(&totals_needed);

    /* entry which will contain the result of the aggregation */
    entry_res = create_entry(
        beg_period, 0, 0, AGGREG_PERIOD | SCOPE_GLOBAL | TYPE_TOTAL
    );
    entry_res->period_len = period_len;

    count = 0;
    
    entry = totals_needed.last;
    while (entry) /* for each entry needed to compute the sum */
    {
        reg_entry = search_entry(
            peer->entries.last, entry->timestamp, entry->flags, 0
        );

        removed_entry = NULL;

        if (reg_entry != NULL)
        {
            printf("found entry: ");
            print_entry(reg_entry);
            count++;

            /* update the aggregated entry */
            entry_res->tamponi += reg_entry->tamponi;
            entry_res->nuovi_casi += reg_entry->nuovi_casi;

            remove_entry(&totals_needed, entry);
            removed_entry = entry;
        }

        /* move back one entry, freeing the current one if needed */
        entry = entry->prev;
        free(removed_entry);
    }

    printf("found %d entries\n", count);

    printf("partial res:\n");
    print_entry(entry_res);

    printf("remaining entries:\n");
    print_entries_asc(&totals_needed);

    /* TODO continue from here */
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

#define TYPE_TAMPONI "t"
#define TYPE_NCASI "c"

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

    if (strcmp(argv[1], TYPE_TAMPONI) == 0)
    {
        tmp_tamponi = atoi(argv[2]);
    }
    else if (strcmp(argv[1], TYPE_NCASI) == 0)
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

#define AGGREG_SUM "sum"
#define AGGREG_VAR "var"

int cmd_get(ThisPeer *peer, int argc, char **argv)
{
    char *token, *str;
    int count;
    time_t period[2];
    /* int32_t period_len; */
    int32_t flags;
    /* bool tamponi; */
    /* Entry *entry; */

    if (argc != 4)
    {
        printf("usage: %s <aggr> <type> <period>\n", argv[0]);
        return -1;
    }

    while (--argc >= 0)
    {
        printf("arg[%d] = \"%s\"\n", argc, argv[argc]);
    }

    if (strcmp(argv[1], AGGREG_SUM) == 0)
        flags = TYPE_TOTAL;
    else if (strcmp(argv[1], AGGREG_VAR) == 0)
        flags = TYPE_VARIATION;
    else
    {
        printf("invalid aggr \"%s\"\n", argv[1]);
        return -1;
    }
    flags |= AGGREG_PERIOD | SCOPE_GLOBAL;

    /* if (strcmp(argv[2], TYPE_TAMPONI))
        tamponi = true;
    else if (strcmp(argv[2], TYPE_NCASI))
        tamponi = false;
    else
    {
        printf("invalid type \"%s\"\n", argv[2]);
        return -1;
    } */

    /* iterate through strings divided by "-" to read the period */
    for (str = argv[3]; ; str = NULL)
    {
        token = strtok(str, "-");
        if (token == NULL)
            break;
        
        if (count >= 2)
            continue;

        period[count] = str_to_time(token);
        if (period[count] == -1)
        {
            printf("invalid date format: \"%s\"\n", token);
            return -1;
        }
        count++;
    }

    /* the dates specified in the period are more or less than two */
    if (count != 2)
    {
        printf("invalid period format\n");
        return -1;
    }

    if (strcmp(argv[1], AGGREG_SUM) == 0)
        get_aggr_tot(peer, period[0], period[1]);
    else if (strcmp(argv[1], AGGREG_VAR) == 0)
        return 0;

    /* period_len = period length in seconds / seconds in a day */
    /* does not account for leap seconds */
    /* period_len = 
        (int)difftime(period[1], period[0]) / 86400 + 1;

    entry = search_entry(peer->entries.last, period[0], flags, period_len);

    if (entry != NULL)
    {
        printf("entry found\n");
        print_entry(entry);
        return 0;
    }
    
    printf("entry not found in local register\n"); */


    


    /* TODO when comparing entries, check period_len only if both(?) are
    of type AGGREG_PERIOD */

    /* search required entries:
        time = end period
        found = empty entry list
        not_found = empty entry list

        flags = SCOPE_GLOBAL
        if looking for variation:
            flags |= TYPE_VARIATION
        else:
            flags |= TYPE_TOTAL

        while time >= start period:
            entry = search entry (time, flags, 0)

            if entry == NULL:
                not_found.push create entry with values zero
            else
                found.push entry

            time -= 1 day

        if not_found.empty:
            print all entries
            return
        
        ask entries to peers

        for each past entry from end to start period:
            - must be daily
            - must be 
     */

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

/* ########## FUNCTIONS THAT HANDLE PEER REQUESTS ########## */





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
