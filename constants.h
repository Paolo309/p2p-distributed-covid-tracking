#ifndef COSTANTS_H
#define COSTANTS_H

/* comment out the three defines below to use the real date */
#define FAKE_DAY 14
#define FAKE_MONTH 9
#define FAKE_YEAR 2021

#define REG_CLOSING_HOUR 23

/* dst hack: Italy's epoch 0 */
#define BEGINNING_OF_TIME -3600
#define DEFAULT_PERIOD_LOWER_BOUND "2020:03:01"

/* used to distinguish the type of query */
#define REQ_SOMMA 0
#define REQ_VARIAZIONE 1
#define REQ_TAMPONI 2
#define REQ_NUOVI_CASI 3

/* paramenters values for command `get` */
#define ARG_TYPE_TAMPONI "t"
#define ARG_TYPE_NCASI "c"
#define ARG_AGGREG_SUM "sum"
#define ARG_AGGREG_VAR "var"

/* states of the peer */
#define STATE_OFF 0
#define STATE_STARTING 1
#define STATE_STARTED 2
#define STATE_STARTING_FLOOD 3
#define STATE_HANDLING_FLOOD 4
#define STATE_STOPPING 5
#define STATE_STOPPED 6

#define NUM_MAX_REQUESTS 3
#define REQUESTER_SELF -1

/* how much to wait for the ds server response on start, in seconds */
#define START_MSG_TIMEOUT 3

#endif
