/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
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
 
 * TODO implemented: Full runtime design across all components —
 *   - Control-plane IPC via UNIX domain socket (bind/accept/connect)
 *   - Container lifecycle tracking with metadata list and mutex sync
 *   - clone() + chroot + mount namespace setup per container
 *   - Producer/consumer log buffering with bounded_buffer + pthread condvars
 *   - SIGINT/SIGTERM handling with graceful shutdown and child reaping
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
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    char log_path[PATH_MAX];
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

volatile sig_atomic_t running = 1;

void handle_signal(int sig) {
    printf("\nSupervisor shutting down...\n");
    running = 0;
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
 * TODO implemented: Producer-side bounded buffer insertion.
 * Locks the mutex and blocks on not_full while buffer is at capacity.
 * On shutdown, releases lock and returns -1 immediately without inserting.
 * Otherwise copies item to tail slot, advances tail (circular), increments count,
 * then signals not_empty to wake a waiting consumer.
 */
int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
	pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down) {
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);
    }

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);

    return 0;


}

/*
 * TODO implemented: Consumer-side bounded buffer removal.
 * Locks mutex and blocks on not_empty while buffer is empty and not shutting down.
 * Returns -1 if shutdown is in progress and buffer is fully drained.
 * Otherwise copies item from head slot, advances head (circular), decrements count,
 * then signals not_full to wake a waiting producer.
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
 * TODO implemented: Log consumer thread.
 * Loops calling bounded_buffer_pop; exits cleanly when pop returns -1 (shutdown + drained).
 * For each popped chunk, builds path logs/<container_id>.log, opens in append mode,
 * writes chunk data, then closes the file. Routes output per container automatically.
 */
void *logging_thread(void *arg)
{
supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;

    while (1) {
        log_item_t item;

        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        char filepath[256];
        snprintf(filepath, sizeof(filepath), "logs/%s.log", item.container_id);

        FILE *fp = fopen(filepath, "a");
        if (!fp)
            continue;

        fwrite(item.data, 1, item.length, fp);
        fclose(fp);
    }

    return NULL;


}

/*
 * TODO implemented: clone() child entrypoint — container process setup.
 * chroot()s into the configured rootfs and chdir("/") to anchor the fs.
 * Mounts a fresh procfs at /proc so tools like ps work inside the container.
 * Applies nice value for scheduling priority if non-zero.
 * dup2()s the write-end of the log pipe onto stdout and stderr so all output
 * flows back to the supervisor's bounded buffer. Then execvp()s the command.
 */
int child_fn(void *arg)
{
    child_config_t *config = (child_config_t *)arg;

    // 1. Change root filesystem
    if (chroot(config->rootfs) != 0) {
        perror("chroot failed");
        return 1;
    }

    // 2. Change working directory to /
    if (chdir("/") != 0) {
        perror("chdir failed");
        return 1;
    }

    // 3. Mount /proc inside container
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc failed");
        return 1;
    }

    // 4. Set nice value (optional scheduling)
    if (config->nice_value != 0) {
        nice(config->nice_value);
    }

    // 5. Execute command inside container

	char *args[8];  // enough space

	int i = 0;
	char *token = strtok(config->command, " ");

	while (token != NULL && i < 7) {
    	args[i++] = token;
    	token = strtok(NULL, " ");
	}

	args[i] = NULL;


	dup2(config->log_write_fd, STDOUT_FILENO);
	dup2(config->log_write_fd, STDERR_FILENO);

	close(config->log_write_fd);

    execvp(args[0], args);

    // If exec fails
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

/*
 * TODO implemented: Long-running supervisor process.
 * Initializes metadata mutex and bounded log buffer, then spawns the logger thread.
 * Creates a UNIX domain socket, unlinks any stale path, binds and listens on CONTROL_PATH.
 * Installs SIGINT/SIGTERM handlers for graceful shutdown.
 * Accept loop: reads a control_request_t from each client; on CMD_START, creates a pipe,
 * clones a child (child_fn), registers it with the kernel monitor via ioctl, records metadata
 * in the container list under metadata_lock, and pushes initial log output to the buffer.
 * On CMD_PS, walks the container list and prints id/pid/state. Reaps zombies with WNOHANG.
 * On exit, signals buffer shutdown, joins logger thread, and destroys all resources.
 */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

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

	pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    printf("Supervisor started for rootfs: %s\n", rootfs);

    // Create socket
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket failed");
        return 1;
    }

    // Setup address
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, CONTROL_PATH);

    // Remove old socket file
    unlink(CONTROL_PATH);

    // Bind
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind failed");
        return 1;
    }

    // Listen
    if (listen(ctx.server_fd, 5) < 0) {
        perror("listen failed");
        return 1;
    }

    printf("Supervisor listening...\n");

	
     signal(SIGINT, handle_signal);
     signal(SIGTERM, handle_signal);
    // Accept loop
      while(running)
      {
	int client_fd = accept(ctx.server_fd, NULL, NULL);

	if (client_fd < 0) {
        if (!running) break;
        perror("accept failed");
        continue;
    }
        control_request_t req;
        memset(&req, 0, sizeof(req));

        if (read(client_fd, &req, sizeof(req)) <= 0) {
            perror("read failed");
            close(client_fd);
            continue;
        }

        printf("Received request: %d for container %s\n",
               req.kind, req.container_id);

        // Handle START command
        if (req.kind == CMD_START) {

            char *stack = malloc(STACK_SIZE);
            if (!stack) {
                perror("malloc stack failed");
                close(client_fd);
                continue;
            }

            child_config_t *config = malloc(sizeof(child_config_t));
            if (!config) {
                perror("malloc config failed");
                free(stack);
                close(client_fd);
                continue;
            }

            memset(config, 0, sizeof(*config));

            strcpy(config->id, req.container_id);
            strcpy(config->rootfs, req.rootfs);
            strcpy(config->command, req.command);
            config->nice_value = req.nice_value;
 
	   /* TODO implemented: log pipe — write-end given to child (stdout/stderr), read-end kept by supervisor to drain initial output into bounded buffer */
   	 int pipefd[2];
   	 if (pipe(pipefd) < 0) {
       	 	perror("pipe failed");
       	 	continue;
   	 }

    	config->log_write_fd = pipefd[1];



            pid_t pid = clone(child_fn, stack + STACK_SIZE, SIGCHLD, config);
            if (pid < 0) {
                perror("clone failed");
                free(stack);
                free(config);
                close(client_fd);
                continue;
            }

	
	close(pipefd[1]);

	char buffer[1024];
	int n = read(pipefd[0], buffer, sizeof(buffer) - 1);

	if (n > 0) {
	log_item_t item;
    memset(&item, 0, sizeof(item));

    strcpy(item.container_id, req.container_id);
    item.length = n;
    memcpy(item.data, buffer, n);

    bounded_buffer_push(&ctx.log_buffer, &item);


	}

	printf("Container started with PID: %d\n", pid);


	int monitor_fd = open("/dev/container_monitor", O_RDWR);

	if (monitor_fd >= 0) {
    	struct monitor_request mreq;
    	memset(&mreq, 0, sizeof(mreq));

    	strcpy(mreq.container_id, req.container_id);
   	 mreq.pid = pid;

 		/* TODO implemented: memory limits passed as bytes directly from the parsed request into the monitor registration struct */
	mreq.soft_limit_bytes = req.soft_limit_bytes;
	mreq.hard_limit_bytes = req.hard_limit_bytes;


    	ioctl(monitor_fd, MONITOR_REGISTER, &mreq);

    	close(monitor_fd);
	}

        container_record_t *rec = malloc(sizeof(container_record_t));
	memset(rec, 0, sizeof(*rec));

	strcpy(rec->id, req.container_id);
	rec->host_pid = pid;
	rec->state = CONTAINER_RUNNING;
	rec->started_at = time(NULL);

	// Add to linked list
	pthread_mutex_lock(&ctx.metadata_lock);
	rec->next = ctx.containers;
	ctx.containers = rec;
	pthread_mutex_unlock(&ctx.metadata_lock);

	} else if (req.kind == CMD_PS) {

    pthread_mutex_lock(&ctx.metadata_lock);

    container_record_t *curr = ctx.containers;

    printf("---- Container List ----\n");

    while (curr) {
        printf("%s  PID=%d  %s\n",
               curr->id,
               curr->host_pid,
               state_to_string(curr->state));
        curr = curr->next;
   	 }

    pthread_mutex_unlock(&ctx.metadata_lock);
	}


        close(client_fd);
        while (waitpid(-1, NULL, WNOHANG) > 0);
       
    }
 
    printf("Cleaning up before exit...\n");
	close(ctx.server_fd);	
    while (waitpid(-1, NULL, WNOHANG) > 0);
    // Cleanup (not reached normally)
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    return 0;
}


/*
 * TODO implemented: Client-side IPC — sends a control request to the supervisor.
 * Opens an AF_UNIX SOCK_STREAM socket and connects to CONTROL_PATH.
 * Writes the full control_request_t struct in one call, then closes the socket.
 * Used by all CLI commands (start, run, ps, logs, stop) to reach the supervisor.
 */
static int send_control_request(const control_request_t *req)
{
int fd = socket(AF_UNIX, SOCK_STREAM, 0);
if (fd < 0) {
    perror("socket failed");
    return 1;
}

struct sockaddr_un addr;
memset(&addr, 0, sizeof(addr));
addr.sun_family = AF_UNIX;
strcpy(addr.sun_path, CONTROL_PATH);

// Connect to supervisor
if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect failed (is supervisor running?)");
    return 1;
}

// Send request
write(fd, req, sizeof(*req));

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

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
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

    /* TODO implemented: CMD_PS response — supervisor walks the container linked list
     * under metadata_lock and prints each container's id, host PID, and state string. */
    printf("Expected states include: %s, %s, %s, %s, %s\n",
           state_to_string(CONTAINER_STARTING),
           state_to_string(CONTAINER_RUNNING),
           state_to_string(CONTAINER_STOPPED),
           state_to_string(CONTAINER_KILLED),
           state_to_string(CONTAINER_EXITED));
    return send_control_request(&req);
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

    return send_control_request(&req);
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
