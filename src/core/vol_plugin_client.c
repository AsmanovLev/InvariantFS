/* vol_plugin_client.c — Worker pool IPC client for InvariantFS.
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <pthread.h>

#include "vol_plugin_client.h"

static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_connected = false;
static int g_req_efd = -1;
static int g_resp_efd = -1;
static int g_slot_idx = -1;
static int g_shm_fd = -1;
static void *g_pool_mem = NULL;
static size_t g_pool_size = 0;
static invf_plugin_slot *g_slot = NULL;
static uint64_t g_req_counter = 1;

static int receive_fds(int sock, int *fd1, int *fd2, int *slot_idx)
{
    struct msghdr msg;
    struct iovec iov;
    char buf[CMSG_SPACE(sizeof(int) * 2)];

    iov.iov_base = slot_idx;
    iov.iov_len = sizeof(*slot_idx);

    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = buf;
    msg.msg_controllen = sizeof(buf);

    if (recvmsg(sock, &msg, 0) <= 0)
        return -1;

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    if (!cmsg || cmsg->cmsg_type != SCM_RIGHTS)
        return -1;

    int *fds = (int *)CMSG_DATA(cmsg);
    *fd1 = fds[0];
    *fd2 = fds[1];
    return 0;
}

bool invfs_plugin_pool_is_available(void)
{
    /* Check control socket existence */
    return (access("/tmp/invfs_plugin_pool.sock", F_OK) == 0);
}

int invfs_plugin_pool_connect(void)
{
    if (g_connected) return 0;

    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, "/tmp/invfs_plugin_pool.sock", sizeof(addr.sun_path) - 1);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    int req_efd = -1, resp_efd = -1, slot_idx = -1;
    if (receive_fds(sock, &req_efd, &resp_efd, &slot_idx) < 0) {
        close(sock);
        return -1;
    }
    close(sock);

    int shm_fd = shm_open(INVF_PLUGIN_DEFAULT_SHM_NAME, O_RDWR, 0666);
    if (shm_fd < 0) {
        close(req_efd);
        close(resp_efd);
        return -1;
    }

    struct stat st;
    if (fstat(shm_fd, &st) < 0) {
        close(shm_fd);
        close(req_efd);
        close(resp_efd);
        return -1;
    }

    void *mem = mmap(NULL, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (mem == MAP_FAILED) {
        close(shm_fd);
        close(req_efd);
        close(resp_efd);
        return -1;
    }

    invf_plugin_pool_hdr *hdr = (invf_plugin_pool_hdr *)mem;
    if (hdr->magic != INVF_PLUGIN_IPC_MAGIC || !hdr->daemon_alive) {
        munmap(mem, st.st_size);
        close(shm_fd);
        close(req_efd);
        close(resp_efd);
        return -1;
    }

    uint8_t *slot_base = (uint8_t *)mem + sizeof(invf_plugin_pool_hdr);
    g_slot = (invf_plugin_slot *)(slot_base + (size_t)slot_idx * sizeof(invf_plugin_slot));
    g_req_efd = req_efd;
    g_resp_efd = resp_efd;
    g_slot_idx = slot_idx;
    g_shm_fd = shm_fd;
    g_pool_mem = mem;
    g_pool_size = st.st_size;
    g_connected = true;

    return 0;
}

void invfs_plugin_pool_disconnect(void)
{
    pthread_mutex_lock(&g_pool_lock);
    if (g_connected) {
        if (g_req_efd >= 0) close(g_req_efd);
        if (g_resp_efd >= 0) close(g_resp_efd);
        if (g_pool_mem) munmap(g_pool_mem, g_pool_size);
        if (g_shm_fd >= 0) close(g_shm_fd);
        g_connected = false;
        g_req_efd = -1;
        g_resp_efd = -1;
        g_slot_idx = -1;
        g_pool_mem = NULL;
        g_slot = NULL;
    }
    pthread_mutex_unlock(&g_pool_lock);
}

int invfs_plugin_pool_container_cmd(const char *pack_name,
                                    const char *pack_so_path,
                                    int cmd,
                                    const char *in_path,
                                    const char *out_path,
                                    const char *recipe_path,
                                    const char *mbr_dir)
{
    if (!invfs_plugin_pool_is_available()) return -1;

    pthread_mutex_lock(&g_pool_lock);
    if (!g_connected && invfs_plugin_pool_connect() != 0) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    invf_plugin_req *req = &g_slot->req;
    memset(req, 0, sizeof(*req));
    req->req_id = g_req_counter++;
    req->msg_type = INVF_MSG_CONTAINER_CMD;
    req->cmd = (uint32_t)cmd;

    strncpy(req->pack_name, pack_name, sizeof(req->pack_name) - 1);
    if (pack_so_path)
        strncpy(req->pack_path, pack_so_path, sizeof(req->pack_path) - 1);

    if (in_path) strncpy(req->in_path, in_path, sizeof(req->in_path) - 1);
    if (out_path) strncpy(req->out_path, out_path, sizeof(req->out_path) - 1);
    if (recipe_path) strncpy(req->recipe_path, recipe_path, sizeof(req->recipe_path) - 1);
    if (mbr_dir) strncpy(req->mbr_dir, mbr_dir, sizeof(req->mbr_dir) - 1);

    g_slot->state = 1; /* BUSY */
    g_slot->client_seq++;

    uint64_t val = 1;
    if (write(g_req_efd, &val, sizeof(val)) != sizeof(val)) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    struct pollfd pfd = { .fd = g_resp_efd, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, 30000); /* 30s timeout */
    if (pr <= 0) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    if (read(g_resp_efd, &val, sizeof(val)) != sizeof(val)) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    invf_plugin_resp *resp = &g_slot->resp;
    int status = resp->status;
    pthread_mutex_unlock(&g_pool_lock);
    return status;
}

int invfs_plugin_pool_container_estimate(const char *pack_name,
                                         const char *pack_so_path,
                                         const char *in_path,
                                         uint64_t *out_mbr_sz)
{
    if (!invfs_plugin_pool_is_available()) return -1;

    pthread_mutex_lock(&g_pool_lock);
    if (!g_connected && invfs_plugin_pool_connect() != 0) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    invf_plugin_req *req = &g_slot->req;
    memset(req, 0, sizeof(*req));
    req->req_id = g_req_counter++;
    req->msg_type = INVF_MSG_CONTAINER_EST;

    strncpy(req->pack_name, pack_name, sizeof(req->pack_name) - 1);
    if (pack_so_path)
        strncpy(req->pack_path, pack_so_path, sizeof(req->pack_path) - 1);
    if (in_path)
        strncpy(req->in_path, in_path, sizeof(req->in_path) - 1);

    g_slot->state = 1; /* BUSY */
    g_slot->client_seq++;

    uint64_t val = 1;
    if (write(g_req_efd, &val, sizeof(val)) != sizeof(val)) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    struct pollfd pfd = { .fd = g_resp_efd, .events = POLLIN, .revents = 0 };
    int pr = poll(&pfd, 1, 10000);
    if (pr <= 0) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    if (read(g_resp_efd, &val, sizeof(val)) != sizeof(val)) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    invf_plugin_resp *resp = &g_slot->resp;
    if (resp->status != 0 || !resp->est_eligible) {
        pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }

    if (out_mbr_sz) *out_mbr_sz = resp->est_mbr_size;
    pthread_mutex_unlock(&g_pool_lock);
    return 0;
}
