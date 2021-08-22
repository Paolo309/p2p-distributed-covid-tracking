/*
entry (timestamp [key], tamponi, nuovi_casi)

register:
    dd-mm-yyyy num_t num_c
where:
- entry not present if no add for timestamp has been done
- num_t = 0 if entry exists but no add for tamponi has been done on timestamp
- num_c = 0 if entry exists but no add for nuovi_casi has been done on timestamp

aggregates:
    dd-mm-yyyy F num_t num_c
where:
- F (if 0 both computed; if 1 num_t computed; if 2 num_c computed)
- entry not present if aggr never computed
- num_t valid value if F = 0 || F = 1
- num_c valid value if F = 0 || F = 2


keep list of entries:
- read ordered from file


*/

#include "data.h"

Entry *create_entry(char* timestamp, int32_t tamponi, int32_t nuovi_casi, uint8_t flag)
{
    Entry* tmp = malloc(sizeof(Entry));
    
    strncpy(tmp->timestamp, timestamp, TIMESTAMP_STRLEN);
    tmp->tamponi = tamponi;
    tmp->nuovi_casi = nuovi_casi;
    tmp->flag = flag;
    tmp->prev = tmp->next = NULL;
    
    return tmp;
}

void init_entry_list(EntryList *list)
{
    list->first = list->last = NULL;
}

void free_entry_list(EntryList *list)
{
    Entry *p, *tmp;
    
    p = list->first;
    while (p != NULL)
    {
        tmp = p->next;
        free(p);
        p = tmp;
    }
}

bool is_entry_list_empty(EntryList *list)
{
    return list->first == NULL;
}

/* Entry *push_back_entry(EntryList *list, char* timestamp, int32_t tamponi, int32_t nuovi_casi)
{
    Entry *tmp;
    
    if (list == NULL) return;
    
    tmp = create_entry(timestamp, tamponi, nuovi_casi);
    
    if (list->first == NULL) 
    {
        list->first = tmp;
    }
    else 
    {
        tmp->prev = list->last;
        list->last->next = tmp;
    }
    
    list->last = tmp;
} */

void load_register_from_file(EntryList *entries, const char* file_name)
{
    FILE *fp;
    Entry *tmp_entry, *prev;
    char tmp_ts[TIMESTAMP_STRLEN];
    int32_t flag;
    int32_t tmp_tamponi, tmp_ncasi;
    
    init_entry_list(entries);
    prev = NULL;
    
    fp = fopen(file_name, "r");
    
    while (fscanf(fp, "%s %d %d %d", tmp_ts, &tmp_tamponi, &tmp_ncasi, &flag) != EOF)
    {
        tmp_entry = create_entry(tmp_ts, tmp_tamponi, tmp_ncasi, flag);
        
        tmp_entry->prev = prev;
        if (prev != NULL) prev->next = tmp_entry;
        prev = tmp_entry;
        
        if (entries->first == NULL)
            entries->first = tmp_entry;
    }
    
    entries->last = tmp_entry;
    
    fclose(fp);
}

void print_entries_asc(EntryList *list)
{
    Entry* p = list->first;
    
    while (p)
    {
        printf("%s t:%d c:%d ", p->timestamp, p->tamponi, p->nuovi_casi);
        
        if (p->flag == ENTRY_LOCAL) printf("LOCAL\n");
        else if (p->flag == ENTRY_GLOBAL) printf("GLOBAL\n");
        else printf("ERR\n");
        
        p = p->next;
    }
}

void print_entries_dsc(EntryList *list)
{
    Entry* p = list->last;
    
    while (p)
    {
        printf("%s t:%d c:%d ", p->timestamp, p->tamponi, p->nuovi_casi);
        
        if (p->flag == ENTRY_LOCAL) printf("LOCAL\n");
        else if (p->flag == ENTRY_GLOBAL) printf("GLOBAL\n");
        else printf("ERR\n");
        
        p = p->prev;
    }
}

/* 
    TODO considerare di passare entry per valore 
    Alternativamente: passare puntatore a entry, ma fare free
    della entry nella lista. (Dipende da come voglio usare la funzione dopo)
*/

/**
 * @brief Add an entry to the specified entry list
 * 
 * @note If entry already exists in list, new entry is copied
 * into already present entry
 * 
 * @param entries 
 * @param entry 
 */
void add_entry(EntryList *entries, Entry *entry)
{
    Entry *p, *succ;
    int cmp_res;
    
    if (is_entry_list_empty(entries))
    {
        /* add entry to list */
        entries->last = entries->first = entry;
        entry->next = entry->prev = NULL;
        return;
    }
    
    /* iterate entries backward */
    p = entries->last;
    while (p)
    {
        cmp_res = strcmp(p->timestamp, entry->timestamp);
        
        /* entry already in register */
        if (cmp_res == 0)
        {
            /* update entry values */
            p->tamponi += entry->tamponi;
            p->nuovi_casi += entry->nuovi_casi;
            p->flag = entry->flag;
            return;
        }
        
        /* found entry in list preceding the entry being added */
        if (cmp_res < 0)
        {
            /* append `entry` after `p` */
            
            succ = p->next;
            p->next = entry;
            
            entry->next = succ;
            entry->prev = p;
            
            if (succ != NULL) /* it's not the last entry */
                succ->prev = entry;
            else /* it is the last entry */
                entries->last = entry;
            
            return;
        }
        
        p = p->prev;
    }
    
    /* no entries found, put `entry` at beginning */
    
    entries->first->prev = entry;
    entry->next = entries->first;
    entry->prev = NULL;
    entries->first = entry;
}

void merge_entry_lists(EntryList *dest, EntryList *src)
{
    Entry *a, *b, *tmp, *tmp_prev;
    int cmp_res;
    
    if (dest == NULL || src == NULL)
        return;
    
    /* if src list empty, nothing to merge */
    if (is_entry_list_empty(src))
        return;
    
    /* if dest list empty, nothing to merge, just copy src list pointers */
    if (is_entry_list_empty(dest))
    {
        dest->first = src->first;
        dest->last = src->last;
        return;
    }
    
    a = dest->last;
    b = src->last;
    
    /* iterate both lists from last entry, backward, until it reaches 
       the head of one of the two */
    while (a != NULL && b != NULL)
    {
        /* move a to first entry in dest where a->timestamp < b->timestamp */
        while (a != NULL && (cmp_res = strcmp(a->timestamp, b->timestamp)) > 0)
            a = a->prev;
        
        /* all entries in src are smaller than entries in dest */
        if (a == NULL)
            break;
        
        /* found an entry in dest with same timestamp of entry in src */
        if (cmp_res == 0)
        {
            /* update dest entry with src entry data, and delete the src entry */
            
            a->tamponi += b->tamponi;
            a->nuovi_casi += b->nuovi_casi;
            a->flag = b->flag; /* ? */
            
            tmp = b->prev;
            free(b);
            b = tmp;
            
            a = a->prev;
            continue;
        }
        
        /* inserting dest entry after src entry pointed by a */
        
        tmp_prev = b->prev; /* used to continue the loop */
        
        tmp = a->next;
        
        /* if a is not the last entry in dest */
        if (a->next != NULL) a->next->prev = b;
        a->next = b;
        
        /* if b is not the first entry in src */
        if (b->prev != NULL) b->prev->next = NULL;
        b->prev = a;
        
        b->next = tmp;
        
        /* keep updated pointer to last entry in dest: b is now after a */
        if (a == dest->last)
            dest->last = b;
        
        b = tmp_prev;
    }
    
    /* keep updated pointer to last entry in src */
    src->last = dest->last;
    
    /* no more entries in src to copy in dest */
    if (b == NULL)
    {
        /* keep updated pointer to fist entry in src */
        src->first = dest->first;
        /* dest head isn't new, so dest->first is already pointing to dest head */
        return;
    }
    
    /* all remaining entries in src are smaller than remaining entries in dest */
    if (a == NULL)
    {
        /* copying remaining src entries in dest */
        b->next = dest->first;
        dest->first->prev = b;
        
        /* keep updated pointer to fist entry in dest */
        dest->first = src->first;
    }
}


int maina()
{
    EntryList entries, others;
    Entry *tmp;
    
    
    init_entry_list(&entries);
    init_entry_list(&others);
    
    load_register_from_file(&entries, REGISTER_FNAME);
    
    printf("entries\n");
    print_entries_asc(&entries);
    
    tmp = create_entry("2020-02-13", 100, 23, ENTRY_LOCAL);
    add_entry(&others, tmp);
    
    printf("others\n");
    print_entries_asc(&others);
    
    merge_entry_lists(&entries, &others);
    
    printf("\nentries\n");
    print_entries_asc(&entries);
    
    printf("others\n");
    print_entries_asc(&others);
    
    printf("\n\nentries dsc\n");
    print_entries_dsc(&entries);
    
    printf("others dsc\n");
    print_entries_dsc(&others);
    
    /* tmp = create_entry("2020-02-09", 100, 23, ENTRY_LOCAL);
    add_entry(&others, tmp); */
    
    return 0;
    
    /* printf("DESC\n");
    print_entries_dsc(&entries); */
    
    tmp = create_entry("2020-02-09", 100, 23, ENTRY_LOCAL);
    
    add_entry(&entries, tmp);
    
    tmp = create_entry("2020-02-10", 100, 0, ENTRY_GLOBAL);
    
    add_entry(&entries, tmp);
    
    printf("2\n");
    print_entries_asc(&entries);
    
    printf("DESC\n");
    print_entries_dsc(&entries);
    
    return 0;
}
