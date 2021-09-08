#include "common_utils.h"

time_t add_days(time_t time, int days)
{
    struct tm *timeinfo = localtime(&time);
    timeinfo->tm_mday += days;
    timeinfo->tm_isdst = -1;
    return mktime(timeinfo);
}

int diff_days(time_t time1, time_t time0)
{
    return difftime(time1, time0) / 86400;
}

time_t get_today_adjusted()
{
    time_t t_now;
    struct tm* tm_now;

    t_now = time(NULL);
    tm_now = localtime(&t_now);

    #ifdef FAKE_DAY
    tm_now->tm_mday = FAKE_DAY;
    #endif

    #ifdef FAKE_MONTH
    tm_now->tm_mon = FAKE_MONTH - 1;
    #endif

    #ifdef FAKE_YEAR
    tm_now->tm_year = FAKE_YEAR - 1900;
    #endif

    if (tm_now->tm_hour >= REG_CLOSING_HOUR)
        tm_now->tm_mday++;

    tm_now->tm_hour = tm_now->tm_min = tm_now->tm_sec = 0;
    tm_now->tm_isdst = -1;

    return mktime(tm_now);
}

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

int32_t get_num_of_peers(Message *msgp)
{
    return msgp->body_len / sizeof(in_port_t);
}

void set_num_of_peers(Message *msgp, int32_t num)
{
    msgp->body_len = num * sizeof(in_port_t);
}

void inc_num_of_peers(Message *msgp, int32_t num)
{
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
