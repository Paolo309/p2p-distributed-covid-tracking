#ifndef DATA_H
#define DATA_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <arpa/inet.h>
#define __USE_XOPEN
#include <time.h>

#define FNAME_REGISTER "register.txt"
#define TIMESTAMP_STRLEN 11

#define COPY_STRICT true
#define COPY_SHALLOW false

/*
Register entry format:
    entry(timestamp flags tamponi nuovi_casi)

Flag field stores:
    entry scope:
        local: entry value is relative to this peer
        global: entry value is aggregated (same for all peers)
    entry type:
        total: entry value is a sum of values relative to the timestamp
        variation: entry value is the variation from the day before
    entry aggreg:
        daily: entry value is relative to the day specified by timestamp
        period: entry value is relative to a period starting from timestamp
                with duration specified by period_len

Some flag values:
    DAILY:
        0b000 = 0 = TOTAL      LOCALE
        0b001 = 1 = TOTAL      GLOBALE 
    AGGREG:
        0b101 = 5 = TOTAL      GLOBALE
        0b111 = 7 = VARIATION  GLOBALE

While adding a new entry into the register, if an entry with same type and
timestamp is already present, the two are aggregated only if the already
existing one is local (SCOPE_LOCAL).
*/

#define ENTRY_SCOPE 1
#define SCOPE_LOCAL 0
#define SCOPE_GLOBAL 1

#define ENTRY_TYPE 2
#define TYPE_TOTAL 0
#define TYPE_VARIATION 2

#define ENTRY_AGGREG 4
#define AGGREG_DAILY 0
#define AGGREG_PERIOD 4

typedef struct Entry {
    time_t timestamp;
    int32_t flags;
    int32_t tamponi;
    int32_t nuovi_casi;
    int32_t period_len;
    struct Entry *prev;
    struct Entry *next;
    
} Entry;

typedef struct EntryList {
    int length;
    Entry *first;
    Entry *last;
} EntryList;

time_t str_to_time(const char *str);
void time_to_str(char *str, time_t *time);
Entry *create_entry_empty();
Entry *copy_entry(Entry *src);
Entry *create_entry(time_t timestamp, int32_t tamponi, int32_t nuovi_casi, uint8_t flags, int32_t period_len);
int cmp_entries(Entry *a, Entry *b);

time_t get_enf_of_period(Entry *entry);

void init_entry_list(EntryList *list);
void free_entry_list(EntryList *list);
bool is_entry_list_empty(EntryList *list);

void load_register_from_file(EntryList *entries, const char* file_name);

void merge_entry_lists(EntryList *entries, EntryList *new_entries, bool strict);

void add_entry(EntryList *entries, Entry *entry);
void remove_entry(EntryList *entries, Entry *entry);

void print_entry(Entry *entry);
void print_entries_asc(EntryList *list, const char *text);
void print_entries_dsc(EntryList *list, const char *text);

char* allocate_entry_list_buffer(int n);
char *serialize_entries(char *dest, EntryList *list);
char *deserialize_entries(char *src, EntryList *list);

Entry *search_entry(Entry *from, time_t timestamp, int32_t flags, int32_t period_len);

#endif
