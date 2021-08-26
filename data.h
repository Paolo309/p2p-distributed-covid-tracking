#ifndef DATA_H
#define DATA_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define FNAME_REGISTER "register.txt"
#define TIMESTAMP_STRLEN 11

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

Flag values:
    0b00 = 0 = TOTAL      LOCALE
    0b01 = 1 = TOTAL      GLOBALE 
    0b10 = 2 = VARIATION  LOCALE 
    0b11 = 3 = VARIATION  GLOBALE

Entries of the same type are ordered by timestamp. Two entries with different
type but same timestamp are ordered by type: TYPE_TOTAL first.

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


typedef struct Entry {
    char timestamp[11];
    int32_t flags;
    int32_t tamponi;
    int32_t nuovi_casi;
    struct Entry *prev;
    struct Entry *next;
    
} Entry;

typedef struct EntryList {
    Entry *first;
    Entry *last;
} EntryList;

Entry *create_entry(char* timestamp, int32_t tamponi, int32_t nuovi_casi, uint8_t flags);
int cmp_entries(const Entry *a, const Entry *b);

void init_entry_list(EntryList *list);
void free_entry_list(EntryList *list);
bool is_entry_list_empty(EntryList *list);

void load_register_from_file(EntryList *entries, const char* file_name);

void merge_entry_lists(EntryList *entries, EntryList *new_entries);

void add_entry(EntryList *entries, Entry *entry);

void print_entry(Entry *entry);
void print_entries_asc(EntryList *list);
void print_entries_dsc(EntryList *list);


#endif
