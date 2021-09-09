#ifndef COMMON_UTILS_H
#define COMMON_UTILS_H

#include <time.h>
#include "constants.h"
#include "comm.h"

int validate_port(const char *str_port);

int32_t get_num_of_peers(Message *msgp);
void set_num_of_peers(Message *msgp, int32_t num);
void inc_num_of_peers(Message *msgp, int32_t num);
int32_t add_peer_to_msg(Message *msgp, in_port_t port);
int32_t merge_peer_list_msgs(Message *dest, Message *src);
in_port_t get_peer_port(Message *msgp, int n);

void add_desc(fd_set *fdsetp, int *fdmax, int fd);
void remove_desc(fd_set *fdsetp, int *fdmax, int fd);

time_t add_days(time_t time, int days);
int diff_days(time_t time1, time_t time0);
time_t get_today_adjusted();

#endif