/*
 * Copyright (C) 2013-2019 Argonne National Laboratory, Department of Energy,
 *                    UChicago Argonne, LLC and The HDF Group.
 * All rights reserved.
 *
 * The full copyright notice, including terms governing use, modification,
 * and redistribution, is contained in the COPYING file that can be
 * found at the root of the source code distribution tree.
 */

#include "mercury_core_header.h"
#include "mercury_error.h"
#include "mercury_proc_buf.h"

#ifdef HG_HAS_CHECKSUMS
#    include <mchecksum.h>
#endif

#ifdef _WIN32
#    include <winsock2.h>
#else
#    include <arpa/inet.h>
#endif
#include <stdlib.h>
#include <string.h>

/****************/
/* Local Macros */
/****************/

#define HG_CORE_HEADER_CHECKSUM "crc16"

/* Helper macros for encoding header */
#ifdef HG_HAS_CHECKSUMS
#    define HG_CORE_HEADER_PROC(hg_header, buf_ptr, data, op)                  \
        do {                                                                   \
            buf_ptr = hg_proc_buf_memcpy(buf_ptr, &data, sizeof(data), op);    \
            mchecksum_update(hg_header->checksum, &data, sizeof(data));        \
        } while (0)
#else
#    define HG_CORE_HEADER_PROC(hg_header, buf_ptr, data, op)                  \
        do {                                                                   \
            buf_ptr = hg_proc_buf_memcpy(buf_ptr, &data, sizeof(data), op);    \
        } while (0)
#endif

#define HG_CORE_HEADER_PROC16(hg_header, buf_ptr, data, op, tmp)               \
    do {                                                                       \
        hg_uint16_t tmp;                                                       \
        if (op == HG_ENCODE)                                                   \
            tmp = htons(data);                                                 \
        HG_CORE_HEADER_PROC(hg_header, buf_ptr, tmp, op);                      \
        if (op == HG_DECODE)                                                   \
            data = ntohs(tmp);                                                 \
    } while (0)

#define HG_CORE_HEADER_PROC64(hg_header, buf_ptr, data, op, tmp)               \
    do {                                                                       \
        hg_uint64_t tmp;                                                       \
        if (op == HG_ENCODE)                                                   \
            tmp = ((hg_uint64_t) htonl(data & 0xFFFFFFFF) << 32) |             \
                  htonl((hg_uint32_t)(data >> 32));                            \
        HG_CORE_HEADER_PROC(hg_header, buf_ptr, tmp, op);                      \
        if (op == HG_DECODE)                                                   \
            data = ((hg_uint64_t) ntohl(tmp & 0xFFFFFFFF) << 32) |             \
                   ntohl((hg_uint32_t)(tmp >> 32));                            \
    } while (0)

/************************************/
/* Local Type and Struct Definition */
/************************************/

/********************/
/* Local Prototypes */
/********************/
extern const char *
HG_Error_to_string(hg_return_t errnum);

/*******************/
/* Local Variables */
/*******************/

/*---------------------------------------------------------------------------*/
void
hg_core_header_request_init(struct hg_core_header *hg_core_header)
{
#ifdef HG_HAS_CHECKSUMS
    /* Create a new checksum (CRC16) */
    mchecksum_init(HG_CORE_HEADER_CHECKSUM, &hg_core_header->checksum);
#endif
    hg_core_header_request_reset(hg_core_header);
}

/*---------------------------------------------------------------------------*/
void
hg_core_header_response_init(struct hg_core_header *hg_core_header)
{
#ifdef HG_HAS_CHECKSUMS
    /* Create a new checksum (CRC16) */
    mchecksum_init(HG_CORE_HEADER_CHECKSUM, &hg_core_header->checksum);
#endif
    hg_core_header_response_reset(hg_core_header);
}

/*---------------------------------------------------------------------------*/
void
hg_core_header_request_finalize(struct hg_core_header *hg_core_header)
{
#ifdef HG_HAS_CHECKSUMS
    mchecksum_destroy(hg_core_header->checksum);
    hg_core_header->checksum = MCHECKSUM_OBJECT_NULL;
#else
    (void) hg_core_header;
#endif
}

/*---------------------------------------------------------------------------*/
void
hg_core_header_response_finalize(struct hg_core_header *hg_core_header)
{
#ifdef HG_HAS_CHECKSUMS
    mchecksum_destroy(hg_core_header->checksum);
    hg_core_header->checksum = MCHECKSUM_OBJECT_NULL;
#else
    (void) hg_core_header;
#endif
}

/*---------------------------------------------------------------------------*/
void
hg_core_header_request_reset(struct hg_core_header *hg_core_header)
{
    memset(
        &hg_core_header->msg.request, 0, sizeof(struct hg_core_header_request));
    hg_core_header->msg.request.hg = HG_CORE_IDENTIFIER;
    hg_core_header->msg.request.protocol = HG_CORE_PROTOCOL_VERSION;
#ifdef HG_HAS_CHECKSUMS
    mchecksum_reset(hg_core_header->checksum);
#endif
}

/*---------------------------------------------------------------------------*/
void
hg_core_header_response_reset(struct hg_core_header *hg_core_header)
{
    memset(&hg_core_header->msg.response, 0,
        sizeof(struct hg_core_header_response));
#ifdef HG_HAS_CHECKSUMS
    mchecksum_reset(hg_core_header->checksum);
#endif
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_core_header_request_proc(hg_proc_op_t op, void *buf, size_t buf_size,
    struct hg_core_header *hg_core_header)
{
    void *buf_ptr = buf;
    struct hg_core_header_request *header = &hg_core_header->msg.request;
#ifdef HG_HAS_CHECKSUMS
    hg_uint16_t n_hash_header;
#endif
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(buf_size < sizeof(struct hg_core_header_request), done, ret,
        HG_INVALID_ARG, "Invalid buffer size");

#ifdef HG_HAS_CHECKSUMS
    /* Reset header checksum first */
    mchecksum_reset(hg_core_header->checksum);
#endif

    /* HG byte */
    HG_CORE_HEADER_PROC(hg_core_header, buf_ptr, header->hg, op);

    /* Protocol */
    HG_CORE_HEADER_PROC(hg_core_header, buf_ptr, header->protocol, op);

    /* Convert ID to network byte order */
    HG_CORE_HEADER_PROC64(hg_core_header, buf_ptr, header->id, op, tmp);

    /* Flags */
    HG_CORE_HEADER_PROC(hg_core_header, buf_ptr, header->flags, op);

    /* Cookie */
    HG_CORE_HEADER_PROC(hg_core_header, buf_ptr, header->cookie, op);

#ifdef HG_HAS_CHECKSUMS
    /* Checksum of header */
    mchecksum_get(hg_core_header->checksum, &header->hash.header,
        sizeof(header->hash.header), MCHECKSUM_FINALIZE);
    if (op == HG_ENCODE)
        n_hash_header = (hg_uint16_t) htons(header->hash.header);
    hg_proc_buf_memcpy(buf_ptr, &n_hash_header, sizeof(n_hash_header), op);
    if (op == HG_DECODE) {
        hg_uint16_t h_hash_header = ntohs(n_hash_header);
        HG_CHECK_ERROR(header->hash.header != h_hash_header, done, ret,
            HG_CHECKSUM_ERROR,
            "checksum 0x%04X does not match (expected 0x%04X!)");
    }
#endif

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_core_header_response_proc(hg_proc_op_t op, void *buf, size_t buf_size,
    struct hg_core_header *hg_core_header)
{
    void *buf_ptr = buf;
    struct hg_core_header_response *header = &hg_core_header->msg.response;
#ifdef HG_HAS_CHECKSUMS
    hg_uint16_t n_hash_header;
#endif
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_ERROR(buf_size < sizeof(struct hg_core_header_response), done, ret,
        HG_OVERFLOW, "Invalid buffer size");

#ifdef HG_HAS_CHECKSUMS
    /* Reset header checksum first */
    mchecksum_reset(hg_core_header->checksum);
#endif

    /* Return code */
    HG_CORE_HEADER_PROC(hg_core_header, buf_ptr, header->ret_code, op);

    /* Flags */
    HG_CORE_HEADER_PROC(hg_core_header, buf_ptr, header->flags, op);

    /* Convert cookie to network byte order */
    HG_CORE_HEADER_PROC16(hg_core_header, buf_ptr, header->cookie, op, tmp);

#ifdef HG_HAS_CHECKSUMS
    /* Checksum of header */
    mchecksum_get(hg_core_header->checksum, &header->hash.header,
        sizeof(header->hash.header), MCHECKSUM_FINALIZE);
    if (op == HG_ENCODE)
        n_hash_header = (hg_uint16_t) htons(header->hash.header);
    hg_proc_buf_memcpy(buf_ptr, &n_hash_header, sizeof(n_hash_header), op);
    if (op == HG_DECODE) {
        hg_uint16_t h_hash_header = ntohs(n_hash_header);
        HG_CHECK_ERROR(header->hash.header != h_hash_header, done, ret,
            HG_CHECKSUM_ERROR,
            "checksum 0x%04X does not match (expected 0x%04X!)");
    }
#endif

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_core_header_request_verify(const struct hg_core_header *hg_core_header)
{
    const struct hg_core_header_request *header = &hg_core_header->msg.request;
    hg_return_t ret = HG_SUCCESS;

    /* Must match HG */
    HG_CHECK_ERROR(
        (((header->hg >> 1) & 'H') != 'H') || (((header->hg) & 'G') != 'G'),
        done, ret, HG_PROTOCOL_ERROR, "Invalid HG byte");

    HG_CHECK_ERROR(header->protocol != HG_CORE_PROTOCOL_VERSION, done, ret,
        HG_PROTONOSUPPORT, "Invalid protocol version");

done:
    return ret;
}

/*---------------------------------------------------------------------------*/
hg_return_t
hg_core_header_response_verify(const struct hg_core_header *hg_core_header)
{
    const struct hg_core_header_response *header =
        &hg_core_header->msg.response;
    hg_return_t ret = HG_SUCCESS;

    HG_CHECK_WARNING(header->ret_code, "Response return code: %s",
        HG_Error_to_string((hg_return_t) header->ret_code));

    return ret;
}
