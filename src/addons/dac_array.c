/**
 * (C) Copyright 2016 Intel Corporation.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * GOVERNMENT LICENSE RIGHTS-OPEN SOURCE SOFTWARE
 * The Government's rights to use, modify, reproduce, release, perform, display,
 * or disclose this software are subject to the terms of the Apache License as
 * provided in Contract No. B609815.
 * Any reproduction of computer software, computer software documentation, or
 * portions thereof marked with this legend must also reproduce the markings.
 */
/**
 * This file is part of daos_m
 *
 * src/addons/dac_array.c
 */

#define DD_SUBSYS	DD_FAC(client)

#include <daos/common.h>
#include <daos/scheduler.h>
#include <daos_api.h>
#include <daos_array.h>
#include "array_internal.h"

/*#define ARRAY_DEBUG */

/** Num blocks to store in each dkey before creating the next group */
#define D_ARRAY_DKEY_NUM_BLOCKS		3
/** Number of dkeys in a group */
#define D_ARRAY_DKEY_NUM		4

#define ARRAY_MD_KEY "daos_array_metadata"
#define CELL_SIZE "daos_array_cell_size"
#define BLOCK_SIZE "daos_array_block_size"

struct dac_array {
	/** DAOS KV object handle */
	daos_handle_t	daos_oh;
	/** Array cell size of each element */
	daos_size_t	cell_size;
	/** elems to store in 1 dkey before moving to the next one in the grp */
	daos_size_t	block_size;
	/** Num blocks to store in each dkey before creating the next group */
	daos_size_t	num_blocks;
	/** Number of dkeys in a group */
	daos_size_t	num_dkeys;
	/** ref count on array */
	unsigned int	cob_ref;
};

struct io_params {
	daos_key_t		dkey;
	char			*dkey_str;
	char			*akey_str;
	daos_iod_t		iod;
	daos_sg_list_t		sgl;
	bool			user_sgl_used;
	struct daos_task	*task;
	struct io_params	*next;
};

static struct dac_array *
array_alloc(void)
{
	struct dac_array *obj;

	D_ALLOC_PTR(obj);
	if (obj == NULL)
		return NULL;

	obj->cob_ref = 1;
	return obj;
}

static void
array_decref(struct dac_array *obj)
{
	obj->cob_ref--;
	if (obj->cob_ref == 0)
		D_FREE_PTR(obj);
}

static void
array_addref(struct dac_array *obj)
{
	obj->cob_ref++;
}

static daos_handle_t
array_ptr2hdl(struct dac_array *obj)
{
	daos_handle_t oh;

	oh.cookie = (uint64_t)obj;
	return oh;
}

static struct dac_array *
array_hdl2ptr(daos_handle_t oh)
{
	struct dac_array *obj;

	obj = (struct dac_array *)oh.cookie;
	array_addref(obj);
	return obj;
}


static int
free_io_params_cb(struct daos_task *task, void *data)
{
	struct io_params *io_list = *((struct io_params **)data);

	while (io_list) {
		struct io_params *current = io_list;

		if (current->iod.iod_recxs) {
			free(current->iod.iod_recxs);
			current->iod.iod_recxs = NULL;
		}
		if (current->sgl.sg_iovs) {
			free(current->sgl.sg_iovs);
			current->sgl.sg_iovs = NULL;
		}
		if (current->dkey_str) {
			free(current->dkey_str);
			current->dkey_str = NULL;
		}
		if (current->akey_str) {
			free(current->akey_str);
			current->akey_str = NULL;
		}

		io_list = current->next;
		D_FREE_PTR(current);
	}

	return 0;
}

static int
create_handle_cb(struct daos_task *task, void *data)
{
	struct dac_array_create_t *args = *((struct dac_array_create_t **)data);
	struct dac_array	*array;
	int			rc = task->dt_result;

	if (rc != 0) {
		/** attempt to close the OH */
		daos_obj_close(*args->oh, NULL);
		return rc;
	}

	/** Create an array OH from the DAOS one */
	array = array_alloc();
	if (array == NULL) {
		D_ERROR("Failed memory allocation\n");
		daos_obj_close(*args->oh, NULL);
		return -DER_NOMEM;
	}

	array->daos_oh = *args->oh;
	array->cell_size = args->cell_size;
	array->block_size = args->block_size;
	array->num_blocks = D_ARRAY_DKEY_NUM_BLOCKS;
	array->num_dkeys = D_ARRAY_DKEY_NUM;

	*args->oh = array_ptr2hdl(array);

	return 0;
}

static int
free_handle_cb(struct daos_task *task, void *data)
{
	daos_handle_t		*oh = (daos_handle_t *)data;
	struct dac_array	*array;
	int			rc = task->dt_result;

	if (rc != 0)
		return rc;

	array = array_hdl2ptr(*oh);
	if (array == NULL)
		return -DER_NO_HDL;

	/** -1 for hdl2ptr */
	array_decref(array);
	/** -1 for array_create/open */
	array_decref(array);

	return 0;
}

static int
write_md_cb(struct daos_task *task, void *data)
{
	struct dac_array_create_t *args = *((struct dac_array_create_t **)data);
	daos_obj_update_t *update_args;
	struct io_params *params;
	int rc = task->dt_result;

	if (rc != 0)
		return rc;

	D_ALLOC_PTR(params);
	if (params == NULL) {
		D_ERROR("Failed memory allocation\n");
		return -DER_NOMEM;
	}
	params->next = NULL;
	params->user_sgl_used = false;

	/** init dkey */
	daos_iov_set(&params->dkey, ARRAY_MD_KEY, strlen(ARRAY_MD_KEY));

	/** init scatter/gather */
	params->sgl.sg_iovs = malloc(sizeof(daos_iov_t) * 2);
	daos_iov_set(&params->sgl.sg_iovs[0], &args->cell_size,
		     sizeof(daos_size_t));
	daos_iov_set(&params->sgl.sg_iovs[1], &args->block_size,
		     sizeof(daos_size_t));
	params->sgl.sg_nr.num		= 2;
	params->sgl.sg_nr.num_out	= 0;

	/** init I/O descriptor */
	daos_iov_set(&params->iod.iod_name, ARRAY_MD_KEY, strlen(ARRAY_MD_KEY));
	daos_csum_set(&params->iod.iod_kcsum, NULL, 0);
	params->iod.iod_recxs = malloc(sizeof(daos_recx_t));
	params->iod.iod_nr = 1;
	params->iod.iod_recxs[0].rx_idx = 0;
	params->iod.iod_recxs[0].rx_nr = 2;
	params->iod.iod_eprs	= NULL;
	params->iod.iod_csums	= NULL;
	params->iod.iod_size = sizeof(daos_size_t);
	params->iod.iod_type = DAOS_IOD_ARRAY;

	/** Set the args for the update task */
	update_args = daos_task_get_args(DAOS_OPC_OBJ_UPDATE, task);
	update_args->oh = *args->oh;
	update_args->epoch = args->epoch;
	update_args->dkey = &params->dkey;
	update_args->nr = 1;
	update_args->iods = &params->iod;
	update_args->sgls = &params->sgl;

	rc = daos_task_register_comp_cb(task, free_io_params_cb, sizeof(params),
					&params);
	if (rc != 0)
		return rc;

	return 0;
}

int
dac_array_create(struct daos_task *task)
{
	struct dac_array_create_t *args = daos_task2arg(task);
	struct daos_task	*open_task, *update_task;
	daos_obj_open_t		open_args;
	int			rc;

	/** Create task to open object */
	D_ALLOC_PTR(open_task);
	if (open_task == NULL)
		return -DER_NOMEM;
	open_args.coh = args->coh;
	open_args.oid = args->oid;
	open_args.epoch = args->epoch;
	open_args.mode = DAOS_OO_RW;
	open_args.oh = args->oh;
	rc = daos_task_create(DAOS_OPC_OBJ_OPEN, daos_task2sched(task),
			      &open_args, 0, NULL, open_task);
	if (rc != 0) {
		D_ERROR("Failed to create object_open task\n");
		D_GOTO(err_put1, rc);
	}

	/** Create task to write object metadata */
	D_ALLOC_PTR(update_task);
	if (update_task == NULL)
		return -DER_NOMEM;
	rc = daos_task_create(DAOS_OPC_OBJ_UPDATE, daos_task2sched(task),
			      NULL, 1, &open_task, update_task);
	if (rc != 0) {
		D_ERROR("Failed to create object_update task\n");
		D_GOTO(err_put1, rc);
	}

	/** add a prepare CB to set the args for the metadata write */
	daos_task_register_cbs(update_task, write_md_cb, &args, sizeof(args),
			       NULL, NULL, 0);

	/** The upper task completes when the update task completes */
	daos_task_register_deps(task, 1, &update_task);

	/** CB to generate the array OH */
	daos_task_register_cbs(task, NULL, NULL, 0, create_handle_cb,
			       &args, sizeof(args));

	daos_sched_progress(daos_task2sched(task));

	return rc;

err_put1:
	D_FREE_PTR(open_task);
	return rc;
}

static int
open_handle_cb(struct daos_task *task, void *data)
{
	struct dac_array_open_t *args = *((struct dac_array_open_t **)data);
	struct dac_array	*array;
	int			rc = task->dt_result;

	if (rc != 0) {
		/** attempt to close the OH */
		daos_obj_close(*args->oh, NULL);
		return rc;
	}

	/** If no cell and block size, this isn't an array obj. */
	if (*args->cell_size == 0 || *args->block_size == 0) {
		daos_obj_close(*args->oh, NULL);
		return -DER_NO_PERM;
	}

	/** Create an array OH from the DAOS one */
	array = array_alloc();
	if (array == NULL) {
		D_ERROR("Failed memory allocation\n");
		daos_obj_close(*args->oh, NULL);
		return -DER_NOMEM;
	}

	array->daos_oh = *args->oh;
	array->cell_size = *args->cell_size;
	array->block_size = *args->block_size;
	array->num_blocks = D_ARRAY_DKEY_NUM_BLOCKS;
	array->num_dkeys = D_ARRAY_DKEY_NUM;

	*args->oh = array_ptr2hdl(array);

	return 0;
}

static int
fetch_md_cb(struct daos_task *task, void *data)
{
	struct dac_array_open_t *args = *((struct dac_array_open_t **)data);
	daos_obj_fetch_t *fetch_args;
	struct io_params *params;
	int rc = task->dt_result;

	if (rc != 0)
		return rc;

	D_ALLOC_PTR(params);
	if (params == NULL) {
		D_ERROR("Failed memory allocation\n");
		return -DER_NOMEM;
	}
	params->next = NULL;
	params->user_sgl_used = false;

	/** init dkey */
	daos_iov_set(&params->dkey, ARRAY_MD_KEY, strlen(ARRAY_MD_KEY));

	/** init scatter/gather */
	params->sgl.sg_iovs = malloc(sizeof(daos_iov_t) * 2);
	daos_iov_set(&params->sgl.sg_iovs[0], args->cell_size,
		     sizeof(daos_size_t));
	daos_iov_set(&params->sgl.sg_iovs[1], args->block_size,
		     sizeof(daos_size_t));
	params->sgl.sg_nr.num		= 2;
	params->sgl.sg_nr.num_out	= 0;

	/** init I/O descriptor */
	daos_iov_set(&params->iod.iod_name, ARRAY_MD_KEY, strlen(ARRAY_MD_KEY));
	daos_csum_set(&params->iod.iod_kcsum, NULL, 0);
	params->iod.iod_recxs = malloc(sizeof(daos_recx_t));
	params->iod.iod_nr = 1;
	params->iod.iod_recxs[0].rx_idx = 0;
	params->iod.iod_recxs[0].rx_nr = 2;
	params->iod.iod_eprs	= NULL;
	params->iod.iod_csums	= NULL;
	params->iod.iod_size = sizeof(daos_size_t);
	params->iod.iod_type = DAOS_IOD_ARRAY;

	/** Set the args for the fetch task */
	fetch_args = daos_task_get_args(DAOS_OPC_OBJ_FETCH, task);
	fetch_args->oh = *args->oh;
	fetch_args->epoch = args->epoch;
	fetch_args->dkey = &params->dkey;
	fetch_args->nr = 1;
	fetch_args->iods = &params->iod;
	fetch_args->sgls = &params->sgl;

	rc = daos_task_register_comp_cb(task, free_io_params_cb, sizeof(params),
					&params);
	if (rc != 0)
		return rc;

	return 0;
}

int
dac_array_open(struct daos_task *task)
{
	struct dac_array_open_t *args = daos_task2arg(task);
	struct daos_task	*open_task, *fetch_task;
	daos_obj_open_t		open_args;
	int			rc;

	/** Open task to open object */
	D_ALLOC_PTR(open_task);
	if (open_task == NULL)
		return -DER_NOMEM;
	open_args.coh = args->coh;
	open_args.oid = args->oid;
	open_args.epoch = args->epoch;
	open_args.mode = args->mode;
	open_args.oh = args->oh;
	rc = daos_task_create(DAOS_OPC_OBJ_OPEN, daos_task2sched(task),
			      &open_args, 0, NULL, open_task);
	if (rc != 0) {
		D_ERROR("Failed to open object_open task\n");
		D_GOTO(err_put1, rc);
	}

	/** Create task to fetch object metadata */
	D_ALLOC_PTR(fetch_task);
	if (fetch_task == NULL)
		return -DER_NOMEM;
	rc = daos_task_create(DAOS_OPC_OBJ_FETCH, daos_task2sched(task),
			      NULL, 1, &open_task, fetch_task);
	if (rc != 0) {
		D_ERROR("Failed to open object_fetch task\n");
		D_GOTO(err_put1, rc);
	}
	/** add a prepare CB to set the args for the metadata fetch */
	daos_task_register_cbs(fetch_task, fetch_md_cb, &args, sizeof(args),
			       NULL, NULL, 0);

	/** The upper task completes when the fetch task completes */
	daos_task_register_deps(task, 1, &fetch_task);

	/** Add a completion CB on the upper task to generate the array OH */
	daos_task_register_cbs(task, NULL, NULL, 0, open_handle_cb,
			       &args, sizeof(args));

	daos_sched_progress(daos_task2sched(task));

	return rc;

err_put1:
	D_FREE_PTR(open_task);
	return rc;
}

int
dac_array_close(struct daos_task *task)
{
	struct dac_array_close_t *args = daos_task2arg(task);
	struct dac_array	*array;
	struct daos_task	*close_task;
	daos_obj_close_t	close_args;
	int			rc;

	array = array_hdl2ptr(args->oh);
	if (array == NULL)
		return -DER_NO_HDL;

	/** Create task to close object */
	D_ALLOC_PTR(close_task);
	if (close_task == NULL)
		D_GOTO(out, rc = -DER_NOMEM);
	close_args.oh = array->daos_oh;
	rc = daos_task_create(DAOS_OPC_OBJ_CLOSE, daos_task2sched(task),
			      &close_args, 0, NULL, close_task);
	if (rc != 0) {
		D_ERROR("Failed to create object_close task\n");
		D_FREE_PTR(close_task);
		D_GOTO(out, rc);
	}

	/** The upper task completes when the close task completes */
	daos_task_register_deps(task, 1, &close_task);

	/** Add a completion CB on the upper task to free the array */
	daos_task_register_cbs(task, NULL, NULL, 0, free_handle_cb,
			       &args->oh, sizeof(args->oh));

	daos_sched_progress(daos_task2sched(task));

out:
	array_decref(array);
	return rc;
}

static bool
io_extent_same(daos_array_ranges_t *ranges, daos_sg_list_t *sgl,
	       daos_size_t cell_size)
{
	daos_size_t ranges_len;
	daos_size_t sgl_len;
	daos_size_t u;

	ranges_len = 0;
#ifdef ARRAY_DEBUG
	printf("USER ARRAY RANGE -----------------------\n");
	printf("ranges_nr = %zu\n", ranges->ranges_nr);
#endif
	for (u = 0 ; u < ranges->ranges_nr ; u++) {
		ranges_len += ranges->ranges[u].len;
#ifdef ARRAY_DEBUG
		printf("%zu: length %zu, index %d\n",
			u, ranges->ranges[u].len, (int)ranges->ranges[u].index);
#endif
	}
#ifdef ARRAY_DEBUG
	printf("------------------------------------\n");
	printf("USER SGL -----------------------\n");
	printf("sg_nr = %u\n", sgl->sg_nr.num);
#endif
	sgl_len = 0;
	for (u = 0 ; u < sgl->sg_nr.num; u++) {
		sgl_len += sgl->sg_iovs[u].iov_len;
#ifdef ARRAY_DEBUG
		printf("%zu: length %zu, Buf %p\n",
			u, sgl->sg_iovs[u].iov_len, sgl->sg_iovs[u].iov_buf);
#endif
	}

	return (ranges_len * cell_size == sgl_len);
}

static int
compute_dkey(struct dac_array *array, daos_off_t array_idx,
	     daos_size_t *num_records, daos_off_t *record_i, char **dkey_str)
{
	daos_size_t	dkey_grp;	/* Which grp of dkeys to look into */
	daos_off_t	dkey_grp_a;	/* Byte address of dkey_grp */
	daos_off_t	rel_indx;	/* offset relative to grp */
	daos_size_t	dkey_num;	/* The dkey number for access */
	daos_size_t	grp_iter;	/* round robin iteration number */
	daos_off_t	dkey_idx;	/* address of dkey relative to group */
	daos_size_t	grp_size;
	daos_size_t	grp_chunk;

	grp_size = array->block_size * array->num_blocks * array->num_dkeys;
	grp_chunk = array->block_size * array->num_dkeys;

	/* Compute dkey group number and address */
	dkey_grp = array_idx / grp_size;
	dkey_grp_a = dkey_grp * grp_size;

	/* Compute dkey number within dkey group */
	rel_indx = array_idx - dkey_grp_a;
	dkey_num = (size_t)(rel_indx / array->block_size) %
		D_ARRAY_DKEY_NUM;

	/* Compute relative offset/index in dkey */
	grp_iter = rel_indx / grp_chunk;
	dkey_idx = (grp_iter * grp_chunk) +
		(dkey_num * array->block_size);
	*record_i = (array->block_size * grp_iter) +
		(rel_indx - dkey_idx);

	/* Number of records to access in current dkey */
	*num_records = ((grp_iter + 1) * array->block_size) - *record_i;

	if (dkey_str) {
		asprintf(dkey_str, "%zu_%zu", dkey_grp, dkey_num);
		if (*dkey_str == NULL) {
			D_ERROR("Failed memory allocation\n");
			return -DER_NOMEM;
		}
	}

	return 0;
}

static int
create_sgl(daos_sg_list_t *user_sgl, daos_size_t cell_size,
	   daos_size_t num_records, daos_off_t *sgl_off, daos_size_t *sgl_i,
	   daos_sg_list_t *sgl)
{
	daos_size_t	k;
	daos_size_t	rem_records;
	daos_size_t	cur_i;
	daos_off_t	cur_off;

	cur_i = *sgl_i;
	cur_off = *sgl_off;
	sgl->sg_nr.num = k = 0;
	sgl->sg_iovs = NULL;
	rem_records = num_records;

	/**
	 * Keep iterating through the user sgl till we populate our sgl to
	 * satisfy the number of records to read/write from the KV object
	 */
	do {
		D_ASSERT(user_sgl->sg_nr.num > cur_i);

		sgl->sg_nr.num++;
		sgl->sg_iovs = (daos_iov_t *)realloc
			(sgl->sg_iovs, sizeof(daos_iov_t) * sgl->sg_nr.num);
		if (sgl->sg_iovs == NULL) {
			D_ERROR("Failed memory allocation\n");
			return -DER_NOMEM;
		}

		sgl->sg_iovs[k].iov_buf = user_sgl->sg_iovs[cur_i].iov_buf +
			cur_off;

		if (rem_records * cell_size >=
		    (user_sgl->sg_iovs[cur_i].iov_len - cur_off)) {
			sgl->sg_iovs[k].iov_len =
				user_sgl->sg_iovs[cur_i].iov_len - cur_off;
			cur_i++;
			cur_off = 0;
		} else {
			sgl->sg_iovs[k].iov_len = rem_records * cell_size;
			cur_off += rem_records * cell_size;
		}

		sgl->sg_iovs[k].iov_buf_len = sgl->sg_iovs[k].iov_len;
		rem_records -= sgl->sg_iovs[k].iov_len / cell_size;

		k++;
	} while (rem_records && user_sgl->sg_nr.num > cur_i);

	sgl->sg_nr.num_out = 0;

	*sgl_i = cur_i;
	*sgl_off = cur_off;

	return 0;
}

int
dac_array_io(struct daos_task *task)
{
	struct dac_array_io_t *args = daos_task2arg(task);
	struct dac_array *array = NULL;
	daos_handle_t	oh;
	daos_epoch_t	epoch = args->epoch;
	daos_array_ranges_t *ranges = args->ranges;
	daos_sg_list_t	*user_sgl = args->sgl;
	enum array_op_t	op_type = args->op;
	daos_off_t	cur_off;/* offset into user buf to track current pos */
	daos_size_t	cur_i;	/* index into user sgl to track current pos */
	daos_size_t	records; /* Number of records to access in cur range */
	daos_off_t	array_idx; /* object array index of current range */
	daos_size_t	u;
	daos_size_t	num_records;
	daos_off_t	record_i;
	daos_csum_buf_t	null_csum;
	struct io_params *head, *current;
	daos_size_t	num_ios;
	int		rc;

	if (ranges == NULL) {
		D_ERROR("NULL ranges passed\n");
		D_GOTO(err_task, rc = -DER_INVAL);
	}
	if (user_sgl == NULL) {
		D_ERROR("NULL scatter-gather list passed\n");
		D_GOTO(err_task, rc = -DER_INVAL);
	}

	array = array_hdl2ptr(args->oh);
	if (array == NULL)
		return -DER_NO_HDL;

	if (!io_extent_same(ranges, user_sgl, array->cell_size)) {
		D_ERROR("Unequal extents of memory and array descriptors\n");
		D_GOTO(err_task, rc = -DER_INVAL);
	}

	oh = array->daos_oh;

	cur_off = 0;
	cur_i = 0;
	u = 0;
	num_ios = 0;
	records = ranges->ranges[0].len;
	array_idx = ranges->ranges[0].index;
	daos_csum_set(&null_csum, NULL, 0);

	head = NULL;

	/**
	 * Loop over every range, but at the same time combine consecutive
	 * ranges that belong to the same dkey. If the user gives ranges that
	 * are not increasing in offset, they probably won't be combined unless
	 * the separating ranges also belong to the same dkey.
	 */
	while (u < ranges->ranges_nr) {
		daos_iod_t	*iod;
		daos_sg_list_t	*sgl;
		char		*dkey_str;
		daos_key_t	*dkey;
		daos_size_t	dkey_records;
		struct daos_task *io_task;
		struct io_params *params = NULL;
		daos_size_t	i;

		if (ranges->ranges[u].len == 0) {
			u++;
			if (u < ranges->ranges_nr) {
				records = ranges->ranges[u].len;
				array_idx = ranges->ranges[u].index;
			}
			continue;
		}

		D_ALLOC_PTR(params);
		if (params == NULL) {
			D_ERROR("Failed memory allocation\n");
			return -1;
		}

		if (num_ios == 0) {
			head = params;
			current = head;
		} else {
			current->next = params;
			current = params;
		}

		iod = &params->iod;
		sgl = &params->sgl;
		D_ALLOC_PTR(params->task);
		io_task = params->task;
		dkey = &params->dkey;
		params->akey_str = strdup("akey_not_used");
		params->next = NULL;
		params->user_sgl_used = false;

		num_ios++;

		/**
		 * Compute the dkey given the array index for this range. Also
		 * compute: - the number of records that the dkey can hold
		 * starting at the index where we start writing. - the record
		 * index relative to the dkey.
		 */
		rc = compute_dkey(array, array_idx, &num_records, &record_i,
				  &params->dkey_str);
		if (rc != 0) {
			D_ERROR("Failed to compute dkey\n");
			return rc;
		}
		dkey_str = params->dkey_str;
#ifdef ARRAY_DEBUG
		printf("DKEY IOD %s ---------------------------\n", dkey_str);
		printf("array_idx = %d\t num_records = %zu\t record_i = %d\n",
		       (int)array_idx, num_records, (int)record_i);
#endif
		daos_iov_set(dkey, (void *)dkey_str, strlen(dkey_str));

		/* set descriptor for KV object */
		daos_iov_set(&iod->iod_name, (void *)params->akey_str,
			     strlen(params->akey_str));
		iod->iod_kcsum = null_csum;
		iod->iod_nr = 0;
		iod->iod_csums = NULL;
		iod->iod_eprs = NULL;
		iod->iod_recxs = NULL;
		iod->iod_size = array->cell_size;
		iod->iod_type = DAOS_IOD_ARRAY;

		i = 0;
		dkey_records = 0;

		/**
		 * Create the IO descriptor for this dkey. If the entire range
		 * fits in the dkey, continue to the next range to see if we can
		 * combine it fully or partially in the current dkey IOD/
		 */
		do {
			daos_off_t	old_array_idx;

			iod->iod_nr++;

			/** add another element to recxs */
			iod->iod_recxs = (daos_recx_t *)realloc
				(iod->iod_recxs, sizeof(daos_recx_t) *
				 iod->iod_nr);
			if (iod->iod_recxs == NULL) {
				D_ERROR("Failed memory allocation\n");
				return -DER_NOMEM;
			}

			/** set the record access for this range */
			iod->iod_recxs[i].rx_idx = record_i;
			iod->iod_recxs[i].rx_nr = (num_records > records) ?
				records : num_records;
#ifdef ARRAY_DEBUG
			printf("Add %zu to ARRAY IOD (size = %zu index = %d)\n",
			       u, iod->iod_recxs[i].rx_nr,
			       (int)iod->iod_recxs[i].rx_idx);
#endif
			/**
			 * if the current range is bigger than what the dkey can
			 * hold, update the array index and number of records in
			 * the current range and break to issue the I/O on the
			 * current KV.
			 */
			if (records > num_records) {
				array_idx += num_records;
				records -= num_records;
				dkey_records += num_records;
				break;
			}

			u++;
			i++;
			dkey_records += records;

			/** if there are no more ranges to write, then break */
			if (ranges->ranges_nr <= u)
				break;

			old_array_idx = array_idx;
			records = ranges->ranges[u].len;
			array_idx = ranges->ranges[u].index;

			/**
			 * Boundary case where number of records align with the
			 * end boundary of the KV. break after advancing to the
			 * next range
			 */
			if (records == num_records)
				break;

			/** cont processing the next range in the cur dkey */
			if (array_idx < old_array_idx + num_records &&
			   array_idx >= ((old_array_idx + num_records) -
				       array->block_size)) {
				char	*dkey_str_tmp = NULL;

				/**
				 * verify that the dkey is the same as the one
				 * we are working on given the array index, and
				 * also compute the number of records left in
				 * the dkey and the record indexin the dkey.
				 */
				rc = compute_dkey(array, array_idx,
						  &num_records, &record_i,
						  &dkey_str_tmp);
				if (rc != 0) {
					D_ERROR("Failed to compute dkey\n");
					return rc;
				}

				D_ASSERT(strcmp(dkey_str_tmp, dkey_str) == 0);

				free(dkey_str_tmp);
				dkey_str_tmp = NULL;
			} else {
				break;
			}
		} while (1);
#ifdef ARRAY_DEBUG
		printf("END DKEY IOD %s ---------------------------\n",
		       dkey_str);
#endif
		/**
		 * if the user sgl maps directly to the array range, no need to
		 * partition it.
		 */
		if (1 == ranges->ranges_nr && 1 == user_sgl->sg_nr.num &&
		    dkey_records == ranges->ranges[0].len) {
			sgl = user_sgl;
			params->user_sgl_used = true;
		}
		/** create an sgl from the user sgl for the current IOD */
		else {
			/* set sgl for current dkey */
			rc = create_sgl(user_sgl, array->cell_size,
					dkey_records, &cur_off, &cur_i, sgl);
			if (rc != 0) {
				D_ERROR("Failed to create sgl\n");
				return rc;
			}
#ifdef ARRAY_DEBUG
			daos_size_t s;

			printf("DKEY SGL -----------------------\n");
			printf("sg_nr = %u\n", sgl->sg_nr.num);
			for (s = 0; s < sgl->sg_nr.num; s++) {
				printf("%zu: length %zu, Buf %p\n",
				       s, sgl->sg_iovs[s].iov_len,
				       sgl->sg_iovs[s].iov_buf);
			}
			printf("------------------------------------\n");
#endif
		}

		/* issue KV IO to DAOS */
		if (op_type == D_ARRAY_OP_READ) {
			daos_obj_fetch_t io_arg;

			io_arg.oh = oh;
			io_arg.epoch = epoch;
			io_arg.dkey = dkey;
			io_arg.nr = 1;
			io_arg.iods = iod;
			io_arg.sgls = sgl;
			io_arg.maps = NULL;

			rc = daos_task_create(DAOS_OPC_OBJ_FETCH,
					      daos_task2sched(task), &io_arg, 0,
					      NULL, io_task);
			if (rc != 0) {
				D_ERROR("KV Fetch of dkey %s failed (%d)\n",
					dkey_str, rc);
				return rc;
			}
		} else if (op_type == D_ARRAY_OP_WRITE) {
			daos_obj_update_t io_arg;

			io_arg.oh = oh;
			io_arg.epoch = epoch;
			io_arg.dkey = dkey;
			io_arg.nr = 1;
			io_arg.iods = iod;
			io_arg.sgls = sgl;

			rc = daos_task_create(DAOS_OPC_OBJ_UPDATE,
					      daos_task2sched(task), &io_arg, 0,
					      NULL, io_task);
			if (rc != 0) {
				D_ERROR("KV Update of dkey %s failed (%d)\n",
					dkey_str, rc);
				return rc;
			}
		} else {
			D_ASSERTF(0, "Invalid array operation.\n");
		}

		daos_task_register_deps(task, 1, &io_task);
	} /* end while */

	if (head)
		daos_task_register_comp_cb(task, free_io_params_cb,
					   sizeof(head), &head);

	array_decref(array);
	daos_sched_progress(daos_task2sched(task));
	return 0;

err_task:
	if (array)
		array_decref(array);
	daos_task_complete(task, rc);
	return rc;
}

#define ENUM_KEY_BUF	32
#define ENUM_DESC_BUF	512
#define ENUM_DESC_NR	5

struct get_size_props {
	struct dac_array *array;
	char		key[ENUM_DESC_BUF];
	char		buf[ENUM_DESC_BUF];
	daos_key_desc_t kds[ENUM_DESC_NR];
	daos_iov_t	iov;
	daos_sg_list_t  sgl;
	uint32_t	nr;
	daos_hash_out_t anchor;
	uint32_t	hi;
	uint32_t	lo;
	daos_size_t	*size;
	struct daos_task *ptask;
};

static int
get_array_size_cb(struct daos_task *task, void *data)
{
	struct get_size_props *props = *((struct get_size_props **)data);
	struct dac_array *array = props->array;
	daos_obj_list_dkey_t *args;
	char		*ptr;
	uint32_t	i;
	daos_off_t	max_offset;
	daos_size_t	max_iter;
	int		rc = task->dt_result;

	args = daos_task_get_args(DAOS_OPC_OBJ_LIST_DKEY, task);

	for (ptr = props->buf, i = 0; i < props->nr; i++) {
		uint32_t hi, lo;
		int ret;

		snprintf(props->key, args->kds[i].kd_key_len + 1, ptr);
#ifdef ARRAY_DEBUG
		printf("%d: key %s len %d\n", i, props->key,
		       (int)args->kds[i].kd_key_len);
#endif
		ptr += args->kds[i].kd_key_len;

		if (!strcmp(ARRAY_MD_KEY, props->key))
			continue;

		/** Keep a record of the highest dkey */
		ret = sscanf(props->key, "%u_%u", &hi, &lo);
		D_ASSERT(ret == 2);

		if (hi >= props->hi) {
			props->hi = hi;
			if (lo > props->lo)
				props->lo = lo;
		}
	}

	if (!daos_hash_is_eof(args->anchor)) {
		props->nr = ENUM_DESC_NR;
		memset(props->buf, 0, ENUM_DESC_BUF);
		args->sgl->sg_nr.num = 1;
		daos_iov_set(&args->sgl->sg_iovs[0], props->buf, ENUM_DESC_BUF);

		rc = daos_task_reinit(task);
		if (rc != 0) {
			D_ERROR("FAILED to continue enumrating task\n");
			D_GOTO(out, rc);
		}

		daos_task_register_cbs(task, NULL, NULL, 0, get_array_size_cb,
				       &props, sizeof(props));

		return rc;
	}

#ifdef ARRAY_DEBUG
	printf("Hi = %u, Lo = %u\n", props->hi, props->lo);
#endif

	/*
	 * Go through all the dkeys in the current group (maxhi_x) and get the
	 * highest index to determine which dkey in the group has the highest
	 * bit.
	 */
	max_iter = 0;
	max_offset = 0;

	daos_size_t	grp_size;
	daos_size_t	grp_chunk;

	grp_size = array->block_size * array->num_blocks * array->num_dkeys;
	grp_chunk = array->block_size * array->num_dkeys;

	for (i = 0 ; i <= props->lo; i++) {
		daos_off_t	offset, index_hi = 0;
		daos_size_t	iter;
		char		key[ENUM_KEY_BUF];

		sprintf(key, "%u_%u", props->hi, i);
		/** retrieve the highest index */
		/** MSC - need new functionality from DAOS to retrieve that. */

		/** Compute the iteration where the highest record is stored */
		iter = index_hi / array->block_size;

		offset = iter * grp_chunk +
			(index_hi - iter * array->block_size);

		if (iter == max_iter || max_iter == 0) {
			/** D_ASSERT(offset > max_offset); */
			max_offset = offset;
			max_iter = iter;
		} else {
			if (i < props->lo)
				break;
		}
	}

	*props->size = props->hi * grp_size + max_offset;

out:
	array_decref(array);
	D_FREE_PTR(props);
	return rc;
}

int
dac_array_get_size(struct daos_task *task)
{
	struct dac_array_get_size_t *args = daos_task2arg(task);
	daos_handle_t	oh;
	struct dac_array *array;
	daos_epoch_t	epoch = args->epoch;
	daos_size_t	*size = args->size;
	daos_obj_list_dkey_t enum_args;
	struct get_size_props *get_size_props;
	struct daos_task *enum_task;
	int		rc;

	array = array_hdl2ptr(args->oh);
	if (array == NULL)
		D_GOTO(err_task, rc = -DER_NO_HDL);

	oh = array->daos_oh;

	D_ALLOC_PTR(get_size_props);
	if (get_size_props == NULL)
		D_GOTO(err_task, rc = -DER_NOMEM);

	get_size_props->hi = 0;
	get_size_props->lo = 0;
	get_size_props->nr = ENUM_DESC_NR;
	get_size_props->ptask = task;
	get_size_props->size = size;
	get_size_props->array = array;
	memset(get_size_props->buf, 0, ENUM_DESC_BUF);
	memset(&get_size_props->anchor, 0, sizeof(get_size_props->anchor));
	get_size_props->sgl.sg_nr.num = 1;
	get_size_props->sgl.sg_iovs = &get_size_props->iov;
	daos_iov_set(&get_size_props->sgl.sg_iovs[0], get_size_props->buf,
		     ENUM_DESC_BUF);

	enum_args.oh = oh;
	enum_args.epoch = epoch;
	enum_args.nr = &get_size_props->nr;
	enum_args.kds = get_size_props->kds;
	enum_args.sgl = &get_size_props->sgl;
	enum_args.anchor = &get_size_props->anchor;

	D_ALLOC_PTR(enum_task);
	if (enum_task == NULL)
		D_GOTO(err_task, rc = -DER_NOMEM);
	rc = daos_task_create(DAOS_OPC_OBJ_LIST_DKEY, daos_task2sched(task),
			      &enum_args, 0, NULL, enum_task);
	if (rc != 0)
		D_GOTO(err_task, rc);

	daos_task_register_cbs(enum_task, NULL, NULL, 0, get_array_size_cb,
			       &get_size_props, sizeof(get_size_props));

	daos_task_register_deps(task, 1, &enum_task);

	daos_sched_progress(daos_task2sched(task));

	return 0;

err_task:
	if (get_size_props)
		D_FREE_PTR(get_size_props);
	if (enum_task)
		D_FREE_PTR(enum_task);
	array_decref(array);
	daos_task_complete(task, rc);
	return rc;
} /* end daos_array_get_size */

struct set_size_props {
	char		key[ENUM_DESC_BUF];
	char		buf[ENUM_DESC_BUF];
	daos_key_desc_t kds[ENUM_DESC_NR];
	char		*val;
	daos_iov_t	iov;
	daos_sg_list_t  sgl;
	uint32_t	nr;
	daos_hash_out_t anchor;
	bool		shrinking;
	uint32_t	hi;
	uint32_t	lo;
	daos_size_t	size;
	daos_size_t	cell_size;
	daos_size_t	num_records;
	daos_off_t	record_i;
	struct daos_task *ptask;
};

static int
free_props_cb(struct daos_task *task, void *data)
{
	struct set_size_props *props = *((struct set_size_props **)data);

	if (props->val)
		free(props->val);
	D_FREE_PTR(props);
	return 0;
}

static int
adjust_array_size_cb(struct daos_task *task, void *data)
{
	struct set_size_props *props = *((struct set_size_props **)data);
	daos_obj_list_dkey_t *args;
	char		*ptr;
	struct daos_task *io_task = NULL;
	struct io_params *params = NULL;
	uint32_t	j;
	int		rc = task->dt_result;

	args = daos_task_get_args(DAOS_OPC_OBJ_LIST_DKEY, task);

	for (ptr = props->buf, j = 0; j < props->nr; j++) {
		uint32_t hi, lo;
		int ret;

		snprintf(props->key, args->kds[j].kd_key_len + 1, ptr);
#ifdef ARRAY_DEBUG
		printf("%d: key %s len %d\n", j, props->key,
		       (int)args->kds[j].kd_key_len);
#endif
		ptr += args->kds[j].kd_key_len;

		if (!strcmp(ARRAY_MD_KEY, props->key))
			continue;

		/** Keep a record of the highest dkey */
		ret = sscanf(props->key, "%u_%u", &hi, &lo);
		D_ASSERT(ret == 2);

		if (hi >= props->hi) {
			/** MSC - Punch this entire dkey */
			if (lo > props->lo)
				props->shrinking = true;
			/** MSC - Punch records in dkey at higher index */
			else if (lo == props->lo)
				props->shrinking = true;
		}

	}

	if (!daos_hash_is_eof(args->anchor)) {
		props->nr = ENUM_DESC_NR;
		memset(props->buf, 0, ENUM_DESC_BUF);
		args->sgl->sg_nr.num = 1;
		daos_iov_set(&args->sgl->sg_iovs[0], props->buf, ENUM_DESC_BUF);

		rc = daos_task_reinit(task);
		if (rc != 0) {
			D_ERROR("FAILED to continue enumrating task\n");
			return rc;
		}

		daos_task_register_cbs(task, NULL, NULL, 0,
				       adjust_array_size_cb, &props,
				       sizeof(props));

		return rc;
	}

	/** if array is smaller, write a record at the new size */
	if (!props->shrinking) {
		daos_obj_update_t io_arg;
		daos_iod_t	*iod;
		daos_sg_list_t	*sgl;
		daos_key_t	*dkey;
		daos_csum_buf_t	null_csum;

		daos_csum_set(&null_csum, NULL, 0);

		D_ALLOC_PTR(params);
		if (params == NULL) {
			D_ERROR("Failed memory allocation\n");
			return -DER_NOMEM;
		}

		iod = &params->iod;
		sgl = &params->sgl;
		dkey = &params->dkey;
		D_ALLOC_PTR(params->task);
		if (params->task == NULL) {
			D_ERROR("Failed memory allocation\n");
			D_GOTO(err_out, -DER_NOMEM);
		}

		io_task = params->task;
		params->akey_str = strdup("akey_not_used");
		params->next = NULL;
		params->user_sgl_used = false;

		asprintf(&params->dkey_str, "%u_%u", props->hi, props->lo);
		if (params->dkey_str == NULL) {
			D_ERROR("Failed memory allocation\n");
			D_GOTO(err_out, -DER_NOMEM);
		}
		daos_iov_set(dkey, (void *)params->dkey_str,
			     strlen(params->dkey_str));

		/** set memory location */
		props->val = calloc(1, props->cell_size);
		sgl->sg_nr.num = 1;
		sgl->sg_iovs = malloc(sizeof(daos_iov_t));
		daos_iov_set(&sgl->sg_iovs[0], props->val, props->cell_size);

		/* set descriptor for KV object */
		daos_iov_set(&iod->iod_name, (void *)params->akey_str,
			     strlen(params->akey_str));
		iod->iod_kcsum = null_csum;
		iod->iod_nr = 1;
		iod->iod_csums = NULL;
		iod->iod_eprs = NULL;
		iod->iod_size = props->cell_size;
		iod->iod_type = DAOS_IOD_ARRAY;
		iod->iod_recxs = malloc(sizeof(daos_recx_t));
		iod->iod_recxs[0].rx_idx = props->record_i;
		iod->iod_recxs[0].rx_nr = 1;

		io_arg.oh = args->oh;
		io_arg.epoch = args->epoch;
		io_arg.dkey = dkey;
		io_arg.nr = 1;
		io_arg.iods = iod;
		io_arg.sgls = sgl;

		rc = daos_task_create(DAOS_OPC_OBJ_UPDATE,
				      daos_task2sched(task), &io_arg, 0,
				      NULL, io_task);
		if (rc != 0) {
			D_ERROR("KV Update of dkey %s failed (%d)\n",
				params->dkey_str, rc);
			D_GOTO(err_out, rc);
		}

		daos_task_register_comp_cb(io_task, free_io_params_cb,
					   sizeof(params), &params);

		daos_task_register_deps(props->ptask, 1, &io_task);
	}

	return rc;

err_out:
	if (params->dkey_str)
		D_FREE_PTR(params->dkey_str);
	if (params)
		D_FREE_PTR(params);
	if (io_task)
		D_FREE_PTR(io_task);

	return rc;
}

int
dac_array_set_size(struct daos_task *task)
{
	struct dac_array_set_size_t *args = daos_task2arg(task);
	daos_handle_t	oh;
	struct dac_array *array;
	daos_epoch_t	epoch = args->epoch;
	daos_size_t	size = args->size;
	char            *dkey_str = NULL;
	daos_size_t	num_records;
	daos_off_t	record_i;
	daos_obj_list_dkey_t enum_args;
	struct set_size_props *set_size_props;
	struct daos_task *enum_task;
	int		rc, ret;

	array = array_hdl2ptr(args->oh);
	if (array == NULL)
		D_GOTO(err_task, rc = -DER_NO_HDL);

	oh = array->daos_oh;

	rc = compute_dkey(array, size, &num_records, &record_i, &dkey_str);
	if (rc != 0) {
		D_ERROR("Failed to compute dkey\n");
		D_GOTO(err_task, rc);
	}

	D_ALLOC_PTR(set_size_props);
	if (set_size_props == NULL) {
		free(dkey_str);
		D_GOTO(err_task, rc = -DER_NOMEM);
	}

	ret = sscanf(dkey_str, "%u_%u", &set_size_props->hi,
		     &set_size_props->lo);
	D_ASSERT(ret == 2);
	free(dkey_str);

	set_size_props->cell_size = array->cell_size;
	set_size_props->num_records = num_records;
	set_size_props->record_i = record_i;
	set_size_props->shrinking = false;
	set_size_props->nr = ENUM_DESC_NR;
	set_size_props->size = args->size;
	set_size_props->ptask = task;
	set_size_props->val = NULL;
	memset(set_size_props->buf, 0, ENUM_DESC_BUF);
	memset(&set_size_props->anchor, 0, sizeof(set_size_props->anchor));
	set_size_props->sgl.sg_nr.num = 1;
	set_size_props->sgl.sg_iovs = &set_size_props->iov;
	daos_iov_set(&set_size_props->sgl.sg_iovs[0], set_size_props->buf,
		     ENUM_DESC_BUF);

	enum_args.oh = oh;
	enum_args.epoch = epoch;
	enum_args.nr = &set_size_props->nr;
	enum_args.kds = set_size_props->kds;
	enum_args.sgl = &set_size_props->sgl;
	enum_args.anchor = &set_size_props->anchor;

	D_ALLOC_PTR(enum_task);
	if (enum_task == NULL)
		D_GOTO(err_task, rc = -DER_NOMEM);
	rc = daos_task_create(DAOS_OPC_OBJ_LIST_DKEY, daos_task2sched(task),
			      &enum_args, 0, NULL, enum_task);
	if (rc != 0)
		return rc;

	daos_task_register_cbs(enum_task, NULL, NULL, 0, adjust_array_size_cb,
			       &set_size_props, sizeof(set_size_props));

	daos_task_register_deps(task, 1, &enum_task);

	daos_task_register_comp_cb(task, free_props_cb, sizeof(set_size_props),
				   &set_size_props);

	daos_sched_progress(daos_task2sched(task));

	array_decref(array);
	return 0;

err_task:
	if (set_size_props)
		D_FREE_PTR(set_size_props);
	if (enum_task)
		D_FREE_PTR(enum_task);
	array_decref(array);
	daos_task_complete(task, rc);
	return rc;
} /* end daos_array_set_size */
