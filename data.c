#include "data.h"

/**
 * Convert a string representation of a date into a time_t value.
 * String format: "%Y:%m:%d", example: "2020:03:01" 
 * 
 * @param str 
 * @return time_t representation of str, -1 if cannot convert
 */
time_t str_to_time(const char *str)
{
    char *p;
    struct tm time = { 0 };
    p = strptime(str, "%Y:%m:%d", &time);
    if (p != &str[TIMESTAMP_STRLEN - 1])
        return -1;
    return mktime(&time);
}

/**
 * Convert a time_t representation of a date into a string pointed by str.
 * 
 * @param str where to write the string
 * @param time
 */
void time_to_str(char *str, time_t *time)
{
    struct tm *timeinfo;
    timeinfo = localtime(time);
    strftime(str, TIMESTAMP_STRLEN, "%Y:%m:%d", timeinfo);
}

/**
 * Create a new entry.
 * 
 */
Entry *create_entry_empty()
{
    Entry *tmp = malloc(sizeof(Entry));
    memset(tmp, 0, sizeof(Entry));
    return tmp;
}

/**
 * Create a new entry.
 * 
 * @param timestamp 
 * @param tamponi 
 * @param nuovi_casi 
 * @param flags Either SCOPE_LOCAL or ENTRY_GLOBAL
 * @return The new entry
 */
Entry *create_entry(time_t timestamp, int32_t tamponi, int32_t nuovi_casi, uint8_t flags)
{
    Entry* tmp = malloc(sizeof(Entry));

    tmp->timestamp = timestamp;
    tmp->tamponi = tamponi;
    tmp->nuovi_casi = nuovi_casi;
    tmp->flags = flags;
    tmp->prev = tmp->next = NULL;
    tmp->period_len = 0;

    return tmp;
}

/** 
 * Compare two entries. Entries are sorted in this manner:
 * (1) earlier timestamp first;
 * (2) if timestamps are equal, TYPE_TOTAL comes first;
 * (3) if ENTRY_TYPEs are equal, AGGREG_DAILY comes first;
 * (4) if ENTRY_AGGREGs are equal, shortest period length first;
 * (5) if period lengths are equal, the entries are equal.
 * 
 * @param a 
 * @param b 
 * @return Returns a negative value if the first entry precedes the second; zero
 * if they are equal; a positive value otherwise.
 */
int cmp_entries(Entry *a, Entry *b)
{
    int cmp_res;
    
    cmp_res = a->timestamp - b->timestamp;
    if (cmp_res != 0)
        return cmp_res;

    cmp_res = (a->flags & ENTRY_TYPE) - (b->flags & ENTRY_TYPE);
    if (cmp_res != 0)
        return cmp_res;

    cmp_res = (a->flags & ENTRY_AGGREG) - (b->flags & ENTRY_AGGREG);
    if (cmp_res != 0)
        return cmp_res;

    return a->period_len - b->period_len;
}

/**
 * Sum to the entry's timestamp the number of days in period_len.
 * 
 * @param entry 
 * @return the last day of the period stored in the entry
 */
time_t get_enf_of_period(Entry *entry)
{
    struct tm *time = localtime(&entry->timestamp);
    time->tm_mday += entry->period_len - 1;
    return mktime(time);
}

/**
 * Initialize the EntryList structure.
 * 
 * @param list 
 */
void init_entry_list(EntryList *list)
{
    list->first = list->last = NULL;
}

/**
 * Free all the entries in the EntryList structure.
 * 
 * @param list 
 */
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

/**
 * @param list 
 * @return true if the list is empty 
 */
bool is_entry_list_empty(EntryList *list)
{
    return list->first == NULL;
}

/**
 * Loads a register from the specified file into a list. The entry list
 * must be already allocated but not initialized. It does not reorder
 * the entries, to do that, use `sort register.txt -o register.txt`.
 * 
 * @param entries 
 * @param file_name 
 */
void load_register_from_file(EntryList *entries, const char* file_name)
{
    FILE *fp;
    Entry *tmp_entry, *prev;
    char tmp_str_time[TIMESTAMP_STRLEN];
    time_t tmp_time;
    int32_t flags;
    int32_t tmp_tamponi, tmp_ncasi;
    
    init_entry_list(entries);
    prev = NULL;
    
    fp = fopen(file_name, "r");
    
    tmp_entry = NULL;

    while (fscanf(fp, "%s %d %d %d", tmp_str_time, &flags, &tmp_tamponi, &tmp_ncasi) != EOF) {
        tmp_time = str_to_time(tmp_str_time);
        tmp_entry = create_entry(tmp_time, tmp_tamponi, tmp_ncasi, flags);
        
        if (tmp_entry->flags & ENTRY_AGGREG)  
            fscanf(fp, "%d", &tmp_entry->period_len);

        tmp_entry->prev = prev;
        if (prev != NULL) prev->next = tmp_entry;
        prev = tmp_entry;
        
        if (entries->first == NULL)
            entries->first = tmp_entry;
    }
    
    entries->last = tmp_entry;
    
    fclose(fp);
}

/**
 * Merge the src entry list into dest. At the end, both entry lists
 * head and tail pointer point to the same list. Duplicate entries are
 * aggregated into the version present in dest. The version in src gets
 * freed. Entries are kept sorted by ascending timestamps.
 * 
 * TODO check what to with flags! Should it only aggregate when dest flags
 * is not local?
 * 
 * @param dest 
 * @param src 
 */
void merge_entry_lists(EntryList *dest, EntryList *src)
{
    Entry *a, *b; /* pointers to an entry in dest and src respectively */
    Entry *tmp, *tmp_prev;
    int cmp_res; /* result of comparison between timestamps */
    
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
        /* make `a` point to first entry in dest where a->timestamp <= b->timestamp */
        while (a != NULL && (cmp_res = cmp_entries(a, b)) > 0)
            a = a->prev;
        
        /* N.B. If two entries have the same timestamp but different type, the one with
        type ENTRY_TOTAL is considered "smaller", and the two will never be merged. */
        
        /* all entries in src are smaller than entries in dest */
        if (a == NULL)
            break;
        
        /* found an entry in dest with same timestamp of entry in src */
        if (cmp_res == 0)
        {
            /* merge entries only if the dest entry is LOCAL */
            if ((a->flags & ENTRY_SCOPE) == SCOPE_LOCAL)
            {
                /* update dest entry with src entry data, and delete the src entry */
                
                a->tamponi += b->tamponi;
                a->nuovi_casi += b->nuovi_casi;
                a->flags = b->flags; /* ? */
            }
            
            tmp = b->prev;
            free(b);
            b = tmp;
            
            a = a->prev;
            continue;
        }
        
        /* inserting dest entry after src entry pointed by a */
        
        tmp_prev = b->prev; /* used later to continue the loop */
        
        tmp = a->next;
        
        /* if a is not the last entry in dest */
        if (a->next != NULL) a->next->prev = b;
        a->next = b;
        
        /* if b is not the first entry in src */
        if (b->prev != NULL) b->prev->next = NULL;
        b->prev = a;
        
        b->next = tmp;
        
        /* keep updated the pointer to the last entry in dest: b is now after a */
        if (a == dest->last)
            dest->last = b;
        
        b = tmp_prev;
    }
    
    /* keep updated the pointer to the last entry in src */
    src->last = dest->last;
    
    /* no more entries in src to copy in dest */
    if (b == NULL)
    {
        /* keep updated the pointer to the fist entry in src */
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
        
        /* keep updated the pointer to the first entry in dest */
        dest->first = src->first;
    }
}

/**
 * Add an entry to the specified entry list. Entries are kept sorted
 * by ascending timestamp. Duplicate entries are aggregated with 
 * already existing entries. In this case, the entry passed as 
 * parameter can be freed. No check on flags is performed.
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
        /* cmp_res = strcmp(p->timestamp, entry->timestamp); */
        cmp_res = cmp_entries(p, entry);
        
        /* entry already in register */
        if (cmp_res == 0)
        {
            /* merge entries only if the dest entry is LOCAL */
            if ((p->flags & ENTRY_SCOPE) == SCOPE_LOCAL)
            {
                /* update entry values */
                p->tamponi += entry->tamponi;
                p->nuovi_casi += entry->nuovi_casi;
                p->flags = entry->flags;
            }
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

void print_entry(Entry *entry) 
{
    time_t tmp_end_period;
    char str_time[TIMESTAMP_STRLEN];

    if (entry->flags & ENTRY_AGGREG)
    {
        /* time = localtime(&entry->timestamp);
        strftime(str_time, TIMESTAMP_STRLEN, "%Y-%m-%d", time); */
        time_to_str(str_time, &entry->timestamp);
        printf("%s", str_time);

        /* time->tm_mday += entry->period_len - 1;
        tmp_end_period = mktime(time);

        time = localtime(&tmp_end_period);
        strftime(str_time, TIMESTAMP_STRLEN, "%Y-%m-%d", time); */
        tmp_end_period = get_enf_of_period(entry);
        time_to_str(str_time, &tmp_end_period);
        printf("-%s (%d days)", str_time, entry->period_len);
    }
    else
    {
        /* time = localtime(&entry->timestamp);
        strftime(str_time, TIMESTAMP_STRLEN, "%Y-%m-%d", time); */
        time_to_str(str_time, &entry->timestamp);
        printf("%s ", str_time);
    }

    printf(" (flag: %d) ", entry->flags);

    if (entry->flags & TYPE_VARIATION)
        printf("VARIA. ");
    else
        printf("TOTALE ");

    if (entry->flags & SCOPE_GLOBAL)
        printf("GLOBALE");
    else
        printf("LOCALE");

    printf(" ");

    printf("t: %d\tc: %d", entry->tamponi, entry->nuovi_casi);
    
    printf("\n");
}

void print_entries_asc(EntryList *list)
{
    Entry* p = list->first;
    
    while (p)
    {
        print_entry(p);        
        p = p->next;
    }
}

void print_entries_dsc(EntryList *list)
{
    Entry* p = list->last;
    
    while (p)
    {
        print_entry(p);
        p = p->prev;
    }
}

char *serialize_entries(char *buffer, EntryList *list)
{
    int32_t count;
    char *start_buffer;
    Entry *entry;

    start_buffer = buffer;
    buffer += sizeof(int32_t);

    count = 0;
    entry = list->first;

    while (entry)
    {
        *(time_t*)buffer = entry->timestamp;
        buffer += sizeof(time_t);

        *((int32_t*)buffer + 0) = entry->flags;
        *((int32_t*)buffer + 1) = entry->tamponi;
        *((int32_t*)buffer + 2) = entry->nuovi_casi;
        *((int32_t*)buffer + 3) = entry->period_len;

        buffer += 4 * sizeof(int32_t);

        count++;
        entry = entry->next;
    }

    *(int32_t*)start_buffer = count;

    return buffer;
}

char *deserialize_entries(char *buffer, EntryList *list)
{
    int32_t count;
    Entry *entry, *prec;

    count = *(int32_t*)buffer;
    buffer += sizeof(int32_t);

    init_entry_list(list);

    entry = NULL;
    prec = NULL;

    while (count > 0)
    {
        entry = create_entry_empty();

        entry->timestamp = *(time_t*)buffer;
        buffer += sizeof(time_t);

        entry->flags      = *((int32_t*)buffer + 0);
        entry->tamponi    = *((int32_t*)buffer + 1);
        entry->nuovi_casi = *((int32_t*)buffer + 2);
        entry->period_len = *((int32_t*)buffer + 3);

        buffer += 4 * sizeof(int32_t);

        entry->prev = prec;

        if (list->first == NULL)
            list->first = entry;

        if (prec != NULL)
            prec->next = entry;

        count--;
        prec = entry;
    }

    list->last = entry;

    return buffer;
}

Entry *search_entry(Entry *from, time_t timestamp, int32_t flags, int32_t period_len)
{
    Entry *entry, *model;
    char tmp[TIMESTAMP_STRLEN];

    model = create_entry(timestamp, 0, 0, flags);
    model->period_len = period_len;

    entry = from;

    while (entry)
    {
        if (cmp_entries(entry, model) == 0) break;
        entry = entry->prev;
    }

    return entry;
}

int main_test()
{
    EntryList entries, others;
    Entry *tmp;    
    time_t tmp_time;
    char buff[1024];
    char buff2[1024];

    init_entry_list(&entries);
    init_entry_list(&others);
    
    load_register_from_file(&entries, FNAME_REGISTER);
    
    printf("entries\n");
    print_entries_asc(&entries);
    
    tmp_time = str_to_time("2020:01:12");
    tmp = create_entry(tmp_time, 100, 23, SCOPE_GLOBAL);
    add_entry(&others, tmp);

    tmp_time = str_to_time("2020:02:07");
    tmp = create_entry(tmp_time, 200, 46, SCOPE_LOCAL | TYPE_VARIATION);
    add_entry(&others, tmp);

    tmp_time = str_to_time("2020:02:08");
    tmp = create_entry(tmp_time, 2000, 460, SCOPE_GLOBAL | TYPE_TOTAL | AGGREG_PERIOD);
    tmp->period_len = 3;
    add_entry(&others, tmp);
    
    printf("others\n");
    print_entries_asc(&others);
    
    merge_entry_lists(&entries, &others);
    
    printf("\nentries\n");
    print_entries_asc(&entries);
    
    printf("others\n");
    print_entries_asc(&others);

    serialize_entries(buff, &entries);
    memcpy(buff2, buff, 1024);
    deserialize_entries(buff2, &others);
    printf("deserialized\n");
    print_entries_asc(&others);

    
    /* printf("\n\nentries dsc\n");
    print_entries_dsc(&entries);
    
    printf("others dsc\n");
    print_entries_dsc(&others); */
    
    /* tmp = create_entry("2020-02-09", 100, 23, ENTRY_LOCAL);
    add_entry(&others, tmp); */
    
    return 0;
}
