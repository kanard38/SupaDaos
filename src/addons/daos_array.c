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
 * src/addons/daos_array.c
 */

#define DD_SUBSYS	DD_FAC(client)

#include <daos/common.h>
#include <daos/event.h>
#include <daos/addons.h>
#include <daos_addons.h>

int
daos_array_create(daos_handle_t coh, daos_obj_id_t oid, daos_epoch_t epoch,
		  daos_size_t cell_size, daos_size_t block_size,
		  daos_handle_t *oh, daos_event_t *ev)
{
	daos_array_create_t	args;
	tse_task_t		*task;

	args.coh	= coh;
	args.oid	= oid;
	args.epoch	= epoch;
	args.cell_size	= cell_size;
	args.block_size	= block_size;
	args.oh		= oh;

	dc_task_create(DAOS_OPC_ARRAY_CREATE, &args, sizeof(args), &task, &ev);
	return daos_client_result_wait(ev);
}

int
daos_array_open(daos_handle_t coh, daos_obj_id_t oid, daos_epoch_t epoch,
		unsigned int mode, daos_size_t *cell_size,
		daos_size_t *block_size, daos_handle_t *oh, daos_event_t *ev)
{
	daos_array_open_t	args;
	tse_task_t		*task;

	*cell_size = 0;
	*block_size = 0;

	args.coh	= coh;
	args.oid	= oid;
	args.epoch	= epoch;
	args.mode	= mode;
	args.cell_size	= cell_size;
	args.block_size	= block_size;
	args.oh		= oh;

	dc_task_create(DAOS_OPC_ARRAY_OPEN, &args, sizeof(args), &task, &ev);
	return daos_client_result_wait(ev);
}

int
daos_array_close(daos_handle_t oh, daos_event_t *ev)
{
	daos_array_close_t	args;
	tse_task_t		*task;

	args.oh         = oh;

	dc_task_create(DAOS_OPC_ARRAY_CLOSE, &args, sizeof(args), &task, &ev);
	return daos_client_result_wait(ev);
}

int
daos_array_read(daos_handle_t oh, daos_epoch_t epoch,
		daos_array_ranges_t *ranges, daos_sg_list_t *sgl,
		daos_csum_buf_t *csums, daos_event_t *ev)
{
	daos_array_io_t	args;
	tse_task_t	*task;

	args.oh		= oh;
	args.epoch	= epoch;
	args.ranges	= ranges;
	args.sgl	= sgl;
	args.csums	= csums;

	dc_task_create(DAOS_OPC_ARRAY_READ, &args, sizeof(args), &task, &ev);
	return daos_client_result_wait(ev);
}

int
daos_array_write(daos_handle_t oh, daos_epoch_t epoch,
		 daos_array_ranges_t *ranges, daos_sg_list_t *sgl,
		 daos_csum_buf_t *csums, daos_event_t *ev)
{
	daos_array_io_t	args;
	tse_task_t	*task;

	args.oh		= oh;
	args.epoch	= epoch;
	args.ranges	= ranges;
	args.sgl	= sgl;
	args.csums	= csums;

	dc_task_create(DAOS_OPC_ARRAY_WRITE, &args, sizeof(args), &task, &ev);
	return daos_client_result_wait(ev);
}

int
daos_array_get_size(daos_handle_t oh, daos_epoch_t epoch, daos_size_t *size,
		    daos_event_t *ev)
{
	daos_array_get_size_t	args;
	tse_task_t		*task;

	args.oh		= oh;
	args.epoch	= epoch;
	args.size	= size;

	dc_task_create(DAOS_OPC_ARRAY_GET_SIZE, &args, sizeof(args), &task,
		       &ev);
	return daos_client_result_wait(ev);
} /* end daos_array_get_size */

int
daos_array_set_size(daos_handle_t oh, daos_epoch_t epoch, daos_size_t size,
		    daos_event_t *ev)
{
	daos_array_set_size_t	args;
	tse_task_t		*task;

	args.oh		= oh;
	args.epoch	= epoch;
	args.size	= size;

	dc_task_create(DAOS_OPC_ARRAY_SET_SIZE, &args, sizeof(args), &task,
		       &ev);
	return daos_client_result_wait(ev);
} /* end daos_array_set_size */
