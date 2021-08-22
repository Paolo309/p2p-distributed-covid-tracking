#ifndef DATA_H
#define DATA_H

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#define REGISTER_FNAME "register.txt"
#define TIMESTAMP_STRLEN 11
#define ENTRY_LOCAL 1
#define ENTRY_GLOBAL 2


typedef struct Entry {
    char timestamp[11];
    int32_t tamponi;
    int32_t nuovi_casi;
    int32_t flag;
    struct Entry *prev;
    struct Entry *next;
} Entry;

typedef struct EntryList
{
    Entry *first;
    Entry *last;
} EntryList;

Entry *create_entry(char* timestamp, int32_t tamponi, int32_t nuovi_casi, uint8_t flag);

void init_entry_list(EntryList *list);
void free_entry_list(EntryList *list);
bool is_entry_list_empty(EntryList *list);

void add_entry(EntryList *entries, Entry *entry);
void merge_entry_lists(EntryList *entries, EntryList *new_entries);

void load_register_from_file(EntryList *entries, const char* file_name);

/* void add_entry_tamponi(EntryList *entries, char* timestamp, int32_t quanti);
void add_entry_nuovi_casi(EntryList *entries, char* timestamp, int32_t quanti); */

void print_entries_asc(EntryList *list);
void print_entries_dsc(EntryList *list);


#endif
