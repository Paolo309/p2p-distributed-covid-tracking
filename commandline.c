#include "commandline.h"

/**
 * Wait for user input. Expands the input stirng returning the 
 * the array of strings inserted before new line, and its length.
 * 
 * @param argc pointer to where to store the length of the array
 * @return pointer to the array of strings inserted
 */
char **get_command_line(int *argc)
{
    wordexp_t p;
    int i, len, check;
    char buffer[MAX_CMD_LEN];
    char **argv;

    if (argc == NULL) return NULL;
    
    /* https://www.adoclib.com/blog/how-to-read-from-input-until-newline-is-found-using-scanf.html */
    check = scanf("%[^\n]%*c", buffer);
    
    if (check == 0)
    {
        getchar();
        *argc = 0;
        return NULL;
    }
    
    /* shell-like expansion of string in buffer into p */
    if (wordexp(buffer, &p, 0)) /* returns 0 on success */
        return NULL;

    *argc = p.we_wordc;

    argv = calloc(*argc, sizeof(char*));
    
    /* copying command and arguments into argv */
    for (i = 0; i < p.we_wordc; i++)
    {
        len = strlen(p.we_wordv[i]) + 1;
        argv[i] = calloc(len, sizeof(char));
        strcpy(argv[i], p.we_wordv[i]);
    }
    
    wordfree(&p);

    return argv;
}

/**
 * Free the strings in the given array.
 * 
 * @param argc length of the array
 * @param argv array of strings
 */
void free_command_line(int argc, char **argv)
{
    while (argc > 0) 
        free(argv[--argc]);
    free(argv);
}

int main_test_cmd()
{
    int argc, i;
    char **argv;

    argv = get_command_line(&argc);

    if (argv == NULL)
    {
        printf("errore\n");
        goto fine;
    }

    for (i = 0; i < argc; i++)
    {
        printf("arg %d: %s\n", i, argv[i]);
    }

    free_command_line(argc, argv);
fine:

    return 0;
}
