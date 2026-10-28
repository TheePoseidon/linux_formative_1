# define _POSIX_C_SOURCE 200809L
# include <errno.h>
# include <signal.h>
# include <stdarg.h>
# include <stdio.h>
# include <stdlib.h>
# include <string.h>
# include <sys/select.h>
# include <time.h>
# include <unistd.h>

#define TICK_SECONDS   5
#define LOG_FILE       "server_controller.log"
#define EXIT_EMERGENCY 143

typedef enum { STATE_NORMAL, STATE_MAINTENANCE } service_state_t;
static volatile sig_atomic_t flag_status   = 0;
static volatile sig_atomic_t flag_toggle   = 0;
static volatile sig_atomic_t flag_shutdown = 0;
static volatile sig_atomic_t flag_terminate = 0;

static void on_sigusr1(int sig) { (void)sig; flag_status    = 1; }
static void on_sigusr2(int sig) { (void)sig; flag_toggle    = 1; }
static void on_sigint (int sig) { (void)sig; flag_shutdown  = 1; }
static void on_sigterm(int sig) { (void)sig; flag_terminate = 1; }

static service_state_t state = STATE_NORMAL;
static FILE *logfp = NULL;
static time_t started;
static unsigned ticks = 0, status_reports = 0, mode_changes = 0;

static const char *state_name(service_state_t s)
{
    return s == STATE_NORMAL ? "NORMAL" : "MAINTENANCE";
}

static void say(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);

    printf("[Server Controller] %s\n", msg);
    fflush(stdout);

    if (logfp) {
        struct timespec ts;
        struct tm tm;
        char when[32];
        clock_gettime(CLOCK_REALTIME, &ts);
        localtime_r(&ts.tv_sec, &tm);
        strftime(when, sizeof when, "%H:%M:%S", &tm);
        fprintf(logfp, "%s.%03ld pid=%d state=%s %s\n", when, ts.tv_nsec / 1000000L,
                (int)getpid(), state_name(state), msg);
        fflush(logfp);
    }
}

static void install_handler(int sig, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(sig, &sa, NULL) < 0) {
        perror("sigaction");
        exit(EXIT_FAILURE);
    }
}

static void timespec_add_sec(struct timespec *t, time_t sec)
{
    t->tv_sec += sec;
}

static struct timespec time_left(const struct timespec *deadline)
{
    struct timespec now, left;
    clock_gettime(CLOCK_MONOTONIC, &now);
    left.tv_sec  = deadline->tv_sec  - now.tv_sec;
    left.tv_nsec = deadline->tv_nsec - now.tv_nsec;
    if (left.tv_nsec < 0) { left.tv_sec--; left.tv_nsec += 1000000000L; }
    if (left.tv_sec < 0)  { left.tv_sec = 0; left.tv_nsec = 0; }
    return left;
}

int main(void)
{
    install_handler(SIGUSR1, on_sigusr1);
    install_handler(SIGUSR2, on_sigusr2);
    install_handler(SIGINT,  on_sigint);
    install_handler(SIGTERM, on_sigterm);
    sigset_t blocked, wait_mask;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGUSR1);
    sigaddset(&blocked, SIGUSR2);
    sigaddset(&blocked, SIGINT);
    sigaddset(&blocked, SIGTERM);
    if (sigprocmask(SIG_BLOCK, &blocked, &wait_mask) < 0) {
        perror("sigprocmask");
        return EXIT_FAILURE;
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
    logfp = fopen(LOG_FILE, "a");
    if (!logfp)
        perror("fopen log (continuing without a log file)");
    started = time(NULL);

    say("Started. PID=%d, state=%s, monitoring every %d seconds.",
        (int)getpid(), state_name(state), TICK_SECONDS);

    struct timespec next_tick;
    clock_gettime(CLOCK_MONOTONIC, &next_tick);
    timespec_add_sec(&next_tick, TICK_SECONDS);

    for (;;) {
        if (flag_terminate) {
            say("EMERGENCY TERMINATION requested (SIGTERM). Exiting immediately, cleanup skipped.");
            _exit(EXIT_EMERGENCY);
        }
        if (flag_shutdown) {
            say("Graceful shutdown requested (SIGINT). Stopping monitoring...");
            say("Summary: uptime=%lds, monitoring ticks=%u, status reports=%u, mode changes=%u, final state=%s",
                (long)(time(NULL) - started), ticks, status_reports, mode_changes, state_name(state));
            say("Cleanup complete. Closing log file. Goodbye.");
            if (logfp)
                fclose(logfp);
            return EXIT_SUCCESS;
        }
        if (flag_status) {
            flag_status = 0;
            status_reports++;
            say("STATUS REPORT: state=%s, PID=%d, uptime=%lds",
                state_name(state), (int)getpid(), (long)(time(NULL) - started));
        }
        if (flag_toggle) {
            flag_toggle = 0;
            service_state_t old = state;
            state = (state == STATE_NORMAL) ? STATE_MAINTENANCE : STATE_NORMAL;
            mode_changes++;
            say("Mode change (SIGUSR2): %s -> %s", state_name(old), state_name(state));
        }
        struct timespec left = time_left(&next_tick);
        if (left.tv_sec == 0 && left.tv_nsec == 0) {
            ticks++;
            if (state == STATE_NORMAL)
                say("Service running normally...");
            else
                say("Service in MAINTENANCE mode - non-essential tasks suspended...");
            timespec_add_sec(&next_tick, TICK_SECONDS);
            continue;
        }
        if (pselect(0, NULL, NULL, NULL, &left, &wait_mask) < 0 && errno != EINTR) {
            perror("pselect");
            return EXIT_FAILURE;
        }
    }
}