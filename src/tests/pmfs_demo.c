#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <daos/common.h>
#include <daos/tests_lib.h>
#include <daos_srv/vos.h>
#include <daos_test.h>
#include <pmfs/pmfs.h>
#include <pmfs/vos_target_fs.h>
#include <pmfs/vos_tasks.h>
#include "perf_internal.h"

static struct vos_fs_cmd_args *g_vfca;
static int count;
static int num;
D_LIST_HEAD(g_test_pool_list);
D_LIST_HEAD(g_test_fini_list);
D_LIST_HEAD(g_present_list);

enum {
	MKFS,
	MOUNT
};

struct mkfs_args {
	daos_handle_t poh;
	uuid_t uuid;
};

struct mount_args {
	daos_handle_t poh;
	daos_handle_t coh;
	int flags;
	struct pmfs **pmfs;
};

struct umount_args {
	struct pmfs *pmfs;
};

struct mkdir_args {
	struct pmfs *pmfs;
	struct pmfs_obj *parent;
	const char *name;
	mode_t mode;
};

struct listdir_args {
	struct pmfs *pmfs;
	struct pmfs_obj *obj;
	uint32_t nr;
};

struct remove_args {
	struct pmfs *pmfs;
	struct pmfs_obj *parent;
	const char *name;
	bool force;
	daos_obj_id_t *oid;
};

struct open_args {
	struct pmfs *pmfs;
	struct pmfs_obj *parent;
	const char *name;
	mode_t mode;
	int flags;
	daos_size_t chunk_size;
	const char *value;
	struct pmfs_obj *obj;
};

struct readdir_args {
	struct pmfs *pmfs;
	struct pmfs_obj *obj;
	uint32_t *nr;
	struct dirent *dirs;
};

struct lookup_args {
	struct pmfs *pmfs;
	const char *path;
	int flags;
	struct pmfs_obj *obj;
	mode_t *mode;
	struct stat *stbuf;
};

struct release_args {
	struct pmfs_obj *obj;
};

struct punch_args {
	struct pmfs *pmfs;
	struct pmfs_obj *obj;
	daos_off_t offset;
	daos_size_t len;
};

struct write_args {
	struct pmfs *pmfs;
	struct pmfs_obj *obj;
	d_sg_list_t *user_sgl;
	daos_size_t buf_size;
	daos_off_t off;
	daos_size_t *write_size;
};

struct read_args {
	struct pmfs *pmfs;
	struct pmfs_obj *obj;
	d_sg_list_t *user_sgl;
	daos_off_t off;
	daos_size_t stride_size;
	daos_size_t *read_size;
};

struct stat_args {
	struct pmfs *pmfs;
	struct pmfs_obj *parent;
	const char *name;
	struct stat *stbuf;
};

static void
pmfs_buffer_render(char *buf, unsigned int buf_len)
{
	int     nr = 'z' - 'a' + 1;
	int     i;

	for (i = 0; i < buf_len - 1; i++) {
		int randv = rand() % (2 * nr);

		if (randv < nr)
			buf[i] = 'a' + randv;
		else
			buf[i] = 'A' + (randv - nr);
	}
	buf[i] = '\0';
}

static  struct pmfs_pool
pmfs_add_single_pool(char *tsc_pmem_file, uint64_t tsc_nvme_size,
		uint64_t tsc_scm_size, bool tsc_skip_cont_create, bool amend)
{
	struct pmfs_pool *pmfs_pool;

	D_ALLOC(pmfs_pool, sizeof(struct pmfs_pool));
	D_ASSERT(pmfs_pool != NULL);

	if (tsc_pmem_file == NULL) {
		char ts_pmem_file[PATH_MAX];

		snprintf(ts_pmem_file, sizeof(ts_pmem_file),
			 "/mnt/daos/pmfs_cli%d.pmem", count);
		tsc_pmem_file = ts_pmem_file;
		D_PRINT("tsc pmem file = %s\r\n", tsc_pmem_file);
	}
	uuid_generate(pmfs_pool->tsc_pool_uuid);
	pmfs_pool->tsc_pmem_file = tsc_pmem_file;
	pmfs_pool->tsc_nvme_size = tsc_nvme_size;
	pmfs_pool->tsc_scm_size = tsc_scm_size;
	pmfs_pool->tsc_skip_cont_create = tsc_skip_cont_create;
	count++;
	d_list_add(&pmfs_pool->pl, &g_test_pool_list);
	if (amend) {
		engine_pool_single_node_init(pmfs_pool, false);
		pmfs_combine_pool_fini_list(&g_test_fini_list);
	}

	return *pmfs_pool;
}

static struct pmfs_context
pmfs_set_ctx(void)
{
	struct pmfs_context *pmfs_ctx;

	D_ALLOC(pmfs_ctx, sizeof(struct pmfs_context));
	D_ASSERT(pmfs_ctx != NULL);

	D_INIT_LIST_HEAD(&pmfs_ctx->pmfs_pool.pl);
	/* add pool mapping to /mnt/daos/pmfs_cli0.pmem, 8G NVME,2G SCM, skip container create */
	pmfs_ctx->pmfs_pool = pmfs_add_single_pool("/mnt/daos/pmfs_cli0.pmem",
						 8ULL << 30, 2ULL << 30, true, false);
	/* That aims to associated with engine lib */
	pmfs_ctx->pmfs_pool.pl = g_test_pool_list;
	pmfs_ctx_combine_pool_list(pmfs_ctx);
	return *pmfs_ctx;
}

static void
pmfs_mkfs_cb(void *arg)
{
	struct mkfs_args *mags = arg;

	pmfs_mkfs(mags->poh, mags->uuid);
}

static void
pmfs_mount_cb(void *arg)
{
	struct mount_args *mount_args = arg;

	pmfs_mount(mount_args->poh, mount_args->coh, mount_args->flags,
		   mount_args->pmfs);
}

static void
pmfs_umount_cb(void *arg)
{
	struct umount_args *umount_args = arg;

	pmfs_umount(umount_args->pmfs);
}

static void
pmfs_mkdir_cb(void *arg)
{
	struct mkdir_args *mkdir_args = arg;

	pmfs_mkdir(mkdir_args->pmfs, mkdir_args->parent, mkdir_args->name,
		   mkdir_args->mode);
}

static void
pmfs_listdir_cb(void *arg)
{
	struct listdir_args *listdir_args = arg;

	pmfs_listdir(listdir_args->pmfs, listdir_args->obj, &listdir_args->nr);
}

static void
pmfs_remove_cb(void *arg)
{
	struct remove_args *remove_args = arg;

	pmfs_remove(remove_args->pmfs, remove_args->parent, remove_args->name,
			remove_args->force, remove_args->oid);
}

static void
pmfs_open_cb(void *arg)
{
	struct open_args *open_args = arg;

	pmfs_open(open_args->pmfs, open_args->parent, open_args->name,
		  open_args->mode, open_args->flags, open_args->chunk_size,
		  open_args->value, &open_args->obj);
}

static void
pmfs_readdir_cb(void *arg)
{
	struct readdir_args *readdir_args = arg;

	pmfs_readdir(readdir_args->pmfs, readdir_args->obj, readdir_args->nr,
		     readdir_args->dirs);
}

static void
pmfs_lookup_cb(void *arg)
{
	struct lookup_args *lookup_args = arg;

	pmfs_lookup(lookup_args->pmfs, lookup_args->path, lookup_args->flags,
		    &lookup_args->obj, lookup_args->mode, lookup_args->stbuf);
}

static void
pmfs_release_cb(void *arg)
{
	struct release_args *release_args = arg;

	pmfs_release(release_args->obj);
}

static void
pmfs_punch_cb(void *arg)
{
	struct punch_args *punch_args = arg;

	pmfs_punch(punch_args->pmfs, punch_args->obj, punch_args->offset, punch_args->len);
}

static int
pmfs_write_cb(void *arg)
{
	struct write_args *write_args = arg;
	char            *buf;
	d_sg_list_t     sgl;
	d_iov_t         iov;
	int     rc = 0;
	daos_size_t     file_size = *(write_args->write_size);
	daos_size_t     buf_size = write_args->buf_size;
	daos_size_t     io_size;
	daos_size_t     size = 0;

	D_ALLOC(buf, buf_size);
	if (buf == NULL) {
		return -DER_NOMEM;
	}
	d_iov_set(&iov, buf, buf_size);
	sgl.sg_nr = 1;
	sgl.sg_iovs = &iov;


	while (size < file_size) {
		io_size = file_size - size;
		io_size = min(io_size, buf_size);

		sgl.sg_iovs[0].iov_len = io_size;
		pmfs_buffer_render(buf, io_size);
		rc = pmfs_write_sync(write_args->pmfs, write_args->obj, &sgl, size);
		if (rc != 0) {
			D_PRINT("write error\r\n");
			return rc;
		}
		size += io_size;
	}
	D_FREE(buf);
	return 0;
}

static int
pmfs_read_cb(void *arg)
{
	struct read_args *read_args = arg;

	char *buf;
	d_sg_list_t sgl;
	d_iov_t iov;
	daos_size_t buf_size;
	daos_size_t read_size, got_size, got_sizes = 0;
	daos_size_t stride_size = read_args->stride_size;
	daos_size_t total_size = *(read_args->read_size)+read_args->off;
	daos_size_t off = read_args->off;
	int rc = 0;

	buf_size = stride_size;
	D_ALLOC(buf, buf_size);
	D_ASSERT(buf != NULL);
	d_iov_set(&iov, buf, buf_size);
	sgl.sg_nr = 1;
	sgl.sg_iovs = &iov;

	while (off < total_size) {
		read_size = min(total_size - off, stride_size);
		sgl.sg_iovs[0].iov_len = read_size;
		rc = pmfs_read_sync(read_args->pmfs, read_args->obj, &sgl, off, &got_size);
		if (rc != 0 || read_size != got_size) {
			if (rc != 0)
				D_PRINT("read error!\r\n");
			else
				D_PRINT("verify size error!\r\n");
			return rc;
		}

		got_sizes += got_size;
		off += stride_size;
	}

	*(read_args->read_size) = got_sizes;
	D_FREE(buf);
	return rc;
}

static void
pmfs_stat_cb(void *arg)
{
	struct stat_args *stat_args = arg;

	pmfs_stat(stat_args->pmfs, stat_args->parent, stat_args->name, stat_args->stbuf);
}


static int
pmfs_mount_start(daos_handle_t poh, daos_handle_t coh, struct pmfs **pmfs)
{
	struct mount_args mount_args;
	static pthread_t mount_thread;

	int rc = 0;

	memset(&mount_args, 0, sizeof(mount_args));
	mount_args.poh = poh;
	mount_args.coh = coh;
	mount_args.flags = O_RDWR;
	mount_args.pmfs = pmfs;
	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs mount---------------------------\r\n");
	rc = pthread_create(&mount_thread, NULL, (void *) pmfs_mount_cb,
			     &mount_args);

	if (rc != 0) {
		D_PRINT("pmfs mount cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(mount_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_mkdir_start(struct pmfs *pmfs, struct pmfs_obj *parent, const char *name,
		 mode_t mode)
{
	struct mkdir_args mkdir_args;
	static pthread_t mkdir_thread;
	int rc = 0;

	memset(&mkdir_args, 0, sizeof(mkdir_args));
	mkdir_args.pmfs = pmfs;
	mkdir_args.parent = parent;
	mkdir_args.name = name;
	mkdir_args.mode = mode;

	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs mkdir---------------------------\r\n");
	rc = pthread_create(&mkdir_thread, NULL, (void *) pmfs_mkdir_cb,
				&mkdir_args);

	if (rc != 0) {
		D_PRINT("pmfs mkdir cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(mkdir_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	D_PRINT("---------------pmfs mkdir done---------------------------\r\n");
	return rc;
}

static int
pmfs_listdir_start(struct pmfs *pmfs, struct pmfs_obj *obj, uint32_t *nr)
{
	struct listdir_args listdir_args;
	static pthread_t listdir_thread;
	int rc = 0;

	memset(&listdir_args, 0, sizeof(listdir_args));
	listdir_args.pmfs = pmfs;
	listdir_args.obj = obj;
	listdir_args.nr = *nr;

	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs listdir---------------------------\r\n");
	rc = pthread_create(&listdir_thread, NULL, (void *) pmfs_listdir_cb,
			&listdir_args);

	if (rc != 0) {
		D_PRINT("pmfs listdir cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(listdir_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	*nr = listdir_args.nr;

	return rc;
}

static int
pmfs_remove_start(struct pmfs *pmfs, struct pmfs_obj *parent, const char *name,
		bool force, daos_obj_id_t *oid)
{
	struct remove_args remove_args;
	static pthread_t remove_thread;
	int rc = 0;

	memset(&remove_args, 0, sizeof(remove_args));
	remove_args.pmfs = pmfs;
	remove_args.parent = parent;
	remove_args.name = name;
	remove_args.force = force;
	remove_args.oid = oid;

	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs remove ---------------------------\r\n");
	rc = pthread_create(&remove_thread, NULL, (void *) pmfs_remove_cb,
			     &remove_args);

	if (rc != 0) {
		D_PRINT("pmfs remove cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(remove_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_open_start(struct pmfs *pmfs, struct pmfs_obj *parent, const char *name,
		mode_t mode, int flags, daos_size_t chunk_size,
		const char *value, struct pmfs_obj **_obj)
{
	struct open_args open_args;
	static pthread_t open_thread;
	int rc = 0;

	memset(&open_args, 0, sizeof(open_args));
	open_args.pmfs = pmfs;
	open_args.parent = parent;
	open_args.name = name;
	open_args.mode = mode;
	open_args.flags = flags;
	open_args.chunk_size = chunk_size;
	open_args.value = value;
	open_args.obj = *_obj;


	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs open obj------------------------\r\n");
	rc = pthread_create(&open_thread, NULL, (void *) pmfs_open_cb,
			     &open_args);

	if (rc != 0) {
		D_PRINT("pmfs open obj cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(open_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	*_obj = open_args.obj;

	return rc;
}

static int
pmfs_readdir_start(struct pmfs *pmfs, struct pmfs_obj *obj, uint32_t *nr,
		   struct dirent *dirs)
{
	struct readdir_args readdir_args;
	static pthread_t readdir_thread;
	int rc = 0;

	memset(&readdir_args, 0, sizeof(readdir_args));
	readdir_args.pmfs = pmfs;
	readdir_args.obj = obj;
	readdir_args.nr = nr;
	readdir_args.dirs = dirs;

	g_vfca->vfcmd = "PMFS_TASKS";

	rc = pthread_create(&readdir_thread, NULL, (void *) pmfs_readdir_cb,
			     &readdir_args);

	if (rc != 0) {
		D_PRINT("pmfs readdir cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(readdir_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_lookup_start(struct pmfs *pmfs, const char *path, int flags, struct pmfs_obj **obj,
		  mode_t *mode, struct stat *stbuf)
{
	struct lookup_args lookup_args;
	static pthread_t lookup_thread;
	int rc = 0;

	memset(&lookup_args, 0, sizeof(lookup_args));
	lookup_args.pmfs = pmfs;
	lookup_args.path = path;
	lookup_args.flags = flags;
	lookup_args.obj = *obj;
	lookup_args.mode = mode;
	lookup_args.stbuf = stbuf;

	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs lookup------------------------\r\n");
	rc = pthread_create(&lookup_thread, NULL, (void *) pmfs_lookup_cb,
			     &lookup_args);

	if (rc != 0) {
		D_PRINT("pmfs lookup cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(lookup_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	*obj = lookup_args.obj;

	return rc;
}

static int
pmfs_punch_start(struct pmfs *pmfs, struct pmfs_obj *obj, daos_off_t offset,
		daos_size_t len)
{
	struct punch_args punch_args;
	static pthread_t punch_thread;
	int rc = 0;

	memset(&punch_args, 0, sizeof(punch_args));
	punch_args.pmfs = pmfs;
	punch_args.obj = obj;
	punch_args.offset = offset;
	punch_args.len = len;

	g_vfca->vfcmd = "PMFS_TASKS";
	D_PRINT("----start pmfs punch file obj offset=%ld, len=%ld--\r\n", offset, len);

	rc = pthread_create(&punch_thread, NULL, (void *)pmfs_punch_cb, &punch_args);
	if (rc != 0) {
		D_PRINT("punch file failed\r\n");
		return rc;
	}

	rc = pthread_join(punch_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_write_start(struct pmfs *pmfs, struct pmfs_obj *obj, d_sg_list_t *user_sgl,
		daos_size_t buf_size, daos_off_t off, daos_size_t *write_size)
{
	struct write_args write_args;
	static pthread_t write_thread;
	int rc = 0;

	memset(&write_args, 0, sizeof(write_args));
	write_args.pmfs = pmfs;
	write_args.obj = obj;
	write_args.user_sgl = user_sgl;
	write_args.buf_size = buf_size;
	write_args.off = off;
	write_args.write_size = write_size;

	g_vfca->vfcmd = "PMFS_TASKS";
	D_PRINT("----start pmfs write file obj offset=%ld\r\n", off);

	rc = pthread_create(&write_thread, NULL, (void *)pmfs_write_cb, &write_args);
	if (rc != 0) {
		D_PRINT("write file failed\r\n");
		return rc;
	}

	rc = pthread_join(write_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_read_start(struct pmfs *pmfs, struct pmfs_obj *obj, d_sg_list_t *user_sgl,
		daos_off_t off,	daos_size_t stride_size, daos_size_t *read_size)
{
	struct read_args read_args;
	static pthread_t read_thread;
	int rc = 0;

	memset(&read_args, 0, sizeof(read_args));
	read_args.pmfs = pmfs;
	read_args.obj = obj;
	read_args.user_sgl = user_sgl;
	read_args.off = off;
	read_args.stride_size = stride_size;
	read_args.read_size = read_size;

	g_vfca->vfcmd = "PMFS_TASKS";
	D_PRINT("---------------start pmfs read file obj -------------------\r\n");

	rc = pthread_create(&read_thread, NULL, (void *)pmfs_read_cb, &read_args);
	if (rc != 0) {
		D_PRINT("punch file failed\r\n");
		return rc;
	}

	rc = pthread_join(read_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_stat_start(struct pmfs *pmfs, struct pmfs_obj *parent, const char *name,
		struct stat *stbuf)
{
	struct stat_args stat_args;
	static pthread_t stat_thread;
	int rc = 0;

	memset(&stat_args, 0, sizeof(stat_args));
	stat_args.pmfs = pmfs;
	stat_args.parent = parent;
	stat_args.name = name;
	stat_args.stbuf = stbuf;

	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs stat start -------------------\r\n");

	rc = pthread_create(&stat_thread, NULL, (void *)pmfs_stat_cb, &stat_args);
	if (rc != 0) {
		D_PRINT("pmfs stat failed\r\n");
		return rc;
	}

	rc = pthread_join(stat_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_release_start(struct pmfs_obj *obj)
{
	struct release_args release_args;
	static pthread_t release_thread;
	int rc = 0;

	memset(&release_args, 0, sizeof(release_args));
	release_args.obj = obj;

	g_vfca->vfcmd = "PMFS_TASKS";

	D_PRINT("---------------start pmfs release obj----------------------\r\n");
	rc = pthread_create(&release_thread, NULL, (void *) pmfs_release_cb,
			     &release_args);

	if (rc != 0) {
		D_PRINT("pmfs release obj cmd process thread failed: %d\n",
			rc);
		return rc;
	}

	rc = pthread_join(release_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}


static int
pmfs_umount_start(struct pmfs *pmfs)
{
	struct umount_args umount_args;
	static pthread_t umount_thread;
	int rc = 0;

	memset(&umount_args, 0, sizeof(umount_args));
	umount_args.pmfs = pmfs;

	D_PRINT("---------------start pmfs umount-------------------------\r\n");
	rc = pthread_create(&umount_thread, NULL, (void *) pmfs_umount_cb,
			     &umount_args);

	if (rc != 0) {
		D_PRINT("pmfs umount cmd process thread failed: %d\n", rc);
		return rc;
	}

	rc = pthread_join(umount_thread, NULL);
	if (rc != 0) {
		return rc;
	}

	return rc;
}

static int
pmfs_init_pool(void *arg, struct scan_context ctx)
{
	struct vos_fs_cmd_args *vfca = arg;
	int rc;

	uuid_copy(ctx.pool_uuid, vfca->pmfs_ctx.pmfs_pool.tsc_pool_uuid);
	ctx.pool_hdl = vfca->pmfs_ctx.pmfs_pool.tsc_poh;
	ctx.cur_cont = vfca->pmfs_ctx.pmfs_pool.pmfs_container;
	ctx.cur_cont.cl = vfca->pmfs_ctx.pmfs_pool.pmfs_container.cl;
	rc = pmfs_scan_pool(&ctx);

	if (rc != 0) {
		D_PRINT("init pool, rebuild container list failed\r\n");
	}

	vfca->pmfs_ctx.pmfs_pool.pmfs_container = ctx.cur_cont;

	return rc;
}

static struct pmfs *
pmfs_start_mount(struct pmfs_pool *pmfs_pool, struct pmfs *pmfs)
{
	struct scan_context ctx = { };
	daos_handle_t test_coh;
	int rc = 0;

	g_vfca->pmfs_ctx.pmfs_pool = *pmfs_pool;
	D_PRINT("---------------start scan pool---------------------------\r\n");
	D_PRINT("---------------rebuild container list before mount-------\r\n");
	rc = pmfs_init_pool(g_vfca, ctx);
	if (rc != 0) {
		return NULL;
	}

	D_PRINT("---------------rebuild container list done---------------\r\n");
	/* start mount thread */
	test_coh = g_vfca->pmfs_ctx.pmfs_pool.pmfs_container.tsc_coh;
	rc = pmfs_mount_start(g_vfca->pmfs_ctx.pmfs_pool.tsc_poh, test_coh,
			      &pmfs);
	if (rc != 0) {
		D_PRINT("pmfs mount start failed\r\n");
		return NULL;
	}
	D_PRINT("---------------pmfs mount done---------------\r\n");
	return pmfs;
}

static int
pmfs_start_mkfs(struct pmfs_pool *pmfs_pool)
{
	struct mkfs_args mags;
	static pthread_t cthread;
	int rc;

	memset(&mags, 0, sizeof(mags));
	mags.poh = pmfs_pool->tsc_poh;
	uuid_generate(mags.uuid);

	D_PRINT("---------------start pmfs mkfs---------------------------\r\n");
	rc = pthread_create(&cthread, NULL, (void *) pmfs_mkfs_cb, &mags);

	if (rc != 0) {
		D_PRINT("create fs cmd process thread failed: %d\n", rc);
		return rc;
	}


	rc = pthread_join(cthread, NULL);
	if (rc != 0) {
		return rc;
	}

	return 0;
}

static int
app_send_thread_cmds_in_pool(void)
{
	static pthread_t pthread;
	int rc;

	pthread_mutex_init(&g_vfca->g_pro_lock, NULL);

	/* Create cmd thread */

	rc = pthread_create(&pthread, NULL, (void *) vos_task_process,
			     g_vfca);
	if (rc != 0) {
		D_PRINT("create main process thread failed: %d\n", rc);
		return rc;
	}

	return 0;
}

static void
check_filesystem(void)
{
	/* check is there a pmfs filesystem in pool that bind to pmem*/
	DIR *dir_pmfs = opendir("/mnt/daos");
	struct dirent *entry = NULL;

	if (dir_pmfs == NULL) {
		D_PRINT("open /mnt/daos failed\r\n");
	} else {
		entry = readdir(dir_pmfs);
		while (entry) {
			if (strcmp(entry->d_name, ".") == 0 ||
					strcmp(entry->d_name, "..") == 0) {

			} else {
				if (entry->d_type == 4)
					D_PRINT("\033[31m %s \033[0m \t", entry->d_name);
				else
					D_PRINT("%s \t", entry->d_name);
			}
			entry = readdir(dir_pmfs);
		}
	}
}

static void
pmfs_list_pools(void)
{
	struct pmfs_pool *pmfs_pool;
	int count1 = 0;

	D_PRINT("pmfs_pool  : \r\n");
	d_list_for_each_entry(pmfs_pool, &g_test_fini_list, pl) {
		if (strcmp(DP_UUID(pmfs_pool->tsc_pool_uuid), "") == 0)
			continue;
		if (++count1 > count)
			break;
		D_PRINT("\t %d:  %s\t %p\r\n", count1, DP_UUID(pmfs_pool->tsc_pool_uuid),
				pmfs_pool);
	}
}

static void
pmfs_set_pool(char *string, struct pmfs_pool **set_pmfs_pool)
{
	struct pmfs_pool *pmfs_pool;
	bool found_pool = false;

	D_PRINT("pmfs_pool  : \r\n");
	d_list_for_each_entry(pmfs_pool, &g_test_fini_list, pl) {
		if (strcmp(DP_UUID(pmfs_pool->tsc_pool_uuid), string) == 0) {
			*set_pmfs_pool = pmfs_pool;
			found_pool = true;
			break;
		}
	}

	if (found_pool == true) {
		D_PRINT("\tSet pool:  %s\t %p\r\n", DP_UUID(pmfs_pool->tsc_pool_uuid), pmfs_pool);
	} else {
		D_PRINT("\t Can't find this pool with UUID: %s\r\n", string);
	}
}

static bool demo;
static bool present;
static int folders;
static double now;
static double then;
static  struct pmfs_pool  *g_pmfs_pool;
static  d_sg_list_t     g_user_sgl;
static  daos_size_t g_read_size;
static  daos_size_t g_write_size;
static  daos_size_t g_punch_size;
static  char *g_name;
static  struct dirent *tmp_dir;
static	char argv1[256], argv2[256], argv3[256], argv4[256], argv5[256], argv6[256];
static  char path_t[256];
enum root_op {
	checkroot = 0,
	addroot,
	removeroot
};

struct present_pool {
	struct pmfs_pool *p_pmfs_pool;
	struct pmfs *p_cmd_pmfs;
	struct pmfs_obj *p_cmd_obj;
	struct pmfs_obj *p_parent_cmd_obj;
	bool is_formatted;
	d_list_t ppl;
};

static  void
pmfs_path_root_reactor(char string[], enum root_op root_op)
{
	int j = 0;

	path_t[0] = '\0';

	while (string[j++] != '\0') {
		if (root_op == removeroot) {
			if (string[0] == '/') {
				path_t[j-1] = string[j];
			}
		} else if (root_op == addroot) {
			path_t[0] = '/';
			path_t[j] = string[j-1];
		} else {

		}
	}

		path_t[j] = '\0';
}

static struct present_pool *g_present_pool;
static struct present_pool *
pool_select_init(struct pmfs_pool *pmfs_pool, struct pmfs *t_cmd_pmfs,
		struct pmfs_obj *pmfs_obj,
		struct pmfs_obj *t_parent_cmd_obj, bool formatted)
{
	struct present_pool *present_pool;

	D_ALLOC(present_pool, sizeof(struct present_pool));
	D_ASSERT(present_pool);

	present_pool->p_pmfs_pool = pmfs_pool;
	present_pool->p_cmd_pmfs = t_cmd_pmfs;
	present_pool->p_cmd_obj = pmfs_obj;
	present_pool->p_parent_cmd_obj = t_parent_cmd_obj;
	present_pool->is_formatted = formatted;

	d_list_add(&present_pool->ppl, &g_present_list);
	return present_pool;
}

static void
pmfs_demo_release_present_pools(void)
{
	struct present_pool *present_pool, *tmp;

	d_list_for_each_entry_safe(present_pool, tmp, &g_present_list, ppl) {
		d_list_del(&present_pool->ppl);
		D_FREE_PTR(present_pool);
	}

}

static void
pmfs_cli_print_usage(void)
{
	if (g_present_pool->is_formatted == true)
		return;
	D_PRINT("Persitent memory filesystem client 1.0\r\n");
	D_PRINT("-----------------------------\r\n");
	D_PRINT("if there's not a pmfs filesystem\r\n");
	D_PRINT("you need to start mkfs\r\n");
	D_PRINT("-----------------------------\r\n");
}

static void
pmfs_cli_print_help_message(void)
{
D_PRINT("Persistent memory filesystem client 1.0\r\n");
D_PRINT("-----------------------------\r\n");
D_PRINT("list support commands\r\n\r\n");
D_PRINT("ap/addpool-------------------------------Add a pool to pool list \r\n");
D_PRINT("sp/setpool---[UUID]--------set the pool with UUID as present pool\r\n");
D_PRINT("lp/listpool-------------------------------lists all pools' uuids \r\n");
D_PRINT("mkfs--------------------------------------------format filesystem\r\n");
D_PRINT("mount--[folder]-------------------------------mount a dir in pmfs\r\n");
D_PRINT("umount-[folder]------------------------------umount a dir in pmfs\r\n");
D_PRINT("ls----------------------------------list all directories or files\r\n");
D_PRINT("lk/lookup /[directory or file]-----------lookup directory or file\r\n");
D_PRINT("rm----[folder]-------------------------------rm directoryor files\r\n");
D_PRINT("cd ../----[folder/file]------------exit present directory or file\r\n");
D_PRINT("s/stat--[folder]-------------------list stat of directory or file\r\n");
D_PRINT("mkdir-[folder]----------------------------create a folder or file\r\n");
D_PRINT("cd-[folder]---------------------------------------enter a folder \r\n");
D_PRINT("c/createfile-[filename]-[chunksize]-----------------create a file\r\n");
D_PRINT("r/readsync-[offset]-[stride size] [readsize]---------read a file \r\n");
D_PRINT("w/writesync-[offset] [buf_size a time] [filesize]---write a file \r\n");
D_PRINT("p/punch-[offset] [length]----punch a file starts from offset-----\r\n");
D_PRINT("-----------------------------and size is length.                 \r\n");

D_PRINT("lspmem-------------------------list pmem files abs path and files\r\n");
D_PRINT("q/quit/exit---------------------------------clean and safely exit\r\n");
D_PRINT("h/help-------------help to list -------------------------commands\r\n");
D_PRINT("--we can trace vos_media_select to see:..........................\r\n");
D_PRINT("--------------------------it's DAOS_MEDIA_SCM or DAOS_MEDIA_NVME-\r\n");
}

static void
pmfs_parse_args(char string[])
{
	int i = 0, j = 0, k = 0, s = 0, r = 0;
	char cmd[65536];

	/* remove blanks in command line.*/
	while (string[j++] != '\0') {
		if (string[j-1] == ' '  && string[j] == ' ') {
			continue;
		} else {
		       cmd[i++] = string[j-1];
		}
	}

	cmd[i] = '\0';
	argv1[0] = '\0';
	argv2[0] = '\0';
	argv3[0] = '\0';
	argv4[0] = '\0';
	argv5[0] = '\0';
	argv6[0] = '\0';

	/* assign argvs */
	while (k < i) {
		if (cmd[k++] == ' ' || cmd[k] == '\0') {
			num++;
			r = s;
		for (j = 0; j < k-r; j++) {
			if (num == 1) {
				argv1[j] = cmd[s++];
			} else if (num == 2) {
				argv2[j] = cmd[s++];
			} else if (num == 3) {
				argv3[j] = cmd[s++];
			} else if (num == 4) {
				argv4[j] = cmd[s++];
			} else if (num == 5) {
				argv5[j] = cmd[s++];
			} else if (num == 6) {
				argv6[j] = cmd[s++];
			} else {
				D_PRINT("Can't support such many args\r\n");
				break;
			}
		}

		if (num == 1) {
			argv1[j] = '\0';
			} else if (num == 2) {
				argv2[j] = '\0';
			} else if (num == 3) {
				argv3[j] = '\0';
			} else if (num == 4) {
				argv4[j] = '\0';
			} else if (num == 5) {
				argv5[j] = '\0';
			} else if (num == 6) {
				argv6[j] = '\0';
			}
		}

	}

	D_PRINT("\targv1 = %s\t", argv1);
	D_PRINT("argv2 = %s\t", argv2);
	D_PRINT("argv3 = %s\t", argv3);
	D_PRINT("argv4 = %s\t", argv4);
	D_PRINT("argv5 = %s\t", argv5);
	D_PRINT("argv6 = %s\t\n", argv6);
	D_PRINT("\tpool = %p\t", g_present_pool->p_pmfs_pool);
	D_PRINT("cmd_pmfs = %p\t", g_present_pool->p_cmd_pmfs);
	D_PRINT("cmd_obj = %p\t", g_present_pool->p_cmd_obj);
	D_PRINT("parent_cmd_obj = %p\t\n", g_present_pool->p_parent_cmd_obj);

	num = 0;
}

static bool
check_cmd(char argv_in[], char *string)
{
	int i;

	if (strlen(argv_in) != strlen(string))
		return false;

	for (i = 0; i < strlen(argv_in); i++) {
		if (argv_in[i] != *(string+i))
			return false;
	}

	return true;
}

static int
cli_parse_cmds(char cmd)
{
	int rc = 0;
	struct stat stbuf = {};

	while (1) {
		if (!scanf("%[^\n]", &cmd)) {
			while (getchar() != '\n')
				;
			continue;
		}

		pmfs_parse_args(&cmd);
demo:
		if (check_cmd(argv1, "mkfs")) {
			g_vfca->vfcmd = "PMFS_MKFS";
			pmfs_start_mkfs(g_present_pool->p_pmfs_pool);
			g_present_pool->is_formatted = true;
			break;
		} else if (check_cmd(argv1, "umount")) {
			pmfs_umount_start(g_present_pool->p_cmd_pmfs);

			D_PRINT("pmfs umount done.\r\n");
			g_present_pool->p_cmd_pmfs = NULL;
			continue;
		} else if (check_cmd(argv1, "mount")) {
			if (g_present_pool->is_formatted == false) {
				D_PRINT("there's no pmfs pool. need to mkfs firstly\r\n");
				continue;
			}
			g_present_pool->p_cmd_pmfs =
				pmfs_start_mount(g_present_pool->p_pmfs_pool,
						g_present_pool->p_cmd_pmfs);
			g_present_pool->p_cmd_obj = NULL;
			break;
		} else if (check_cmd(argv1, "lookup ") ||  check_cmd(argv1, "lk ")) {
			struct pmfs_obj *tmp1_obj = NULL;
			mode_t mode;

			rc = pmfs_lookup_start(g_present_pool->p_cmd_pmfs, argv2, O_RDONLY,
					&tmp1_obj, &mode, NULL);
			if (rc != 0) {
				D_PRINT("lookup failed\r\n");
				continue;
			}
			D_PRINT("\t find path= %s with mode = %x obj %p\r\n", argv2,
					mode, tmp1_obj);

			continue;
		} else if (check_cmd(argv1, "mkdir ")) {
			if (g_present_pool->p_cmd_pmfs == NULL) {
				D_PRINT("no mount point\r\n");
				continue;
			}
			rc = pmfs_mkdir_start(g_present_pool->p_cmd_pmfs,
					g_present_pool->p_cmd_obj, argv2, O_RDWR);
			if (rc < 0) {
				D_PRINT("mkdir failed\r\n");
				continue;
			}
			if (demo) {
				folders++;
				if (folders > 20) {
					folders = 0;
					D_PRINT("demo app cmd last = %-10.6f sec \r\n",
							*(g_vfca->duration));
					D_PRINT("demo app cmd update = %-10.3f IO/sec \r\n",
							2/(*(g_vfca->duration)));
					demo = false;
					continue;
				}
				sprintf(argv1, "%s", "demo");
				goto demo;
			}

			continue;
		} else if (check_cmd(argv1, "pwd")) {
			if (g_present_pool->p_parent_cmd_obj == NULL) {
				D_PRINT("\r/\n");
			} else {
				D_PRINT("\rneed further support\n");
			}
			continue;
		} else if (check_cmd(argv1, "ls")) {
			if (g_present_pool->p_cmd_pmfs == NULL) {
				D_PRINT("no mount point\r\n");
				continue;
			}

			uint32_t nr = 0;

			rc = pmfs_listdir_start(g_present_pool->p_cmd_pmfs,
					g_present_pool->p_cmd_obj, &nr);
			if (rc < 0) {
				D_PRINT("list dir failed\r\n");
				continue;
			}
			D_PRINT("----total %d files---\r\n", nr);
			continue;
		} else if (check_cmd(argv1, "lspmem")) {
			check_filesystem();
			continue;
		} else if (check_cmd(argv1, "rm ")) {
			pmfs_path_root_reactor(argv2, 1);
			daos_obj_id_t oid;

			rc = pmfs_remove_start(g_present_pool->p_cmd_pmfs,
					g_present_pool->p_cmd_obj, argv2, true, &oid);
			if (rc != 0)
				D_PRINT("pmfs remove start failed\r\n");
			D_PRINT("pmfs remove done \r\n");
			continue;
		} else if (check_cmd(argv1, "cd ") && check_cmd(argv2, "../")) {
			if (!g_present_pool->p_cmd_obj || !g_present_pool->p_cmd_pmfs) {
				continue;
			}
			mode_t mode;
			struct pmfs_obj *tmp_obj = g_present_pool->p_cmd_obj;

			rc = pmfs_lookup_start(g_present_pool->p_cmd_pmfs, g_name, O_RDONLY,
					&tmp_obj, &mode, NULL);
			if (rc != 0  || !tmp_obj) {
				D_PRINT("no such file or dir %s\r\n", argv2);
				continue;
			}

			rc = pmfs_release_start(tmp_obj);
			if (rc != 0) {
				D_PRINT("pmfs release tmp_obj start failed\r\n");
			continue;
			}
			D_PRINT("pmfs release obj %p\r\n", tmp_obj);
			g_present_pool->p_cmd_obj = g_present_pool->p_parent_cmd_obj;
			g_name = NULL;
			continue;
		} else if (check_cmd(argv1, "stat ") || check_cmd(argv1, "s ") ||
				check_cmd(argv1, "stat")) {
			rc = pmfs_stat_start(g_present_pool->p_cmd_pmfs, g_present_pool->p_cmd_obj,
					argv2, &stbuf);
			if (rc != 0) {
				D_PRINT("pmfs stat start failed\r\n");
				continue;
			}
			D_PRINT("total size =%ld", stbuf.st_size);
			D_PRINT("\t  File type and mode  =%x \r\n", stbuf.st_mode);
			continue;
		} else if (check_cmd(argv1, "cd ")) {
			g_present_pool->p_parent_cmd_obj = g_present_pool->p_cmd_obj;
			rc = pmfs_open_start(g_present_pool->p_cmd_pmfs, g_present_pool->p_cmd_obj,
					argv2, S_IFDIR, O_RDWR,
			     1024, NULL, &g_present_pool->p_cmd_obj);
			if (rc != 0) {
				D_PRINT("pmfs open failed\r\n");
				continue;
			}

			g_name = argv2;
			D_PRINT("enter folder %s obj %p\r\n", g_name, g_present_pool->p_cmd_obj);
			continue;
		} else if (check_cmd(argv1, "createfile ") || check_cmd(argv1, "c ")) {
			g_present_pool->p_parent_cmd_obj = g_present_pool->p_cmd_obj;
			rc = pmfs_open_start(g_present_pool->p_cmd_pmfs, g_present_pool->p_cmd_obj,
						argv2, S_IFREG,	O_CREAT | O_RDWR,
						atoi(argv3), argv4, &g_present_pool->p_cmd_obj);
			if (rc != 0) {
				D_PRINT("pmfs open file failed\r\n");
				continue;
			}
			D_PRINT("create file obj=%p\r\n", g_present_pool->p_cmd_obj);

			continue;
		} else if (check_cmd(argv1, "r ") || check_cmd(argv1, "readsync ")) {
			then = dts_time_now();
			daos_size_t stride_size = 0;

			if (argv4 == NULL) {
				g_read_size = atoll(argv3);
				stride_size = g_read_size;
			} else {
				g_read_size = atoll(argv4);
				stride_size = atoll(argv3);
			}

			if (g_read_size > g_write_size-atoll(argv2))
				g_read_size = g_write_size - atoll(argv2);

			if (stride_size > g_read_size)
				stride_size = g_read_size;

			rc = pmfs_read_start(g_present_pool->p_cmd_pmfs, g_present_pool->p_cmd_obj,
					 &g_user_sgl, atoll(argv2), stride_size, &g_read_size);
			if (rc != 0) {
				D_PRINT("pmfs read file failed\r\n");
				continue;
			}
			now = dts_time_now();
			D_PRINT("read file size=%ld\r\n", g_read_size);
			D_PRINT("last time for whole cmd= %-10.6fs\r\n", now-then);
			D_PRINT("read speed= %-10.3f  MiB/sec\r\n",
				g_read_size/(now-then)/(1024*1024));
			D_PRINT("pmfs read file done\r\n");
			continue;
		} else if (check_cmd(argv1, "w ") || check_cmd(argv1, "writesync ")) {
			then = dts_time_now();
			daos_size_t tmp_buf_size;

			if (atoll(argv4) == 0) {
				tmp_buf_size = atoll(argv3);
				g_write_size = atoll(argv3);
			} else {
				tmp_buf_size = atoll(argv3);
				g_write_size = atoll(argv4);
			}

			if (atoll(argv3) == 0) {
				tmp_buf_size = 4096;
			} else {
				tmp_buf_size = atoll(argv3);
			}
			rc = pmfs_write_start(g_present_pool->p_cmd_pmfs, g_present_pool->p_cmd_obj,
					&g_user_sgl, tmp_buf_size, atoll(argv2), &g_write_size);
			if (rc != 0) {
				D_PRINT("pmfs write file failed\r\n");
				continue;
			}
			now = dts_time_now();
			D_PRINT("write file obj=%p\r\n", &g_user_sgl);
			D_PRINT("last time for whole cmd= %-10.6fs\r\n", now-then);
			D_PRINT("write speed= %-10.3f  MiB/sec\r\n",
					(g_write_size - atoll(argv2))/(now-then)/(1024*1024));
			D_PRINT("pmfs write file done\r\n");
			continue;
		} else if (check_cmd(argv1, "p ") || check_cmd(argv1, "punch ")) {
			then = dts_time_now();
			g_punch_size = atoll(argv3);
			if (atoll(argv2) > g_write_size) {
				D_PRINT("pmfs punch offset is over file size\r\n");
				continue;
			}
			if (g_punch_size + atoll(argv2) > g_write_size) {
				D_PRINT("pmfs punch size over file size");
				D_PRINT("punch size equals filesize-offset\r\n");
				g_punch_size = g_write_size - atoll(argv2);
			}
			rc = pmfs_punch_start(g_present_pool->p_cmd_pmfs,
					g_present_pool->p_cmd_obj,
					atoll(argv2), g_punch_size);
			if (rc != 0) {
				D_PRINT("pmfs punch file failed\r\n");
				continue;
			}
			now = dts_time_now();
			D_PRINT("pmfs punch file done\r\n");
			D_PRINT("last time for whole cmd= %-10.6fs\r\n", now-then);
			D_PRINT("punch speed= %-10.3f  MiB/sec\r\n",
					g_punch_size/(now-then)/(1024*1024));
			continue;
		} else if (check_cmd(argv1, "ls ")) {
			mode_t mode;
			struct pmfs_obj *tmp_obj = g_present_pool->p_cmd_obj;

			rc = pmfs_lookup_start(g_present_pool->p_cmd_pmfs, argv2,
					O_RDONLY, &tmp_obj, &mode, NULL);
			if (rc != 0  || !tmp_obj) {
				D_PRINT("no such file or dir %s\r\n", argv2);
				continue;
			}

			uint32_t nr, i;

			rc = pmfs_listdir(g_present_pool->p_cmd_pmfs, tmp_obj, &nr);
			D_PRINT("start to readdir\r\n");
			rc = pmfs_readdir_start(g_present_pool->p_cmd_pmfs, tmp_obj,
					&nr, tmp_dir);
			if (rc != 0) {
				D_PRINT("pmfs readdir failed\r\n");
				continue;
			}
			D_PRINT("\n--------------list--------------------------\n");
			for (i = 0; i < nr; i++) {
				if (tmp_dir->d_type == 4)
					D_PRINT("\033[31m %s \033[0m \t", tmp_dir[i].d_name);
				else
					D_PRINT("%s \t", tmp_dir[i].d_name);
			}
			D_PRINT("\n");
			D_PRINT("demo app cmd last time = %-10.6fs\r\n",
					*(g_vfca->duration));
			D_PRINT("demo app fetch = %-10.3f IO/sec\r\n",
					nr/(*(g_vfca->duration)));
			continue;
		} else if (check_cmd(argv1, "addpool") || check_cmd(argv1, "ap")) {
			pmfs_add_single_pool(NULL, 8ULL << 30, 2ULL << 30, true, true);
			break;
		} else if (check_cmd(argv1, "listpool") || check_cmd(argv1, "lp")) {
			pmfs_list_pools();
			continue;
		} else if (check_cmd(argv1, "sp ") || check_cmd(argv1, "setpool ")) {
			struct present_pool *ppool;

			present = false;
			pmfs_set_pool(argv2, &g_pmfs_pool);
			d_list_for_each_entry(ppool, &g_present_list, ppl) {
				if (ppool->p_pmfs_pool == g_pmfs_pool) {
					g_present_pool = ppool;
					present = true;
					D_PRINT("pool formatted\r\n");
					break;
				}
			}
			if (present == true) {
				/* already exists, don't need to create. keep mounted state*/
				g_present_pool->p_cmd_pmfs =
					pmfs_start_mount(g_present_pool->p_pmfs_pool,
							g_present_pool->p_cmd_pmfs);
				g_present_pool->p_cmd_obj = NULL;

			} else {
				g_present_pool = pool_select_init(g_pmfs_pool, NULL, NULL, NULL, 0);
			}
			continue;
		} else if (check_cmd(argv1, "exit") || check_cmd(argv1, "q")
				|| check_cmd(argv1, "quit")) {
			g_vfca->force_exit = true;
			break;
		} else if (check_cmd(argv1, "close") || check_cmd(argv1, "cl")) {
			break;
		} else if (check_cmd(argv1, "demo")) {
			*(g_vfca->duration) = 0;
			sprintf(argv1, "%s", "mkdir ");
			pmfs_buffer_render(argv2, 10);
			demo = true;
			goto demo;
		} else if (check_cmd(argv1, "help") || check_cmd(argv1, "h")) {
			pmfs_cli_print_help_message();
			continue;
		} else {
			D_PRINT("Unknown command!\n");
			pmfs_cli_print_usage();
			continue;
		}
	}

	return 0;
}

int
main(int argc, char **argv)
{
	char cmd = 'h';

	g_vfca = calloc(1, sizeof(struct vos_fs_cmd_args));
	if (!g_vfca) {
		D_PRINT("cannot allocate memory for fs cmd\r\n");
		exit(-1);
	}

	g_vfca->pmfs_ctx = pmfs_set_ctx();
	g_vfca->vfcmd = NULL;
	D_ALLOC(g_vfca->duration, sizeof(g_vfca->duration));
	D_ASSERT(g_vfca->duration != NULL);
	D_ALLOC(tmp_dir, sizeof(struct dirent));
	D_ASSERT(tmp_dir != 0);
	/* Start to init process env */
	vos_task_process_init(g_vfca);
	pmfs_combine_pool_fini_list(&g_test_fini_list);
	D_ALLOC(g_vfca->vct, sizeof(g_vfca->vct));
	D_ASSERT(g_vfca->vct != NULL);

	daos_debug_init(DAOS_LOG_DEFAULT);
	/* Start to process cmds */
	app_send_thread_cmds_in_pool();

	g_pmfs_pool = pmfs_find_pool("/mnt/daos/pmfs_cli0.pmem");
	g_present_pool = pool_select_init(g_pmfs_pool, NULL, NULL, NULL, 0);
	pmfs_cli_print_usage();
start_cmd:
	if (g_vfca->force_exit)
		goto fini;
	cli_parse_cmds(cmd);
	D_PRINT("demo app cmd last time = %-10.6fs\r\n", *(g_vfca->duration));
	goto  start_cmd;
fini:
	vos_task_process_fini(g_vfca);

	D_FREE(g_vfca->duration);
	D_FREE(g_vfca->vct);
	D_FREE(g_vfca);
	D_FREE(tmp_dir);
	pmfs_demo_release_present_pools();
	return 0;
}
