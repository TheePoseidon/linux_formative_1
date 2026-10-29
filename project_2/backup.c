# define _POSIX_C_SOURCE 200809L
# include <errno.h>
# include <stdarg.h>
# include <fcntl.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/stat.h>
# include <sys/types.h>
# include <sys/wait.h>
# include <time.h>
# include <unistd.h>

#define BUF_SIZE 4096

enum { CH_OK = 0, CH_OPEN_SRC = 10, CH_OPEN_DST = 11, CH_READ = 12,
       CH_WRITE = 13, CH_CLOSE = 14, CH_NOMEM = 15 };

static int  log_fd = -1;
static int  log_seq = 0;
static char log_role = 'P';

static void log_msg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void log_msg(const char *fmt, ...)
{
    if (log_fd < 0) return;
    char body[256], line[320];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof body, fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof line, "%c#%03d [%ld] pid=%d %s\n",
                     log_role, ++log_seq, (long)time(NULL), (int)getpid(), body);
    if (n > 0 && write(log_fd, line, (size_t)n) < 0)
        perror("log write");
}

static int write_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        ssize_t w = write(fd, buf, len);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        buf += w; len -= (size_t)w;
    }
    return 0;
}

static int child_copy(const char *src, const char *dst)
{
    int rc = CH_OK, in = -1, out = -1;
    char *buf = malloc(BUF_SIZE);
    if (!buf) return CH_NOMEM;

    in = open(src, O_RDONLY);
    if (in < 0) {
        log_msg("child: open(%s) failed: %s", src, strerror(errno));
        fprintf(stderr, "child: cannot open source '%s': %s\n", src, strerror(errno));
        rc = CH_OPEN_SRC; goto done;
    }
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        log_msg("child: open(%s) failed: %s", dst, strerror(errno));
        fprintf(stderr, "child: cannot create destination '%s': %s\n", dst, strerror(errno));
        rc = CH_OPEN_DST; goto done;
    }
    for (;;) {
        ssize_t r = read(in, buf, BUF_SIZE);
        if (r < 0) {
            if (errno == EINTR) continue;
            log_msg("child: read failed: %s", strerror(errno));
            rc = CH_READ; break;
        }
        if (r == 0) break;
        if (write_all(out, buf, (size_t)r) < 0) {
            log_msg("child: write failed: %s", strerror(errno));
            rc = CH_WRITE; break;
        }
    }
done:
    if (out >= 0 && close(out) < 0 && rc == CH_OK) {
        log_msg("child: close(dst) failed: %s", strerror(errno));
        rc = CH_CLOSE;
    }
    if (in >= 0) close(in);
    if (rc != CH_OK && rc != CH_OPEN_SRC && rc != CH_OPEN_DST)
        unlink(dst);
    free(buf);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <source_file> <backup_dir>\n", argv[0]);
        return 2;
    }
    const char *src = argv[1], *dir = argv[2];
    char logpath[1024], dstpath[1024];
    struct stat sst, dst_st;

    if (stat(src, &sst) < 0) {
        printf("[FAIL] source check: stat(%s): %s\n", src, strerror(errno));
        return 1;
    }
    if (!S_ISREG(sst.st_mode)) {
        printf("[FAIL] source check: %s is not a regular file\n", src);
        return 1;
    }
    printf("[ OK ] source check: %s (%lld bytes)\n", src, (long long)sst.st_size);

    if (mkdir(dir, 0755) < 0) {
        if (errno == EEXIST) {
            printf("[ OK ] backup directory already exists: %s\n", dir);
        } else {
            printf("[FAIL] mkdir(%s): %s\n", dir, strerror(errno));
            return 1;
        }
    } else {
        printf("[ OK ] backup directory created: %s\n", dir);
    }

    snprintf(logpath, sizeof logpath, "%s/backup.log", dir);
    log_fd = open(logpath, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (log_fd < 0) {
        printf("[FAIL] open log %s: %s\n", logpath, strerror(errno));
        return 1;
    }
    printf("[ OK ] log file opened: %s\n", logpath);
    log_msg("backup started: src=%s dir=%s", src, dir);

    const char *base = strrchr(src, '/');
    base = base ? base + 1 : src;
    snprintf(dstpath, sizeof dstpath, "%s/%s", dir, base);

    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        printf("[FAIL] fork: %s\n", strerror(errno));
        log_msg("fork failed: %s", strerror(errno));
        close(log_fd);
        return 1;
    }
    if (pid == 0) {
        log_role = 'C'; log_seq = 0;
        int rc = child_copy(src, dstpath);
        if (rc == CH_OK) log_msg("child: copy finished");
        close(log_fd);
        _exit(rc);
    }

    int status = 0, result = 1;
    pid_t w;
    do { w = waitpid(pid, &status, 0); } while (w < 0 && errno == EINTR);
    if (w < 0) {
        printf("[FAIL] waitpid: %s\n", strerror(errno));
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == CH_OK) {

        if (stat(dstpath, &dst_st) == 0 && dst_st.st_size == sst.st_size) {
            printf("[ OK ] copy completed and verified: %s (%lld bytes)\n",
                   dstpath, (long long)dst_st.st_size);
            log_msg("backup OK: %s (%lld bytes)", dstpath, (long long)dst_st.st_size);
            result = 0;
        } else {
            printf("[FAIL] verification: destination missing or size mismatch\n");
            log_msg("verification failed");
        }
    } else if (WIFEXITED(status)) {
        printf("[FAIL] child exited with status %d\n", WEXITSTATUS(status));
        log_msg("backup FAILED: child status %d", WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        printf("[FAIL] child killed by signal %d\n", WTERMSIG(status));
        log_msg("backup FAILED: child killed by signal %d", WTERMSIG(status));
    }
    if (close(log_fd) < 0) perror("close log");
    return result;
}