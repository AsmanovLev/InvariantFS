/* invf-plugin-host.c — Worker pool daemon for InvariantFS plugins (ADR-007).
 *
 * Spawns a pool of sandboxed worker processes. Each worker:
 * - Operates in an isolated link map via dlmopen(LM_ID_NEWLM, ...) or dlopen
 * - Connects to a dedicated SPSC shared-memory slot (64 MiB)
 * - Waits on eventfd for incoming commands
 * - Executes commands (e.g. qcow2 containerpack operations) without fork/exec overhead
 * - Writes response and signals back via eventfd
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <dlfcn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#include "core/invf_plugin_ipc.h"
#include "include/ivpack_api.h"

static volatile sig_atomic_t g_stop = 0;

static void sig_handler(int sig)
{
    (void)sig;
    g_stop = 1;
}

typedef struct worker_plugin {
    char name[INVF_PLUGIN_NAME_MAX];
    char path[INVF_PLUGIN_PATH_MAX];
    void *handle;
    const ivpack_desc *desc;
    ivpack_container_cmd_fn cmd_fn;
    ivpack_container_estimate_fn est_fn;
} worker_plugin;

#define MAX_LOADED_PLUGINS 32

static worker_plugin g_plugins[MAX_LOADED_PLUGINS];
static size_t g_num_plugins = 0;

static worker_plugin *find_or_load_plugin(const char *name, const char *path)
{
    for (size_t i = 0; i < g_num_plugins; i++) {
        if (strcmp(g_plugins[i].name, name) == 0)
            return &g_plugins[i];
    }
    if (g_num_plugins >= MAX_LOADED_PLUGINS)
        return NULL;

    void *h = NULL;
#ifdef LM_ID_NEWLM
    /* Load into isolated link-map namespace */
    h = dlmopen(LM_ID_NEWLM, path, RTLD_NOW | RTLD_LOCAL);
#endif
    if (!h) {
        /* Fallback to standard dlopen */
        h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!h) {
        fprintf(stderr, "invf-plugin-host: dlopen(%s) failed: %s\n", path, dlerror());
        return NULL;
    }

    worker_plugin *p = &g_plugins[g_num_plugins++];
    strncpy(p->name, name, sizeof(p->name) - 1);
    strncpy(p->path, path, sizeof(p->path) - 1);
    p->handle = h;

    ivpack_get_desc_fn desc_fn = (ivpack_get_desc_fn)dlsym(h, "ivpack_get_desc");
    if (desc_fn) p->desc = desc_fn();
    p->cmd_fn = (ivpack_container_cmd_fn)dlsym(h, "ivpack_container_cmd");
    p->est_fn = (ivpack_container_estimate_fn)dlsym(h, "ivpack_container_estimate");

    return p;
}

static void worker_loop(int slot_idx, invf_plugin_slot *slot, int req_efd, int resp_efd)
{
    while (!g_stop) {
        uint64_t val = 0;
        struct pollfd pfd = { .fd = req_efd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 1000);
        if (pr <= 0) {
            if (pr < 0 && errno != EINTR) break;
            continue;
        }

        if (read(req_efd, &val, sizeof(val)) != sizeof(val))
            continue;

        invf_plugin_req *req = &slot->req;
        invf_plugin_resp *resp = &slot->resp;
        memset(resp, 0, sizeof(*resp));
        resp->req_id = req->req_id;

        if (req->msg_type == INVF_MSG_PING) {
            resp->msg_type = INVF_MSG_PONG;
            resp->status = 0;
        } else if (req->msg_type == INVF_MSG_CONTAINER_CMD) {
            worker_plugin *p = find_or_load_plugin(req->pack_name, req->pack_path);
            if (!p || !p->cmd_fn) {
                resp->status = -1;
                resp->msg_type = INVF_MSG_ERROR;
                snprintf(resp->err_msg, sizeof(resp->err_msg), "plugin %s not loaded or lacks cmd_fn", req->pack_name);
            } else {
                size_t out_len = 0;
                ivpack_container_args cargs = {
                    .cmd = req->cmd,
                    .in_path = req->in_path[0] ? req->in_path : NULL,
                    .out_path = req->out_path[0] ? req->out_path : NULL,
                    .recipe_path = req->recipe_path[0] ? req->recipe_path : NULL,
                    .mbr_dir = req->mbr_dir[0] ? req->mbr_dir : NULL,
                    .in_buf = (req->is_in_memory && req->in_buf_len > 0) ? (slot->data + req->in_buf_offset) : NULL,
                    .in_len = req->in_buf_len,
                    .out_buf = (req->is_in_memory && req->out_buf_cap > 0) ? (slot->data + req->out_buf_offset) : NULL,
                    .out_cap = req->out_buf_cap,
                    .out_len = &out_len,
                    .err_msg = resp->err_msg,
                    .err_msg_cap = sizeof(resp->err_msg)
                };
                resp->status = p->cmd_fn(&cargs);
                resp->out_buf_len = out_len;
                resp->msg_type = (resp->status == 0) ? INVF_MSG_RESPONSE : INVF_MSG_ERROR;
            }
        } else if (req->msg_type == INVF_MSG_CONTAINER_EST) {
            worker_plugin *p = find_or_load_plugin(req->pack_name, req->pack_path);
            if (!p || !p->est_fn) {
                resp->status = -1;
                resp->msg_type = INVF_MSG_ERROR;
                snprintf(resp->err_msg, sizeof(resp->err_msg), "plugin %s not loaded or lacks est_fn", req->pack_name);
            } else {
                ivpack_estimate_res eres;
                resp->status = p->est_fn(req->in_path, &eres);
                resp->msg_type = (resp->status == 0) ? INVF_MSG_RESPONSE : INVF_MSG_ERROR;
                resp->est_orig_size = eres.orig_size;
                resp->est_mbr_size = eres.mbr_size;
                resp->est_recipe_size = eres.recipe_size;
                resp->est_eligible = eres.eligible;
            }
        } else {
            resp->status = -2;
            resp->msg_type = INVF_MSG_ERROR;
            snprintf(resp->err_msg, sizeof(resp->err_msg), "unknown msg_type %u", req->msg_type);
        }

        slot->worker_seq++;
        slot->state = 2; /* DONE */
        val = 1;
        if (write(resp_efd, &val, sizeof(val)) != sizeof(val)) {
            /* ignore or log */
        }
    }

    close(req_efd);
    close(resp_efd);
    exit(0);
}

typedef struct worker_info {
    pid_t pid;
    int req_efd;
    int resp_efd;
    invf_plugin_slot *slot;
} worker_info;

/* Unix domain control socket for passing eventfds and slot indices to clients */
static int setup_control_socket(const char *sock_path)
{
    unlink(sock_path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 16) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Helper to send 2 file descriptors (req_efd, resp_efd) over unix socket */
static int send_fds(int sock, int fd1, int fd2, int slot_idx)
{
    struct msghdr msg;
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int) * 2)];

    iov.iov_base = &slot_idx;
    iov.iov_len = sizeof(slot_idx);

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int) * 2);

    int *fds = (int *)CMSG_DATA(cmsg);
    fds[0] = fd1;
    fds[1] = fd2;

    return sendmsg(sock, &msg, 0);
}

int main(int argc, char **argv)
{
    int num_workers = 4;
    const char *shm_name = INVF_PLUGIN_DEFAULT_SHM_NAME;
    const char *sock_path = "/tmp/invfs_plugin_pool.sock";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            num_workers = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--shm") == 0 && i + 1 < argc) {
            shm_name = argv[++i];
        } else if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        }
    }

    if (num_workers < 1) num_workers = 1;
    if (num_workers > INVF_PLUGIN_MAX_WORKERS) num_workers = INVF_PLUGIN_MAX_WORKERS;

    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Allocate shared memory */
    shm_unlink(shm_name);
    int shm_fd = shm_open(shm_name, O_CREAT | O_RDWR | O_EXCL, 0666);
    if (shm_fd < 0) {
        perror("shm_open");
        return 1;
    }

    size_t total_size = sizeof(invf_plugin_pool_hdr) + (size_t)num_workers * sizeof(invf_plugin_slot);
    if (ftruncate(shm_fd, total_size) < 0) {
        perror("ftruncate");
        return 1;
    }

    void *pool_mem = mmap(NULL, total_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (pool_mem == MAP_FAILED) {
        perror("mmap");
        return 1;
    }

    invf_plugin_pool_hdr *hdr = (invf_plugin_pool_hdr *)pool_mem;
    memset(hdr, 0, sizeof(*hdr));
    hdr->magic = INVF_PLUGIN_IPC_MAGIC;
    hdr->version = INVF_PLUGIN_IPC_VERSION;
    hdr->num_slots = num_workers;
    hdr->total_size = total_size;
    hdr->daemon_pid = getpid();
    hdr->daemon_alive = 1;

    worker_info workers[INVF_PLUGIN_MAX_WORKERS];
    uint8_t *slot_base = (uint8_t *)pool_mem + sizeof(invf_plugin_pool_hdr);

    for (int i = 0; i < num_workers; i++) {
        workers[i].slot = (invf_plugin_slot *)(slot_base + (size_t)i * sizeof(invf_plugin_slot));
        memset(workers[i].slot, 0, sizeof(invf_plugin_slot));

        workers[i].req_efd = eventfd(0, EFD_CLOEXEC);
        workers[i].resp_efd = eventfd(0, EFD_CLOEXEC);

        pid_t pid = fork();
        if (pid == 0) {
            /* Worker process */
            close(shm_fd);
            worker_loop(i, workers[i].slot, workers[i].req_efd, workers[i].resp_efd);
            _exit(0);
        }
        workers[i].pid = pid;
    }

    int ctrl_fd = setup_control_socket(sock_path);
    if (ctrl_fd < 0) {
        fprintf(stderr, "invf-plugin-host: failed to bind control socket %s\n", sock_path);
    }

    printf("invf-plugin-host: running with %d workers, shm=%s, sock=%s\n", num_workers, shm_name, sock_path);
    fflush(stdout);

    int next_slot = 0;
    while (!g_stop) {
        struct pollfd pfd = { .fd = ctrl_fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0) continue;

        int client_sock = accept(ctrl_fd, NULL, NULL);
        if (client_sock >= 0) {
            int assigned = next_slot;
            next_slot = (next_slot + 1) % num_workers;
            send_fds(client_sock, workers[assigned].req_efd, workers[assigned].resp_efd, assigned);
            close(client_sock);
        }
    }

    /* Cleanup */
    hdr->daemon_alive = 0;
    for (int i = 0; i < num_workers; i++) {
        if (workers[i].pid > 0) {
            kill(workers[i].pid, SIGTERM);
            waitpid(workers[i].pid, NULL, 0);
            close(workers[i].req_efd);
            close(workers[i].resp_efd);
        }
    }

    if (ctrl_fd >= 0) close(ctrl_fd);
    unlink(sock_path);
    munmap(pool_mem, total_size);
    close(shm_fd);
    shm_unlink(shm_name);

    return 0;
}
