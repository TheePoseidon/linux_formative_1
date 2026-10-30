# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <dirent.h>

# define MAX_FILES 256

static int list_dir(const char *path, char names[][256], int max){
    DIR *d = opendir(path);
    if (!d) {perror("opendir"); return -1;}
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (strcmp(e -> d_name, ".") == 0 || strcmp (e -> d_name, "..") == 0) 
            continue;
        strncpy(names[n], e -> d_name, 256);
        names[n][255] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

static int contains(char names[][256], int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) return 1;
    return 0;
}
 
int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <src_dir> <dst_dir>\n", argv[0]);
        return 1;
    }
    static char src[MAX_FILES][256], dst[MAX_FILES][256];
    int ns = list_dir(argv[1], src, MAX_FILES);
    int nd = list_dir(argv[2], dst, MAX_FILES);
    if (ns < 0 || nd < 0) return 2;
 
    int pending = 0;
    for (int i = 0; i < ns; i++) {
        if (!contains(dst, nd, src[i])) {
            printf("NEEDS SYNC: %s\n", src[i]);
            pending++;
        }
    }
    printf("%d file(s) require synchronization\n", pending);
    return 0;
}