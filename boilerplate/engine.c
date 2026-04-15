/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Intentionally partial starter:
 *   - command-line shape is defined
 *   - key runtime data structures are defined
 *   - bounded-buffer skeleton is defined
 *   - supervisor / client split is outlined
 *
 * Students are expected to design:
 *   - the control-plane IPC implementation
 *   - container lifecycle and metadata synchronization
 *   - clone + namespace setup for each container
 *   - producer/consumer behavior for log buffering
 *   - signal handling and graceful shutdown
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED,
    CONTAINER_HARD_LIMIT_KILLED
} container_state_t;

typedef struct container_record {
    char rootfs[PATH_MAX];
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
    int stop_requested;
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Forward declarations */
static int send_control_request(const control_request_t *req);

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index);
static void handle_start(supervisor_ctx_t *ctx, int client_fd, const control_request_t *req);
static void handle_run  (supervisor_ctx_t *ctx, int client_fd, const control_request_t *req);
static void handle_ps   (supervisor_ctx_t *ctx, int client_fd);
static void handle_logs (supervisor_ctx_t *ctx, int client_fd, const control_request_t *req);
static void handle_stop (supervisor_ctx_t *ctx, int client_fd, const control_request_t *req);
volatile sig_atomic_t shutdown_requested = 0;

/* Add these globals at top of file, near shutdown_requested */
static volatile sig_atomic_t run_interrupted = 0;
static char run_container_id[CONTAINER_ID_LEN];

static void sigchld_handler(int sig) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}
static void run_signal_handler(int sig) {
    run_interrupted = 1;
}

static int send_stop_for_run(const char *id) {
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, id, CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) { /* ... */ return 1; }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(run_container_id, argv[2], sizeof(run_container_id) - 1); /* save for signal */
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;

    /* Install signal handlers BEFORE connecting */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = run_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return 1;
    }
    write(fd, &req, sizeof(req));

    control_response_t resp;
    ssize_t n;
    while ((n = read(fd, &resp, sizeof(resp))) > 0) {
        if (run_interrupted) {
            /* Forward stop to supervisor, then keep waiting for final status */
            send_stop_for_run(run_container_id);
            run_interrupted = 0;
        }
        if (resp.status < 0)
            fprintf(stderr, "Error: %s\n", resp.message);
        else
            printf("%s\n", resp.message);
    }
    /* Check once more after read loop ends */
    if (run_interrupted)
        send_stop_for_run(run_container_id);

    close(fd);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    case CONTAINER_HARD_LIMIT_KILLED:
        return "hard_limit_killed";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

/*
 * TODO:
 * Implement producer-side insertion into the bounded buffer.
 *
 * Requirements:
 *   - block or fail according to your chosen policy when the buffer is full
 *   - wake consumers correctly
 *   - stop cleanly if shutdown begins
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down && buffer->count == LOG_BUFFER_CAPACITY) {
    pthread_mutex_unlock(&buffer->mutex);
    return -1;  // only drop if TRULY full AND shutting down
}

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}

/*
 * TODO:
 * Implement consumer-side removal from the bounded buffer.
 *
 * Requirements:
 *   - wait correctly while the buffer is empty
 *   - return a useful status when shutdown is in progress
 *   - avoid races with producers and shutdown
 */
int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);
    }

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;
}


/*
 * TODO:
 * Implement the logging consumer thread.
 *
 * Suggested responsibilities:
 *   - remove log chunks from the bounded buffer
 *   - route each chunk to the correct per-container log file
 *   - exit cleanly when shutdown begins and pending work is drained
 */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);

        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ssize_t written = 0;
while (written < item.length) {
    ssize_t w = write(fd, item.data + written, item.length - written);
    if (w <= 0) break;
    written += w;
}
            close(fd);
        }
    }

    return NULL;
}

/*
 * TODO:
 * Implement the clone child entrypoint.
 *
 * Required outcomes:
 *   - isolated PID / UTS / mount context
 *   - chroot or pivot_root into rootfs
 *   - working /proc inside container
 *   - stdout / stderr redirected to the supervisor logging path
 *   - configured command executed inside the container
 */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    
    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) != 0) {
        perror("mount MS_PRIVATE");
    }

    // hostname
    if (sethostname(cfg->id, strlen(cfg->id)) != 0)
        perror("sethostname");

    // chroot
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    chdir("/");

   
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        return 1;
    }

    // redirect output
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    execl("/bin/sh", "sh", "-c", cfg->command, NULL);

    perror("exec failed");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Per-container pipe reader — drains stdout/stderr into log buffer   */
/* ------------------------------------------------------------------ */

typedef struct {
    supervisor_ctx_t *ctx;
    int               read_fd;
    char              container_id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    int               client_fd;   /* >= 0 only for CMD_RUN, else -1 */
} pipe_reader_arg_t;

static void *pipe_reader_thread(void *arg)
{
    pipe_reader_arg_t *pra = (pipe_reader_arg_t *)arg;
    supervisor_ctx_t  *ctx = pra->ctx;
    log_item_t         item;
    ssize_t            n;

    memset(item.container_id, 0, sizeof(item.container_id));
    strncpy(item.container_id, pra->container_id, CONTAINER_ID_LEN - 1);

    /* Drain the pipe until the container closes its end (EOF) */
    while ((n = read(pra->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(&ctx->log_buffer, &item);
    }
    close(pra->read_fd);

    /* Reap the child so it does not become a zombie */
    int   wstatus  = 0;
    pid_t reaped   = waitpid(pra->host_pid, &wstatus, 0);
    int   exit_code    = 0;
    int   exit_signal  = 0;

   if (reaped > 0) {
        if (WIFEXITED(wstatus))
            exit_code = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus)) {
            exit_signal = WTERMSIG(wstatus);
            exit_code   = 128 + exit_signal;
        }
    }
  
    /* Update metadata with termination classification */
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = ctx->containers;
    while (rec) {
        if (strncmp(rec->id, pra->container_id, CONTAINER_ID_LEN) == 0) {
            rec->exit_code   = exit_code;
            rec->exit_signal = exit_signal;

            if (rec->stop_requested) {
                /* Supervisor explicitly stopped this container */
                rec->state = CONTAINER_STOPPED;
                /* SIGKILL without stop_requested ⇒ kernel hard limit kill */
            } else if (exit_signal == SIGKILL) {
                /* Killed by external actor — attribute to hard limit */
                rec->state = CONTAINER_HARD_LIMIT_KILLED;
            } else {
                rec->state = CONTAINER_EXITED;
            }
            break;
        }
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);;

    /* Unregister from kernel monitor if open */
    if (ctx->monitor_fd >= 0)
        unregister_from_monitor(ctx->monitor_fd, pra->container_id, pra->host_pid);

    /*
     * If this was a CMD_RUN, send the exit status back to the waiting
     * client and close its fd.
     */
    if (pra->client_fd >= 0) {
        control_response_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.status = exit_code;
        snprintf(resp.message, sizeof(resp.message),
                 "exited with code %d", exit_code);
        write(pra->client_fd, &resp, sizeof(resp));
        close(pra->client_fd);
    }

    free(pra);
    return NULL;
}
/*
 * TODO:
 * Implement the long-running supervisor process.
 *
 * Suggested responsibilities:
 *   - create and bind the control-plane IPC endpoint
 *   - initialize shared metadata and the bounded buffer
 *   - start the logging thread
 *   - accept control requests and update container state
 *   - reap children and respond to signals
 */
void handle_shutdown(int sig) {
    shutdown_requested = 1;
}

void *scheduler_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    char current_id[CONTAINER_ID_LEN] = {0};  /* ID of last scheduled container */

    while (!ctx->should_stop) {
        pthread_mutex_lock(&ctx->metadata_lock);

        /* Build a snapshot of currently running PIDs/IDs */
        /* (avoids holding the lock during sleep) */
        typedef struct { pid_t pid; char id[CONTAINER_ID_LEN]; } slot_t;
        slot_t slots[64];
        int count = 0;

        container_record_t *rec = ctx->containers;
        while (rec && count < 64) {
            if (rec->state == CONTAINER_RUNNING && rec->host_pid > 0) {
                slots[count].pid = rec->host_pid;
                strncpy(slots[count].id, rec->id, CONTAINER_ID_LEN - 1);
                count++;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (count == 0) {
            usleep(50000);  /* nothing to schedule, back off */
            continue;
        }

        /* Find the next container after current_id (round-robin cursor) */
        int next = 0;
        if (current_id[0] != '\0') {
            for (int i = 0; i < count; i++) {
                if (strncmp(slots[i].id, current_id, CONTAINER_ID_LEN) == 0) {
                    next = (i + 1) % count;  /* advance past current */
                    break;
                }
            }
        }

        strncpy(current_id, slots[next].id, CONTAINER_ID_LEN - 1);
        pid_t chosen = slots[next].pid;

        /* Give this container its quantum */
        if (kill(chosen, SIGCONT) == 0) {
            usleep(100000);          /* 100ms quantum */
            kill(chosen, SIGSTOP);   /* preempt */
        } else {
            /* PID is gone — clear cursor so we don't get stuck */
            current_id[0] = '\0';
        }
    }

    return NULL;
}
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;
    signal(SIGPIPE, SIG_IGN);

struct sigaction sa;
memset(&sa, 0, sizeof(sa));
sa.sa_handler = handle_shutdown;
sigemptyset(&sa.sa_mask);
sa.sa_flags = 0;

sigaction(SIGINT, &sa, NULL);
sigaction(SIGTERM, &sa, NULL);
    printf("Supervisor starting...\n");
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
if (ctx.monitor_fd < 0) {
    printf("Running WITHOUT kernel monitoring\n");
} else {
    printf("Monitor connected successfully\n");
}
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    unlink(CONTROL_PATH);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
  printf("Socket created\n");
    if (listen(ctx.server_fd, 5) < 0) {
    perror("listen");
    return 1;
}

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0) {
    perror("pthread_create logger");
    return 1;
}
  pthread_t sched_thread;
if (pthread_create(&sched_thread, NULL, scheduler_thread, &ctx) != 0) {
    perror("scheduler thread");
    return 1;
}

  mkdir(LOG_DIR, 0755);   /* ensure logs/ directory exists */
  
    while (!shutdown_requested) {
        int client_fd = accept(ctx.server_fd, NULL, NULL);

if (client_fd < 0) {
    if (errno == EINTR && shutdown_requested)
        break;   
    continue;
}

        control_request_t req;
        memset(&req, 0, sizeof(req));
        if (read(client_fd, &req, sizeof(req)) <= 0) {
            close(client_fd);
            continue;
        }

        switch (req.kind) {
        case CMD_START:
            handle_start(&ctx, client_fd, &req);
            close(client_fd);
            break;
        case CMD_RUN:
            handle_run(&ctx, client_fd, &req);
            /* do NOT close client_fd — thread owns it */
            break;
        case CMD_PS:
            handle_ps(&ctx, client_fd);
            close(client_fd);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, client_fd, &req);
            close(client_fd);
            break;
        case CMD_STOP:
            handle_stop(&ctx, client_fd, &req);
            close(client_fd);
            break;
        default:
            close(client_fd);
            break;
        }
    }
    
    pthread_mutex_lock(&ctx.metadata_lock);

container_record_t *rec = ctx.containers;
while (rec) {
    if (rec->state == CONTAINER_RUNNING) {
        printf("Stopping container %s (pid %d)\n", rec->id, rec->host_pid);
        kill(rec->host_pid, SIGCONT);
        usleep(10000);
        kill(rec->host_pid, SIGTERM);
        sleep(1);
        kill(rec->host_pid, SIGKILL);   // fallback
        rec->stop_requested = 1;
    }
    rec = rec->next;
}

    pthread_mutex_unlock(&ctx.metadata_lock);

/* Give containers time to exit and flush logs */
    sleep(1);
/* Ensure all children are reaped */
    while (waitpid(-1, NULL, WNOHANG) > 0);

    close(ctx.server_fd);
    unlink(CONTROL_PATH);
    printf("Draining logs before shutdown...\n");
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    
    
container_record_t *cur = ctx.containers;
while (cur) {
    container_record_t *next = cur->next;
    free(cur);
    cur = next;
}
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);
    if (ctx.monitor_fd >= 0)
    close(ctx.monitor_fd);
    ctx.should_stop = 1;
pthread_join(sched_thread, NULL);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  CMD_START — launch a container, return immediately                 */
/* ------------------------------------------------------------------ */

/* Add this helper before handle_start */
static int rootfs_already_in_use(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *rec = ctx->containers;
    while (rec) {
        if (rec->state == CONTAINER_RUNNING &&
            strncmp(rec->log_path, rootfs, PATH_MAX) != 0) {
            /* We don't store rootfs in record — add a rootfs field, or do this: */
        }
        rec = rec->next;
    }
    return 0;
}

static void handle_start(supervisor_ctx_t  *ctx,
                         int                client_fd,
                         const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
  pthread_mutex_lock(&ctx->metadata_lock);

container_record_t *tmp = ctx->containers;
while (tmp) {
    if (tmp->state == CONTAINER_RUNNING &&
        strcmp(tmp->rootfs, req->rootfs) == 0) {

        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "rootfs already in use by another container");
        write(client_fd, &resp, sizeof(resp));
        return;
    }
    tmp = tmp->next;
}

pthread_mutex_unlock(&ctx->metadata_lock);
    /* Allocate clone stack */
    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "out of memory");
        write(client_fd, &resp, sizeof(resp));
        
        return;
    }

    /* Pipe: container stdout/stderr → supervisor log pipeline */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        free(stack);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "pipe() failed: %s", strerror(errno));
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Build child config */
    /* Build child config (FIXED) */
child_config_t *cfg = malloc(sizeof(child_config_t));
if (!cfg) {
    perror("malloc cfg");
    close(pipefd[0]);
    close(pipefd[1]);
    
    free(stack);
    return;
}

memset(cfg, 0, sizeof(*cfg));
strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
cfg->nice_value   = req->nice_value;
cfg->log_write_fd = pipefd[1];

    /* Clone into new PID / UTS / mount namespaces */
    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    kill(pid,SIGSTOP);

    /* Parent no longer needs the write end */
    close(pipefd[1]);
    free(stack);
    free(cfg);
    
    
    if (pid < 0) {
        close(pipefd[0]);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "clone() failed: %s", strerror(errno));
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Allocate and populate metadata record */
    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        /* Can't track it — kill the child to avoid orphan */
        kill(pid, SIGKILL);
        close(pipefd[0]);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "out of memory for record");
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(rec->rootfs, req->rootfs, PATH_MAX - 1);
    rec->host_pid          = pid;
    rec->started_at        = time(NULL);
    rec->state             = CONTAINER_RUNNING;
    rec->soft_limit_bytes  = req->soft_limit_bytes;
    rec->hard_limit_bytes  = req->hard_limit_bytes;
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    /* Prepend to linked list (lock around mutation) */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next      = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Register with kernel monitor (best-effort) */
   if (ctx->monitor_fd >= 0) {
    if (register_with_monitor(ctx->monitor_fd,
                              req->container_id,
                              pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes) < 0) {
        perror("monitor register failed");
    }
}

    /* Spawn the pipe-reader thread for this container */
    pipe_reader_arg_t *pra = calloc(1, sizeof(*pra));
    if (!pra) {
        /* Non-fatal: logs won't be captured, but container keeps running */
        close(pipefd[0]);
    } else {
        pra->ctx       = ctx;
        pra->read_fd   = pipefd[0];
        pra->host_pid  = pid;
        pra->client_fd = -1;   /* CMD_START does not block the client */
        strncpy(pra->container_id, req->container_id, CONTAINER_ID_LEN - 1);

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, pipe_reader_thread, pra);
        pthread_attr_destroy(&attr);
    }

    /* Reply to client */
    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message),
             "started container %s (pid %d)", req->container_id, pid);
    write(client_fd, &resp, sizeof(resp));
    shutdown(client_fd, SHUT_WR);
    
  
}

/* ------------------------------------------------------------------ */
/*  CMD_RUN — same as start, but block client until container exits    */
/* ------------------------------------------------------------------ */

static void handle_run(supervisor_ctx_t        *ctx,
                       int                      client_fd,
                       const control_request_t *req)
{

    control_response_t resp;
    memset(&resp, 0, sizeof(resp));
    
    pthread_mutex_lock(&ctx->metadata_lock);

container_record_t *tmp = ctx->containers;
while (tmp) {
    if (tmp->state == CONTAINER_RUNNING &&
        strcmp(tmp->rootfs, req->rootfs) == 0) {

        pthread_mutex_unlock(&ctx->metadata_lock);

        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "rootfs already in use by another container");
        write(client_fd, &resp, sizeof(resp));
        return;
    }
    tmp = tmp->next;
}

pthread_mutex_unlock(&ctx->metadata_lock);

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "out of memory");
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free(stack);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "pipe() failed: %s", strerror(errno));
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Build child config (FIXED) */
child_config_t *cfg = malloc(sizeof(child_config_t));
if (!cfg) {
    perror("malloc cfg");
    close(pipefd[0]);
    close(pipefd[1]);
    free(stack);

    return;
}

memset(cfg, 0, sizeof(*cfg));
strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
cfg->nice_value   = req->nice_value;
cfg->log_write_fd = pipefd[1];

    pid_t pid = clone(child_fn,
                      stack + STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      cfg);
    kill(pid,SIGSTOP);
    close(pipefd[1]);
    free(stack);
    free(cfg);
    if (pid < 0) {
        close(pipefd[0]);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "clone() failed: %s", strerror(errno));
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        kill(pid, SIGKILL);
        close(pipefd[0]);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message), "out of memory for record");
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(rec->rootfs, req->rootfs, PATH_MAX - 1);
    rec->host_pid         = pid;
    rec->started_at       = time(NULL);
    rec->state            = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, sizeof(rec->log_path),
             "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next       = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0) {
    if (register_with_monitor(ctx->monitor_fd,
                              req->container_id,
                              pid,
                              req->soft_limit_bytes,
                              req->hard_limit_bytes) < 0) {
        perror("monitor register failed");
    }
}

    /*
     * Pass client_fd to the reader thread.  The thread writes the final
     * exit-status response and closes client_fd when the container exits.
     * Do NOT close client_fd here.
     */
    pipe_reader_arg_t *pra = calloc(1, sizeof(*pra));
    if (!pra) {
        close(pipefd[0]);
        close(client_fd);
        return;
    }

    pra->ctx       = ctx;
    pra->read_fd   = pipefd[0];
    pra->host_pid  = pid;
    pra->client_fd = client_fd;   /* thread owns this fd now */
    strncpy(pra->container_id, req->container_id, CONTAINER_ID_LEN - 1);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, pipe_reader_thread, pra);
    pthread_attr_destroy(&attr);

    /*
     * Return WITHOUT writing a response — the thread does that when the
     * container exits.  Also return WITHOUT closing client_fd.
     */
}
/* ------------------------------------------------------------------ */
/*  CMD_PS — list all known containers                                 */
/* ------------------------------------------------------------------ */

static void handle_ps(supervisor_ctx_t *ctx, int client_fd)
{
    control_response_t resp;

    /* Header */
    memset(&resp, 0, sizeof(resp));
    resp.status = 0;
    snprintf(resp.message, sizeof(resp.message),
             "%-32s  %-8s  %-18s  %-8s  %-8s  %s",
             "ID", "PID", "STATE", "SOFT_MiB", "HARD_MiB", "EXIT_INFO");
    write(client_fd, &resp, sizeof(resp));

    pthread_mutex_lock(&ctx->metadata_lock);

    container_record_t *rec = ctx->containers;

    if (!rec) {
        memset(&resp, 0, sizeof(resp));
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message), "(no containers)");
        write(client_fd, &resp, sizeof(resp));
    }

    while (rec) {
        memset(&resp, 0, sizeof(resp));
        resp.status = 0;
        snprintf(resp.message, sizeof(resp.message),
                 "%-32s  %-8d  %-18s  %-8lu  %-8lu  exit=%d sig=%d",
                 rec->id,
                 rec->host_pid,
                 state_to_string(rec->state),
                 rec->soft_limit_bytes >> 20,
                 rec->hard_limit_bytes >> 20,
                 rec->exit_code,
                 rec->exit_signal);
        write(client_fd, &resp, sizeof(resp));
        rec = rec->next;
    }

    pthread_mutex_unlock(&ctx->metadata_lock);
}
/* ------------------------------------------------------------------ */
/*  CMD_LOGS — stream the log file back to the client                  */
/* ------------------------------------------------------------------ */

static void handle_logs(supervisor_ctx_t        *ctx,
                        int                      client_fd,
                        const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, req->container_id);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "no log file for '%s': %s", req->container_id, strerror(errno));
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    /* Send log content in message-sized chunks */
    ssize_t n;
    while ((n = read(fd, resp.message, sizeof(resp.message) - 1)) > 0) {
        resp.message[n] = '\0';
        resp.status = 0;
        write(client_fd, &resp, sizeof(resp));
        memset(&resp, 0, sizeof(resp));
    }

    close(fd);
}

/* ------------------------------------------------------------------ */
/*  CMD_STOP — send SIGTERM to a running container                     */
/* ------------------------------------------------------------------ */

static void handle_stop(supervisor_ctx_t        *ctx,
                        int                      client_fd,
                        const control_request_t *req)
{
    control_response_t resp;
    memset(&resp, 0, sizeof(resp));

    pthread_mutex_lock(&ctx->metadata_lock);

    container_record_t *rec = ctx->containers;
    while (rec) {
        if (strncmp(rec->id, req->container_id, CONTAINER_ID_LEN) == 0)
            break;
        rec = rec->next;
    }

    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "container '%s' not found", req->container_id);
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    if (rec->state != CONTAINER_RUNNING) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = -1;
        snprintf(resp.message, sizeof(resp.message),
                 "container '%s' is not running (state: %s)",
                 req->container_id, state_to_string(rec->state));
        write(client_fd, &resp, sizeof(resp));
        return;
    }

    pid_t target = rec->host_pid;
    rec->stop_requested = 1;
    rec->state   = CONTAINER_STOPPED;   /* optimistic update */

    pthread_mutex_unlock(&ctx->metadata_lock);
    kill(target, SIGCONT);
usleep(10000);
    if (kill(target, SIGTERM) < 0) {
    resp.status = -1;
    snprintf(resp.message, sizeof(resp.message),
             "kill(%d, SIGTERM) failed: %s", target, strerror(errno));
} else {
    sleep(1);  // give it time to exit

    if (kill(target, 0) == 0) {  // still alive
        kill(target, SIGKILL);
        snprintf(resp.message, sizeof(resp.message),
                 "forced kill (SIGKILL) container '%s' (pid %d)",
                 req->container_id, target);
    } else {
        snprintf(resp.message, sizeof(resp.message),
                 "gracefully stopped container '%s' (pid %d)",
                 req->container_id, target);
    }

    resp.status = 0;
}

    write(client_fd, &resp, sizeof(resp));
    shutdown(client_fd, SHUT_WR);
}

  

/*
 * TODO:
 * Implement the client-side control request path.
 *
 * The CLI commands should use a second IPC mechanism distinct from the
 * logging pipe. A UNIX domain socket is the most direct option, but a
 * FIFO or shared memory design is also acceptable if justified.
 */
static int send_control_request(const control_request_t *req)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    write(fd, req, sizeof(*req));


control_response_t resp;
ssize_t n;

while ((n = read(fd, &resp, sizeof(resp))) > 0) {
    if (resp.status < 0)
        fprintf(stderr, "Error: %s\n", resp.message);
    else
        printf("%s\n", resp.message);
}

close(fd);
return 0;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}


static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    write(fd, &req, sizeof(req));

    /* Read all responses until supervisor closes the connection */
    control_response_t resp;
    ssize_t n;
    while ((n = read(fd, &resp, sizeof(resp))) > 0) {
        if (resp.status < 0)
            fprintf(stderr, "Error: %s\n", resp.message);
        else
            printf("%s\n", resp.message);
    }

    close(fd);
    return 0;
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(fd); return 1;
    }

    write(fd, &req, sizeof(req));

    control_response_t resp;
    while (read(fd, &resp, sizeof(resp)) > 0) {
        if (resp.status < 0) {
            fprintf(stderr, "Error: %s\n", resp.message);
            break;
        }
        printf("%s", resp.message);
    }

    close(fd);
    return 0;
}


static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
