// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2017-2020, Linaro Limited
 */

/* BINARY_PREFIX is expected by teec_trace.h */
#ifndef BINARY_PREFIX
#define BINARY_PREFIX		"ckteec"
#endif

#include <inttypes.h>
#include <pkcs11.h>
#include <pkcs11_ta.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <tee_client_api.h>
#include <teec_trace.h>

#include "ck_helpers.h"
#include "invoke_ta.h"
#include "local_utils.h"

struct ta_context {
	pthread_mutex_t init_mutex;
	bool initiated;
	TEEC_Context context;
	TEEC_Session session;
};

static struct ta_context ta_ctx = {
	.init_mutex = PTHREAD_MUTEX_INITIALIZER,
};

bool ckteec_invoke_initiated(void)
{
	return ta_ctx.initiated;
}

TEEC_SharedMemory *ckteec_alloc_shm(size_t size, enum ckteec_shm_dir dir)
{
	TEEC_SharedMemory *shm;

	switch (dir) {
	case CKTEEC_SHM_IN:
	case CKTEEC_SHM_OUT:
	case CKTEEC_SHM_INOUT:
		break;
	default:
		return NULL;
	}

	shm = calloc(1, sizeof(TEEC_SharedMemory));
	if (!shm)
		return NULL;

	shm->size = size;

	if (dir == CKTEEC_SHM_IN || dir == CKTEEC_SHM_INOUT)
		shm->flags |= TEEC_MEM_INPUT;
	if (dir == CKTEEC_SHM_OUT || dir == CKTEEC_SHM_INOUT)
		shm->flags |= TEEC_MEM_OUTPUT;

	if (TEEC_AllocateSharedMemory(&ta_ctx.context, shm)) {
		free(shm);
		return NULL;
	}

	return shm;
}

TEEC_SharedMemory *ckteec_register_shm(void *buffer, size_t size,
				       enum ckteec_shm_dir dir)
{
	TEEC_SharedMemory *shm;

	switch (dir) {
	case CKTEEC_SHM_IN:
	case CKTEEC_SHM_OUT:
	case CKTEEC_SHM_INOUT:
		break;
	default:
		return NULL;
	}

	shm = calloc(1, sizeof(TEEC_SharedMemory));
	if (!shm)
		return NULL;

	shm->buffer = buffer;
	shm->size = size;

	if (dir == CKTEEC_SHM_IN || dir == CKTEEC_SHM_INOUT)
		shm->flags |= TEEC_MEM_INPUT;
	if (dir == CKTEEC_SHM_OUT || dir == CKTEEC_SHM_INOUT)
		shm->flags |= TEEC_MEM_OUTPUT;

	if (TEEC_RegisterSharedMemory(&ta_ctx.context, shm)) {
		free(shm);
		return NULL;
	}

	return shm;
}

void ckteec_free_shm(TEEC_SharedMemory *shm)
{
	TEEC_ReleaseSharedMemory(shm);
	free(shm);
}

CK_RV ckteec_invoke_ta(unsigned long cmd, TEEC_SharedMemory *ctrl,
		       TEEC_SharedMemory *io1, TEEC_SharedMemory *io2,
		       TEEC_SharedMemory *io3)
{
	uint32_t command = (uint32_t)cmd;
	TEEC_Operation op;
	uint32_t origin = 0;
	TEEC_Result res = TEEC_ERROR_GENERIC;
	uint32_t ta_rc = PKCS11_CKR_GENERAL_ERROR;

	memset(&op, 0, sizeof(op));

	if (ctrl && !(ctrl->flags & TEEC_MEM_INPUT &&
		      ctrl->flags & TEEC_MEM_OUTPUT))
		return CKR_ARGUMENTS_BAD;

	if (ctrl) {
		op.paramTypes |= TEEC_PARAM_TYPES(TEEC_MEMREF_WHOLE, 0, 0, 0);
		op.params[0].memref.parent = ctrl;
	} else {
		/* TA mandates param#0 as in/out memref for output status */
		op.paramTypes |= TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INOUT,
						  0, 0, 0);
		op.params[0].tmpref.buffer = &ta_rc;
		op.params[0].tmpref.size = sizeof(ta_rc);
	}

	if (io1) {
		op.paramTypes |= TEEC_PARAM_TYPES(0, TEEC_MEMREF_WHOLE, 0, 0);
		op.params[1].memref.parent = io1;
	}

	if (io2) {
		op.paramTypes |= TEEC_PARAM_TYPES(0, 0, TEEC_MEMREF_WHOLE, 0);
		op.params[2].memref.parent = io2;
	}

	if (io3) {
		op.paramTypes |= TEEC_PARAM_TYPES(0, 0, 0, TEEC_MEMREF_WHOLE);
		op.params[3].memref.parent = io3;
	}

	res = TEEC_InvokeCommand(&ta_ctx.session, command, &op, &origin);
	switch (res) {
	case TEEC_SUCCESS:
		break;
	case TEEC_ERROR_SHORT_BUFFER:
		return CKR_BUFFER_TOO_SMALL;
	case TEEC_ERROR_OUT_OF_MEMORY:
		return CKR_DEVICE_MEMORY;
	default:
		return CKR_GENERAL_ERROR;
	}

	/* Get PKCS11 TA return value from ctrl buffer */
	if (ctrl) {
		if (op.params[0].memref.size == sizeof(ta_rc))
			memcpy(&ta_rc, ctrl->buffer, sizeof(ta_rc));
	} else {
		if (op.params[0].tmpref.size != sizeof(ta_rc))
			ta_rc = PKCS11_CKR_GENERAL_ERROR;
	}

	return ta_rc;
}

static CK_RV ping_ta(void)
{
	TEEC_Operation op;
	uint32_t origin;
	TEEC_Result res;
	uint32_t ta_version[3];
	uint32_t status;

	memset(&op, 0, sizeof(op));
	op.params[0].tmpref.buffer = &status;
	op.params[0].tmpref.size = sizeof(status);
	op.params[2].tmpref.buffer = ta_version;
	op.params[2].tmpref.size = sizeof(ta_version);
	op.paramTypes = TEEC_PARAM_TYPES(TEEC_MEMREF_TEMP_INOUT, TEEC_NONE,
					 TEEC_MEMREF_TEMP_OUTPUT, TEEC_NONE);

	res = TEEC_InvokeCommand(&ta_ctx.session, PKCS11_CMD_PING, &op,
				 &origin);

	if (res != TEEC_SUCCESS ||
	    origin != TEEC_ORIGIN_TRUSTED_APP ||
	    op.params[0].tmpref.size != sizeof(status) ||
	    status != PKCS11_CKR_OK)
		return CKR_DEVICE_ERROR;

	if (ta_version[0] != PKCS11_TA_VERSION_MAJOR &&
	    ta_version[1] > PKCS11_TA_VERSION_MINOR) {
		EMSG("PKCS11 TA version mismatch: %"PRIu32".%"PRIu32".%"PRIu32,
		     ta_version[0], ta_version[1], ta_version[2]);

		return CKR_DEVICE_ERROR;
	}

	DMSG("PKCS11 TA version %"PRIu32".%"PRIu32".%"PRIu32,
	     ta_version[0], ta_version[1], ta_version[2]);

	return CKR_OK;
}

CK_RV ckteec_invoke_init(void)
{
	TEEC_UUID uuid = PKCS11_TA_UUID;
	uint32_t origin = 0;
	TEEC_Result res = TEEC_SUCCESS;
	CK_RV rv = CKR_CRYPTOKI_ALREADY_INITIALIZED;
	int e;

	e = pthread_mutex_lock(&ta_ctx.init_mutex);
	if (e) {
		EMSG("pthread_mutex_lock: %s", strerror(e));
		EMSG("terminating...");
		exit(EXIT_FAILURE);
	}

	if (ta_ctx.initiated) {
		rv = CKR_CRYPTOKI_ALREADY_INITIALIZED;
		goto out;
	}

	res = TEEC_InitializeContext(NULL, &ta_ctx.context);
	if (res != TEEC_SUCCESS) {
		EMSG("TEEC init context failed\n");
		rv = CKR_DEVICE_ERROR;
		goto out;
	}

	res = TEEC_OpenSession(&ta_ctx.context, &ta_ctx.session, &uuid,
			       TEEC_LOGIN_PUBLIC, NULL, NULL, &origin);
	if (res != TEEC_SUCCESS) {
		EMSG("TEEC open session failed %x from %d\n", res, origin);
		TEEC_FinalizeContext(&ta_ctx.context);
		rv = CKR_DEVICE_ERROR;
		goto out;
	}

	rv = ping_ta();

	if (rv == CKR_OK) {
		ta_ctx.initiated = true;
	} else {
		TEEC_CloseSession(&ta_ctx.session);
		TEEC_FinalizeContext(&ta_ctx.context);
	}

out:
	e = pthread_mutex_unlock(&ta_ctx.init_mutex);
	if (e) {
		EMSG("pthread_mutex_unlock: %s", strerror(e));
		EMSG("terminating...");
		exit(EXIT_FAILURE);
	}

	return rv;
}

CK_RV ckteec_invoke_terminate(void)
{
	CK_RV rv = CKR_CRYPTOKI_NOT_INITIALIZED;
	int e;

	e = pthread_mutex_lock(&ta_ctx.init_mutex);
	if (e) {
		EMSG("pthread_mutex_lock: %s", strerror(e));
		EMSG("terminating...");
		exit(EXIT_FAILURE);
	}

	if (!ta_ctx.initiated)
		goto out;

	ta_ctx.initiated = false;
	TEEC_CloseSession(&ta_ctx.session);
	TEEC_FinalizeContext(&ta_ctx.context);

	rv = CKR_OK;

out:
	e = pthread_mutex_unlock(&ta_ctx.init_mutex);
	if (e) {
		EMSG("pthread_mutex_unlock: %s", strerror(e));
		EMSG("terminating...");
		exit(EXIT_FAILURE);
	}

	return rv;
}
