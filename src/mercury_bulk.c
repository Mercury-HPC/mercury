/*
 * Copyright (C) 2013-2020 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#include "mercury_bulk.h"
#include "mercury_bulk_proc.h"
#include "mercury_core.h"
#include "mercury_error.h"
#include "mercury_private.h"

#include "mercury_atomic.h"
#include "mercury_list.h"
#include "mercury_thread_condition.h"
#include "mercury_thread_spin.h"

#include <stdlib.h>
#include <string.h>

/****************/
/* Local Macros */
/****************/

/* Limit for number of segments statically allocated */
#define HG_BULK_STATIC_MAX (8)

/* Additional internal bulk flags (can hold up to 8 bits) */
#define HG_BULK_ALLOC (1 << 4) /* memory is allocated */
#define HG_BULK_BIND  (1 << 5) /* address is bound to segment */
#define HG_BULK_REGV  (1 << 6) /* single registration for multiple segments */
#define HG_BULK_VIRT  (1 << 7) /* addresses are virtual */

/* Op ID status bits */
#define HG_BULK_OP_COMPLETED (1 << 0)
#define HG_BULK_OP_CANCELED  (1 << 1)
#define HG_BULK_OP_ERRORED   (1 << 2)

/* Encode type */
#define HG_BULK_TYPE_ENCODE(label, ret, buf_ptr, buf_size_left, data, size)    \
    do {                                                                       \
        HG_CHECK_ERROR(buf_size_left < size, label, ret, HG_OVERFLOW,          \
            "Buffer size too small (%" PRIu64 ")", buf_size_left);             \
        memcpy(buf_ptr, data, size);                                           \
        buf_ptr += size;                                                       \
        buf_size_left -= size;                                                 \
    } while (0)

#define HG_BULK_ENCODE(label, ret, buf_ptr, buf_size_left, data, type)         \
    HG_BULK_TYPE_ENCODE(label, ret, buf_ptr, buf_size_left, data, sizeof(type))

#define HG_BULK_ENCODE_ARRAY(                                                  \
    label, ret, buf_ptr, buf_size_left, data, type, count)                     \
    HG_BULK_TYPE_ENCODE(                                                       \
        label, ret, buf_ptr, buf_size_left, data, sizeof(type) * count)

/* Decode type */
#define HG_BULK_TYPE_DECODE(label, ret, buf_ptr, buf_size_left, data, size)    \
    do {                                                                       \
        HG_CHECK_ERROR(buf_size_left < size, label, ret, HG_OVERFLOW,          \
            "Buffer size too small (%" PRIu64 ")", buf_size_left);             \
        memcpy(data, buf_ptr, size);                                           \
        buf_ptr += size;                                                       \
        buf_size_left -= size;                                                 \
    } while (0)

#define HG_BULK_DECODE(label, ret, buf_ptr, buf_size_left, data, type)         \
    HG_BULK_TYPE_DECODE(label, ret, buf_ptr, buf_size_left, data, sizeof(type))

#define HG_BULK_DECODE_ARRAY(                                                  \
    label, ret, buf_ptr, buf_size_left, data, type, count)                     \
    HG_BULK_TYPE_DECODE(                                                       \
        label, ret, buf_ptr, buf_size_left, data, sizeof(type) * count)

/* Min/max macros */
#define HG_BULK_MIN(a, b) (a < b) ? a : b

/* Get segments */
#define HG_BULK_SEGMENTS(x)                                                    \
    ((x)->desc.info.segment_count > HG_BULK_STATIC_MAX) ? (x)->desc.segments.d \
                                                        : (x)->desc.segments.s

#define HG_BULK_MEM_HANDLES(x, count, flags)                                   \
    (count > HG_BULK_STATIC_MAX && !(flags & HG_BULK_REGV)) ? (x)->handles.d   \
                                                            : (x)->handles.s

#define HG_BULK_NA_OP_IDS(x)                                                   \
    ((x)->op_count > HG_BULK_STATIC_MAX) ? (x)->na_op_ids.d : (x)->na_op_ids.s

#define HG_BULK_NA_SM_OP_IDS(x)                                                \
    ((x)->op_count > HG_BULK_STATIC_MAX) ? (x)->na_sm_op_ids.d                 \
                                         : (x)->na_sm_op_ids.s

/* Check permission flags */
#define HG_BULK_CHECK_FLAGS(op, origin_flags, local_flags, label, ret)         \
    switch (op) {                                                              \
        case HG_BULK_PUSH:                                                     \
            HG_CHECK_ERROR(!(origin_flags & HG_BULK_WRITE_ONLY) ||             \
                               !(local_flags & HG_BULK_READ_ONLY),             \
                label, ret, HG_PERMISSION,                                     \
                "Invalid permission flags for PUSH operation "                 \
                "(origin=0x%x, local=0x%x)",                                   \
                origin_flags, local_flags);                                    \
            break;                                                             \
        case HG_BULK_PULL:                                                     \
            HG_CHECK_ERROR(!(origin_flags & HG_BULK_READ_ONLY) ||              \
                               !(local_flags & HG_BULK_WRITE_ONLY),            \
                label, ret, HG_PERMISSION,                                     \
                "Invalid permission flags for PULL operation "                 \
                "(origin=%d, local=%d)",                                       \
                origin_flags, local_flags);                                    \
            break;                                                             \
        default:                                                               \
            HG_GOTO_ERROR(                                                     \
                label, ret, HG_INVALID_ARG, "Unknown bulk operation");         \
    }

/************************************/
/* Local Type and Struct Definition */
/************************************/

/* HG class */
struct hg_class {
    hg_core_class_t *core_class; /* Core class */
};

/* HG context */
struct hg_context {
    hg_core_context_t *core_context; /* Core context */
};

/* HG bulk segment */
struct hg_bulk_segment {
    hg_ptr_t base; /* Address of the segment */
    hg_size_t len; /* Size of the segment in bytes */
};

/* HG bulk descriptor (cannot use flexible array members because count of
 * segments may not match count of handles) */
struct hg_bulk_desc {
    struct hg_bulk_desc_info info; /* Segment info */
    union {
        struct hg_bulk_segment s[HG_BULK_STATIC_MAX]; /* Static array */
        struct hg_bulk_segment *d;                    /* Dynamic array */
    } segments;                                       /* Remain last */
};

/* NA descriptors */
struct hg_bulk_na_mem_desc {
    union {
        na_size_t s[HG_BULK_STATIC_MAX]; /* Static array */
        na_size_t *d;                    /* Dynamic array */
    } serialize_sizes;                   /* Serialize sizes */
    union {
        na_mem_handle_t s[HG_BULK_STATIC_MAX]; /* Static array */
        na_mem_handle_t *d;                    /* Dynamic array */
    } handles;                                 /* NA mem handles */
};

/* HG bulk handle */
struct hg_bulk {
    struct hg_bulk_desc desc;                /* Bulk descriptor   */
    struct hg_bulk_na_mem_desc na_mem_descs; /* NA memory handles */
#ifdef NA_HAS_SM
    struct hg_bulk_na_mem_desc na_sm_mem_descs; /* NA SM memory handles */
#endif
    hg_core_class_t *core_class; /* HG core class */
    na_class_t *na_class;        /* NA class */
#ifdef NA_HAS_SM
    na_class_t *na_sm_class; /* NA SM class */
#endif
    hg_core_addr_t addr;         /* Addr (valid if bound to handle) */
    void *serialize_ptr;         /* Cached serialization buffer */
    hg_size_t serialize_size;    /* Cached serialization size */
    hg_atomic_int32_t ref_count; /* Reference count */
    hg_uint8_t context_id;       /* Context ID (valid if bound to handle) */
    hg_bool_t registered;        /* Handle was registered */
};

/* HG bulk NA op IDs (not a union as we re-use op IDs) */
typedef struct {
    na_op_id_t *s[HG_BULK_STATIC_MAX]; /* Static array */
    na_op_id_t **d;                    /* Dynamic array */
} hg_bulk_na_op_id_t;

/* HG Bulk op ID */
struct hg_bulk_op_id {
    struct hg_completion_entry
        hg_completion_entry;              /* Entry in completion queue */
    struct hg_cb_info callback_info;      /* Callback info struct */
    HG_LIST_ENTRY(hg_bulk_op_id) pending; /* Pending list entry */
    struct hg_bulk_op_pool *op_pool;      /* Pool that op ID belongs to */
    hg_cb_t callback;                     /* Pointer to function */
    hg_bulk_na_op_id_t na_op_ids;         /* NA operations IDs */
#ifdef NA_HAS_SM
    hg_bulk_na_op_id_t na_sm_op_ids; /* NA SM operations IDs */
#endif
    hg_core_context_t *core_context;      /* Context */
    na_class_t *na_class;                 /* NA class */
    na_context_t *na_context;             /* NA context */
    hg_atomic_int32_t status;             /* Operation status */
    hg_atomic_int32_t op_completed_count; /* Number of operations completed */
    hg_atomic_int32_t ref_count;          /* Refcount */
    hg_return_t err_ret;                  /* Return value if errored status */
    hg_uint32_t op_count;                 /* Number of ongoing operations */
    hg_bool_t reuse;                      /* Re-use op ID once ref_count is 0 */
};

/* Pool of op IDs */
struct hg_bulk_op_pool {
    hg_thread_mutex_t extend_mutex;           /* To extend pool */
    hg_thread_cond_t extend_cond;             /* To extend pool */
    hg_core_context_t *core_context;          /* Context */
    HG_LIST_HEAD(hg_bulk_op_id) pending_list; /* Pending op IDs */
    hg_thread_spin_t pending_list_lock;       /* Pending list lock */
    unsigned long count;                      /* Number of op IDs */
    hg_bool_t extending;                      /* When extending the pool */
};

/* Wrapper on top of memcpy */
typedef void (*hg_bulk_copy_op_t)(hg_ptr_t local_address,
    hg_size_t local_offset, hg_ptr_t remote_address, hg_size_t remote_offset,
    hg_size_t data_size);

/* Wrapper on top of NA layer */
typedef na_return_t (*na_bulk_op_t)(na_class_t *na_class, na_context_t *context,
    na_cb_t callback, void *arg, na_mem_handle_t local_mem_handle,
    na_offset_t local_offset, na_mem_handle_t remote_mem_handle,
    na_offset_t remote_offset, na_size_t data_size, na_addr_t remote_addr,
    na_uint8_t remote_id, na_op_id_t *op_id);

/********************/
/* Local Prototypes */
/********************/

/**
 * Create handle.
 */
static hg_return_t
hg_bulk_create(hg_core_class_t *core_class, hg_uint32_t count, void **bufs,
    const hg_size_t *lens, hg_uint8_t flags, struct hg_bulk **hg_bulk_ptr);

/**
 * Free handle.
 */
static hg_return_t
hg_bulk_free(struct hg_bulk *hg_bulk);

/**
 * Create NA memory descriptors.
 */
static hg_return_t
hg_bulk_create_na_mem_descs(struct hg_bulk_na_mem_desc *na_mem_descs,
    na_class_t *na_class, struct hg_bulk_segment *segments, hg_uint32_t count,
    hg_uint8_t flags);

/**
 * Free NA memory descriptors.
 */
static hg_return_t
hg_bulk_free_na_mem_descs(struct hg_bulk_na_mem_desc *na_mem_descs,
    na_class_t *na_class, hg_uint32_t count, na_bool_t registered);

/**
 * Register single segment.
 */
static hg_return_t
hg_bulk_register(na_class_t *na_class, void *base, na_size_t len,
    unsigned long flags, na_mem_handle_t *mem_handle_ptr,
    na_size_t *serialize_size_ptr);

/**
 * Register multiple segments.
 */
static hg_return_t
hg_bulk_register_segments(na_class_t *na_class, struct na_segment *segments,
    na_size_t count, unsigned long flags, na_mem_handle_t *mem_handle_ptr,
    na_size_t *serialize_size_ptr);

/**
 * Deregister segment.
 */
static hg_return_t
hg_bulk_deregister(
    na_class_t *na_class, na_mem_handle_t mem_handle, na_bool_t registered);

/**
 * Get serialize size.
 */
static hg_size_t
hg_bulk_get_serialize_size(struct hg_bulk *hg_bulk, hg_uint8_t flags);

/**
 * Get serialize size of NA memory descriptors.
 */
static hg_size_t
hg_bulk_get_serialize_size_mem_descs(
    struct hg_bulk_na_mem_desc *na_mem_descs, hg_uint32_t count);

/**
 * Serialize bulk handle.
 */
static hg_return_t
hg_bulk_serialize(
    void *buf, hg_size_t buf_size, hg_uint8_t flags, struct hg_bulk *hg_bulk);

/**
 * Serialize NA memory descriptors.
 */
static hg_return_t
hg_bulk_serialize_mem_descs(na_class_t *na_class, char **buf_ptr,
    hg_size_t *buf_size_left, struct hg_bulk_na_mem_desc *na_mem_descs,
    const struct hg_bulk_segment *segments, hg_uint32_t count);

/**
 * Deserialize bulk handle.
 */
static hg_return_t
hg_bulk_deserialize(hg_core_class_t *core_class, struct hg_bulk **hg_bulk_ptr,
    const void *buf, hg_size_t buf_size);

/**
 * Deserialize NA memory descriptors.
 */
static hg_return_t
hg_bulk_deserialize_mem_descs(na_class_t *na_class, const char **buf_ptr,
    hg_size_t *buf_size_left, struct hg_bulk_na_mem_desc *na_mem_descs,
    const struct hg_bulk_segment *segments, hg_uint32_t count);

/**
 * Access bulk handle and get segment addresses/sizes.
 */
static void
hg_bulk_access(struct hg_bulk *hg_bulk, hg_size_t offset, hg_size_t size,
    hg_uint8_t flags, hg_uint32_t max_count, void **buf_ptrs,
    hg_size_t *buf_sizes, hg_uint32_t *actual_count);

/**
 * Get info for bulk transfer.
 */
static HG_INLINE void
hg_bulk_offset_translate(const struct hg_bulk_segment *segments,
    hg_uint32_t count, hg_size_t offset, hg_uint32_t *segment_start_index,
    hg_size_t *segment_start_offset);

/**
 * Create bulk operation ID.
 */
static hg_return_t
hg_bulk_op_create(
    hg_core_context_t *core_context, struct hg_bulk_op_id **hg_bulk_op_id_ptr);

/**
 * Destroy bulk operation ID.
 */
static hg_return_t
hg_bulk_op_destroy(struct hg_bulk_op_id *hg_bulk_op_id);

/**
 * Retrive bulk operation ID from pool.
 */
static hg_return_t
hg_bulk_op_pool_get(struct hg_bulk_op_pool *hg_bulk_op_pool,
    struct hg_bulk_op_id **hg_bulk_op_id_ptr);

/**
 * Bulk transfer.
 */
static hg_return_t
hg_bulk_transfer(hg_core_context_t *core_context, hg_cb_t callback, void *arg,
    hg_bulk_op_t op, struct hg_core_addr *origin_addr, hg_uint8_t origin_id,
    struct hg_bulk *hg_bulk_origin, hg_size_t origin_offset,
    struct hg_bulk *hg_bulk_local, hg_size_t local_offset, hg_size_t size,
    hg_op_id_t *op_id);

/**
 * Bulk transfer to self.
 */
static hg_return_t
hg_bulk_transfer_self(hg_bulk_op_t op,
    const struct hg_bulk_segment *origin_segments, hg_uint32_t origin_count,
    hg_size_t origin_offset, const struct hg_bulk_segment *local_segments,
    hg_uint32_t local_count, hg_size_t local_offset, hg_size_t size,
    struct hg_bulk_op_id *hg_bulk_op_id);

/**
 * Transfer segments to self (local copy).
 */
static void
hg_bulk_transfer_segments_self(hg_bulk_copy_op_t copy_op,
    const struct hg_bulk_segment *origin_segments, hg_uint32_t origin_count,
    hg_size_t origin_segment_start_index, hg_size_t origin_segment_start_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    hg_size_t local_segment_start_index, hg_size_t local_segment_start_offset,
    hg_size_t size);

/**
 * Memcpy.
 */
static HG_INLINE void
hg_bulk_memcpy_put(hg_ptr_t local_address, hg_size_t local_offset,
    hg_ptr_t remote_address, hg_size_t remote_offset, hg_size_t data_size)
{
    memcpy((void *) (remote_address + remote_offset),
        (const void *) (local_address + local_offset), data_size);
}

/**
 * Memcpy.
 */
static HG_INLINE void
hg_bulk_memcpy_get(hg_ptr_t local_address, hg_size_t local_offset,
    hg_ptr_t remote_address, hg_size_t remote_offset, hg_size_t data_size)
{
    memcpy((void *) (local_address + local_offset),
        (const void *) (remote_address + remote_offset), data_size);
}

/**
 * Bulk transfer over NA.
 */
static hg_return_t
hg_bulk_transfer_na(hg_bulk_op_t op, na_addr_t na_origin_addr,
    hg_uint8_t origin_id, const struct hg_bulk_segment *origin_segments,
    hg_uint32_t origin_count, na_mem_handle_t *origin_mem_handles,
    hg_uint8_t origin_flags, hg_size_t origin_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    na_mem_handle_t *local_mem_handles, hg_uint8_t local_flags,
    hg_size_t local_offset, hg_size_t size,
    struct hg_bulk_op_id *hg_bulk_op_id);

/**
 * Get number of required operations to transfer data.
 */
static hg_uint32_t
hg_bulk_transfer_get_op_count(const struct hg_bulk_segment *origin_segments,
    hg_uint32_t origin_count, hg_size_t origin_segment_start_index,
    hg_size_t origin_segment_start_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    hg_size_t local_segment_start_index, hg_size_t local_segment_start_offset,
    hg_size_t size);

/**
 * Transfer segments.
 */
static hg_return_t
hg_bulk_transfer_segments_na(na_class_t *na_class, na_context_t *na_context,
    na_bulk_op_t na_bulk_op, na_cb_t callback, void *arg, na_addr_t origin_addr,
    na_uint8_t origin_id, const struct hg_bulk_segment *origin_segments,
    hg_uint32_t origin_count, na_mem_handle_t *origin_mem_handles,
    hg_size_t origin_segment_start_index, hg_size_t origin_segment_start_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    na_mem_handle_t *local_mem_handles, hg_size_t local_segment_start_index,
    hg_size_t local_segment_start_offset, hg_size_t size,
    na_op_id_t *na_op_ids[], hg_uint32_t na_op_count);

/**
 * NA_Put wrapper
 */
static HG_INLINE na_return_t
hg_bulk_na_put(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t data_size, na_addr_t remote_addr, na_uint8_t remote_id,
    na_op_id_t *op_id)
{
    return NA_Put(na_class, context, callback, arg, local_mem_handle,
        local_offset, remote_mem_handle, remote_offset, data_size, remote_addr,
        remote_id, op_id);
}

/**
 * NA_Get wrapper
 */
static HG_INLINE na_return_t
hg_bulk_na_get(na_class_t *na_class, na_context_t *context, na_cb_t callback,
    void *arg, na_mem_handle_t local_mem_handle, na_offset_t local_offset,
    na_mem_handle_t remote_mem_handle, na_offset_t remote_offset,
    na_size_t data_size, na_addr_t remote_addr, na_uint8_t remote_id,
    na_op_id_t *op_id)
{
    return NA_Get(na_class, context, callback, arg, local_mem_handle,
        local_offset, remote_mem_handle, remote_offset, data_size, remote_addr,
        remote_id, op_id);
}

/**
 * Transfer callback.
 */
static int
hg_bulk_transfer_cb(const struct na_cb_info *callback_info);

/**
 * Complete operation ID.
 */
static hg_return_t
hg_bulk_complete(struct hg_bulk_op_id *hg_bulk_op_id, hg_bool_t self_notify);

/**
 * Cancel operation ID.
 */
static hg_return_t
hg_bulk_cancel(struct hg_bulk_op_id *hg_bulk_op_id);

/*******************/
/* Local Variables */
/*******************/

static hg_return_t
hg_bulk_create(hg_core_class_t *core_class, hg_uint32_t count, void **bufs,
    const hg_size_t *lens, hg_uint8_t flags, struct hg_bulk **hg_bulk_ptr)
{
    struct hg_bulk *hg_bulk = NULL;
    struct hg_bulk_segment *segments;
    na_class_t *na_class = HG_Core_class_get_na(core_class);
#ifdef NA_HAS_SM
    na_class_t *na_sm_class = HG_Core_class_get_na_sm(core_class);
#endif
    hg_return_t ret = HG_SUCCESS;

    hg_bulk = (struct hg_bulk *) malloc(sizeof(struct hg_bulk));
    HG_CHECK_ERROR(
        hg_bulk == NULL, error, ret, HG_NOMEM, "Could not allocate handle");

    memset(hg_bulk, 0, sizeof(struct hg_bulk));
    hg_bulk->core_class = core_class;
    hg_bulk->na_class = na_class;
#ifdef NA_HAS_SM
    hg_bulk->na_sm_class = na_sm_class;
#endif
    hg_bulk->desc.info.segment_count = count;
    hg_bulk->desc.info.flags = flags;
    hg_atomic_init32(&hg_bulk->ref_count, 1);

    if (count > HG_BULK_STATIC_MAX) {
        /* Allocate segments */
        hg_bulk->desc.segments.d = (struct hg_bulk_segment *) calloc(
            count, sizeof(struct hg_bulk_segment));
        HG_CHECK_ERROR(hg_bulk->desc.segments.d == NULL, error, ret, HG_NOMEM,
            "Could not allocate segment array");

        segments = hg_bulk->desc.segments.d;
    } else
        segments = hg_bulk->desc.segments.s;

    /* Loop over the list of segments */
    if (!bufs) {
        hg_uint32_t i;

        /* Allocate buffers internally if only lengths are provided */
        hg_bulk->desc.info.flags |= HG_BULK_ALLOC;
        for (i = 0; i < count; i++) {
            if (lens[i] == 0)
                continue;

            segments[i].base = (hg_ptr_t) calloc(1, lens[i]);
            HG_CHECK_ERROR(segments[i].base == (hg_ptr_t) NULL, error, ret,
                HG_NOMEM, "Could not allocate segment");

            segments[i].len = lens[i];
            hg_bulk->desc.info.len += lens[i];
        }
    } else {
        hg_uint32_t i;

        for (i = 0; i < count; i++) {
            segments[i].base = (hg_ptr_t) bufs[i];
            segments[i].len = lens[i];
            hg_bulk->desc.info.len += lens[i];
        }
    }

    HG_LOG_DEBUG("Creating bulk handle with %u segment(s), len is %" PRIu64
                 " bytes",
        hg_bulk->desc.info.segment_count, hg_bulk->desc.info.len);

    /* Query max segment limit that NA plugin can handle */
    if ((count > 1) && na_class->ops->mem_handle_create_segments) {
        na_size_t max_segments =
            na_class->ops->mem_handle_get_max_segments(na_class);

        /* Will use one single descriptor if supported */
        if ((max_segments > 1) && (count <= max_segments))
            hg_bulk->desc.info.flags |= HG_BULK_REGV;

#ifdef NA_HAS_SM
        /* Make sure SM can register as many segments */
        if (na_sm_class) {
            na_size_t max_sm_segments =
                na_sm_class->ops->mem_handle_get_max_segments(na_sm_class);

            HG_CHECK_ERROR(!na_sm_class->ops->mem_handle_create_segments, error,
                ret, HG_OPNOTSUPPORTED,
                "Registration of segments not supported with SM");
            HG_CHECK_ERROR(count > max_sm_segments, error, ret,
                HG_OPNOTSUPPORTED,
                "SM class cannot register %" PRIu32 " segments", count);
        }
#endif
    }

    /* Register using one single descriptor if supported */
    if (hg_bulk->desc.info.flags & HG_BULK_REGV) {
        /* Register segments */
        ret =
            hg_bulk_register_segments(na_class, (struct na_segment *) segments,
                count, flags, &hg_bulk->na_mem_descs.handles.s[0],
                &hg_bulk->na_mem_descs.serialize_sizes.s[0]);
        HG_CHECK_HG_ERROR(error, ret, "Could not register segments");

#ifdef NA_HAS_SM
        if (na_sm_class) {
            /* Register segments */
            ret = hg_bulk_register_segments(na_sm_class,
                (struct na_segment *) segments, count, flags,
                &hg_bulk->na_sm_mem_descs.handles.s[0],
                &hg_bulk->na_sm_mem_descs.serialize_sizes.s[0]);
            HG_CHECK_HG_ERROR(
                error, ret, "Could not register segments with SM");
        }
#endif
    } else {
        /* Register segments individually */
        ret = hg_bulk_create_na_mem_descs(
            &hg_bulk->na_mem_descs, na_class, segments, count, flags);
        HG_CHECK_HG_ERROR(error, ret, "Could not create NA mem descriptors");

#ifdef NA_HAS_SM
        if (na_sm_class) {
            ret = hg_bulk_create_na_mem_descs(
                &hg_bulk->na_sm_mem_descs, na_sm_class, segments, count, flags);
            HG_CHECK_HG_ERROR(
                error, ret, "Could not create NA SM mem descriptors");
        }
#endif
    }
    hg_bulk->registered = HG_TRUE;

    *hg_bulk_ptr = hg_bulk;

    return ret;

error:
    hg_bulk_free(hg_bulk);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_free(struct hg_bulk *hg_bulk)
{
    struct hg_bulk_segment *segments;
    hg_return_t ret = HG_SUCCESS;

    if (!hg_bulk)
        goto done;

    /* Cannot free yet */
    if (hg_atomic_decr32(&hg_bulk->ref_count))
        goto done;

    /* Deregister segments */
    if (hg_bulk->desc.info.flags & HG_BULK_REGV ||
        (hg_bulk->desc.info.segment_count == 1)) {
        if (hg_bulk->na_mem_descs.handles.s[0] != NA_MEM_HANDLE_NULL) {
            ret = hg_bulk_deregister(hg_bulk->na_class,
                hg_bulk->na_mem_descs.handles.s[0], hg_bulk->registered);
            HG_CHECK_HG_ERROR(done, ret, "Could not deregister segment");
        }

#ifdef NA_HAS_SM
        if (hg_bulk->na_sm_mem_descs.handles.s[0] != NA_MEM_HANDLE_NULL) {
            ret = hg_bulk_deregister(hg_bulk->na_sm_class,
                hg_bulk->na_sm_mem_descs.handles.s[0], hg_bulk->registered);
            HG_CHECK_HG_ERROR(
                done, ret, "Could not deregister segment with SM");
        }
#endif
    } else {
        /* Free segments individually */
        ret =
            hg_bulk_free_na_mem_descs(&hg_bulk->na_mem_descs, hg_bulk->na_class,
                hg_bulk->desc.info.segment_count, hg_bulk->registered);
        HG_CHECK_HG_ERROR(done, ret, "Could not free NA mem descriptors");

#ifdef NA_HAS_SM
        if (hg_bulk->na_sm_class) {
            ret = hg_bulk_free_na_mem_descs(&hg_bulk->na_sm_mem_descs,
                hg_bulk->na_sm_class, hg_bulk->desc.info.segment_count,
                hg_bulk->registered);
            HG_CHECK_HG_ERROR(
                done, ret, "Could not free NA SM mem descriptors");
        }
#endif
    }

    /* Free addr if any was attached to handle */
    if (hg_bulk->desc.info.flags & HG_BULK_BIND) {
        ret = HG_Core_addr_free(hg_bulk->addr);
        HG_CHECK_HG_ERROR(done, ret, "Could not free addr");
    }

    segments = (hg_bulk->desc.info.segment_count > HG_BULK_STATIC_MAX)
                   ? hg_bulk->desc.segments.d
                   : hg_bulk->desc.segments.s;

    /* Free segments if we allocated them */
    if (hg_bulk->desc.info.flags & HG_BULK_ALLOC) {
        hg_uint32_t i;

        for (i = 0; i < hg_bulk->desc.info.segment_count; i++)
            free((void *) segments[i].base);
    }

    if (hg_bulk->desc.info.segment_count > HG_BULK_STATIC_MAX)
        free(segments);

    free(hg_bulk);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_create_na_mem_descs(struct hg_bulk_na_mem_desc *na_mem_descs,
    na_class_t *na_class, struct hg_bulk_segment *segments, hg_uint32_t count,
    hg_uint8_t flags)
{
    na_mem_handle_t *na_mem_handles;
    na_size_t *na_mem_serialize_sizes;
    hg_return_t ret = HG_SUCCESS;
    hg_uint32_t i;

    if (count > HG_BULK_STATIC_MAX) {
        /* Allocate NA memory handles */
        na_mem_descs->handles.d =
            (na_mem_handle_t *) calloc(count, sizeof(na_mem_handle_t));
        HG_CHECK_ERROR(na_mem_descs->handles.d == NULL, error, ret, HG_NOMEM,
            "Could not allocate mem handle array");

        /* Allocate serialize sizes */
        na_mem_descs->serialize_sizes.d =
            (na_size_t *) calloc(count, sizeof(na_size_t));
        HG_CHECK_ERROR(na_mem_descs->serialize_sizes.d == NULL, error, ret,
            HG_NOMEM, "Could not allocate serialize sizes array");

        na_mem_handles = na_mem_descs->handles.d;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.d;
    } else {
        na_mem_handles = na_mem_descs->handles.s;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.s;
    }

    for (i = 0; i < count; i++) {
        /* Skip null segments */
        if (segments[i].base == (hg_ptr_t) NULL)
            continue;

        /* Register segment */
        ret = hg_bulk_register(na_class, (void *) segments[i].base,
            segments[i].len, flags, &na_mem_handles[i],
            &na_mem_serialize_sizes[i]);
        HG_CHECK_HG_ERROR(error, ret, "Could not register segment");
    }

    return ret;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_free_na_mem_descs(struct hg_bulk_na_mem_desc *na_mem_descs,
    na_class_t *na_class, hg_uint32_t count, na_bool_t registered)
{
    na_mem_handle_t *na_mem_handles;
    hg_return_t ret = HG_SUCCESS;

    if (count > HG_BULK_STATIC_MAX) {
        na_mem_handles = na_mem_descs->handles.d;
        free(na_mem_descs->serialize_sizes.d);
    } else
        na_mem_handles = na_mem_descs->handles.s;

    if (na_mem_handles) {
        hg_uint32_t i;

        for (i = 0; i < count; i++) {
            if (na_mem_handles[i] == NA_MEM_HANDLE_NULL)
                continue;

            ret = hg_bulk_deregister(na_class, na_mem_handles[i], registered);
            HG_CHECK_HG_ERROR(done, ret, "Could not deregister segment");
        }
        if (count > HG_BULK_STATIC_MAX)
            free(na_mem_handles);
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_bind(struct hg_bulk *hg_bulk, hg_core_context_t *core_context)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(hg_bulk->addr != HG_CORE_ADDR_NULL, done, ret,
        HG_INVALID_ARG, "Handle is already bound to an existing address");

    /* Retrieve self address */
    ret = HG_Core_addr_self(hg_bulk->core_class, &hg_bulk->addr);
    HG_CHECK_HG_ERROR(done, ret, "Could not get self address");

    /* Add context ID */
    hg_bulk->context_id = HG_Core_context_get_id(core_context);

    /* Set flags */
    hg_bulk->desc.info.flags |= HG_BULK_BIND;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_register(na_class_t *na_class, void *base, na_size_t len,
    unsigned long flags, na_mem_handle_t *mem_handle_ptr,
    na_size_t *serialize_size_ptr)
{
    na_mem_handle_t mem_handle = NA_MEM_HANDLE_NULL;
    na_size_t serialize_size = 0;
    hg_bool_t registered = HG_FALSE;
    hg_return_t ret = HG_SUCCESS;
    na_return_t na_ret;

    /* Create NA memory handle */
    na_ret = NA_Mem_handle_create(na_class, base, len, flags, &mem_handle);
    HG_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret, (hg_return_t) na_ret,
        "NA_Mem_handle_create() failed (%s)", NA_Error_to_string(na_ret));

    /* Register NA memory handle */
    na_ret = NA_Mem_register(na_class, mem_handle);
    HG_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret, (hg_return_t) na_ret,
        "NA_Mem_register() failed (%s)", NA_Error_to_string(na_ret));
    registered = HG_TRUE;

    /* Cache serialize size */
    serialize_size = NA_Mem_handle_get_serialize_size(na_class, mem_handle);
    HG_CHECK_ERROR(serialize_size == 0, error, ret, HG_PROTOCOL_ERROR,
        "NA_Mem_handle_get_serialize_size() failed");

    *mem_handle_ptr = mem_handle;
    *serialize_size_ptr = serialize_size;

    return ret;

error:
    if (mem_handle != NA_MEM_HANDLE_NULL) {
        if (registered) {
            na_ret = NA_Mem_deregister(na_class, mem_handle);
            HG_CHECK_ERROR_DONE(na_ret != NA_SUCCESS,
                "NA_Mem_deregister() failed (%s)", NA_Error_to_string(na_ret));
        }
        na_ret = NA_Mem_handle_free(na_class, mem_handle);
        HG_CHECK_ERROR_DONE(na_ret != NA_SUCCESS,
            "NA_Mem_handle_free() failed (%s)", NA_Error_to_string(na_ret));
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_register_segments(na_class_t *na_class, struct na_segment *segments,
    na_size_t count, unsigned long flags, na_mem_handle_t *mem_handle_ptr,
    na_size_t *serialize_size_ptr)
{
    na_mem_handle_t mem_handle = NA_MEM_HANDLE_NULL;
    na_size_t serialize_size = 0;
    hg_bool_t registered = HG_FALSE;
    hg_return_t ret = HG_SUCCESS;
    na_return_t na_ret;

    /* Create NA memory handle */
    na_ret = NA_Mem_handle_create_segments(
        na_class, segments, count, flags, &mem_handle);
    HG_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret, (hg_return_t) na_ret,
        "NA_Mem_handle_create_segments() failed (%s)",
        NA_Error_to_string(na_ret));

    /* Register NA memory handle */
    na_ret = NA_Mem_register(na_class, mem_handle);
    HG_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret, (hg_return_t) na_ret,
        "NA_Mem_register() failed (%s)", NA_Error_to_string(na_ret));
    registered = HG_TRUE;

    /* Cache serialize size */
    serialize_size = NA_Mem_handle_get_serialize_size(na_class, mem_handle);
    HG_CHECK_ERROR(serialize_size == 0, error, ret, HG_PROTOCOL_ERROR,
        "NA_Mem_handle_get_serialize_size() failed");

    *mem_handle_ptr = mem_handle;
    *serialize_size_ptr = serialize_size;

    return ret;

error:
    if (mem_handle != NA_MEM_HANDLE_NULL) {
        if (registered) {
            na_ret = NA_Mem_deregister(na_class, mem_handle);
            HG_CHECK_ERROR_DONE(na_ret != NA_SUCCESS,
                "NA_Mem_deregister() failed (%s)", NA_Error_to_string(na_ret));
        }
        na_ret = NA_Mem_handle_free(na_class, mem_handle);
        HG_CHECK_ERROR_DONE(na_ret != NA_SUCCESS,
            "NA_Mem_handle_free() failed (%s)", NA_Error_to_string(na_ret));
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_deregister(
    na_class_t *na_class, na_mem_handle_t mem_handle, na_bool_t registered)
{
    hg_return_t ret = HG_SUCCESS;
    na_return_t na_ret;

    if (registered) {
        na_ret = NA_Mem_deregister(na_class, mem_handle);
        HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret, (hg_return_t) na_ret,
            "NA_Mem_deregister() failed (%s)", NA_Error_to_string(na_ret));
    }

    na_ret = NA_Mem_handle_free(na_class, mem_handle);
    HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret, (hg_return_t) na_ret,
        "NA_Mem_handle_free() failed (%s)", NA_Error_to_string(na_ret));

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_size_t
hg_bulk_get_serialize_size(struct hg_bulk *hg_bulk, hg_uint8_t flags)
{
    hg_size_t ret = 0;

    /* Descriptor info + segments */
    ret = sizeof(hg_bulk->desc.info) +
          hg_bulk->desc.info.segment_count * sizeof(struct hg_bulk_segment);

    /* Memory handles */
    if ((hg_bulk->desc.info.flags & HG_BULK_REGV) ||
        (hg_bulk->desc.info.segment_count == 1)) {
        /* Only one single memory handle in that case */
        if (hg_bulk->na_mem_descs.handles.s[0] != NA_MEM_HANDLE_NULL)
            ret +=
                hg_bulk->na_mem_descs.serialize_sizes.s[0] + sizeof(na_size_t);

#ifdef NA_HAS_SM
        /* Only add SM serialized handles if we're sending over SM, otherwise
         * skip it. */
        if ((flags & HG_BULK_SM) &&
            (hg_bulk->na_sm_mem_descs.handles.s[0] != NA_MEM_HANDLE_NULL))
            ret += hg_bulk->na_sm_mem_descs.serialize_sizes.s[0] +
                   sizeof(na_size_t);
#endif
    } else {
        ret += hg_bulk_get_serialize_size_mem_descs(
            &hg_bulk->na_mem_descs, hg_bulk->desc.info.segment_count);

#ifdef NA_HAS_SM
        /* Only add SM serialized handles if we're sending over SM, otherwise
         * skip it. */
        if ((flags & HG_BULK_SM) && hg_bulk->na_sm_class)
            ret += hg_bulk_get_serialize_size_mem_descs(
                &hg_bulk->na_sm_mem_descs, hg_bulk->desc.info.segment_count);
#endif
    }

    /* Address information (context ID + serialize size + address) */
    if (hg_bulk->desc.info.flags & HG_BULK_BIND) {
        unsigned long addr_flags = 0;

#ifdef NA_HAS_SM
        if (flags & HG_BULK_SM)
            addr_flags |= HG_CORE_SM;
#endif
        ret += sizeof(hg_uint8_t) + sizeof(hg_size_t) +
               HG_Core_addr_get_serialize_size(hg_bulk->addr, addr_flags);
    }

    /* Eager mode (in eager mode, the actual data will be copied) */
    if ((flags & HG_BULK_EAGER) &&
        (hg_bulk->desc.info.flags & HG_BULK_READ_ONLY) &&
        !(hg_bulk->desc.info.flags & HG_BULK_VIRT))
        ret += hg_bulk->desc.info.len;

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_size_t
hg_bulk_get_serialize_size_mem_descs(
    struct hg_bulk_na_mem_desc *na_mem_descs, hg_uint32_t count)
{
    na_mem_handle_t *na_mem_handles;
    na_size_t *na_mem_serialize_sizes;
    hg_uint32_t i;
    hg_size_t ret = 0;

    if (count > HG_BULK_STATIC_MAX) {
        na_mem_handles = na_mem_descs->handles.d;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.d;
    } else {
        na_mem_handles = na_mem_descs->handles.s;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.s;
    }

    /* Serialize sizes */
    ret += count * sizeof(na_size_t);

    for (i = 0; i < count; i++)
        if (na_mem_handles[i] != NA_MEM_HANDLE_NULL)
            ret += na_mem_serialize_sizes[i];

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_serialize(
    void *buf, hg_size_t buf_size, hg_uint8_t flags, struct hg_bulk *hg_bulk)
{
    struct hg_bulk_segment *segments = HG_BULK_SEGMENTS(hg_bulk);
    char *buf_ptr = (char *) buf;
    hg_size_t buf_size_left = buf_size;
    struct hg_bulk_desc_info desc_info = hg_bulk->desc.info;
    hg_return_t ret = HG_SUCCESS;

    /* Always reset bulk alloc flag (only local) */
    desc_info.flags &= (~HG_BULK_ALLOC & 0xff);

    /* Add eager flag to descriptor */
    if ((flags & HG_BULK_EAGER) && (desc_info.flags & HG_BULK_READ_ONLY) &&
        !(hg_bulk->desc.info.flags & HG_BULK_VIRT)) {
        HG_LOG_DEBUG("HG_BULK_EAGER flag set");
        desc_info.flags |= HG_BULK_EAGER;
    } else
        desc_info.flags &= (~HG_BULK_EAGER & 0xff);

#ifdef NA_HAS_SM
    /* Add SM flag */
    if (flags & HG_BULK_SM) {
        HG_LOG_DEBUG("HG_BULK_SM flag set");
        desc_info.flags |= HG_BULK_SM;
    } else
        desc_info.flags &= (~HG_BULK_SM & 0xff);
#endif

    HG_LOG_DEBUG("Serializing bulk handle with %u segment(s), len is %" PRIu64
                 " bytes",
        hg_bulk->desc.info.segment_count, hg_bulk->desc.info.len);

    /* Descriptor info */
    HG_BULK_ENCODE(done, ret, buf_ptr, buf_size_left, &desc_info,
        struct hg_bulk_desc_info);

    /* Segments */
    HG_BULK_ENCODE_ARRAY(done, ret, buf_ptr, buf_size_left, segments,
        struct hg_bulk_segment, desc_info.segment_count);

    /* TODO if eager or self flag, skip mem handles ? */

    /* Add the NA memory handles */
    if ((desc_info.flags & HG_BULK_REGV) || (desc_info.segment_count == 1)) {
        /* N.B. skip serialize size if no handle */
        if (hg_bulk->na_mem_descs.handles.s[0] != NA_MEM_HANDLE_NULL) {
            na_return_t na_ret;

            HG_LOG_DEBUG("Serializing single NA memory handle");

            HG_BULK_ENCODE(done, ret, buf_ptr, buf_size_left,
                &hg_bulk->na_mem_descs.serialize_sizes.s[0], na_size_t);

            na_ret = NA_Mem_handle_serialize(hg_bulk->na_class, buf_ptr,
                buf_size_left, hg_bulk->na_mem_descs.handles.s[0]);
            HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret,
                (hg_return_t) na_ret, "Could not serialize memory handle (%s)",
                NA_Error_to_string(na_ret));
            buf_ptr += hg_bulk->na_mem_descs.serialize_sizes.s[0];
            buf_size_left -= hg_bulk->na_mem_descs.serialize_sizes.s[0];
        }

#ifdef NA_HAS_SM
        /* Only add SM serialized handles if we're sending over SM, otherwise
         * skip then. */
        if ((desc_info.flags & HG_BULK_SM) &&
            (hg_bulk->na_sm_mem_descs.handles.s[0] != NA_MEM_HANDLE_NULL)) {
            na_return_t na_ret;

            HG_BULK_ENCODE(done, ret, buf_ptr, buf_size_left,
                &hg_bulk->na_sm_mem_descs.serialize_sizes.s[0], na_size_t);

            na_ret = NA_Mem_handle_serialize(hg_bulk->na_sm_class, buf_ptr,
                buf_size_left, hg_bulk->na_sm_mem_descs.handles.s[0]);
            HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret,
                (hg_return_t) na_ret,
                "Could not serialize SM memory handle (%s)",
                NA_Error_to_string(na_ret));
            buf_ptr += hg_bulk->na_sm_mem_descs.serialize_sizes.s[0];
            buf_size_left -= hg_bulk->na_sm_mem_descs.serialize_sizes.s[0];
        }
#endif
    } else {
        HG_LOG_DEBUG("Serializing %u NA memory handle(s)",
            hg_bulk->desc.info.segment_count);

        ret = hg_bulk_serialize_mem_descs(hg_bulk->na_class, &buf_ptr,
            &buf_size_left, &hg_bulk->na_mem_descs, segments,
            desc_info.segment_count);
        HG_CHECK_HG_ERROR(done, ret, "Could not serialize NA mem descriptors");

#ifdef NA_HAS_SM
        /* Only add SM serialized handles if we're sending over SM, otherwise
         * skip it. */
        if ((desc_info.flags & HG_BULK_SM) && hg_bulk->na_sm_class) {
            ret = hg_bulk_serialize_mem_descs(hg_bulk->na_sm_class, &buf_ptr,
                &buf_size_left, &hg_bulk->na_sm_mem_descs, segments,
                desc_info.segment_count);
            HG_CHECK_HG_ERROR(
                done, ret, "Could not serialize NA SM mem descriptors");
        }
#endif
    }

    /* Address information */
    if (desc_info.flags & HG_BULK_BIND) {
        hg_size_t serialize_size;
        unsigned long addr_flags = 0;

        HG_LOG_DEBUG("HG_BULK_BIND flag set, serializing address information");

#ifdef NA_HAS_SM
        if (flags & HG_BULK_SM)
            addr_flags |= HG_CORE_SM;
#endif

        serialize_size =
            HG_Core_addr_get_serialize_size(hg_bulk->addr, addr_flags);

        HG_BULK_ENCODE(
            done, ret, buf_ptr, buf_size_left, &serialize_size, hg_size_t);

        ret = HG_Core_addr_serialize(
            buf_ptr, buf_size_left, addr_flags, hg_bulk->addr);
        HG_CHECK_HG_ERROR(done, ret, "Could not serialize address");
        buf_ptr += serialize_size;
        buf_size_left -= serialize_size;

        /* Add context ID */
        HG_BULK_ENCODE(done, ret, buf_ptr, buf_size_left, &hg_bulk->context_id,
            hg_uint8_t);
    }

    /* Add the serialized data if eager mode is requested */
    if (desc_info.flags & HG_BULK_EAGER) {
        hg_uint32_t i;

        HG_LOG_DEBUG("Serializing eager bulk data, %u segment(s)",
            hg_bulk->desc.info.segment_count);
        for (i = 0; i < desc_info.segment_count; i++) {
            if (!segments[i].len)
                continue;

            HG_BULK_ENCODE_ARRAY(done, ret, buf_ptr, buf_size_left,
                (const void *) segments[i].base, char, segments[i].len);
        }
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_serialize_mem_descs(na_class_t *na_class, char **buf_ptr,
    hg_size_t *buf_size_left, struct hg_bulk_na_mem_desc *na_mem_descs,
    const struct hg_bulk_segment *segments, hg_uint32_t count)
{
    na_mem_handle_t *na_mem_handles;
    na_size_t *na_mem_serialize_sizes;
    hg_return_t ret = HG_SUCCESS;
    hg_uint32_t i;

    if (count > HG_BULK_STATIC_MAX) {
        na_mem_handles = na_mem_descs->handles.d;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.d;
    } else {
        na_mem_handles = na_mem_descs->handles.s;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.s;
    }

    /* Encode serialize sizes */
    HG_BULK_ENCODE_ARRAY(done, ret, *buf_ptr, *buf_size_left,
        na_mem_serialize_sizes, na_size_t, count);

    for (i = 0; i < count; i++) {
        na_return_t na_ret;

        /* Skip null segments */
        if (segments[i].base == (hg_ptr_t) NULL)
            continue;

        na_ret = NA_Mem_handle_serialize(
            na_class, *buf_ptr, *buf_size_left, na_mem_handles[i]);
        HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret, (hg_return_t) na_ret,
            "Could not serialize memory handle (%s)",
            NA_Error_to_string(na_ret));

        *buf_ptr += na_mem_serialize_sizes[i];
        *buf_size_left -= na_mem_serialize_sizes[i];
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_deserialize(hg_core_class_t *core_class, struct hg_bulk **hg_bulk_ptr,
    const void *buf, hg_size_t buf_size)
{
    struct hg_bulk *hg_bulk = NULL;
    struct hg_bulk_segment *segments;
    const char *buf_ptr = (const char *) buf;
    hg_size_t buf_size_left = buf_size;
    hg_return_t ret = HG_SUCCESS;

    hg_bulk = (struct hg_bulk *) malloc(sizeof(struct hg_bulk));
    HG_CHECK_ERROR(
        hg_bulk == NULL, error, ret, HG_NOMEM, "Could not allocate handle");

    memset(hg_bulk, 0, sizeof(struct hg_bulk));
    hg_bulk->core_class = core_class;
    hg_bulk->na_class = HG_Core_class_get_na(core_class);
    hg_bulk->registered = HG_FALSE;
    hg_atomic_init32(&hg_bulk->ref_count, 1);

    /* Descriptor info */
    HG_BULK_DECODE(error, ret, buf_ptr, buf_size_left, &hg_bulk->desc.info,
        struct hg_bulk_desc_info);

    HG_LOG_DEBUG("Deserializing bulk handle with %u segment(s), len is %" PRIu64
                 " bytes",
        hg_bulk->desc.info.segment_count, hg_bulk->desc.info.len);

#ifdef NA_HAS_SM
    /* Use SM classes if requested */
    if (hg_bulk->desc.info.flags & HG_BULK_SM) {
        HG_LOG_DEBUG("HG_BULK_SM flag is set");
        hg_bulk->na_sm_class = HG_Core_class_get_na_sm(core_class);
        HG_CHECK_ERROR(hg_bulk->na_sm_class == NULL, error, ret,
            HG_PROTOCOL_ERROR, "SM class is not set");
    }
#endif

    /* Segments */
    if (hg_bulk->desc.info.segment_count > HG_BULK_STATIC_MAX) {
        /* Allocate segments */
        hg_bulk->desc.segments.d = (struct hg_bulk_segment *) calloc(
            hg_bulk->desc.info.segment_count, sizeof(struct hg_bulk_segment));
        HG_CHECK_ERROR(hg_bulk->desc.segments.d == NULL, error, ret, HG_NOMEM,
            "Could not allocate segment array");

        segments = hg_bulk->desc.segments.d;
    } else
        segments = hg_bulk->desc.segments.s;
    HG_BULK_DECODE_ARRAY(error, ret, buf_ptr, buf_size_left, segments,
        struct hg_bulk_segment, hg_bulk->desc.info.segment_count);

    /* Get the NA memory handles */
    if (hg_bulk->desc.info.flags & HG_BULK_REGV ||
        (hg_bulk->desc.info.segment_count == 1)) {
        /* Always deserialize handle if HG_BULK_REGV is set */
        if ((segments[0].base != (hg_ptr_t) NULL) ||
            (hg_bulk->desc.info.flags & HG_BULK_REGV)) {
            na_return_t na_ret;

            HG_LOG_DEBUG("Deserializing single NA memory handle");

            HG_BULK_DECODE(error, ret, buf_ptr, buf_size_left,
                &hg_bulk->na_mem_descs.serialize_sizes.s[0], na_size_t);

            na_ret = NA_Mem_handle_deserialize(hg_bulk->na_class,
                &hg_bulk->na_mem_descs.handles.s[0], buf_ptr, buf_size_left);
            HG_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret,
                (hg_return_t) na_ret,
                "Could not deserialize memory handle (%s)",
                NA_Error_to_string(na_ret));
            buf_ptr += hg_bulk->na_mem_descs.serialize_sizes.s[0];
            buf_size_left -= hg_bulk->na_mem_descs.serialize_sizes.s[0];

#ifdef NA_HAS_SM
            /* Only deserialize handles if we were sending over SM */
            if (hg_bulk->desc.info.flags & HG_BULK_SM) {
                HG_BULK_DECODE(error, ret, buf_ptr, buf_size_left,
                    &hg_bulk->na_sm_mem_descs.serialize_sizes.s[0], na_size_t);

                na_ret = NA_Mem_handle_deserialize(hg_bulk->na_sm_class,
                    &hg_bulk->na_sm_mem_descs.handles.s[0], buf_ptr,
                    buf_size_left);
                HG_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret,
                    (hg_return_t) na_ret,
                    "Could not deserialize SM memory handle (%s)",
                    NA_Error_to_string(na_ret));
                buf_ptr += hg_bulk->na_sm_mem_descs.serialize_sizes.s[0];
                buf_size_left -= hg_bulk->na_sm_mem_descs.serialize_sizes.s[0];
            }
#endif
        }
    } else {
        HG_LOG_DEBUG("Deserializing %u NA memory handle(s)",
            hg_bulk->desc.info.segment_count);

        ret = hg_bulk_deserialize_mem_descs(hg_bulk->na_class, &buf_ptr,
            &buf_size_left, &hg_bulk->na_mem_descs, segments,
            hg_bulk->desc.info.segment_count);
        HG_CHECK_HG_ERROR(
            error, ret, "Could not deserialize NA mem descriptors");

#ifdef NA_HAS_SM
        /* Only deserialize handles if we were sending over SM */
        if (hg_bulk->desc.info.flags & HG_BULK_SM) {
            ret = hg_bulk_deserialize_mem_descs(hg_bulk->na_sm_class, &buf_ptr,
                &buf_size_left, &hg_bulk->na_sm_mem_descs, segments,
                hg_bulk->desc.info.segment_count);
            HG_CHECK_HG_ERROR(
                error, ret, "Could not deserialize NA SM mem descriptors");
        }
#endif
    }

    /* Address information */
    if (hg_bulk->desc.info.flags & HG_BULK_BIND) {
        hg_size_t serialize_size;

        HG_LOG_DEBUG(
            "HG_BULK_BIND flag set, deserializing address information");

        HG_BULK_DECODE(
            error, ret, buf_ptr, buf_size_left, &serialize_size, hg_size_t);

        ret = HG_Core_addr_deserialize(
            hg_bulk->core_class, &hg_bulk->addr, buf_ptr, buf_size_left);
        HG_CHECK_HG_ERROR(error, ret, "Could not deserialize address");
        buf_ptr += serialize_size;
        buf_size_left -= serialize_size;

        /* Get context ID */
        HG_BULK_DECODE(error, ret, buf_ptr, buf_size_left, &hg_bulk->context_id,
            hg_uint8_t);
    }

    /* Get the serialized data */
    if (hg_bulk->desc.info.flags & HG_BULK_EAGER) {
        hg_uint32_t i;

        HG_LOG_DEBUG("Deserializing eager bulk data, %u segment(s)",
            hg_bulk->desc.info.segment_count);
        hg_bulk->desc.info.flags |= HG_BULK_ALLOC;
        for (i = 0; i < hg_bulk->desc.info.segment_count; i++) {
            if (!segments[i].len)
                continue;

            /* Override base address to store data */
            segments[i].base = (hg_ptr_t) calloc(1, segments[i].len);
            HG_CHECK_ERROR(segments[i].base == (hg_ptr_t) NULL, error, ret,
                HG_NOMEM, "Could not allocate segment");

            HG_BULK_DECODE_ARRAY(error, ret, buf_ptr, buf_size_left,
                (void *) segments[i].base, char, segments[i].len);
        }
    } else
        /* Addresses are virtual and do not point to physical memory */
        hg_bulk->desc.info.flags |= HG_BULK_VIRT;

    HG_CHECK_WARNING(buf_size_left != 0,
        "Buffer size left for decoding bulk handle is not zero");

    *hg_bulk_ptr = hg_bulk;

    return ret;

error:
    hg_bulk_free(hg_bulk);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_deserialize_mem_descs(na_class_t *na_class, const char **buf_ptr,
    hg_size_t *buf_size_left, struct hg_bulk_na_mem_desc *na_mem_descs,
    const struct hg_bulk_segment *segments, hg_uint32_t count)
{
    na_mem_handle_t *na_mem_handles;
    na_size_t *na_mem_serialize_sizes;
    hg_return_t ret = HG_SUCCESS;
    hg_uint32_t i;

    if (count > HG_BULK_STATIC_MAX) {
        /* Allocate NA memory handles */
        na_mem_descs->handles.d =
            (na_mem_handle_t *) calloc(count, sizeof(na_mem_handle_t));
        HG_CHECK_ERROR(na_mem_descs->handles.d == NULL, error, ret, HG_NOMEM,
            "Could not allocate mem handle array");

        /* Allocate serialize sizes */
        na_mem_descs->serialize_sizes.d =
            (na_size_t *) calloc(count, sizeof(na_size_t));
        HG_CHECK_ERROR(na_mem_descs->serialize_sizes.d == NULL, error, ret,
            HG_NOMEM, "Could not allocate serialize sizes array");

        na_mem_handles = na_mem_descs->handles.d;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.d;
    } else {
        na_mem_handles = na_mem_descs->handles.s;
        na_mem_serialize_sizes = na_mem_descs->serialize_sizes.s;
    }

    /* Decode serialize sizes */
    HG_BULK_DECODE_ARRAY(error, ret, *buf_ptr, *buf_size_left,
        na_mem_serialize_sizes, na_size_t, count);

    for (i = 0; i < count; i++) {
        na_return_t na_ret;

        /* Skip null segments */
        if (segments[i].base == (hg_ptr_t) NULL)
            continue;

        na_ret = NA_Mem_handle_deserialize(
            na_class, &na_mem_handles[i], *buf_ptr, *buf_size_left);
        HG_CHECK_ERROR(na_ret != NA_SUCCESS, error, ret, (hg_return_t) na_ret,
            "Could not deserialize memory handle (%s)",
            NA_Error_to_string(na_ret));

        *buf_ptr += na_mem_serialize_sizes[i];
        *buf_size_left -= na_mem_serialize_sizes[i];
    }

    return ret;

error:
    return ret;
}

/*---------------------------------------------------------------------------*/
void *
hg_bulk_get_serialize_cached_ptr(struct hg_bulk *hg_bulk)
{
    return hg_bulk->serialize_ptr;
}

/*---------------------------------------------------------------------------*/
hg_size_t
hg_bulk_get_serialize_cached_size(struct hg_bulk *hg_bulk)
{
    return hg_bulk->serialize_size;
}

/*---------------------------------------------------------------------------*/
void
hg_bulk_set_serialize_cached_ptr(
    struct hg_bulk *hg_bulk, void *buf, na_size_t buf_size)
{
    hg_bulk->serialize_ptr = buf;
    hg_bulk->serialize_size = buf_size;
}

/*---------------------------------------------------------------------------*/
static void
hg_bulk_access(struct hg_bulk *hg_bulk, hg_size_t offset, hg_size_t size,
    hg_uint8_t flags, hg_uint32_t max_count, void **buf_ptrs,
    hg_size_t *buf_sizes, hg_uint32_t *actual_count)
{
    struct hg_bulk_segment *segments = HG_BULK_SEGMENTS(hg_bulk);
    hg_uint32_t segment_index;
    hg_size_t segment_offset;
    hg_size_t remaining_size = size;
    hg_uint32_t count = 0;

    /* TODO use flags */
    (void) flags;

    hg_bulk_offset_translate(segments, hg_bulk->desc.info.segment_count, offset,
        &segment_index, &segment_offset);

    while ((remaining_size > 0) && (count < max_count)) {
        hg_ptr_t base;
        hg_size_t len;

        /* Can only transfer smallest size */
        len = segments[segment_index].len - segment_offset;

        /* Remaining size may be smaller */
        len = HG_BULK_MIN(remaining_size, len);
        base = segments[segment_index].base + (hg_ptr_t) segment_offset;

        /* Fill segments */
        if (buf_ptrs)
            buf_ptrs[count] = (void *) base;
        if (buf_sizes)
            buf_sizes[count] = len;
        /*
        printf("Segment %d: address=0x%lX\n", count, segment_address);
        printf("Segment %d: size=%zu\n", count, segment_size);
         */

        /* Decrease remaining size from the size of data we transferred */
        remaining_size -= len;

        /* Change segment */
        segment_index++;
        segment_offset = 0;
        count++;
    }

    if (actual_count)
        *actual_count = count;
}

/*---------------------------------------------------------------------------*/
static HG_INLINE void
hg_bulk_offset_translate(const struct hg_bulk_segment *segments,
    hg_uint32_t count, hg_size_t offset, hg_uint32_t *segment_start_index,
    hg_size_t *segment_start_offset)
{
    hg_uint32_t i, new_segment_start_index = 0;
    hg_size_t new_segment_offset = offset, next_offset = 0;

    /* Get start index and handle offset */
    for (i = 0; i < count; i++) {
        next_offset += segments[i].len;
        if (offset < next_offset) {
            new_segment_start_index = i;
            break;
        }
        new_segment_offset -= segments[i].len;
    }

    *segment_start_index = new_segment_start_index;
    *segment_start_offset = new_segment_offset;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_op_create(
    hg_core_context_t *core_context, struct hg_bulk_op_id **hg_bulk_op_id_ptr)
{
    struct hg_bulk_op_id *hg_bulk_op_id = NULL;
    hg_return_t ret = HG_SUCCESS;
    int i;

    hg_bulk_op_id =
        (struct hg_bulk_op_id *) malloc(sizeof(struct hg_bulk_op_id));
    HG_CHECK_ERROR(hg_bulk_op_id == NULL, error, ret, HG_NOMEM,
        "Could not allocate HG Bulk operation ID");
    memset(hg_bulk_op_id, 0, sizeof(struct hg_bulk_op_id));

    hg_bulk_op_id->core_context = core_context;
    hg_atomic_init32(&hg_bulk_op_id->ref_count, 1);

    /* Completed by default */
    hg_atomic_init32(&hg_bulk_op_id->status, HG_BULK_OP_COMPLETED);

    hg_bulk_op_id->callback_info.type = HG_CB_BULK;
    hg_bulk_op_id->op_count = 1; /* Default */
    hg_atomic_init32(&hg_bulk_op_id->op_completed_count, 0);

    /* Preallocate NA OP IDs */
    for (i = 0; i < HG_BULK_STATIC_MAX; i++) {
        hg_bulk_op_id->na_op_ids.s[i] =
            NA_Op_create(core_context->core_class->na_class);
        HG_CHECK_ERROR(hg_bulk_op_id->na_op_ids.s[i] == NULL, error, ret,
            HG_NA_ERROR, "NA_Op_create() failed");
    }
#ifdef NA_HAS_SM
    if (core_context->core_class->na_sm_class) {
        for (i = 0; i < HG_BULK_STATIC_MAX; i++) {
            hg_bulk_op_id->na_sm_op_ids.s[i] =
                NA_Op_create(core_context->core_class->na_sm_class);
            HG_CHECK_ERROR(hg_bulk_op_id->na_sm_op_ids.s[i] == NULL, error, ret,
                HG_NA_ERROR, "NA_Op_create() failed");
        }
    }
#endif

    HG_LOG_DEBUG("Created new bulk op ID (%p)", (void *) hg_bulk_op_id);

    *hg_bulk_op_id_ptr = hg_bulk_op_id;

    return ret;

error:
    if (hg_bulk_op_id) {
        for (i = 0; i < HG_BULK_STATIC_MAX; i++) {
            na_return_t na_ret;

            if (hg_bulk_op_id->na_op_ids.s[i] == NULL)
                continue;

            na_ret = NA_Op_destroy(core_context->core_class->na_class,
                hg_bulk_op_id->na_op_ids.s[i]);
            HG_CHECK_ERROR_DONE(na_ret != NA_SUCCESS, "NA_Op_destroy() failed");
        }
#ifdef NA_HAS_SM
        for (i = 0; i < HG_BULK_STATIC_MAX; i++) {
            na_return_t na_ret;

            if (hg_bulk_op_id->na_sm_op_ids.s[i] == NULL)
                continue;

            na_ret = NA_Op_destroy(core_context->core_class->na_sm_class,
                hg_bulk_op_id->na_sm_op_ids.s[i]);
            HG_CHECK_ERROR_DONE(na_ret != NA_SUCCESS, "NA_Op_destroy() failed");
        }
#endif
        free(hg_bulk_op_id);
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_op_destroy(struct hg_bulk_op_id *hg_bulk_op_id)
{
    hg_return_t ret = HG_SUCCESS;
    hg_uint32_t i;

    if (hg_atomic_decr32(&hg_bulk_op_id->ref_count)) {
        /* Cannot free yet */
        goto done;
    }

    /* We may have used extra op IDs if this NA class was used */
    if (hg_bulk_op_id->na_class &&
        hg_bulk_op_id->op_count > HG_BULK_STATIC_MAX) {
        na_op_id_t **na_op_ids = NULL;
#ifdef NA_HAS_SM
        if (hg_bulk_op_id->na_class ==
            hg_bulk_op_id->core_context->core_class->na_sm_class)
            na_op_ids = hg_bulk_op_id->na_sm_op_ids.d;
        else
#endif
            na_op_ids = hg_bulk_op_id->na_op_ids.d;

        if (na_op_ids) {
            for (i = 0; i < hg_bulk_op_id->op_count; i++) {
                na_return_t na_ret;

                if (na_op_ids[i] == NULL)
                    continue;

                na_ret = NA_Op_destroy(hg_bulk_op_id->na_class, na_op_ids[i]);
                HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret,
                    (hg_return_t) na_ret, "NA_Op_destroy() failed (%s)",
                    NA_Error_to_string(na_ret));
            }
            free(na_op_ids);
            hg_bulk_op_id->na_op_ids.d = NULL;
#ifdef NA_HAS_SM
            hg_bulk_op_id->na_sm_op_ids.d = NULL;
#endif
        }
    }

    /* Repost handle if we were listening, otherwise destroy it */
    if (hg_bulk_op_id->reuse) {
        HG_LOG_DEBUG("Re-using bulk op ID (%p)", (void *) hg_bulk_op_id);

        /* Reset ref_count */
        hg_atomic_set32(&hg_bulk_op_id->ref_count, 1);

        /* Reset status */
        hg_atomic_set32(&hg_bulk_op_id->status, HG_BULK_OP_COMPLETED);

        hg_thread_spin_lock(&hg_bulk_op_id->op_pool->pending_list_lock);
        HG_LIST_INSERT_HEAD(
            &hg_bulk_op_id->op_pool->pending_list, hg_bulk_op_id, pending);
        hg_thread_spin_unlock(&hg_bulk_op_id->op_pool->pending_list_lock);
    } else {
        HG_LOG_DEBUG("Freeing bulk op ID (%p)", (void *) hg_bulk_op_id);

        for (i = 0; i < HG_BULK_STATIC_MAX; i++) {
            na_return_t na_ret;

            if (hg_bulk_op_id->na_op_ids.s[i] == NULL)
                continue;

            na_ret =
                NA_Op_destroy(hg_bulk_op_id->core_context->core_class->na_class,
                    hg_bulk_op_id->na_op_ids.s[i]);
            HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret,
                (hg_return_t) na_ret, "NA_Op_destroy() failed (%s)",
                NA_Error_to_string(na_ret));
        }

#ifdef NA_HAS_SM
        for (i = 0; i < HG_BULK_STATIC_MAX; i++) {
            na_return_t na_ret;

            if (hg_bulk_op_id->na_sm_op_ids.s[i] == NULL)
                continue;

            na_ret = NA_Op_destroy(
                hg_bulk_op_id->core_context->core_class->na_sm_class,
                hg_bulk_op_id->na_sm_op_ids.s[i]);
            HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret,
                (hg_return_t) na_ret, "NA_Op_destroy() failed (%s)",
                NA_Error_to_string(na_ret));
        }
#endif

        free(hg_bulk_op_id);
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_bulk_op_pool_create(hg_core_context_t *core_context, unsigned int init_count,
    struct hg_bulk_op_pool **hg_bulk_op_pool_ptr)
{
    struct hg_bulk_op_pool *hg_bulk_op_pool = NULL;
    hg_return_t ret = HG_SUCCESS;
    unsigned int i;

    HG_LOG_DEBUG("Creating pool with %u bulk op IDs", init_count);

    hg_bulk_op_pool =
        (struct hg_bulk_op_pool *) malloc(sizeof(struct hg_bulk_op_pool));
    HG_CHECK_ERROR(hg_bulk_op_pool == NULL, error, ret, HG_NOMEM,
        "Could not allocate bulk op pool");

    hg_thread_mutex_init(&hg_bulk_op_pool->extend_mutex);
    hg_thread_cond_init(&hg_bulk_op_pool->extend_cond);
    hg_bulk_op_pool->core_context = core_context;
    HG_LIST_INIT(&hg_bulk_op_pool->pending_list);
    hg_thread_spin_init(&hg_bulk_op_pool->pending_list_lock);
    hg_bulk_op_pool->count = init_count;
    hg_bulk_op_pool->extending = HG_FALSE;

    for (i = 0; i < init_count; i++) {
        struct hg_bulk_op_id *hg_bulk_op_id = NULL;

        ret = hg_bulk_op_create(core_context, &hg_bulk_op_id);
        HG_CHECK_HG_ERROR(error, ret, "Could not create bulk op ID");

        hg_bulk_op_id->reuse = HG_TRUE;
        hg_bulk_op_id->op_pool = hg_bulk_op_pool;

        hg_thread_spin_lock(&hg_bulk_op_pool->pending_list_lock);
        HG_LIST_INSERT_HEAD(
            &hg_bulk_op_pool->pending_list, hg_bulk_op_id, pending);
        hg_thread_spin_unlock(&hg_bulk_op_pool->pending_list_lock);
    }

    HG_LOG_DEBUG("Created bulk op ID pool (%p)", (void *) hg_bulk_op_pool);

    *hg_bulk_op_pool_ptr = hg_bulk_op_pool;

    return ret;

error:
    if (hg_bulk_op_pool)
        hg_bulk_op_pool_destroy(hg_bulk_op_pool);
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_bulk_op_pool_destroy(struct hg_bulk_op_pool *hg_bulk_op_pool)
{
    struct hg_bulk_op_id *hg_bulk_op_id = NULL;
    hg_return_t ret = HG_SUCCESS;

    HG_LOG_DEBUG("Free bulk op ID pool (%p)", (void *) hg_bulk_op_pool);

    hg_thread_spin_lock(&hg_bulk_op_pool->pending_list_lock);

    hg_bulk_op_id = HG_LIST_FIRST(&hg_bulk_op_pool->pending_list);

    while (hg_bulk_op_id) {
        struct hg_bulk_op_id *hg_bulk_op_id_next =
            HG_LIST_NEXT(hg_bulk_op_id, pending);
        HG_LIST_REMOVE(hg_bulk_op_id, pending);

        /* Prevent re-initialization */
        hg_bulk_op_id->reuse = HG_FALSE;

        /* Destroy op IDs */
        ret = hg_bulk_op_destroy(hg_bulk_op_id);
        HG_CHECK_HG_ERROR(unlock, ret, "Could not destroy bulk op ID");

        hg_bulk_op_id = hg_bulk_op_id_next;
    }
    hg_thread_spin_unlock(&hg_bulk_op_pool->pending_list_lock);

    hg_thread_mutex_destroy(&hg_bulk_op_pool->extend_mutex);
    hg_thread_cond_destroy(&hg_bulk_op_pool->extend_cond);
    hg_thread_spin_destroy(&hg_bulk_op_pool->pending_list_lock);

    free(hg_bulk_op_pool);

    return ret;

unlock:
    hg_thread_spin_unlock(&hg_bulk_op_pool->pending_list_lock);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_op_pool_get(struct hg_bulk_op_pool *hg_bulk_op_pool,
    struct hg_bulk_op_id **hg_bulk_op_id_ptr)
{
    struct hg_bulk_op_id *hg_bulk_op_id = NULL;
    hg_return_t ret = HG_SUCCESS;

    do {
        unsigned int i;

        hg_thread_spin_lock(&hg_bulk_op_pool->pending_list_lock);
        if ((hg_bulk_op_id = HG_LIST_FIRST(&hg_bulk_op_pool->pending_list)))
            HG_LIST_REMOVE(hg_bulk_op_id, pending);
        hg_thread_spin_unlock(&hg_bulk_op_pool->pending_list_lock);

        if (hg_bulk_op_id)
            break;

        /* Create another batch of IDs if empty */
        hg_thread_mutex_lock(&hg_bulk_op_pool->extend_mutex);
        if (hg_bulk_op_pool->extending) {
            hg_thread_cond_wait(
                &hg_bulk_op_pool->extend_cond, &hg_bulk_op_pool->extend_mutex);
            hg_thread_mutex_unlock(&hg_bulk_op_pool->extend_mutex);
            continue;
        }
        hg_bulk_op_pool->extending = HG_TRUE;
        hg_thread_mutex_unlock(&hg_bulk_op_pool->extend_mutex);

        /* Only a single thread can extend the pool */
        for (i = 0; i < hg_bulk_op_pool->count; i++) {
            struct hg_bulk_op_id *new_op_id = NULL;

            ret = hg_bulk_op_create(hg_bulk_op_pool->core_context, &new_op_id);
            HG_CHECK_HG_ERROR(error, ret, "Could not create bulk op ID");

            new_op_id->reuse = HG_TRUE;
            new_op_id->op_pool = hg_bulk_op_pool;

            hg_thread_spin_lock(&hg_bulk_op_pool->pending_list_lock);
            HG_LIST_INSERT_HEAD(
                &hg_bulk_op_pool->pending_list, new_op_id, pending);
            hg_thread_spin_unlock(&hg_bulk_op_pool->pending_list_lock);
        }
        hg_bulk_op_pool->count *= 2;

        hg_thread_mutex_lock(&hg_bulk_op_pool->extend_mutex);
        hg_bulk_op_pool->extending = HG_FALSE;
        hg_thread_cond_broadcast(&hg_bulk_op_pool->extend_cond);
        hg_thread_mutex_unlock(&hg_bulk_op_pool->extend_mutex);
    } while (!hg_bulk_op_id);

    *hg_bulk_op_id_ptr = hg_bulk_op_id;

    return ret;

error:
    hg_thread_mutex_lock(&hg_bulk_op_pool->extend_mutex);
    hg_bulk_op_pool->extending = HG_FALSE;
    hg_thread_cond_broadcast(&hg_bulk_op_pool->extend_cond);
    hg_thread_mutex_unlock(&hg_bulk_op_pool->extend_mutex);

    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_transfer(hg_core_context_t *core_context, hg_cb_t callback, void *arg,
    hg_bulk_op_t op, struct hg_core_addr *origin_addr, hg_uint8_t origin_id,
    struct hg_bulk *hg_bulk_origin, hg_size_t origin_offset,
    struct hg_bulk *hg_bulk_local, hg_size_t local_offset, hg_size_t size,
    hg_op_id_t *op_id)
{
    const struct hg_bulk_segment *origin_segments =
        HG_BULK_SEGMENTS(hg_bulk_origin);
    const struct hg_bulk_segment *local_segments =
        HG_BULK_SEGMENTS(hg_bulk_local);
    hg_uint32_t origin_count = hg_bulk_origin->desc.info.segment_count,
                local_count = hg_bulk_local->desc.info.segment_count;
    uint8_t origin_flags = hg_bulk_origin->desc.info.flags;
    uint8_t local_flags = hg_bulk_local->desc.info.flags;
    struct hg_bulk_op_id *hg_bulk_op_id = NULL;
    struct hg_bulk_op_pool *hg_bulk_op_pool =
        hg_core_context_get_bulk_op_pool(core_context);
    hg_return_t ret = HG_SUCCESS;

    /* Get a new OP ID from context */
    if (hg_bulk_op_pool) {
        ret = hg_bulk_op_pool_get(hg_bulk_op_pool, &hg_bulk_op_id);
        HG_CHECK_HG_ERROR(error, ret, "Could not get bulk op ID");
    } else {
        ret = hg_bulk_op_create(core_context, &hg_bulk_op_id);
        HG_CHECK_HG_ERROR(error, ret, "Could not create bulk op ID");
    }

    hg_bulk_op_id->callback = callback;
    hg_bulk_op_id->callback_info.arg = arg;
    hg_bulk_op_id->callback_info.info.bulk.origin_handle = hg_bulk_origin;
    hg_atomic_incr32(&hg_bulk_origin->ref_count);
    hg_bulk_op_id->callback_info.info.bulk.local_handle = hg_bulk_local;
    hg_atomic_incr32(&hg_bulk_local->ref_count);
    hg_bulk_op_id->callback_info.info.bulk.op = op;
    hg_bulk_op_id->callback_info.info.bulk.size = size;

    /* Reset status */
    hg_atomic_set32(&hg_bulk_op_id->status, 0);
    hg_bulk_op_id->err_ret = HG_SUCCESS;

    /* Expected op count */
    hg_bulk_op_id->op_count = (size > 0) ? 1 : 0; /* Default */
    hg_atomic_set32(&hg_bulk_op_id->op_completed_count, 0);

    if (size == 0) {
        /* Complete immediately */
        ret = hg_bulk_complete(hg_bulk_op_id, HG_TRUE);
        HG_CHECK_HG_ERROR(error, ret, "Could not complete bulk operation");
    } else if (HG_Core_addr_is_self(origin_addr) ||
               ((origin_flags & HG_BULK_EAGER) && (op != HG_BULK_PUSH))) {
        hg_bulk_op_id->na_class = NULL;
        hg_bulk_op_id->na_context = NULL;

        /* When doing eager transfers, use self code path to copy data locally
         */
        ret = hg_bulk_transfer_self(op, origin_segments, origin_count,
            origin_offset, local_segments, local_count, local_offset, size,
            hg_bulk_op_id);
    } else {
        struct hg_bulk_na_mem_desc *origin_mem_descs, *local_mem_descs;
        na_mem_handle_t *origin_mem_handles, *local_mem_handles;
        na_addr_t na_origin_addr = NA_ADDR_NULL;

#ifdef NA_HAS_SM
        /* Use SM if we can */
        if (hg_bulk_origin->desc.info.flags & HG_BULK_SM) {
            HG_LOG_DEBUG("Using NA SM class for this transfer");

            hg_bulk_op_id->na_class = hg_bulk_origin->na_sm_class;
            hg_bulk_op_id->na_context = HG_Core_context_get_na_sm(core_context);
            na_origin_addr = HG_Core_addr_get_na_sm(origin_addr);
            origin_mem_descs = &hg_bulk_origin->na_sm_mem_descs;
            local_mem_descs = &hg_bulk_local->na_sm_mem_descs;
        } else {
#endif
            HG_LOG_DEBUG("Using default NA class for this transfer");

            hg_bulk_op_id->na_class = hg_bulk_origin->na_class;
            hg_bulk_op_id->na_context = HG_Core_context_get_na(core_context);
            na_origin_addr = HG_Core_addr_get_na(origin_addr);
            origin_mem_descs = &hg_bulk_origin->na_mem_descs;
            local_mem_descs = &hg_bulk_local->na_mem_descs;
#ifdef NA_HAS_SM
        }
#endif

        origin_mem_handles =
            HG_BULK_MEM_HANDLES(origin_mem_descs, origin_count, origin_flags);
        local_mem_handles =
            HG_BULK_MEM_HANDLES(local_mem_descs, local_count, local_flags);

        ret = hg_bulk_transfer_na(op, na_origin_addr, origin_id,
            origin_segments, origin_count, origin_mem_handles, origin_flags,
            origin_offset, local_segments, local_count, local_mem_handles,
            local_flags, local_offset, size, hg_bulk_op_id);
    }

    /* Assign op_id */
    if (op_id && op_id != HG_OP_ID_IGNORE)
        *op_id = (hg_op_id_t) hg_bulk_op_id;

    return ret;

error:
    if (hg_bulk_op_id) {
        hg_return_t hg_ret = hg_bulk_op_destroy(hg_bulk_op_id);
        HG_CHECK_ERROR_DONE(hg_ret != HG_SUCCESS, "Could not destroy op ID");
    }
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_transfer_self(hg_bulk_op_t op,
    const struct hg_bulk_segment *origin_segments, hg_uint32_t origin_count,
    hg_size_t origin_offset, const struct hg_bulk_segment *local_segments,
    hg_uint32_t local_count, hg_size_t local_offset, hg_size_t size,
    struct hg_bulk_op_id *hg_bulk_op_id)
{
    hg_uint32_t origin_segment_start_index = 0, local_segment_start_index = 0;
    hg_size_t origin_segment_start_offset = 0, local_segment_start_offset = 0;
    hg_bulk_copy_op_t copy_op;
    hg_return_t ret = HG_SUCCESS;

    switch (op) {
        case HG_BULK_PUSH:
            copy_op = hg_bulk_memcpy_put;
            break;
        case HG_BULK_PULL:
            copy_op = hg_bulk_memcpy_get;
            break;
        default:
            HG_GOTO_ERROR(done, ret, HG_INVALID_ARG, "Unknown bulk operation");
    }

    HG_LOG_DEBUG("Transferring data through self");

    /* Translate origin offset */
    if (origin_offset > 0)
        hg_bulk_offset_translate(origin_segments, origin_count, origin_offset,
            &origin_segment_start_index, &origin_segment_start_offset);

    /* Translate local offset */
    if (local_offset > 0)
        hg_bulk_offset_translate(local_segments, local_count, local_offset,
            &local_segment_start_index, &local_segment_start_offset);

    /* Do actual transfer */
    hg_bulk_transfer_segments_self(copy_op, origin_segments, origin_count,
        origin_segment_start_index, origin_segment_start_offset, local_segments,
        local_count, local_segment_start_index, local_segment_start_offset,
        size);

    /* Complete immediately */
    ret = hg_bulk_complete(hg_bulk_op_id, HG_TRUE);
    HG_CHECK_HG_ERROR(done, ret, "Could not complete bulk operation");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static void
hg_bulk_transfer_segments_self(hg_bulk_copy_op_t copy_op,
    const struct hg_bulk_segment *origin_segments, hg_uint32_t origin_count,
    hg_size_t origin_segment_start_index, hg_size_t origin_segment_start_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    hg_size_t local_segment_start_index, hg_size_t local_segment_start_offset,
    hg_size_t size)
{
    hg_size_t origin_segment_index = origin_segment_start_index;
    hg_size_t local_segment_index = local_segment_start_index;
    hg_size_t origin_segment_offset = origin_segment_start_offset;
    hg_size_t local_segment_offset = local_segment_start_offset;
    hg_size_t remaining_size = size;

    while (remaining_size > 0 && origin_segment_index < origin_count &&
           local_segment_index < local_count) {
        /* Can only transfer smallest size */
        hg_size_t transfer_size = HG_BULK_MIN(
            (origin_segments[origin_segment_index].len - origin_segment_offset),
            (local_segments[local_segment_index].len - local_segment_offset));

        /* Remaining size may be smaller */
        transfer_size = HG_BULK_MIN(remaining_size, transfer_size);

        /* Copy segment */
        copy_op(local_segments[local_segment_index].base, local_segment_offset,
            origin_segments[origin_segment_index].base, origin_segment_offset,
            transfer_size);

        /* Decrease remaining size from the size of data we transferred
         * and exit if everything has been transferred */
        remaining_size -= transfer_size;
        if (remaining_size == 0)
            break;

        /* Increment offsets from the size of data we transferred */
        origin_segment_offset += transfer_size;
        local_segment_offset += transfer_size;

        /* Change segment if new offset exceeds segment size */
        if (origin_segment_offset >=
            origin_segments[origin_segment_index].len) {
            origin_segment_index++;
            origin_segment_offset = 0;
        }
        if (local_segment_offset >= local_segments[local_segment_index].len) {
            local_segment_index++;
            local_segment_offset = 0;
        }
    }
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_transfer_na(hg_bulk_op_t op, na_addr_t na_origin_addr,
    hg_uint8_t origin_id, const struct hg_bulk_segment *origin_segments,
    hg_uint32_t origin_count, na_mem_handle_t *origin_mem_handles,
    hg_uint8_t origin_flags, hg_size_t origin_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    na_mem_handle_t *local_mem_handles, hg_uint8_t local_flags,
    hg_size_t local_offset, hg_size_t size, struct hg_bulk_op_id *hg_bulk_op_id)
{
    hg_bulk_na_op_id_t *hg_bulk_na_op_ids;
    na_bulk_op_t na_bulk_op;
    hg_return_t ret = HG_SUCCESS;

    /* Map op to NA op */
    switch (op) {
        case HG_BULK_PUSH:
            na_bulk_op = hg_bulk_na_put;
            break;
        case HG_BULK_PULL:
            na_bulk_op = hg_bulk_na_get;
            break;
        default:
            HG_GOTO_ERROR(done, ret, HG_INVALID_ARG, "Unknown bulk operation");
    }

#ifdef NA_HAS_SM
    /* Use NA SM op IDs if needed */
    if (origin_flags & HG_BULK_SM)
        hg_bulk_na_op_ids = &hg_bulk_op_id->na_sm_op_ids;
    else
#endif
        hg_bulk_na_op_ids = &hg_bulk_op_id->na_op_ids;

    if (((origin_flags & HG_BULK_REGV) || origin_count == 1) &&
        ((local_flags & HG_BULK_REGV) || local_count == 1)) {
        na_return_t na_ret;

        HG_LOG_DEBUG("Transferring data through NA in single operation");

        na_ret = na_bulk_op(hg_bulk_op_id->na_class, hg_bulk_op_id->na_context,
            hg_bulk_transfer_cb, hg_bulk_op_id, local_mem_handles[0],
            local_offset, origin_mem_handles[0], origin_offset, size,
            na_origin_addr, origin_id, hg_bulk_na_op_ids->s[0]);
        HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret, (hg_return_t) na_ret,
            "Could not transfer data (%s)", NA_Error_to_string(na_ret));
    } else {
        hg_uint32_t origin_segment_start_index = 0,
                    local_segment_start_index = 0;
        hg_size_t origin_segment_start_offset = 0,
                  local_segment_start_offset = 0;
        na_op_id_t **na_op_ids;

        /* Translate bulk_offset */
        if (origin_offset > 0)
            hg_bulk_offset_translate(origin_segments, origin_count,
                origin_offset, &origin_segment_start_index,
                &origin_segment_start_offset);

        /* Translate block offset */
        if (local_offset > 0)
            hg_bulk_offset_translate(local_segments, local_count, local_offset,
                &local_segment_start_index, &local_segment_start_offset);

        /* Determine number of NA operations that will be needed */
        hg_bulk_op_id->op_count = hg_bulk_transfer_get_op_count(origin_segments,
            origin_count, origin_segment_start_index,
            origin_segment_start_offset, local_segments, local_count,
            local_segment_start_index, local_segment_start_offset, size);
        HG_CHECK_ERROR(hg_bulk_op_id->op_count == 0, done, ret, HG_INVALID_ARG,
            "Could not get bulk op_count");

        HG_LOG_DEBUG("Transferring data through NA in %u operation(s)",
            hg_bulk_op_id->op_count);

        /* Create extra operation IDs if the number of operations exceeds
         * the number of pre-allocated op IDs */
        if (hg_bulk_op_id->op_count > HG_BULK_STATIC_MAX) {
            unsigned int i;

            /* Allocate memory for NA operation IDs */
            hg_bulk_na_op_ids->d =
                malloc(sizeof(na_op_id_t *) * hg_bulk_op_id->op_count);
            HG_CHECK_ERROR(hg_bulk_na_op_ids->d == NULL, done, ret, HG_NOMEM,
                "Could not allocate memory for op_ids");

            for (i = 0; i < hg_bulk_op_id->op_count; i++) {
                hg_bulk_na_op_ids->d[i] = NA_Op_create(hg_bulk_op_id->na_class);
                HG_CHECK_ERROR(hg_bulk_na_op_ids->d[i] == NULL, done, ret,
                    HG_NA_ERROR, "Could not create NA op ID");
            }

            na_op_ids = hg_bulk_na_op_ids->d;
        } else
            na_op_ids = hg_bulk_na_op_ids->s;

        /* Do actual transfer */
        ret = hg_bulk_transfer_segments_na(hg_bulk_op_id->na_class,
            hg_bulk_op_id->na_context, na_bulk_op, hg_bulk_transfer_cb,
            hg_bulk_op_id, na_origin_addr, origin_id, origin_segments,
            origin_count, origin_mem_handles, origin_segment_start_index,
            origin_segment_start_offset, local_segments, local_count,
            local_mem_handles, local_segment_start_index,
            local_segment_start_offset, size, na_op_ids,
            hg_bulk_op_id->op_count);
        HG_CHECK_HG_ERROR(done, ret, "Could not transfer data segments");
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_uint32_t
hg_bulk_transfer_get_op_count(const struct hg_bulk_segment *origin_segments,
    hg_uint32_t origin_count, hg_size_t origin_segment_start_index,
    hg_size_t origin_segment_start_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    hg_size_t local_segment_start_index, hg_size_t local_segment_start_offset,
    hg_size_t size)
{
    hg_size_t origin_segment_index = origin_segment_start_index;
    hg_size_t local_segment_index = local_segment_start_index;
    hg_size_t origin_segment_offset = origin_segment_start_offset;
    hg_size_t local_segment_offset = local_segment_start_offset;
    hg_size_t remaining_size = size;
    hg_uint32_t count = 0;

    while (remaining_size > 0 && origin_segment_index < origin_count &&
           local_segment_index < local_count) {
        /* Can only transfer smallest size */
        hg_size_t transfer_size = HG_BULK_MIN(
            (origin_segments[origin_segment_index].len - origin_segment_offset),
            (local_segments[local_segment_index].len - local_segment_offset));

        /* Remaining size may be smaller */
        transfer_size = HG_BULK_MIN(remaining_size, transfer_size);

        /* Increment op count */
        count++;

        /* Decrease remaining size from the size of data we transferred
         * and exit if everything has been transferred */
        remaining_size -= transfer_size;
        if (remaining_size == 0)
            break;

        /* Increment offsets from the size of data we transferred */
        origin_segment_offset += transfer_size;
        local_segment_offset += transfer_size;

        /* Change segment if new offset exceeds segment size */
        if (origin_segment_offset >=
            origin_segments[origin_segment_index].len) {
            origin_segment_index++;
            origin_segment_offset = 0;
        }
        if (local_segment_offset >= local_segments[local_segment_index].len) {
            local_segment_index++;
            local_segment_offset = 0;
        }
    }

    return count;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_transfer_segments_na(na_class_t *na_class, na_context_t *na_context,
    na_bulk_op_t na_bulk_op, na_cb_t callback, void *arg, na_addr_t origin_addr,
    na_uint8_t origin_id, const struct hg_bulk_segment *origin_segments,
    hg_uint32_t origin_count, na_mem_handle_t *origin_mem_handles,
    hg_size_t origin_segment_start_index, hg_size_t origin_segment_start_offset,
    const struct hg_bulk_segment *local_segments, hg_uint32_t local_count,
    na_mem_handle_t *local_mem_handles, hg_size_t local_segment_start_index,
    hg_size_t local_segment_start_offset, hg_size_t size,
    na_op_id_t *na_op_ids[], hg_uint32_t na_op_count)
{
    hg_size_t origin_segment_index = origin_segment_start_index;
    hg_size_t local_segment_index = local_segment_start_index;
    hg_size_t origin_segment_offset = origin_segment_start_offset;
    hg_size_t local_segment_offset = local_segment_start_offset;
    hg_size_t remaining_size = size;
    hg_uint32_t count = 0;
    hg_return_t ret = HG_SUCCESS;

    while (remaining_size > 0 && origin_segment_index < origin_count &&
           local_segment_index < local_count) {
        /* Can only transfer smallest size */
        hg_size_t transfer_size = HG_BULK_MIN(
            (origin_segments[origin_segment_index].len - origin_segment_offset),
            (local_segments[local_segment_index].len - local_segment_offset));
        na_return_t na_ret;

        /* Remaining size may be smaller */
        transfer_size = HG_BULK_MIN(remaining_size, transfer_size);

        na_ret = na_bulk_op(na_class, na_context, callback, arg,
            local_mem_handles[local_segment_index], local_segment_offset,
            origin_mem_handles[origin_segment_index], origin_segment_offset,
            transfer_size, origin_addr, origin_id, na_op_ids[count]);
        HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret, (hg_return_t) na_ret,
            "Could not transfer data (%s)", NA_Error_to_string(na_ret));

        count++;

        /* Decrease remaining size from the size of data we transferred
         * and exit if everything has been transferred */
        remaining_size -= transfer_size;
        if (remaining_size == 0)
            break;

        /* Increment offsets from the size of data we transferred */
        origin_segment_offset += transfer_size;
        local_segment_offset += transfer_size;

        /* Change segment if new offset exceeds segment size */
        if (origin_segment_offset >=
            origin_segments[origin_segment_index].len) {
            origin_segment_index++;
            origin_segment_offset = 0;
        }
        if (local_segment_offset >= local_segments[local_segment_index].len) {
            local_segment_index++;
            local_segment_offset = 0;
        }
    }

    HG_CHECK_ERROR(count != na_op_count, done, ret, HG_PROTOCOL_ERROR,
        "Expected %u operations, issued %u", na_op_count, count);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static int
hg_bulk_transfer_cb(const struct na_cb_info *callback_info)
{
    struct hg_bulk_op_id *hg_bulk_op_id =
        (struct hg_bulk_op_id *) callback_info->arg;
    hg_bool_t completed = HG_TRUE;

    /* If canceled, mark handle as canceled */
    if (callback_info->ret == NA_CANCELED) {
        HG_CHECK_WARNING(
            hg_atomic_get32(&hg_bulk_op_id->status) & HG_BULK_OP_COMPLETED,
            "Operation was completed");
        HG_LOG_DEBUG("NA_CANCELED event on op ID %p", (void *) hg_bulk_op_id);
        HG_CHECK_WARNING(
            !(hg_atomic_get32(&hg_bulk_op_id->status) & HG_BULK_OP_CANCELED),
            "Received NA_CANCELED event on op ID that was not canceled");
        /* Operations can be canceled by NA */
        if (!(hg_atomic_get32(&hg_bulk_op_id->status) & HG_BULK_OP_CANCELED))
            hg_atomic_or32(&hg_bulk_op_id->status, HG_BULK_OP_CANCELED);
    } else if (callback_info->ret != NA_SUCCESS) {
        HG_LOG_ERROR("NA callback returned error (%s)",
            NA_Error_to_string(callback_info->ret));

        /* Mark handle as errored */
        hg_atomic_or32(&hg_bulk_op_id->status, HG_BULK_OP_ERRORED);
        if (hg_bulk_op_id->err_ret == HG_SUCCESS)
            hg_bulk_op_id->err_ret = (hg_return_t) callback_info->ret;
    }

    /* When all NA transfers that correspond to bulk operation complete
     * add HG user callback to completion queue
     */
    if ((hg_uint32_t) hg_atomic_incr32(&hg_bulk_op_id->op_completed_count) ==
        hg_bulk_op_id->op_count) {
        hg_return_t ret = hg_bulk_complete(hg_bulk_op_id, HG_FALSE);
        HG_CHECK_ERROR_DONE(ret != HG_SUCCESS, "Could not complete operation");
    }

    return (int) completed;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_complete(struct hg_bulk_op_id *hg_bulk_op_id, hg_bool_t self_notify)
{
    struct hg_cb_info *callback_info = NULL;
    hg_return_t ret = HG_SUCCESS;
    hg_util_int32_t status;

    /* Mark op id as completed before checking for cancelation */
    status = hg_atomic_or32(&hg_bulk_op_id->status, HG_BULK_OP_COMPLETED);

    /* Init callback info */
    callback_info = &hg_bulk_op_id->callback_info;

    /* Check for current status before completing */
    if (status & HG_BULK_OP_CANCELED) {
        /* If it was canceled while being processed, set callback ret
         * accordingly */
        HG_LOG_DEBUG("Operation ID %p is canceled", (void *) hg_bulk_op_id);
        callback_info->ret = HG_CANCELED;
    } else if (status & HG_BULK_OP_ERRORED) {
        /* If it was errored, set callback ret accordingly */
        HG_LOG_DEBUG("Operation ID %p is errored", (void *) hg_bulk_op_id);
        callback_info->ret = hg_bulk_op_id->err_ret;
    } else
        callback_info->ret = HG_SUCCESS;

    if (callback_info->info.bulk.origin_handle->desc.info.flags &
        HG_BULK_EAGER) {
        /* In the case of eager bulk transfer, directly trigger the operation
         * to avoid potential deadlocks */
        ret = hg_bulk_trigger_entry(hg_bulk_op_id);
        HG_CHECK_HG_ERROR(done, ret, "Could not trigger completion entry");
    } else {
        struct hg_completion_entry *hg_completion_entry =
            &hg_bulk_op_id->hg_completion_entry;

        hg_completion_entry->op_type = HG_BULK;
        hg_completion_entry->op_id.hg_bulk_op_id = hg_bulk_op_id;

        ret = hg_core_completion_add(
            hg_bulk_op_id->core_context, hg_completion_entry, self_notify);
        HG_CHECK_HG_ERROR(
            done, ret, "Could not add HG completion entry to completion queue");
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
static hg_return_t
hg_bulk_cancel(struct hg_bulk_op_id *hg_bulk_op_id)
{
    na_op_id_t **na_op_ids;
    hg_return_t ret = HG_SUCCESS;
    hg_util_int32_t status;
    unsigned int i;

    /* Exit if op has already completed */
    status = hg_atomic_or32(&hg_bulk_op_id->status, HG_BULK_OP_CANCELED);
    if ((status & HG_BULK_OP_COMPLETED) || (status & HG_BULK_OP_ERRORED))
        goto done;

        /* Cancel all NA operations issued */
#ifdef NA_HAS_SM
    if (hg_bulk_op_id->na_class ==
        hg_bulk_op_id->core_context->core_class->na_sm_class)
        na_op_ids = HG_BULK_NA_SM_OP_IDS(hg_bulk_op_id);
    else
#endif
        na_op_ids = HG_BULK_NA_OP_IDS(hg_bulk_op_id);

    for (i = 0; i < hg_bulk_op_id->op_count; i++) {
        na_return_t na_ret = NA_Cancel(
            hg_bulk_op_id->na_class, hg_bulk_op_id->na_context, na_op_ids[i]);
        HG_CHECK_ERROR(na_ret != NA_SUCCESS, done, ret, (hg_return_t) na_ret,
            "Could not cancel NA op ID (%s)", NA_Error_to_string(na_ret));
    }

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_bulk_trigger_entry(struct hg_bulk_op_id *hg_bulk_op_id)
{
    hg_return_t ret = HG_SUCCESS;

    /* Execute callback */
    if (hg_bulk_op_id->callback)
        hg_bulk_op_id->callback(&hg_bulk_op_id->callback_info);

    /* Decrement ref_count */
    ret = hg_bulk_free(hg_bulk_op_id->callback_info.info.bulk.origin_handle);
    HG_CHECK_HG_ERROR(done, ret, "Could not free origin handle");

    ret = hg_bulk_free(hg_bulk_op_id->callback_info.info.bulk.local_handle);
    HG_CHECK_HG_ERROR(done, ret, "Could not free local handle");

    /* Release bulk op ID (can be released after callback execution since
     * op IDs are managed internally) */
    ret = hg_bulk_op_destroy(hg_bulk_op_id);
    HG_CHECK_HG_ERROR(done, ret, "Could not destroy bulk op ID");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_create(hg_class_t *hg_class, hg_uint32_t count, void **buf_ptrs,
    const hg_size_t *buf_sizes, hg_uint8_t flags, hg_bulk_t *handle)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(
        hg_class == NULL, done, ret, HG_INVALID_ARG, "NULL HG class");
    HG_CHECK_ERROR(
        count == 0, done, ret, HG_INVALID_ARG, "Invalid number of segments");
    HG_CHECK_ERROR(buf_sizes == NULL, done, ret, HG_INVALID_ARG,
        "NULL segment size pointer");
    /* We allow for 0-sized segments though. */

    switch (flags) {
        case HG_BULK_READWRITE:
        case HG_BULK_READ_ONLY:
        case HG_BULK_WRITE_ONLY:
            break;
        default:
            HG_GOTO_ERROR(
                done, ret, HG_INVALID_ARG, "Unrecognized handle flag");
    }

    HG_LOG_DEBUG("Creating new bulk handle with %u segment(s)", count);

    ret = hg_bulk_create(hg_class->core_class, count, buf_ptrs, buf_sizes,
        flags, (struct hg_bulk **) handle);
    HG_CHECK_HG_ERROR(done, ret, "Could not create bulk handle");

    HG_LOG_DEBUG("Created new bulk handle (%p)", (void *) *handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_free(hg_bulk_t handle)
{
    hg_return_t ret = HG_SUCCESS;

    if (handle == HG_BULK_NULL)
        goto done;

    HG_LOG_DEBUG("Freeing bulk handle (%p)", (void *) handle);

    ret = hg_bulk_free((struct hg_bulk *) handle);
    HG_CHECK_HG_ERROR(done, ret, "Could not free bulk handle");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_ref_incr(hg_bulk_t handle)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(handle == HG_BULK_NULL, done, ret, HG_INVALID_ARG,
        "NULL bulk handle passed");

    /* Increment ref count */
    hg_atomic_incr32(&(((struct hg_bulk *) handle)->ref_count));

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_bind(hg_bulk_t handle, hg_context_t *context)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(handle == HG_BULK_NULL, done, ret, HG_INVALID_ARG,
        "NULL bulk handle passed");
    HG_CHECK_ERROR(
        context == NULL, done, ret, HG_INVALID_ARG, "NULL HG context");

    HG_LOG_DEBUG("Binding bulk handle (%p) to context (%p)", (void *) handle,
        (void *) context);

    ret = hg_bulk_bind((struct hg_bulk *) handle, context->core_context);
    HG_CHECK_HG_ERROR(done, ret, "Could not bind context to bulk handle");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_addr_t
HG_Bulk_get_addr(hg_bulk_t handle)
{
    hg_core_addr_t ret = HG_CORE_ADDR_NULL;

    HG_CHECK_ERROR_NORET(
        handle == HG_BULK_NULL, done, "NULL bulk handle passed");

    ret = ((struct hg_bulk *) handle)->addr;

done:
    return (hg_addr_t) ret;
}

/*---------------------------------------------------------------------------*/
hg_uint8_t
HG_Bulk_get_context_id(hg_bulk_t handle)
{
    hg_uint8_t ret = 0;

    HG_CHECK_ERROR_NORET(
        handle == HG_BULK_NULL, done, "NULL bulk handle passed");

    ret = ((struct hg_bulk *) handle)->context_id;

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_access(hg_bulk_t handle, hg_size_t offset, hg_size_t size,
    hg_uint8_t flags, hg_uint32_t max_count, void **buf_ptrs,
    hg_size_t *buf_sizes, hg_uint32_t *actual_count)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(handle == HG_BULK_NULL, done, ret, HG_INVALID_ARG,
        "NULL bulk handle passed");

    if (!size || !max_count)
        goto done;

    HG_LOG_DEBUG("Accessing bulk handle (%p)", (void *) handle);

    hg_bulk_access((struct hg_bulk *) handle, offset, size, flags, max_count,
        buf_ptrs, buf_sizes, actual_count);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_size_t
HG_Bulk_get_serialize_size(hg_bulk_t handle, unsigned long flags)
{
    hg_size_t ret = 0;

    HG_CHECK_ERROR_NORET(
        handle == HG_BULK_NULL, done, "NULL bulk handle passed");

    ret = hg_bulk_get_serialize_size((struct hg_bulk *) handle, flags & 0xff);

    HG_LOG_DEBUG("Serialize size with flags eager=%d, sm=%d, is %" PRIu64
                 " bytes for bulk handle (%p)",
        (flags & HG_BULK_EAGER) ? HG_TRUE : HG_FALSE,
        (flags & HG_BULK_SM) ? HG_TRUE : HG_FALSE, ret, (void *) handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_serialize(
    void *buf, hg_size_t buf_size, unsigned long flags, hg_bulk_t handle)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(handle == HG_BULK_NULL, done, ret, HG_INVALID_ARG,
        "NULL bulk handle passed");

    HG_LOG_DEBUG("Serializing bulk handle (%p) with flags eager=%d, sm=%d",
        (void *) handle, (flags & HG_BULK_EAGER) ? HG_TRUE : HG_FALSE,
        (flags & HG_BULK_SM) ? HG_TRUE : HG_FALSE);

    ret = hg_bulk_serialize(
        buf, buf_size, flags & 0xff, (struct hg_bulk *) handle);
    HG_CHECK_HG_ERROR(done, ret, "Could not serialize handle");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_deserialize(hg_class_t *hg_class, hg_bulk_t *handle, const void *buf,
    hg_size_t buf_size)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(
        handle == NULL, done, ret, HG_INVALID_ARG, "NULL bulk handle passed");

    ret = hg_bulk_deserialize(
        hg_class->core_class, (struct hg_bulk **) handle, buf, buf_size);
    HG_CHECK_HG_ERROR(done, ret, "Could not deserialize handle");

    HG_LOG_DEBUG("Deserialized into new bulk handle (%p)", (void *) *handle);

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_transfer(hg_context_t *context, hg_cb_t callback, void *arg,
    hg_bulk_op_t op, hg_addr_t origin_addr, hg_bulk_t origin_handle,
    hg_size_t origin_offset, hg_bulk_t local_handle, hg_size_t local_offset,
    hg_size_t size, hg_op_id_t *op_id)
{
    struct hg_bulk *hg_bulk_origin = (struct hg_bulk *) origin_handle;
    struct hg_bulk *hg_bulk_local = (struct hg_bulk *) local_handle;
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(
        context == NULL, done, ret, HG_INVALID_ARG, "NULL HG context");

    /* Origin handle sanity checks */
    HG_CHECK_ERROR(hg_bulk_origin == NULL, done, ret, HG_INVALID_ARG,
        "NULL origin handle passed");
    HG_CHECK_ERROR((origin_offset + size) > hg_bulk_origin->desc.info.len, done,
        ret, HG_INVALID_ARG,
        "Exceeding size of memory exposed by origin handle (%" PRIu64
        " + %" PRIu64 " > %" PRIu64 ")",
        origin_offset, size, hg_bulk_origin->desc.info.len);
    HG_CHECK_ERROR(hg_bulk_origin->addr != HG_CORE_ADDR_NULL, done, ret,
        HG_INVALID_ARG,
        "Address information embedded into origin_handle, use "
        "HG_Bulk_bind_transfer() instead");

    /* Local handle sanity checks */
    HG_CHECK_ERROR(hg_bulk_local == NULL, done, ret, HG_INVALID_ARG,
        "NULL origin handle passed");
    HG_CHECK_ERROR((local_offset + size) > hg_bulk_local->desc.info.len, done,
        ret, HG_INVALID_ARG,
        "Exceeding size of memory exposed by local handle (%" PRIu64
        " + %" PRIu64 " > %" PRIu64 ")",
        local_offset, size, hg_bulk_local->desc.info.len);

    /* Check permission flags */
    HG_BULK_CHECK_FLAGS(op, hg_bulk_origin->desc.info.flags,
        hg_bulk_local->desc.info.flags, done, ret);

    HG_LOG_DEBUG(
        "Transferring data between bulk handle (%p) and bulk handle (%p)",
        (void *) hg_bulk_origin, (void *) hg_bulk_local);

    /* Do bulk transfer */
    ret = hg_bulk_transfer(context->core_context, callback, arg, op,
        (hg_core_addr_t) origin_addr, 0, hg_bulk_origin, origin_offset,
        hg_bulk_local, local_offset, size, op_id);
    HG_CHECK_HG_ERROR(done, ret, "Could not start transfer of bulk data");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_bind_transfer(hg_context_t *context, hg_cb_t callback, void *arg,
    hg_bulk_op_t op, hg_bulk_t origin_handle, hg_size_t origin_offset,
    hg_bulk_t local_handle, hg_size_t local_offset, hg_size_t size,
    hg_op_id_t *op_id)
{
    struct hg_bulk *hg_bulk_origin = (struct hg_bulk *) origin_handle;
    struct hg_bulk *hg_bulk_local = (struct hg_bulk *) local_handle;
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(
        context == NULL, done, ret, HG_INVALID_ARG, "NULL HG context");

    /* Origin handle sanity checks */
    HG_CHECK_ERROR(hg_bulk_origin == NULL, done, ret, HG_INVALID_ARG,
        "NULL origin handle passed");
    HG_CHECK_ERROR((origin_offset + size) > hg_bulk_origin->desc.info.len, done,
        ret, HG_INVALID_ARG,
        "Exceeding size of memory exposed by origin handle (%" PRIu64
        " + %" PRIu64 " > %" PRIu64 ")",
        origin_offset, size, hg_bulk_origin->desc.info.len);
    HG_CHECK_ERROR(hg_bulk_origin->addr == HG_CORE_ADDR_NULL, done, ret,
        HG_INVALID_ARG,
        "Address information is not embedded onto origin_handle, "
        "call HG_Bulk_bind() on bulk handle or use HG_Bulk_transfer() instead");

    /* Local handle sanity checks */
    HG_CHECK_ERROR(hg_bulk_local == NULL, done, ret, HG_INVALID_ARG,
        "NULL origin handle passed");
    HG_CHECK_ERROR((local_offset + size) > hg_bulk_local->desc.info.len, done,
        ret, HG_INVALID_ARG,
        "Exceeding size of memory exposed by local handle (%" PRIu64
        " + %" PRIu64 " > %" PRIu64 ")",
        local_offset, size, hg_bulk_local->desc.info.len);

    /* Check permission flags */
    HG_BULK_CHECK_FLAGS(op, hg_bulk_origin->desc.info.flags,
        hg_bulk_local->desc.info.flags, done, ret);

    HG_LOG_DEBUG(
        "Transferring data between bulk handle (%p) and bulk handle (%p)",
        (void *) hg_bulk_origin, (void *) hg_bulk_local);

    /* Do bulk transfer */
    ret = hg_bulk_transfer(context->core_context, callback, arg, op,
        hg_bulk_origin->addr, hg_bulk_origin->context_id, hg_bulk_origin,
        origin_offset, hg_bulk_local, local_offset, size, op_id);
    HG_CHECK_HG_ERROR(done, ret, "Could not start transfer of bulk data");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_transfer_id(hg_context_t *context, hg_cb_t callback, void *arg,
    hg_bulk_op_t op, hg_addr_t origin_addr, hg_uint8_t origin_id,
    hg_bulk_t origin_handle, hg_size_t origin_offset, hg_bulk_t local_handle,
    hg_size_t local_offset, hg_size_t size, hg_op_id_t *op_id)
{
    struct hg_bulk *hg_bulk_origin = (struct hg_bulk *) origin_handle;
    struct hg_bulk *hg_bulk_local = (struct hg_bulk *) local_handle;
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(
        context == NULL, done, ret, HG_INVALID_ARG, "NULL HG context");

    /* Origin handle sanity checks */
    HG_CHECK_ERROR(hg_bulk_origin == NULL, done, ret, HG_INVALID_ARG,
        "NULL origin handle passed");
    HG_CHECK_ERROR((origin_offset + size) > hg_bulk_origin->desc.info.len, done,
        ret, HG_INVALID_ARG,
        "Exceeding size of memory exposed by origin handle (%" PRIu64
        " + %" PRIu64 " > %" PRIu64 ")",
        origin_offset, size, hg_bulk_origin->desc.info.len);
    HG_CHECK_ERROR(hg_bulk_origin->addr != HG_CORE_ADDR_NULL, done, ret,
        HG_INVALID_ARG,
        "Address information embedded into origin_handle, use "
        "HG_Bulk_bind_transfer() instead");

    /* Local handle sanity checks */
    HG_CHECK_ERROR(hg_bulk_local == NULL, done, ret, HG_INVALID_ARG,
        "NULL origin handle passed");
    HG_CHECK_ERROR((local_offset + size) > hg_bulk_local->desc.info.len, done,
        ret, HG_INVALID_ARG,
        "Exceeding size of memory exposed by local handle (%" PRIu64
        " + %" PRIu64 " > %" PRIu64 ")",
        local_offset, size, hg_bulk_local->desc.info.len);

    /* Check permission flags */
    HG_BULK_CHECK_FLAGS(op, hg_bulk_origin->desc.info.flags,
        hg_bulk_local->desc.info.flags, done, ret);

    HG_LOG_DEBUG(
        "Transferring data between bulk handle (%p) and bulk handle (%p)",
        (void *) hg_bulk_origin, (void *) hg_bulk_local);

    /* Do bulk transfer */
    ret = hg_bulk_transfer(context->core_context, callback, arg, op,
        (hg_core_addr_t) origin_addr, origin_id, hg_bulk_origin, origin_offset,
        hg_bulk_local, local_offset, size, op_id);
    HG_CHECK_HG_ERROR(done, ret, "Could not start transfer of bulk data");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
HG_Bulk_cancel(hg_op_id_t op_id)
{
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(op_id == HG_OP_ID_NULL, done, ret, HG_INVALID_ARG,
        "NULL HG bulk operation ID");

    HG_LOG_DEBUG("Canceling bulk op ID (%p)", (void *) op_id);

    ret = hg_bulk_cancel((struct hg_bulk_op_id *) op_id);
    HG_CHECK_HG_ERROR(done, ret, "Could not cancel bulk operation");

done:
    return ret;
}
