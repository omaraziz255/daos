/**
 * (C) Copyright 2022 Intel Corporation.
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 */
 #include <stdio.h>
 #include <string.h>
 #include <errno.h>
 #include <sys/types.h>
 #include <sys/stat.h>
 #include <sys/mman.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <daos/common.h>
 #include <daos_srv/vos.h>
 #include <daos_fs_sys.h>
 #include <spdk/env.h>
 #include "vos_target_fs.h"
 #include "vos_tasks.h"
 #include "vos_target_engine.h"
 #include <pmfs/pmfs.h>

char dfs_pmem_file[PATH_MAX];
static pthread_mutex_t vos_fs_cmd_lock = PTHREAD_MUTEX_INITIALIZER;
D_LIST_HEAD(g_task_ring);

static inline double
dts_time_now(void)
{
	struct timeval	tv;

	gettimeofday(&tv, NULL);
	return (tv.tv_sec + tv.tv_usec / 1000000.0);
}

const char *cmd_string[] = {"none", "update", "fetch", "punch", "get_num_dkeys", "list_dkeys"};

static int
vos_obj_get_info_dkeys(daos_handle_t coh, daos_unit_oid_t oid,
			enum task_op opc, uint32_t *nr, uint64_t *len,
			daos_key_desc_t *kds, void *buf)
{
	struct scan_context ctx = { 0 };
	struct pmfs_obj_info *uoi = NULL;
	int rc = 0;

	ctx.cur_cont.tsc_coh = coh;
	ctx.uoi.oid = oid;
	ctx.uoi.nr = *nr;
	ctx.uoi.len = *len;

	D_ALLOC(uoi, sizeof(struct pmfs_obj_info));
	D_ASSERT(uoi != NULL);

	rc = pmfs_scan_cont(&ctx, &uoi, opc);
	if (rc != 0) {
		D_ERROR("Object get dkeys info failed: " DF_RC "\n",
			DP_RC(rc));
		return rc;
	}

	*nr = uoi->nr;
	*len = uoi->len;

	return 0;
}

static int
vos_obj_list_info_dkeys(daos_handle_t coh, daos_unit_oid_t oid,
			enum task_op opc, uint32_t *nr, uint64_t *len,
			daos_key_desc_t *kds, void *buf)
{
	struct scan_context ctx = { 0 };
	struct pmfs_obj_info *uoi = NULL;
	int rc = 0;

	ctx.cur_cont.tsc_coh = coh;
	ctx.uoi.oid = oid;
	ctx.uoi.nr =  *nr;
	ctx.uoi.len = *len;
	ctx.uoi.kds = kds;
	ctx.uoi.buf = buf;

	D_ALLOC(uoi, sizeof(struct pmfs_obj_info));
	D_ASSERT(uoi != NULL);

	rc = pmfs_scan_cont(&ctx, &uoi, opc);
	if (rc != 0) {
		D_ERROR("Object get dkeys info failed: " DF_RC "\n",
			DP_RC(rc));
		return rc;
	}

	*nr = uoi->nr;
	*len = uoi->len;
	kds = uoi->kds;
	buf = uoi->buf;

	return 0;
}


struct vos_fs_ult_arg {
	void *args;
	enum task_op opc;
	double *duration;
	int status;
};

static int
_vos_parse_commands(enum task_op opc, void *union_args)
{
	int rc = 0;

	if (opc == VOS_OBJ_UPDATE) {
		struct vos_client_obj_rw_args *args = union_args;

		  rc = vos_obj_update(args->coh, args->oid,
				       args->epoch, args->pm_ver,
				       args->flags, args->dkey,
				       args->iod_nr, args->iods, NULL,
				       args->sgls);

	} else if (opc == VOS_OBJ_FETCH) {
		struct vos_client_obj_rw_args *args = union_args;

		  rc = vos_obj_fetch(args->coh, args->oid,
				      args->epoch, args->flags,
				      args->dkey, args->iod_nr,
				      args->iods, args->sgls);
	} else if (opc == VOS_OBJ_PUNCH) {
		struct vos_client_obj_rw_args *args = union_args;

		  rc = vos_obj_punch(args->coh, args->oid,
				      args->epoch, args->pm_ver,
				      args->flags, args->dkey,
				      args->akey_nr, args->akeys,
				      NULL);

	} else if (opc == VOS_OBJ_GET_NUM_DKEYS) {
		struct vos_client_obj_list_args *args = union_args;

		  rc = vos_obj_get_info_dkeys(args->coh, args->oid,
					       opc, args->nr, args->len,
					       args->kds, args->buf);
	} else if (opc == VOS_OBJ_LIST_DKEYS) {
		struct vos_client_obj_list_args *args = union_args;

		  rc = vos_obj_list_info_dkeys(args->coh, args->oid,
					       opc, args->nr, args->len,
					       args->kds, args->buf);
	} else {
		  D_ERROR("Command is not recongnized.");
		  rc = -1;
	}

	return rc;
}

static int
_vos_fs_execute_command(void *_args, enum task_op opc, double *duration)
{
	double then, now;
	int rc = 0;

	then = dts_time_now();
	rc = _vos_parse_commands(opc, _args);

	now = dts_time_now();
	*duration = now-then;

	D_PRINT("execute command %s end success\r\n", cmd_string[opc]);
	D_PRINT("Dts last time = %-10.6fs\r\n", now-then);
	return rc;
}

static void
vos_fs_execute_command_ult(void *arg)
{
	struct vos_fs_ult_arg *vfua = arg;

	D_INFO("execute command %s\r\n", cmd_string[vfua->opc]);
	vfua->status =
		_vos_fs_execute_command(vfua->args, vfua->opc,
					 vfua->duration);
}


static int
vos_task_ult(void *args, enum task_op opc, double *duration)
{
	struct vos_fs_ult_arg vfua;
	int rc;

	vfua.args = args;
	vfua.duration = duration;
	vfua.opc = opc;
	pthread_t uid;

	rc = pthread_create(&uid, NULL, (void *) vos_fs_execute_command_ult,
			     &vfua);
	if (rc != 0) {
		return rc;
	}

	rc = pthread_join(uid, NULL);
	if (rc != 0) {
		return rc;
	}

	duration = vfua.duration;
	return 0;
}

static int
vos_task_dequeue(struct vos_fs_cmd_args *vfca)
{
	struct spdk_ring *task_ring;
	struct vos_client_task *vct;
	int rc = 0;

	task_ring = vos_task_get_ring(vfca->vfcmd, vfca);
	if (task_ring == NULL || !spdk_ring_count(task_ring)) {
		return 0;
	}

	do {
		D_INFO("find the ring\r\n");
		spdk_ring_dequeue(task_ring, (void **) &vct, 1);
		if (vct->opc == VOS_OBJ_UPDATE || vct->opc == VOS_OBJ_FETCH
				|| vct->opc == VOS_OBJ_PUNCH) {
			D_INFO("start to ult obj rw\r\n");
			vos_task_ult(&vct->args.obj_rw, vct->opc,
					vfca->duration);
		} else {
			D_INFO("start to ult obj list\r\n");
			vos_task_ult(&vct->args.obj_list, vct->opc,
					vfca->duration);
		}
		D_INFO("start cb to sem wait\r\n");
		vct->cb_fn(vct->cb_args, rc);
		D_INFO("sem pos\r\n");
	} while (spdk_ring_count(task_ring));

	return 1;
}

static void
collect_fs_tasks(void *arg)
{
	struct vos_fs_cmd_args *vfca = arg;

	D_ASSERT(vfca != NULL);
	D_ALLOC(vfca->task_ring_list, sizeof(struct ring_list));
	if (vfca->task_ring_list == NULL) {
		return;
	}

	D_INIT_LIST_HEAD(&vfca->task_ring_list->rl);
}

static void
vos_end_tasks(void *arg)
{
	struct ring_list *task_ring_list, *tmp;

	d_list_for_each_entry_safe(task_ring_list, tmp, &g_task_ring, rl) {
		d_list_del(&task_ring_list->rl);
		D_FREE_PTR(task_ring_list);
	}

}

static int
vos_task_completion(void *args)
{
	struct vos_fs_cmd_args *vfca = args;
	int rc;

	rc = vos_task_dequeue(vfca);
	return rc;
}

struct spdk_ring *
vos_task_get_ring(const char *name, void *arg)
{
	struct ring_list *task_ring_list;
	struct vos_fs_cmd_args *vfca = arg;

	if (!name || !vfca->task_ring_list) {
		return NULL;
	}

	vfca->task_ring_list->rl = g_task_ring;
	if (d_list_empty(&vfca->task_ring_list->rl)) {
		return NULL;
	}

	d_list_for_each_entry(task_ring_list, &vfca->task_ring_list->rl, rl) {
		if (strcmp(task_ring_list->ring_name, name) == 0) {
			return task_ring_list->task_ring;
		}
	}

	return NULL;
}

void
vos_task_bind_ring(const char *name,
		    struct spdk_ring *ring, struct ring_list *ring_list)
{
	D_MUTEX_LOCK(&vos_fs_cmd_lock);

	D_ALLOC(ring_list, sizeof(struct ring_list));
	D_ASSERT(ring_list != NULL);
	ring_list->ring_name = name;
	ring_list->task_ring = ring;
	d_list_add(&ring_list->rl, &g_task_ring);

	D_MUTEX_UNLOCK(&vos_fs_cmd_lock);
}

void
vos_task_process_init(void *arg)
{
	struct vos_fs_cmd_args *vfca = arg;
	int rc = 0;

	rc = vt_ctx_init(&vfca->pmfs_ctx);
	if (rc) {
		return;
	}

	collect_fs_tasks(vfca);
}

void
vos_task_process_fini(void *arg)
{
	struct vos_fs_cmd_args *vfca = arg;

	D_INFO("process fini\r\n");
	vos_end_tasks(vfca);
	vt_ctx_fini(&vfca->pmfs_ctx);
}

void
vos_task_process(void *arg)
{
	struct vos_fs_cmd_args *vfca = arg;

	while (!vfca->force_exit) {
		vos_task_completion(vfca);
	}
}
