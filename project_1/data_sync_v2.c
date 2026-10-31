# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <dirent.h>
# include <sys/stat.h>
# include <time.h>
 
#define MAX_FILES 256
#define LOG_PATH  "sync_v2.log"
 
static void log_msg(const char *msg)
{
    FILE *f = fopen(LOG_PATH, "a");
    if (!f) return;
    time_t now = time(NULL);
    fprintf(f, "[%ld] %s\n", (long)now, msg);
    fclose(f);
}

static unsigned long checksum_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned long sum = 0;
    int c;
    while ((c = fgetc(f)) != EOF)
        sum = ((sum << 5) | (sum >> 27)) ^ (unsigned long)c;
    fclose(f);
    return sum;
}
 
static int list_dir(const char *path, char names[][256], int max) {
    DIR *d = opendir(path);
    if (!d) { perror("opendir"); log_msg("opendir failed"); return -1; }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && n < max) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        strncpy(names[n], e->d_name, 255);
        names[n][255] = '\0';
        n++;
    }
    closedir(d);
    return n;
}
 
static int index_of(char names[][256], int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (strcmp(names[i], name) == 0) return i;
    return -1;
}
 
int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <src_dir> <dst_dir>\n", argv[0]);
        return 1;
    }
    static char src[MAX_FILES][256], dst[MAX_FILES][256];
    log_msg("scan started");
    int ns = list_dir(argv[1], src, MAX_FILES);
    int nd = list_dir(argv[2], dst, MAX_FILES);
    if (ns < 0 || nd < 0) return 2;
 
    int pending = 0;
    char p1[1024], p2[1024];
    struct stat s1, s2;
    for (int i = 0; i < ns; i++) {
        snprintf(p1, sizeof p1, "%s/%s", argv[1], src[i]);
        int j = index_of(dst, nd, src[i]);
        if (j < 0) {
            printf("MISSING: %s\n", src[i]);
            pending++;
            continue;
        }
        snprintf(p2, sizeof p2, "%s/%s", argv[2], dst[j]);
        if (stat(p1, &s1) != 0 || stat(p2, &s2) != 0) continue;
        if (s1.st_size != s2.st_size || s1.st_mtime > s2.st_mtime ||
            checksum_file(p1) != checksum_file(p2)) {
            printf("CHANGED: %s (size %lld vs %lld)\n", src[i],
                   (long long)s1.st_size, (long long)s2.st_size);
            pending++;
        }
    }
    printf("%d file(s) require synchronization\n", pending);
    log_msg("scan finished");
    return 0;
}