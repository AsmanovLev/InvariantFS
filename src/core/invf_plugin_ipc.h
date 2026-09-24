/* invf_plugin_ipc.h — High-performance SPSC shared-memory ring buffer IPC
 * for InvariantFS out-of-process worker pool (ADR-007).
 *
 * Provides sub-microsecond command dispatch and direct in-memory data transfer
 * between multi-threaded InvariantFS engines and sandboxed worker pool daemons.
 */
#ifndef INVF_PLUGIN_IPC_H
#define INVF_PLUGIN_IPC_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define INVF_PLUGIN_IPC_MAGIC        0x504F4F4C49564653ULL /* "POOLIVFS" */
#define INVF_PLUGIN_IPC_VERSION      2   /* v2: + req.extract_idx, in-memory buffers, CAS slot lock */
#define INVF_PLUGIN_DEFAULT_SHM_NAME "/invfs_plugin_pool"
#define INVF_PLUGIN_MAX_WORKERS      16
#define INVF_PLUGIN_SLOT_SIZE        (64 * 1024 * 1024) /* 64 MiB per slot */
#define INVF_PLUGIN_NAME_MAX         64
#define INVF_PLUGIN_PATH_MAX         512

/* Message types */
enum invf_plugin_msg_type {
    INVF_MSG_PING            = 1,
    INVF_MSG_PONG            = 2,
    INVF_MSG_LOAD_PACK       = 3,
    INVF_MSG_CONTAINER_CMD   = 4,
    INVF_MSG_CONTAINER_EST   = 5,
    INVF_MSG_RESPONSE        = 100,
    INVF_MSG_ERROR           = 101,
};

/* Container pack command codes (matching InvariantFS containerpack CLI/ABI) */
enum invf_container_cmd_type {
    INVF_CPACK_CMD_ENUMERATE = 1,
    INVF_CPACK_CMD_EXTRACT   = 2,
    INVF_CPACK_CMD_STRIP     = 3,
    INVF_CPACK_CMD_REBUILD   = 4,
    INVF_CPACK_CMD_MAP       = 5,
};

#define INVF_SLOT_HEADER_SIZE 4096
#define INVF_SLOT_DATA_CAP    (INVF_PLUGIN_SLOT_SIZE - INVF_SLOT_HEADER_SIZE)

#pragma pack(push, 8)

/* Request payload inside slot */
typedef struct invf_plugin_req {
    uint64_t req_id;
    uint32_t msg_type;        /* enum invf_plugin_msg_type */
    uint32_t cmd;             /* enum invf_container_cmd_type if CONTAINER_CMD */
    char     pack_name[INVF_PLUGIN_NAME_MAX]; /* e.g. "qcow2" */
    char     pack_path[INVF_PLUGIN_PATH_MAX]; /* path to .so or .ivpack */
    
    /* File-path parameters (for disk-based operations or when spilled to disk) */
    char     in_path[INVF_PLUGIN_PATH_MAX];
    char     out_path[INVF_PLUGIN_PATH_MAX];
    char     recipe_path[INVF_PLUGIN_PATH_MAX];
    char     mbr_dir[INVF_PLUGIN_PATH_MAX];
    char     extract_idx[32];   /* EXTRACT: the member index string (argv[3]) */
    
    /* In-memory buffer parameters (data lives in slot data area) */
    uint32_t is_in_memory;    /* 1 = use in_buf/out_buf offsets, 0 = use file paths */
    uint32_t is_spilled;      /* 1 = payload was too large for slot, spilled to disk in_path */
    uint64_t in_buf_offset;   /* offset relative to slot data area */
    uint64_t in_buf_len;
    uint64_t out_buf_offset;  /* offset relative to slot data area where out should be written */
    uint64_t out_buf_cap;
} invf_plugin_req;

/* Response payload inside slot */
typedef struct invf_plugin_resp {
    uint64_t req_id;
    int32_t  status;          /* 0 = success, negative = error code */
    uint32_t msg_type;        /* INVF_MSG_RESPONSE or INVF_MSG_ERROR */
    uint64_t out_buf_len;     /* bytes written to out buffer */
    char     err_msg[256];
    
    /* For CONTAINER_EST: */
    uint64_t est_orig_size;
    uint64_t est_mbr_size;
    uint64_t est_recipe_size;
    int32_t  est_eligible;
} invf_plugin_resp;

/* Single slot shared between 1 client thread and 1 worker process */
typedef struct invf_plugin_slot {
    volatile uint32_t client_seq;  /* client increments after writing req */
    volatile uint32_t worker_seq;  /* worker increments after writing resp */
    volatile uint32_t state;       /* 0=IDLE, 1=BUSY, 2=DONE, 3=ERROR */
    volatile uint32_t lock;        /* client-side slot checkout lock (CAS) */

    invf_plugin_req   req;
    invf_plugin_resp  resp;
    uint8_t           pad[INVF_SLOT_HEADER_SIZE - sizeof(invf_plugin_req) - sizeof(invf_plugin_resp) - 16];

    /* Raw data buffer area (64MB - 4KB header) */
    uint8_t           data[INVF_SLOT_DATA_CAP];
} invf_plugin_slot;

/* Master header at start of shared memory */
typedef struct invf_plugin_pool_hdr {
    uint64_t magic;           /* INVF_PLUGIN_IPC_MAGIC */
    uint32_t version;         /* INVF_PLUGIN_IPC_VERSION */
    uint32_t num_slots;       /* number of worker slots (e.g. 4..16) */
    uint64_t total_size;      /* num_slots * INVF_PLUGIN_SLOT_SIZE + sizeof(pool_hdr) */
    pid_t    daemon_pid;      /* host daemon pid */
    volatile uint32_t daemon_alive;
    uint8_t  padding[40];
} invf_plugin_pool_hdr;

#pragma pack(pop)

#endif /* INVF_PLUGIN_IPC_H */
