/*-
 *   BSD LICENSE
 *
 *   Copyright(c) 2017 Marvell International Ltd.
 *   Copyright(c) 2017 Semihalf.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <rte_common.h>
#include <rte_hexdump.h>
#include <rte_cryptodev.h>
#include <rte_cryptodev_pmd.h>
#include <rte_bus_vdev.h>
#include <rte_malloc.h>
#include <rte_cpuflags.h>

#include "rte_mrvl_pmd_private.h"

#define MRVL_MUSDK_DMA_MEMSIZE 41943040

static uint8_t cryptodev_driver_id;

/**
 * Flag if particular crypto algorithm is supported by PMD/MUSDK.
 *
 * The idea is to have Not Supported value as default (0).
 * This way we need only to define proper map sizes,
 * non-initialized entries will be by default not supported.
 */
enum algo_supported {
	ALGO_NOT_SUPPORTED = 0,
	ALGO_SUPPORTED = 1,
};

/** Map elements for cipher mapping.*/
struct cipher_params_mapping {
	enum algo_supported  supported;   /**< On/Off switch */
	enum sam_cipher_alg  cipher_alg;  /**< Cipher algorithm */
	enum sam_cipher_mode cipher_mode; /**< Cipher mode */
	unsigned int max_key_len;         /**< Maximum key length (in bytes)*/
}
/* We want to squeeze in multiple maps into the cache line. */
__rte_aligned(32);

/** Map elements for auth mapping.*/
struct auth_params_mapping {
	enum algo_supported supported;  /**< On/off switch */
	enum sam_auth_alg   auth_alg;   /**< Auth algorithm */
}
/* We want to squeeze in multiple maps into the cache line. */
__rte_aligned(32);

/**
 * Map of supported cipher algorithms.
 */
static const
struct cipher_params_mapping cipher_map[RTE_CRYPTO_CIPHER_LIST_END] = {
	[RTE_CRYPTO_CIPHER_NULL] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_NONE },
	[RTE_CRYPTO_CIPHER_3DES_CBC] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_3DES,
		.cipher_mode = SAM_CIPHER_CBC,
		.max_key_len = BITS2BYTES(192) },
	[RTE_CRYPTO_CIPHER_3DES_CTR] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_3DES,
		.cipher_mode = SAM_CIPHER_CTR,
		.max_key_len = BITS2BYTES(192) },
	[RTE_CRYPTO_CIPHER_3DES_ECB] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_3DES,
		.cipher_mode = SAM_CIPHER_ECB,
		.max_key_len = BITS2BYTES(192) },
	[RTE_CRYPTO_CIPHER_AES_CBC] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_AES,
		.cipher_mode = SAM_CIPHER_CBC,
		.max_key_len = BITS2BYTES(256) },
	[RTE_CRYPTO_CIPHER_AES_CTR] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_AES,
		.cipher_mode = SAM_CIPHER_CTR,
		.max_key_len = BITS2BYTES(256) },
	[RTE_CRYPTO_CIPHER_AES_ECB] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_AES,
		.cipher_mode = SAM_CIPHER_ECB,
		.max_key_len = BITS2BYTES(256) },
};

/**
 * Map of supported auth algorithms.
 */
static const
struct auth_params_mapping auth_map[RTE_CRYPTO_AUTH_LIST_END] = {
	[RTE_CRYPTO_AUTH_NULL] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_NONE },
	[RTE_CRYPTO_AUTH_MD5_HMAC] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HMAC_MD5 },
	[RTE_CRYPTO_AUTH_MD5] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HASH_MD5 },
	[RTE_CRYPTO_AUTH_SHA1_HMAC] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HMAC_SHA1 },
	[RTE_CRYPTO_AUTH_SHA1] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HASH_SHA1 },
	[RTE_CRYPTO_AUTH_SHA224_HMAC] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HMAC_SHA2_224 },
	[RTE_CRYPTO_AUTH_SHA224] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HASH_SHA2_224 },
	[RTE_CRYPTO_AUTH_SHA256_HMAC] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HMAC_SHA2_256 },
	[RTE_CRYPTO_AUTH_SHA256] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HASH_SHA2_256 },
	[RTE_CRYPTO_AUTH_SHA384_HMAC] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HMAC_SHA2_384 },
	[RTE_CRYPTO_AUTH_SHA384] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HASH_SHA2_384 },
	[RTE_CRYPTO_AUTH_SHA512_HMAC] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HMAC_SHA2_512 },
	[RTE_CRYPTO_AUTH_SHA512] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_HASH_SHA2_512 },
	[RTE_CRYPTO_AUTH_AES_GMAC] = {
		.supported = ALGO_SUPPORTED,
		.auth_alg = SAM_AUTH_AES_GMAC },
};

/**
 * Map of supported aead algorithms.
 */
static const
struct cipher_params_mapping aead_map[RTE_CRYPTO_AEAD_LIST_END] = {
	[RTE_CRYPTO_AEAD_AES_GCM] = {
		.supported = ALGO_SUPPORTED,
		.cipher_alg = SAM_CIPHER_AES,
		.cipher_mode = SAM_CIPHER_GCM,
		.max_key_len = BITS2BYTES(256) },
};

/*
 *-----------------------------------------------------------------------------
 * Forward declarations.
 *-----------------------------------------------------------------------------
 */
static int cryptodev_mrvl_crypto_uninit(struct rte_vdev_device *vdev);

/*
 *-----------------------------------------------------------------------------
 * Session Preparation.
 *-----------------------------------------------------------------------------
 */

/**
 * Get xform chain order.
 *
 * @param xform Pointer to configuration structure chain for crypto operations.
 * @returns Order of crypto operations.
 */
static enum mrvl_crypto_chain_order
mrvl_crypto_get_chain_order(const struct rte_crypto_sym_xform *xform)
{
	/* Currently, Marvell supports max 2 operations in chain */
	if (xform->next != NULL && xform->next->next != NULL)
		return MRVL_CRYPTO_CHAIN_NOT_SUPPORTED;

	if (xform->next != NULL) {
		if ((xform->type == RTE_CRYPTO_SYM_XFORM_AUTH) &&
			(xform->next->type == RTE_CRYPTO_SYM_XFORM_CIPHER))
			return MRVL_CRYPTO_CHAIN_AUTH_CIPHER;

		if ((xform->type == RTE_CRYPTO_SYM_XFORM_CIPHER) &&
			(xform->next->type == RTE_CRYPTO_SYM_XFORM_AUTH))
			return MRVL_CRYPTO_CHAIN_CIPHER_AUTH;
	} else {
		if (xform->type == RTE_CRYPTO_SYM_XFORM_AUTH)
			return MRVL_CRYPTO_CHAIN_AUTH_ONLY;

		if (xform->type == RTE_CRYPTO_SYM_XFORM_CIPHER)
			return MRVL_CRYPTO_CHAIN_CIPHER_ONLY;

		if (xform->type == RTE_CRYPTO_SYM_XFORM_AEAD)
			return MRVL_CRYPTO_CHAIN_COMBINED;
	}
	return MRVL_CRYPTO_CHAIN_NOT_SUPPORTED;
}

/**
 * Set session parameters for cipher part.
 *
 * @param sess Crypto session pointer.
 * @param cipher_xform Pointer to configuration structure for cipher operations.
 * @returns 0 in case of success, negative value otherwise.
 */
static int
mrvl_crypto_set_cipher_session_parameters(struct mrvl_crypto_session *sess,
		const struct rte_crypto_sym_xform *cipher_xform)
{
	/* Make sure we've got proper struct */
	if (cipher_xform->type != RTE_CRYPTO_SYM_XFORM_CIPHER) {
		MRVL_CRYPTO_LOG_ERR("Wrong xform struct provided!");
		return -EINVAL;
	}

	/* See if map data is present and valid */
	if ((cipher_xform->cipher.algo > RTE_DIM(cipher_map)) ||
		(cipher_map[cipher_xform->cipher.algo].supported
			!= ALGO_SUPPORTED)) {
		MRVL_CRYPTO_LOG_ERR("Cipher algorithm not supported!");
		return -EINVAL;
	}

	sess->cipher_iv_offset = cipher_xform->cipher.iv.offset;

	sess->sam_sess_params.dir =
		(cipher_xform->cipher.op == RTE_CRYPTO_CIPHER_OP_ENCRYPT) ?
		SAM_DIR_ENCRYPT : SAM_DIR_DECRYPT;
	sess->sam_sess_params.cipher_alg =
		cipher_map[cipher_xform->cipher.algo].cipher_alg;
	sess->sam_sess_params.cipher_mode =
		cipher_map[cipher_xform->cipher.algo].cipher_mode;

	/* Assume IV will be passed together with data. */
	sess->sam_sess_params.cipher_iv = NULL;

	/* Get max key length. */
	if (cipher_xform->cipher.key.length >
		cipher_map[cipher_xform->cipher.algo].max_key_len) {
		MRVL_CRYPTO_LOG_ERR("Wrong key length!");
		return -EINVAL;
	}

	sess->sam_sess_params.cipher_key_len = cipher_xform->cipher.key.length;
	sess->sam_sess_params.cipher_key = cipher_xform->cipher.key.data;

	return 0;
}

/**
 * Set session parameters for authentication part.
 *
 * @param sess Crypto session pointer.
 * @param auth_xform Pointer to configuration structure for auth operations.
 * @returns 0 in case of success, negative value otherwise.
 */
static int
mrvl_crypto_set_auth_session_parameters(struct mrvl_crypto_session *sess,
		const struct rte_crypto_sym_xform *auth_xform)
{
	/* Make sure we've got proper struct */
	if (auth_xform->type != RTE_CRYPTO_SYM_XFORM_AUTH) {
		MRVL_CRYPTO_LOG_ERR("Wrong xform struct provided!");
		return -EINVAL;
	}

	/* See if map data is present and valid */
	if ((auth_xform->auth.algo > RTE_DIM(auth_map)) ||
		(auth_map[auth_xform->auth.algo].supported != ALGO_SUPPORTED)) {
		MRVL_CRYPTO_LOG_ERR("Auth algorithm not supported!");
		return -EINVAL;
	}

	sess->sam_sess_params.dir =
		(auth_xform->auth.op == RTE_CRYPTO_AUTH_OP_GENERATE) ?
		SAM_DIR_ENCRYPT : SAM_DIR_DECRYPT;
	sess->sam_sess_params.auth_alg =
		auth_map[auth_xform->auth.algo].auth_alg;
	sess->sam_sess_params.u.basic.auth_icv_len =
		auth_xform->auth.digest_length;
	/* auth_key must be NULL if auth algorithm does not use HMAC */
	sess->sam_sess_params.auth_key = auth_xform->auth.key.length ?
					 auth_xform->auth.key.data : NULL;
	sess->sam_sess_params.auth_key_len = auth_xform->auth.key.length;

	return 0;
}

/**
 * Set session parameters for aead part.
 *
 * @param sess Crypto session pointer.
 * @param aead_xform Pointer to configuration structure for aead operations.
 * @returns 0 in case of success, negative value otherwise.
 */
static int
mrvl_crypto_set_aead_session_parameters(struct mrvl_crypto_session *sess,
		const struct rte_crypto_sym_xform *aead_xform)
{
	/* Make sure we've got proper struct */
	if (aead_xform->type != RTE_CRYPTO_SYM_XFORM_AEAD) {
		MRVL_CRYPTO_LOG_ERR("Wrong xform struct provided!");
		return -EINVAL;
	}

	/* See if map data is present and valid */
	if ((aead_xform->aead.algo > RTE_DIM(aead_map)) ||
		(aead_map[aead_xform->aead.algo].supported
			!= ALGO_SUPPORTED)) {
		MRVL_CRYPTO_LOG_ERR("AEAD algorithm not supported!");
		return -EINVAL;
	}

	sess->sam_sess_params.dir =
		(aead_xform->aead.op == RTE_CRYPTO_AEAD_OP_ENCRYPT) ?
		SAM_DIR_ENCRYPT : SAM_DIR_DECRYPT;
	sess->sam_sess_params.cipher_alg =
		aead_map[aead_xform->aead.algo].cipher_alg;
	sess->sam_sess_params.cipher_mode =
		aead_map[aead_xform->aead.algo].cipher_mode;

	/* Assume IV will be passed together with data. */
	sess->sam_sess_params.cipher_iv = NULL;

	/* Get max key length. */
	if (aead_xform->aead.key.length >
		aead_map[aead_xform->aead.algo].max_key_len) {
		MRVL_CRYPTO_LOG_ERR("Wrong key length!");
		return -EINVAL;
	}

	sess->sam_sess_params.cipher_key = aead_xform->aead.key.data;
	sess->sam_sess_params.cipher_key_len = aead_xform->aead.key.length;

	if (sess->sam_sess_params.cipher_mode == SAM_CIPHER_GCM)
		sess->sam_sess_params.auth_alg = SAM_AUTH_AES_GCM;

	sess->sam_sess_params.u.basic.auth_icv_len =
		aead_xform->aead.digest_length;

	sess->sam_sess_params.u.basic.auth_aad_len =
		aead_xform->aead.aad_length;

	return 0;
}

/**
 * Parse crypto transform chain and setup session parameters.
 *
 * @param dev Pointer to crypto device
 * @param sess Poiner to crypto session
 * @param xform Pointer to configuration structure chain for crypto operations.
 * @returns 0 in case of success, negative value otherwise.
 */
int
mrvl_crypto_set_session_parameters(struct mrvl_crypto_session *sess,
		const struct rte_crypto_sym_xform *xform)
{
	const struct rte_crypto_sym_xform *cipher_xform = NULL;
	const struct rte_crypto_sym_xform *auth_xform = NULL;
	const struct rte_crypto_sym_xform *aead_xform = NULL;

	/* Filter out spurious/broken requests */
	if (xform == NULL)
		return -EINVAL;

	sess->chain_order = mrvl_crypto_get_chain_order(xform);
	switch (sess->chain_order) {
	case MRVL_CRYPTO_CHAIN_CIPHER_AUTH:
		cipher_xform = xform;
		auth_xform = xform->next;
		break;
	case MRVL_CRYPTO_CHAIN_AUTH_CIPHER:
		auth_xform = xform;
		cipher_xform = xform->next;
		break;
	case MRVL_CRYPTO_CHAIN_CIPHER_ONLY:
		cipher_xform = xform;
		break;
	case MRVL_CRYPTO_CHAIN_AUTH_ONLY:
		auth_xform = xform;
		break;
	case MRVL_CRYPTO_CHAIN_COMBINED:
		aead_xform = xform;
		break;
	default:
		return -EINVAL;
	}

	if ((cipher_xform != NULL) &&
		(mrvl_crypto_set_cipher_session_parameters(
			sess, cipher_xform) < 0)) {
		MRVL_CRYPTO_LOG_ERR("Invalid/unsupported cipher parameters");
		return -EINVAL;
	}

	if ((auth_xform != NULL) &&
		(mrvl_crypto_set_auth_session_parameters(
			sess, auth_xform) < 0)) {
		MRVL_CRYPTO_LOG_ERR("Invalid/unsupported auth parameters");
		return -EINVAL;
	}

	if ((aead_xform != NULL) &&
		(mrvl_crypto_set_aead_session_parameters(
			sess, aead_xform) < 0)) {
		MRVL_CRYPTO_LOG_ERR("Invalid/unsupported aead parameters");
		return -EINVAL;
	}

	return 0;
}

/*
 *-----------------------------------------------------------------------------
 * Process Operations
 *-----------------------------------------------------------------------------
 */

/**
 * Prepare a single request.
 *
 * This function basically translates DPDK crypto request into one
 * understandable by MUDSK's SAM. If this is a first request in a session,
 * it starts the session.
 *
 * @param request Pointer to pre-allocated && reset request buffer [Out].
 * @param src_bd Pointer to pre-allocated source descriptor [Out].
 * @param dst_bd Pointer to pre-allocated destination descriptor [Out].
 * @param op Pointer to DPDK crypto operation struct [In].
 */
static inline int
mrvl_request_prepare(struct sam_cio_op_params *request,
		struct sam_buf_info *src_bd,
		struct sam_buf_info *dst_bd,
		struct rte_crypto_op *op)
{
	struct mrvl_crypto_session *sess;
	struct rte_mbuf *src_mbuf, *dst_mbuf;
	uint16_t segments_nb;
	uint8_t *digest;
	int i;

	if (unlikely(op->sess_type == RTE_CRYPTO_OP_SESSIONLESS)) {
		MRVL_CRYPTO_LOG_ERR("MRVL CRYPTO PMD only supports session "
				"oriented requests, op (%p) is sessionless.",
				op);
		return -EINVAL;
	}

	sess = (struct mrvl_crypto_session *)get_session_private_data(
			op->sym->session, cryptodev_driver_id);
	if (unlikely(sess == NULL)) {
		MRVL_CRYPTO_LOG_ERR("Session was not created for this device");
		return -EINVAL;
	}

	request->sa = sess->sam_sess;
	request->cookie = op;

	src_mbuf = op->sym->m_src;
	segments_nb = src_mbuf->nb_segs;
	/* The following conditions must be met:
	 * - Destination buffer is required when segmented source buffer
	 * - Segmented destination buffer is not supported
	 */
	if ((segments_nb > 1) && (!op->sym->m_dst)) {
		MRVL_CRYPTO_LOG_ERR("op->sym->m_dst = NULL!\n");
		return -1;
	}
	/* For non SG case:
	 * If application delivered us null dst buffer, it means it expects
	 * us to deliver the result in src buffer.
	 */
	dst_mbuf = op->sym->m_dst ? op->sym->m_dst : op->sym->m_src;

	if (!rte_pktmbuf_is_contiguous(dst_mbuf)) {
		MRVL_CRYPTO_LOG_ERR("Segmented destination buffer "
				    "not supported.\n");
		return -1;
	}

	request->num_bufs = segments_nb;
	for (i = 0; i < segments_nb; i++) {
		/* Empty source. */
		if (rte_pktmbuf_data_len(src_mbuf) == 0) {
			/* EIP does not support 0 length buffers. */
			MRVL_CRYPTO_LOG_ERR("Buffer length == 0 not supported!");
			return -1;
		}
		src_bd[i].vaddr = rte_pktmbuf_mtod(src_mbuf, void *);
		src_bd[i].paddr = rte_pktmbuf_iova(src_mbuf);
		src_bd[i].len = rte_pktmbuf_data_len(src_mbuf);

		src_mbuf = src_mbuf->next;
	}
	request->src = src_bd;

	/* Empty destination. */
	if (rte_pktmbuf_data_len(dst_mbuf) == 0) {
		/* Make dst buffer fit at least source data. */
		if (rte_pktmbuf_append(dst_mbuf,
			rte_pktmbuf_data_len(op->sym->m_src)) == NULL) {
			MRVL_CRYPTO_LOG_ERR("Unable to set big enough dst buffer!");
			return -1;
		}
	}

	request->dst = dst_bd;
	dst_bd->vaddr = rte_pktmbuf_mtod(dst_mbuf, void *);
	dst_bd->paddr = rte_pktmbuf_iova(dst_mbuf);

	/*
	 * We can use all available space in dst_mbuf,
	 * not only what's used currently.
	 */
	dst_bd->len = dst_mbuf->buf_len - rte_pktmbuf_headroom(dst_mbuf);

	if (sess->chain_order == MRVL_CRYPTO_CHAIN_COMBINED) {
		request->cipher_len = op->sym->aead.data.length;
		request->cipher_offset = op->sym->aead.data.offset;
		request->cipher_iv = rte_crypto_op_ctod_offset(op, uint8_t *,
			sess->cipher_iv_offset);

		request->auth_aad = op->sym->aead.aad.data;
		request->auth_offset = request->cipher_offset;
		request->auth_len = request->cipher_len;
	} else {
		request->cipher_len = op->sym->cipher.data.length;
		request->cipher_offset = op->sym->cipher.data.offset;
		request->cipher_iv = rte_crypto_op_ctod_offset(op, uint8_t *,
				sess->cipher_iv_offset);

		request->auth_offset = op->sym->auth.data.offset;
		request->auth_len = op->sym->auth.data.length;
	}

	digest = sess->chain_order == MRVL_CRYPTO_CHAIN_COMBINED ?
		op->sym->aead.digest.data : op->sym->auth.digest.data;
	if (digest == NULL) {
		/* No auth - no worry. */
		return 0;
	}

	request->auth_icv_offset = request->auth_offset + request->auth_len;

	/*
	 * EIP supports only scenarios where ICV(digest buffer) is placed at
	 * auth_icv_offset.
	 */
	if (sess->sam_sess_params.dir == SAM_DIR_ENCRYPT) {
		/*
		 * This should be the most common case anyway,
		 * EIP will overwrite DST buffer at auth_icv_offset.
		 */
		if (rte_pktmbuf_mtod_offset(
				dst_mbuf, uint8_t *,
				request->auth_icv_offset) == digest)
			return 0;
	} else {/* sess->sam_sess_params.dir == SAM_DIR_DECRYPT */
		/*
		 * EIP will look for digest at auth_icv_offset
		 * offset in SRC buffer. It must be placed in the last
		 * segment and the offset must be set to reach digest
		 * in the last segment
		 */
		struct rte_mbuf *last_seg =  op->sym->m_src;
		uint32_t d_offset = request->auth_icv_offset;
		u32 d_size = sess->sam_sess_params.u.basic.auth_icv_len;
		unsigned char *d_ptr;

		/* Find the last segment and the offset for the last segment */
		while ((last_seg->next != NULL) &&
				(d_offset >= last_seg->data_len)) {
			d_offset -= last_seg->data_len;
			last_seg = last_seg->next;
		}

		if (rte_pktmbuf_mtod_offset(last_seg, uint8_t *,
					    d_offset) == digest)
			return 0;

		/* copy digest to last segment */
		if (last_seg->buf_len >= (d_size + d_offset)) {
			d_ptr = (unsigned char *)last_seg->buf_addr +
				 d_offset;
			rte_memcpy(d_ptr, digest, d_size);
			return 0;
		}
	}

	/*
	 * If we landed here it means that digest pointer is
	 * at different than expected place.
	 */
	return -1;
}

/*
 *-----------------------------------------------------------------------------
 * PMD Framework handlers
 *-----------------------------------------------------------------------------
 */

/**
 * Enqueue burst.
 *
 * @param queue_pair Pointer to queue pair.
 * @param ops Pointer to ops requests array.
 * @param nb_ops Number of elements in ops requests array.
 * @returns Number of elements consumed from ops.
 */
static uint16_t
mrvl_crypto_pmd_enqueue_burst(void *queue_pair, struct rte_crypto_op **ops,
		uint16_t nb_ops)
{
	uint16_t iter_ops = 0;
	uint16_t to_enq = 0;
	uint16_t consumed = 0;
	int ret;
	struct sam_cio_op_params requests[nb_ops];
	/*
	 * SAM does not store bd pointers, so on-stack scope will be enough.
	 */
	struct mrvl_crypto_src_table src_bd[nb_ops];
	struct sam_buf_info          dst_bd[nb_ops];
	struct mrvl_crypto_qp *qp = (struct mrvl_crypto_qp *)queue_pair;

	if (nb_ops == 0)
		return 0;

	/* Prepare the burst. */
	memset(&requests, 0, sizeof(requests));
	memset(&src_bd, 0, sizeof(src_bd));

	/* Iterate through */
	for (; iter_ops < nb_ops; ++iter_ops) {
		/* store the op id for debug */
		src_bd[iter_ops].iter_ops = iter_ops;
		if (mrvl_request_prepare(&requests[iter_ops],
					src_bd[iter_ops].src_bd,
					&dst_bd[iter_ops],
					ops[iter_ops]) < 0) {
			MRVL_CRYPTO_LOG_ERR(
				"Error while parameters preparation!");
			qp->stats.enqueue_err_count++;
			ops[iter_ops]->status = RTE_CRYPTO_OP_STATUS_ERROR;

			/*
			 * Number of handled ops is increased
			 * (even if the result of handling is error).
			 */
			++consumed;
			break;
		}

		ops[iter_ops]->status =
			RTE_CRYPTO_OP_STATUS_NOT_PROCESSED;

		/* Increase the number of ops to enqueue. */
		++to_enq;
	} /* for (; iter_ops < nb_ops;... */

	if (to_enq > 0) {
		/* Send the burst */
		ret = sam_cio_enq(qp->cio, requests, &to_enq);
		consumed += to_enq;
		if (ret < 0) {
			/*
			 * Trust SAM that in this case returned value will be at
			 * some point correct (now it is returned unmodified).
			 */
			qp->stats.enqueue_err_count += to_enq;
			for (iter_ops = 0; iter_ops < to_enq; ++iter_ops)
				ops[iter_ops]->status =
					RTE_CRYPTO_OP_STATUS_ERROR;
		}
	}

	qp->stats.enqueued_count += to_enq;
	return consumed;
}

/**
 * Dequeue burst.
 *
 * @param queue_pair Pointer to queue pair.
 * @param ops Pointer to ops requests array.
 * @param nb_ops Number of elements in ops requests array.
 * @returns Number of elements dequeued.
 */
static uint16_t
mrvl_crypto_pmd_dequeue_burst(void *queue_pair,
		struct rte_crypto_op **ops,
		uint16_t nb_ops)
{
	int ret;
	struct mrvl_crypto_qp *qp = queue_pair;
	struct sam_cio *cio = qp->cio;
	struct sam_cio_op_result results[nb_ops];
	uint16_t i;

	ret = sam_cio_deq(cio, results, &nb_ops);
	if (ret < 0) {
		/* Count all dequeued as error. */
		qp->stats.dequeue_err_count += nb_ops;

		/* But act as they were dequeued anyway*/
		qp->stats.dequeued_count += nb_ops;

		return 0;
	}

	/* Unpack and check results. */
	for (i = 0; i < nb_ops; ++i) {
		ops[i] = results[i].cookie;

		switch (results[i].status) {
		case SAM_CIO_OK:
			ops[i]->status = RTE_CRYPTO_OP_STATUS_SUCCESS;
			break;
		case SAM_CIO_ERR_ICV:
			MRVL_CRYPTO_LOG_DBG("CIO returned SAM_CIO_ERR_ICV.");
			ops[i]->status = RTE_CRYPTO_OP_STATUS_AUTH_FAILED;
			break;
		default:
			MRVL_CRYPTO_LOG_DBG(
				"CIO returned Error: %d", results[i].status);
			ops[i]->status = RTE_CRYPTO_OP_STATUS_ERROR;
			break;
		}
	}

	qp->stats.dequeued_count += nb_ops;
	return nb_ops;
}

/**
 * Create a new crypto device.
 *
 * @param name Driver name.
 * @param vdev Pointer to device structure.
 * @param init_params Pointer to initialization parameters.
 * @returns 0 in case of success, negative value otherwise.
 */
static int
cryptodev_mrvl_crypto_create(const char *name,
		struct rte_vdev_device *vdev,
		struct rte_cryptodev_pmd_init_params *init_params)
{
	struct rte_cryptodev *dev;
	struct mrvl_crypto_private *internals;
	struct sam_init_params	sam_params;
	int ret;

	dev = rte_cryptodev_pmd_create(name, &vdev->device, init_params);
	if (dev == NULL) {
		MRVL_CRYPTO_LOG_ERR("failed to create cryptodev vdev");
		goto init_error;
	}

	dev->driver_id = cryptodev_driver_id;
	dev->dev_ops = rte_mrvl_crypto_pmd_ops;

	/* Register rx/tx burst functions for data path. */
	dev->enqueue_burst = mrvl_crypto_pmd_enqueue_burst;
	dev->dequeue_burst = mrvl_crypto_pmd_dequeue_burst;

	dev->feature_flags = RTE_CRYPTODEV_FF_SYMMETRIC_CRYPTO |
			RTE_CRYPTODEV_FF_SYM_OPERATION_CHAINING |
			RTE_CRYPTODEV_FF_HW_ACCELERATED;

	/* Set vector instructions mode supported */
	internals = dev->data->dev_private;

	internals->max_nb_qpairs = init_params->max_nb_queue_pairs;
	internals->max_nb_sessions = init_params->max_nb_sessions;

	/*
	 * ret == -EEXIST is correct, it means DMA
	 * has been already initialized.
	 */
	ret = mv_sys_dma_mem_init(MRVL_MUSDK_DMA_MEMSIZE);
	if (ret < 0) {
		if (ret != -EEXIST)
			return ret;

		MRVL_CRYPTO_LOG_INFO(
			"DMA memory has been already initialized by a different driver.");
	}

	sam_params.max_num_sessions = internals->max_nb_sessions;

	/*sam_set_debug_flags(3);*/

	return sam_init(&sam_params);

init_error:
	MRVL_CRYPTO_LOG_ERR(
		"driver %s: %s failed", init_params->name, __func__);

	cryptodev_mrvl_crypto_uninit(vdev);
	return -EFAULT;
}

/**
 * Initialize the crypto device.
 *
 * @param vdev Pointer to device structure.
 * @returns 0 in case of success, negative value otherwise.
 */
static int
cryptodev_mrvl_crypto_init(struct rte_vdev_device *vdev)
{
	struct rte_cryptodev_pmd_init_params init_params = { };
	const char *name, *args;
	int ret;

	name = rte_vdev_device_name(vdev);
	if (name == NULL)
		return -EINVAL;
	args = rte_vdev_device_args(vdev);

	init_params.private_data_size = sizeof(struct mrvl_crypto_private);
	init_params.max_nb_queue_pairs = sam_get_num_inst() *
					sam_get_num_cios(0);
	init_params.max_nb_sessions =
		RTE_CRYPTODEV_PMD_DEFAULT_MAX_NB_SESSIONS;
	init_params.socket_id = rte_socket_id();

	ret = rte_cryptodev_pmd_parse_input_args(&init_params, args);
	if (ret) {
		RTE_LOG(ERR, PMD,
			"Failed to parse initialisation arguments[%s]\n",
			args);
		return -EINVAL;
	}

	return cryptodev_mrvl_crypto_create(name, vdev, &init_params);
}

/**
 * Uninitialize the crypto device
 *
 * @param vdev Pointer to device structure.
 * @returns 0 in case of success, negative value otherwise.
 */
static int
cryptodev_mrvl_crypto_uninit(struct rte_vdev_device *vdev)
{
	struct rte_cryptodev *cryptodev;
	const char *name = rte_vdev_device_name(vdev);

	if (name == NULL)
		return -EINVAL;

	RTE_LOG(INFO, PMD,
		"Closing Marvell crypto device %s on numa socket %u\n",
		name, rte_socket_id());

	sam_deinit();

	cryptodev = rte_cryptodev_pmd_get_named_dev(name);
	if (cryptodev == NULL)
		return -ENODEV;

	return rte_cryptodev_pmd_destroy(cryptodev);
}

/**
 * Basic driver handlers for use in the constructor.
 */
static struct rte_vdev_driver cryptodev_mrvl_pmd_drv = {
	.probe = cryptodev_mrvl_crypto_init,
	.remove = cryptodev_mrvl_crypto_uninit
};

static struct cryptodev_driver mrvl_crypto_drv;

/* Register the driver in constructor. */
RTE_PMD_REGISTER_VDEV(CRYPTODEV_NAME_MRVL_PMD, cryptodev_mrvl_pmd_drv);
RTE_PMD_REGISTER_PARAM_STRING(CRYPTODEV_NAME_MRVL_PMD,
	"max_nb_queue_pairs=<int> "
	"max_nb_sessions=<int> "
	"socket_id=<int>");
RTE_PMD_REGISTER_CRYPTO_DRIVER(mrvl_crypto_drv, cryptodev_mrvl_pmd_drv,
		cryptodev_driver_id);
