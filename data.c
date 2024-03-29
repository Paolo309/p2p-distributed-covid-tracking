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
    struct tm time_tm = { 0 };

    p = strptime(str, "%Y:%m:%d", &time_tm);
    if (p != &str[TIMESTAMP_STRLEN - 1])
        return -1;

    time_tm.tm_isdst = -1;
    return mktime(&time_tm);
}

/**
 * Convert a time_t representation of a date into a string pointed by str.
 * 
 * @param str where to write the string
 * @param time
 */
void time_to_str(char *str, time_t *timep)
{
    struct tm *timeinfo;
    /* printf("\nPRINT: %ld\n", *timep); */
    timeinfo = localtime(timep);
    
    timeinfo->tm_isdst = -1;
    
    strftime(str, TIMESTAMP_STRLEN, "%Y:%m:%d", timeinfo);

    /* making sure the time is correctly set */
    /* if (timeinfo->tm_hour || timeinfo->tm_min || timeinfo->tm_sec)
        printf(
            "\n{ERR %s -> %d:%d:%d}\n", 
            str, timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec
        ); */
}

/**
 * Create a new entry.
 * 
 * @return newly allocated entry
 */
Entry *create_entry_empty()
{
    Entry *tmp = malloc(sizeof(Entry));
    memset(tmp, 0, sizeof(Entry));
    return tmp;
}

/**
 * Create a new entry as copy of src. Pointer to next and preceding
 * entries are set to NULL.
 * 
 * @param src entry to be copied
 * @return newly allocated entry
 */
Entry *copy_entry(Entry *src)
{
    Entry *tmp = malloc(sizeof(Entry));
    memcpy(tmp, src, sizeof(Entry));
    tmp->next = tmp->prev = NULL;
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
Entry *create_entry(time_t timestamp, int32_t tamponi, int32_t nuovi_casi, uint8_t flags, int32_t period_len)
{
    Entry* tmp = malloc(sizeof(Entry));

    tmp->timestamp = timestamp;
    tmp->tamponi = tamponi;
    tmp->nuovi_casi = nuovi_casi;
    tmp->flags = flags;
    tmp->prev = tmp->next = NULL;
    tmp->period_len = period_len;

    return tmp;
}

/** 
 * Compare two entries. 
 * 
 * @param a 
 * @param b 
 * @return <0 if the first entry precedes the second; =0 if they are equal; >0 otherwise.
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
    time->tm_isdst = -1;
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
    list->length = 0;
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

    list->length = 0;
}

/**
 * @param list 
 * @return true if the list is empty 
 */
bool is_entry_list_empty(EntryList *list)
{    
    return list->length == 0;
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
    
    if (fp == NULL) 
    {
        printf("cannot open file %s\n", file_name);
        printf("no entries loaded\n");
        return;
    }
    
    tmp_entry = NULL;

    while (fscanf(fp, "%s %d %d %d", tmp_str_time, &flags, &tmp_tamponi, &tmp_ncasi) != EOF) {
        tmp_time = str_to_time(tmp_str_time);
        tmp_entry = create_entry(tmp_time, tmp_tamponi, tmp_ncasi, flags, 0);
        
        /* only read period_len if needed */
        if (tmp_entry->flags & ENTRY_AGGREG)  
            fscanf(fp, "%d", &tmp_entry->period_len);

        tmp_entry->prev = prev;
        if (prev != NULL) prev->next = tmp_entry;
        prev = tmp_entry;
        
        if (entries->first == NULL)
            entries->first = tmp_entry;
        
        entries->length++;
    }
    
    entries->last = tmp_entry;
    
    fclose(fp);

    printf("loaded %d entries\n", entries->length);
}

/**
 * Merge the src entry list into dest. At the end, both dest and src
 * pointers, though pointing to the originals EntryList instances, actually
 * contain the same list (same head and tail pointers). Entries that get
 * merged and duplicate entries are free. Entries are kept sorted by 
 * ascending timestamps.
 * Caution! Freeing src also frees dest.
 * 
 * @param dest 
 * @param src 
 */
void merge_entry_lists(EntryList *dest, EntryList *src, bool strict)
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
        dest->length = src->length;
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
            if (!strict || (a->flags & ENTRY_SCOPE) == SCOPE_LOCAL)
            {
                /* update dest entry with src entry data, and delete the src entry */
                
                /* adding LOCAL entry values to LOCAL entry values */
                if ((b->flags & ENTRY_SCOPE) == SCOPE_LOCAL)
                {
                    a->tamponi += b->tamponi;
                    a->nuovi_casi += b->nuovi_casi;
                    /* a->flags = b->flags; */
                }
                /* copying GLOBAL entry values into LOCAL entry values */
                else
                {
                    a->tamponi = b->tamponi;
                    a->nuovi_casi = b->nuovi_casi;
                    a->flags = b->flags;
                }
            }

            remove_entry(src, b);
            
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

        dest->length++;
        src->length--;

        b = tmp_prev;
    }
    
    /* keep updated the pointer to the last entry in src */
    src->last = dest->last;
    
    /* no more entries in src to copy in dest */
    if (b == NULL)
    {
        /* keep updated the pointer to the fist entry in src */
        src->first = dest->first;
        src->length = dest->length;
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

        dest->length += src->length;
        src->length = dest->length;
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
        entries->length = 1;
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
                /* adding LOCAL entry values to LOCAL entry values */
                if ((entry->flags & ENTRY_SCOPE) == SCOPE_LOCAL)
                {
                    
                    p->tamponi += entry->tamponi;
                    p->nuovi_casi += entry->nuovi_casi;
                    /* p->flags = entry->flags; */
                }
                /* copying GLOBAL entry values into LOCAL entry values */
                else
                {
                    p->tamponi = entry->tamponi;
                    p->nuovi_casi = entry->nuovi_casi;
                    p->flags = entry->flags;
                }
            }
            free(entry);
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

            entries->length++;
            
            return;
        }
        
        p = p->prev;
    }
    
    /* no entries found, put `entry` at beginning */
    
    entries->first->prev = entry;
    entry->next = entries->first;
    entry->prev = NULL;
    entries->first = entry;

    entries->length++;
}

/**
 * Removes the specified entry from the list. Does not check whether the
 * list contains or not the entries.
 * N.B. If the entry is not in the list, the operation is similar to a
 * concatenation which excludes the first entry in the second list.
 * 
 * @param entries 
 * @param entry 
 */
void remove_entry(EntryList *entries, Entry *entry)
{
    if (entry->next != NULL)
        entry->next->prev = entry->prev;
    else
        entries->last = entry->prev;
    
    if (entry->prev != NULL)
        entry->prev->next = entry->next;
    else
        entries->first = entry->next;
    
    entries->length--;
}

void print_entry(Entry *entry) 
{
    time_t tmp_end_period;
    char str_time[TIMESTAMP_STRLEN + 10];

    if (entry->flags & ENTRY_AGGREG)
    {
        time_to_str(str_time, &entry->timestamp);
        printf("%s", str_time);

        tmp_end_period = get_enf_of_period(entry);
        time_to_str(str_time, &tmp_end_period);
        printf("-%s (%d days)", str_time, entry->period_len);
    }
    else
    {
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
        printf("LOCALE ");

    printf(" ");

    printf("t: %d\tc: %d", entry->tamponi, entry->nuovi_casi);
    
    printf("\n");
}

void print_entries_asc(EntryList *list, const char* text)
{
    Entry* p = list->first;
    int count = 0;

    if (text == NULL)
        printf("\n==== %d entries ====", list->length);
    else
    {
        printf("\n==== %s (%d) ====", text, list->length);
    }

    if (list->length == 0)
    {
        printf(" [empty list]\n");
        return;
    }

    printf("\n");

    while (p)
    {
        print_entry(p);
        p = p->next;
        count++;
    }

    printf("====================\n\n");
    
    if (count != list->length)
    {
        printf("\n\n\n##############################\n");
        printf("l %d, c %d\n", list->length, count);
        printf("##############################\n\n\n\n");
    }
}

void print_entries_dsc(EntryList *list, const char *text)
{
    Entry* p = list->last;
    int count = 0;
    
    if (text == NULL)
        printf("\n==== %d entries DEscending ====\n", list->length);
    else
        printf("\n==== %s (%d) ====\n", text, list->length);

    while (p)
    {
        print_entry(p);
        p = p->prev;
        count++;
    }

    printf("====================\n\n");

    if (count != list->length)
    {
        printf("\n\n\n##############################\n");
        printf("l %d, c %d\n", list->length, count);
        printf("##############################\n\n\n\n");
    }
}

char* allocate_entry_list_buffer(int n)
{
    size_t size;

    if (n <= 0) return NULL;

    size = sizeof(int32_t) + n * ( sizeof(time_t) + 4 * sizeof(int32_t) );
    return malloc(size);
}

/**
 * Serialize list of entries into buffer dest.
 * 
 * @param dest 
 * @param list 
 * @return pointer to the byte after the last one written
 */
char *serialize_entries(char *dest, EntryList *list)
{
    Entry *entry;

    *(int32_t*)dest = htonl(list->length);
    dest += sizeof(int32_t);

    entry = list->first;
    while (entry)
    {
        *(time_t*)dest = htonl(entry->timestamp);
        dest += sizeof(time_t);

        *((int32_t*)dest + 0) = htonl(entry->flags);
        *((int32_t*)dest + 1) = htonl(entry->tamponi);
        *((int32_t*)dest + 2) = htonl(entry->nuovi_casi);
        *((int32_t*)dest + 3) = htonl(entry->period_len);

        dest += 4 * sizeof(int32_t);

        entry = entry->next;
    }

    return dest;
}

/**
 * Deserialize buffer into list of entries. The list does not
 * need to be initialized, but it must be allocated.
 * 
 * @param dest 
 * @param list 
 * @return pointer to the byte after the last one read
 */
char *deserialize_entries(char *src, EntryList *list)
{
    int32_t count;
    Entry *entry, *prec;

    count = ntohl(*(int32_t*)src);
    src += sizeof(int32_t);

    init_entry_list(list);
    list->length = count;

    entry = NULL;
    prec = NULL;

    while (count > 0)
    {
        entry = create_entry_empty();

        entry->timestamp = (int)ntohl(*(time_t*)src);
        src += sizeof(time_t);

        entry->flags      = ntohl(*((int32_t*)src + 0));
        entry->tamponi    = ntohl(*((int32_t*)src + 1));
        entry->nuovi_casi = ntohl(*((int32_t*)src + 2));
        entry->period_len = ntohl(*((int32_t*)src + 3));

        src += 4 * sizeof(int32_t);

        entry->prev = prec;

        if (list->first == NULL)
            list->first = entry;

        if (prec != NULL)
            prec->next = entry;

        count--;
        prec = entry;
    }

    list->last = entry;

    return src;
}

/**
 * Search entry with given parameter and return its o=pointer if found, NULL otherwise.
 * Argument `from` must point to the last element of the list that is being looked up.
 * 
 * @param from 
 * @param timestamp 
 * @param flags 
 * @param period_len 
 * @return pointer to found entry, NULL otherwise
 */
Entry *search_entry(Entry *from, time_t timestamp, int32_t flags, int32_t period_len)
{
    Entry *entry, *model;

    model = create_entry(timestamp, 0, 0, flags, period_len);

    entry = from;

    while (entry)
    {
        if (cmp_entries(entry, model) == 0) break;
        entry = entry->prev;
    }

    return entry;
}
