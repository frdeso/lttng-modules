/* SPDX-License-Identifier: (GPL-2.0-only or LGPL-2.1-only)
 *
 * lttng-abi.c
 *
 * LTTng ABI
 *
 * Copyright (C) 2010-2012 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * Mimic system calls for:
 * - session creation, returns a file descriptor or failure.
 *   - channel creation, returns a file descriptor or failure.
 *     - Operates on a session file descriptor
 *     - Takes all channel options as parameters.
 *   - stream get, returns a file descriptor or failure.
 *     - Operates on a channel file descriptor.
 *   - stream notifier get, returns a file descriptor or failure.
 *     - Operates on a channel file descriptor.
 *   - event creation, returns a file descriptor or failure.
 *     - Operates on a channel file descriptor
 *     - Takes an event name as parameter
 *     - Takes an instrumentation source as parameter
 *       - e.g. tracepoints, dynamic_probes...
 *     - Takes instrumentation source specific arguments.
 */

#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/err.h>
#include <wrapper/vmalloc.h>	/* for wrapper_vmalloc_sync_mappings() */
#include <ringbuffer/vfs.h>
#include <ringbuffer/backend.h>
#include <ringbuffer/frontend.h>
#include <wrapper/poll.h>
#include <wrapper/file.h>
#include <wrapper/kref.h>
#include <lttng/string-utils.h>
#include <lttng/abi.h>
#include <lttng/abi-old.h>
#include <lttng/events.h>
#include <lttng/tracer.h>
#include <lttng/tp-mempool.h>
#include <ringbuffer/frontend_types.h>
#include <ringbuffer/iterator.h>

/*
 * This is LTTng's own personal way to create a system call as an external
 * module. We use ioctl() on /proc/lttng.
 */

static struct proc_dir_entry *lttng_proc_dentry;

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0))
static const struct proc_ops lttng_proc_ops;
#else
static const struct file_operations lttng_proc_ops;
#endif

static const struct file_operations lttng_session_fops;
static const struct file_operations lttng_trigger_group_fops;
static const struct file_operations lttng_channel_fops;
static const struct file_operations lttng_metadata_fops;
static const struct file_operations lttng_event_fops;
static struct file_operations lttng_stream_ring_buffer_file_operations;

static int put_u64(uint64_t val, unsigned long arg);
static int put_u32(uint32_t val, unsigned long arg);

/*
 * Teardown management: opened file descriptors keep a refcount on the module,
 * so it can only exit when all file descriptors are closed.
 */

static
int lttng_abi_create_session(void)
{
	struct lttng_session *session;
	struct file *session_file;
	int session_fd, ret;

	session = lttng_session_create();
	if (!session)
		return -ENOMEM;
	session_fd = lttng_get_unused_fd();
	if (session_fd < 0) {
		ret = session_fd;
		goto fd_error;
	}
	session_file = anon_inode_getfile("[lttng_session]",
					  &lttng_session_fops,
					  session, O_RDWR);
	if (IS_ERR(session_file)) {
		ret = PTR_ERR(session_file);
		goto file_error;
	}
	session->file = session_file;
	fd_install(session_fd, session_file);
	return session_fd;

file_error:
	put_unused_fd(session_fd);
fd_error:
	lttng_session_destroy(session);
	return ret;
}

static
void trigger_send_notification_work_wakeup(struct irq_work *entry)
{
	struct lttng_trigger_group *trigger_group = container_of(entry,
			struct lttng_trigger_group, wakeup_pending);
	wake_up_interruptible(&trigger_group->read_wait);
}

static
int lttng_abi_create_trigger_group(void)
{
	struct lttng_trigger_group *trigger_group;
	struct file *trigger_group_file;
	int trigger_group_fd, ret;

	trigger_group = lttng_trigger_group_create();
	if (!trigger_group)
		return -ENOMEM;

	trigger_group_fd = lttng_get_unused_fd();
	if (trigger_group_fd < 0) {
		ret = trigger_group_fd;
		goto fd_error;
	}
	trigger_group_file = anon_inode_getfile("[lttng_trigger_group]",
					  &lttng_trigger_group_fops,
					  trigger_group, O_RDWR);
	if (IS_ERR(trigger_group_file)) {
		ret = PTR_ERR(trigger_group_file);
		goto file_error;
	}

	trigger_group->file = trigger_group_file;
	init_waitqueue_head(&trigger_group->read_wait);
	init_irq_work(&trigger_group->wakeup_pending,
		      trigger_send_notification_work_wakeup);
	fd_install(trigger_group_fd, trigger_group_file);
	return trigger_group_fd;

file_error:
	put_unused_fd(trigger_group_fd);
fd_error:
	lttng_trigger_group_destroy(trigger_group);
	return ret;
}

static
int lttng_abi_tracepoint_list(void)
{
	struct file *tracepoint_list_file;
	int file_fd, ret;

	file_fd = lttng_get_unused_fd();
	if (file_fd < 0) {
		ret = file_fd;
		goto fd_error;
	}

	tracepoint_list_file = anon_inode_getfile("[lttng_tracepoint_list]",
					  &lttng_tracepoint_list_fops,
					  NULL, O_RDWR);
	if (IS_ERR(tracepoint_list_file)) {
		ret = PTR_ERR(tracepoint_list_file);
		goto file_error;
	}
	ret = lttng_tracepoint_list_fops.open(NULL, tracepoint_list_file);
	if (ret < 0)
		goto open_error;
	fd_install(file_fd, tracepoint_list_file);
	return file_fd;

open_error:
	fput(tracepoint_list_file);
file_error:
	put_unused_fd(file_fd);
fd_error:
	return ret;
}

#ifndef CONFIG_HAVE_SYSCALL_TRACEPOINTS
static inline
int lttng_abi_syscall_list(void)
{
	return -ENOSYS;
}
#else
static
int lttng_abi_syscall_list(void)
{
	struct file *syscall_list_file;
	int file_fd, ret;

	file_fd = lttng_get_unused_fd();
	if (file_fd < 0) {
		ret = file_fd;
		goto fd_error;
	}

	syscall_list_file = anon_inode_getfile("[lttng_syscall_list]",
					  &lttng_syscall_list_fops,
					  NULL, O_RDWR);
	if (IS_ERR(syscall_list_file)) {
		ret = PTR_ERR(syscall_list_file);
		goto file_error;
	}
	ret = lttng_syscall_list_fops.open(NULL, syscall_list_file);
	if (ret < 0)
		goto open_error;
	fd_install(file_fd, syscall_list_file);
	return file_fd;

open_error:
	fput(syscall_list_file);
file_error:
	put_unused_fd(file_fd);
fd_error:
	return ret;
}
#endif

static
void lttng_abi_tracer_version(struct lttng_kernel_tracer_version *v)
{
	v->major = LTTNG_MODULES_MAJOR_VERSION;
	v->minor = LTTNG_MODULES_MINOR_VERSION;
	v->patchlevel = LTTNG_MODULES_PATCHLEVEL_VERSION;
}

static
void lttng_abi_tracer_abi_version(struct lttng_kernel_tracer_abi_version *v)
{
	v->major = LTTNG_MODULES_ABI_MAJOR_VERSION;
	v->minor = LTTNG_MODULES_ABI_MINOR_VERSION;
}

static
long lttng_abi_add_context(struct file *file,
	struct lttng_kernel_context *context_param,
	struct lttng_ctx **ctx, struct lttng_session *session)
{

	if (session->been_active)
		return -EPERM;

	switch (context_param->ctx) {
	case LTTNG_KERNEL_CONTEXT_PID:
		return lttng_add_pid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_PRIO:
		return lttng_add_prio_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_NICE:
		return lttng_add_nice_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VPID:
		return lttng_add_vpid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_TID:
		return lttng_add_tid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VTID:
		return lttng_add_vtid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_PPID:
		return lttng_add_ppid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VPPID:
		return lttng_add_vppid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_PERF_COUNTER:
		context_param->u.perf_counter.name[LTTNG_KERNEL_SYM_NAME_LEN - 1] = '\0';
		return lttng_add_perf_counter_to_ctx(context_param->u.perf_counter.type,
				context_param->u.perf_counter.config,
				context_param->u.perf_counter.name,
				ctx);
	case LTTNG_KERNEL_CONTEXT_PROCNAME:
		return lttng_add_procname_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_HOSTNAME:
		return lttng_add_hostname_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_CPU_ID:
		return lttng_add_cpu_id_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_INTERRUPTIBLE:
		return lttng_add_interruptible_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_NEED_RESCHEDULE:
		return lttng_add_need_reschedule_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_PREEMPTIBLE:
		return lttng_add_preemptible_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_MIGRATABLE:
		return lttng_add_migratable_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_CALLSTACK_KERNEL:
	case LTTNG_KERNEL_CONTEXT_CALLSTACK_USER:
		return lttng_add_callstack_to_ctx(ctx, context_param->ctx);
	case LTTNG_KERNEL_CONTEXT_CGROUP_NS:
		return lttng_add_cgroup_ns_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_IPC_NS:
		return lttng_add_ipc_ns_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_MNT_NS:
		return lttng_add_mnt_ns_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_NET_NS:
		return lttng_add_net_ns_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_PID_NS:
		return lttng_add_pid_ns_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_USER_NS:
		return lttng_add_user_ns_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_UTS_NS:
		return lttng_add_uts_ns_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_UID:
		return lttng_add_uid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_EUID:
		return lttng_add_euid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_SUID:
		return lttng_add_suid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_GID:
		return lttng_add_gid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_EGID:
		return lttng_add_egid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_SGID:
		return lttng_add_sgid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VUID:
		return lttng_add_vuid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VEUID:
		return lttng_add_veuid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VSUID:
		return lttng_add_vsuid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VGID:
		return lttng_add_vgid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VEGID:
		return lttng_add_vegid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_VSGID:
		return lttng_add_vsgid_to_ctx(ctx);
	case LTTNG_KERNEL_CONTEXT_TIME_NS:
		return lttng_add_time_ns_to_ctx(ctx);
	default:
		return -EINVAL;
	}
}

/**
 *	lttng_ioctl - lttng syscall through ioctl
 *
 *	@file: the file
 *	@cmd: the command
 *	@arg: command arg
 *
 *	This ioctl implements lttng commands:
 *	LTTNG_KERNEL_SESSION
 *		Returns a LTTng trace session file descriptor
 *	LTTNG_KERNEL_TRACER_VERSION
 *		Returns the LTTng kernel tracer version
 *	LTTNG_KERNEL_TRACEPOINT_LIST
 *		Returns a file descriptor listing available tracepoints
 *	LTTNG_KERNEL_WAIT_QUIESCENT
 *		Returns after all previously running probes have completed
 *	LTTNG_KERNEL_TRACER_ABI_VERSION
 *		Returns the LTTng kernel tracer ABI version
 *	LTTNG_KERNEL_TRIGGER_GROUP_CREATE
 *		Returns a LTTng trigger group file descriptor
 *
 * The returned session will be deleted when its file descriptor is closed.
 */
static
long lttng_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case LTTNG_KERNEL_OLD_SESSION:
	case LTTNG_KERNEL_SESSION:
		return lttng_abi_create_session();
	case LTTNG_KERNEL_TRIGGER_GROUP_CREATE:
		return lttng_abi_create_trigger_group();
	case LTTNG_KERNEL_OLD_TRACER_VERSION:
	{
		struct lttng_kernel_tracer_version v;
		struct lttng_kernel_old_tracer_version oldv;
		struct lttng_kernel_old_tracer_version *uversion =
			(struct lttng_kernel_old_tracer_version __user *) arg;

		lttng_abi_tracer_version(&v);
		oldv.major = v.major;
		oldv.minor = v.minor;
		oldv.patchlevel = v.patchlevel;

		if (copy_to_user(uversion, &oldv, sizeof(oldv)))
			return -EFAULT;
		return 0;
	}
	case LTTNG_KERNEL_TRACER_VERSION:
	{
		struct lttng_kernel_tracer_version version;
		struct lttng_kernel_tracer_version *uversion =
			(struct lttng_kernel_tracer_version __user *) arg;

		lttng_abi_tracer_version(&version);

		if (copy_to_user(uversion, &version, sizeof(version)))
			return -EFAULT;
		return 0;
	}
	case LTTNG_KERNEL_TRACER_ABI_VERSION:
	{
		struct lttng_kernel_tracer_abi_version version;
		struct lttng_kernel_tracer_abi_version *uversion =
			(struct lttng_kernel_tracer_abi_version __user *) arg;

		lttng_abi_tracer_abi_version(&version);

		if (copy_to_user(uversion, &version, sizeof(version)))
			return -EFAULT;
		return 0;
	}
	case LTTNG_KERNEL_OLD_TRACEPOINT_LIST:
	case LTTNG_KERNEL_TRACEPOINT_LIST:
		return lttng_abi_tracepoint_list();
	case LTTNG_KERNEL_SYSCALL_LIST:
		return lttng_abi_syscall_list();
	case LTTNG_KERNEL_OLD_WAIT_QUIESCENT:
	case LTTNG_KERNEL_WAIT_QUIESCENT:
		synchronize_trace();
		return 0;
	case LTTNG_KERNEL_OLD_CALIBRATE:
	{
		struct lttng_kernel_old_calibrate __user *ucalibrate =
			(struct lttng_kernel_old_calibrate __user *) arg;
		struct lttng_kernel_old_calibrate old_calibrate;
		struct lttng_kernel_calibrate calibrate;
		int ret;

		if (copy_from_user(&old_calibrate, ucalibrate, sizeof(old_calibrate)))
			return -EFAULT;
		calibrate.type = old_calibrate.type;
		ret = lttng_calibrate(&calibrate);
		if (copy_to_user(ucalibrate, &old_calibrate, sizeof(old_calibrate)))
			return -EFAULT;
		return ret;
	}
	case LTTNG_KERNEL_CALIBRATE:
	{
		struct lttng_kernel_calibrate __user *ucalibrate =
			(struct lttng_kernel_calibrate __user *) arg;
		struct lttng_kernel_calibrate calibrate;
		int ret;

		if (copy_from_user(&calibrate, ucalibrate, sizeof(calibrate)))
			return -EFAULT;
		ret = lttng_calibrate(&calibrate);
		if (copy_to_user(ucalibrate, &calibrate, sizeof(calibrate)))
			return -EFAULT;
		return ret;
	}
	default:
		return -ENOIOCTLCMD;
	}
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5,6,0))
static const struct proc_ops lttng_proc_ops = {
	.proc_ioctl = lttng_ioctl,
#ifdef CONFIG_COMPAT
	.proc_compat_ioctl = lttng_ioctl,
#endif /* CONFIG_COMPAT */
};
#else
static const struct file_operations lttng_proc_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = lttng_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_ioctl,
#endif /* CONFIG_COMPAT */
};
#endif

static
int lttng_abi_create_channel(struct file *session_file,
			     struct lttng_kernel_channel *chan_param,
			     enum channel_type channel_type)
{
	struct lttng_session *session = session_file->private_data;
	const struct file_operations *fops = NULL;
	const char *transport_name;
	struct lttng_channel *chan;
	struct file *chan_file;
	int chan_fd;
	int ret = 0;

	chan_fd = lttng_get_unused_fd();
	if (chan_fd < 0) {
		ret = chan_fd;
		goto fd_error;
	}
	switch (channel_type) {
	case PER_CPU_CHANNEL:
		fops = &lttng_channel_fops;
		break;
	case METADATA_CHANNEL:
		fops = &lttng_metadata_fops;
		break;
	}

	chan_file = anon_inode_getfile("[lttng_channel]",
				       fops,
				       NULL, O_RDWR);
	if (IS_ERR(chan_file)) {
		ret = PTR_ERR(chan_file);
		goto file_error;
	}
	switch (channel_type) {
	case PER_CPU_CHANNEL:
		if (chan_param->output == LTTNG_KERNEL_SPLICE) {
			transport_name = chan_param->overwrite ?
				"relay-overwrite" : "relay-discard";
		} else if (chan_param->output == LTTNG_KERNEL_MMAP) {
			transport_name = chan_param->overwrite ?
				"relay-overwrite-mmap" : "relay-discard-mmap";
		} else {
			return -EINVAL;
		}
		break;
	case METADATA_CHANNEL:
		if (chan_param->output == LTTNG_KERNEL_SPLICE)
			transport_name = "relay-metadata";
		else if (chan_param->output == LTTNG_KERNEL_MMAP)
			transport_name = "relay-metadata-mmap";
		else
			return -EINVAL;
		break;
	default:
		transport_name = "<unknown>";
		break;
	}
	if (!atomic_long_add_unless(&session_file->f_count, 1, LONG_MAX)) {
		ret = -EOVERFLOW;
		goto refcount_error;
	}
	/*
	 * We tolerate no failure path after channel creation. It will stay
	 * invariant for the rest of the session.
	 */
	chan = lttng_channel_create(session, transport_name, NULL,
				  chan_param->subbuf_size,
				  chan_param->num_subbuf,
				  chan_param->switch_timer_interval,
				  chan_param->read_timer_interval,
				  channel_type);
	if (!chan) {
		ret = -EINVAL;
		goto chan_error;
	}
	chan->file = chan_file;
	chan_file->private_data = chan;
	fd_install(chan_fd, chan_file);

	return chan_fd;

chan_error:
	atomic_long_dec(&session_file->f_count);
refcount_error:
	fput(chan_file);
file_error:
	put_unused_fd(chan_fd);
fd_error:
	return ret;
}

static
int lttng_abi_session_set_name(struct lttng_session *session,
		struct lttng_kernel_session_name *name)
{
	size_t len;

	len = strnlen(name->name, LTTNG_KERNEL_SESSION_NAME_LEN);

	if (len == LTTNG_KERNEL_SESSION_NAME_LEN) {
		/* Name is too long/malformed */
		return -EINVAL;
	}

	strcpy(session->name, name->name);
	return 0;
}

static
int lttng_abi_session_set_creation_time(struct lttng_session *session,
		struct lttng_kernel_session_creation_time *time)
{
	size_t len;

	len = strnlen(time->iso8601, LTTNG_KERNEL_SESSION_CREATION_TIME_ISO8601_LEN);

	if (len == LTTNG_KERNEL_SESSION_CREATION_TIME_ISO8601_LEN) {
		/* Time is too long/malformed */
		return -EINVAL;
	}

	strcpy(session->creation_time, time->iso8601);
	return 0;
}

static
enum tracker_type get_tracker_type(struct lttng_kernel_tracker_args *tracker)
{
	switch (tracker->type) {
	case LTTNG_KERNEL_TRACKER_PID:
		return TRACKER_PID;
	case LTTNG_KERNEL_TRACKER_VPID:
		return TRACKER_VPID;
	case LTTNG_KERNEL_TRACKER_UID:
		return TRACKER_UID;
	case LTTNG_KERNEL_TRACKER_VUID:
		return TRACKER_VUID;
	case LTTNG_KERNEL_TRACKER_GID:
		return TRACKER_GID;
	case LTTNG_KERNEL_TRACKER_VGID:
		return TRACKER_VGID;
	default:
		return TRACKER_UNKNOWN;
	}
}

/**
 *	lttng_session_ioctl - lttng session fd ioctl
 *
 *	@file: the file
 *	@cmd: the command
 *	@arg: command arg
 *
 *	This ioctl implements lttng commands:
 *	LTTNG_KERNEL_CHANNEL
 *		Returns a LTTng channel file descriptor
 *	LTTNG_KERNEL_ENABLE
 *		Enables tracing for a session (weak enable)
 *	LTTNG_KERNEL_DISABLE
 *		Disables tracing for a session (strong disable)
 *	LTTNG_KERNEL_METADATA
 *		Returns a LTTng metadata file descriptor
 *	LTTNG_KERNEL_SESSION_TRACK_PID
 *		Add PID to session PID tracker
 *	LTTNG_KERNEL_SESSION_UNTRACK_PID
 *		Remove PID from session PID tracker
 *	LTTNG_KERNEL_SESSION_TRACK_ID
 *		Add ID to tracker
 *	LTTNG_KERNEL_SESSION_UNTRACK_ID
 *		Remove ID from tracker
 *
 * The returned channel will be deleted when its file descriptor is closed.
 */
static
long lttng_session_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lttng_session *session = file->private_data;
	struct lttng_kernel_channel chan_param;
	struct lttng_kernel_old_channel old_chan_param;

	switch (cmd) {
	case LTTNG_KERNEL_OLD_CHANNEL:
	{
		if (copy_from_user(&old_chan_param,
				(struct lttng_kernel_old_channel __user *) arg,
				sizeof(struct lttng_kernel_old_channel)))
			return -EFAULT;
		chan_param.overwrite = old_chan_param.overwrite;
		chan_param.subbuf_size = old_chan_param.subbuf_size;
		chan_param.num_subbuf = old_chan_param.num_subbuf;
		chan_param.switch_timer_interval = old_chan_param.switch_timer_interval;
		chan_param.read_timer_interval = old_chan_param.read_timer_interval;
		chan_param.output = old_chan_param.output;

		return lttng_abi_create_channel(file, &chan_param,
				PER_CPU_CHANNEL);
	}
	case LTTNG_KERNEL_CHANNEL:
	{
		if (copy_from_user(&chan_param,
				(struct lttng_kernel_channel __user *) arg,
				sizeof(struct lttng_kernel_channel)))
			return -EFAULT;
		return lttng_abi_create_channel(file, &chan_param,
				PER_CPU_CHANNEL);
	}
	case LTTNG_KERNEL_OLD_SESSION_START:
	case LTTNG_KERNEL_OLD_ENABLE:
	case LTTNG_KERNEL_SESSION_START:
	case LTTNG_KERNEL_ENABLE:
		return lttng_session_enable(session);
	case LTTNG_KERNEL_OLD_SESSION_STOP:
	case LTTNG_KERNEL_OLD_DISABLE:
	case LTTNG_KERNEL_SESSION_STOP:
	case LTTNG_KERNEL_DISABLE:
		return lttng_session_disable(session);
	case LTTNG_KERNEL_OLD_METADATA:
	{
		if (copy_from_user(&old_chan_param,
				(struct lttng_kernel_old_channel __user *) arg,
				sizeof(struct lttng_kernel_old_channel)))
			return -EFAULT;
		chan_param.overwrite = old_chan_param.overwrite;
		chan_param.subbuf_size = old_chan_param.subbuf_size;
		chan_param.num_subbuf = old_chan_param.num_subbuf;
		chan_param.switch_timer_interval = old_chan_param.switch_timer_interval;
		chan_param.read_timer_interval = old_chan_param.read_timer_interval;
		chan_param.output = old_chan_param.output;

		return lttng_abi_create_channel(file, &chan_param,
				METADATA_CHANNEL);
	}
	case LTTNG_KERNEL_METADATA:
	{
		if (copy_from_user(&chan_param,
					(struct lttng_kernel_channel __user *) arg,
					sizeof(struct lttng_kernel_channel)))
			return -EFAULT;
		return lttng_abi_create_channel(file, &chan_param,
				METADATA_CHANNEL);
	}
	case LTTNG_KERNEL_SESSION_TRACK_PID:
		return lttng_session_track_id(session, TRACKER_PID, (int) arg);
	case LTTNG_KERNEL_SESSION_UNTRACK_PID:
		return lttng_session_untrack_id(session, TRACKER_PID, (int) arg);
	case LTTNG_KERNEL_SESSION_TRACK_ID:
	{
		struct lttng_kernel_tracker_args tracker;
		enum tracker_type tracker_type;

		if (copy_from_user(&tracker,
				(struct lttng_kernel_tracker_args __user *) arg,
				sizeof(struct lttng_kernel_tracker_args)))
			return -EFAULT;
		tracker_type = get_tracker_type(&tracker);
		if (tracker_type == TRACKER_UNKNOWN)
			return -EINVAL;
		return lttng_session_track_id(session, tracker_type, tracker.id);
	}
	case LTTNG_KERNEL_SESSION_UNTRACK_ID:
	{
		struct lttng_kernel_tracker_args tracker;
		enum tracker_type tracker_type;

		if (copy_from_user(&tracker,
				(struct lttng_kernel_tracker_args __user *) arg,
				sizeof(struct lttng_kernel_tracker_args)))
			return -EFAULT;
		tracker_type = get_tracker_type(&tracker);
		if (tracker_type == TRACKER_UNKNOWN)
			return -EINVAL;
		return lttng_session_untrack_id(session, tracker_type,
				tracker.id);
	}
	case LTTNG_KERNEL_SESSION_LIST_TRACKER_PIDS:
		return lttng_session_list_tracker_ids(session, TRACKER_PID);
	case LTTNG_KERNEL_SESSION_LIST_TRACKER_IDS:
	{
		struct lttng_kernel_tracker_args tracker;
		enum tracker_type tracker_type;

		if (copy_from_user(&tracker,
				(struct lttng_kernel_tracker_args __user *) arg,
				sizeof(struct lttng_kernel_tracker_args)))
			return -EFAULT;
		tracker_type = get_tracker_type(&tracker);
		if (tracker_type == TRACKER_UNKNOWN)
			return -EINVAL;
		return lttng_session_list_tracker_ids(session, tracker_type);
	}
	case LTTNG_KERNEL_SESSION_METADATA_REGEN:
		return lttng_session_metadata_regenerate(session);
	case LTTNG_KERNEL_SESSION_STATEDUMP:
		return lttng_session_statedump(session);
	case LTTNG_KERNEL_SESSION_SET_NAME:
	{
		struct lttng_kernel_session_name name;

		if (copy_from_user(&name,
				(struct lttng_kernel_session_name __user *) arg,
				sizeof(struct lttng_kernel_session_name)))
			return -EFAULT;
		return lttng_abi_session_set_name(session, &name);
	}
	case LTTNG_KERNEL_SESSION_SET_CREATION_TIME:
	{
		struct lttng_kernel_session_creation_time time;

		if (copy_from_user(&time,
				(struct lttng_kernel_session_creation_time __user *) arg,
				sizeof(struct lttng_kernel_session_creation_time)))
			return -EFAULT;
		return lttng_abi_session_set_creation_time(session, &time);
	}
	default:
		return -ENOIOCTLCMD;
	}
}

/*
 * Called when the last file reference is dropped.
 *
 * Big fat note: channels and events are invariant for the whole session after
 * their creation. So this session destruction also destroys all channel and
 * event structures specific to this session (they are not destroyed when their
 * individual file is released).
 */
static
int lttng_session_release(struct inode *inode, struct file *file)
{
	struct lttng_session *session = file->private_data;

	if (session)
		lttng_session_destroy(session);
	return 0;
}

static const struct file_operations lttng_session_fops = {
	.owner = THIS_MODULE,
	.release = lttng_session_release,
	.unlocked_ioctl = lttng_session_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_session_ioctl,
#endif
};

/*
 * When encountering empty buffer, flush current sub-buffer if non-empty
 * and retry (if new data available to read after flush).
 */
static
ssize_t lttng_trigger_group_notif_read(struct file *filp, char __user *user_buf,
		size_t count, loff_t *ppos)
{
	struct lttng_trigger_group *trigger_group = filp->private_data;
	struct channel *chan = trigger_group->chan;
	struct lib_ring_buffer *buf = trigger_group->buf;
	ssize_t read_count = 0, len;
	size_t read_offset;

	might_sleep();
	if (!lttng_access_ok(VERIFY_WRITE, user_buf, count))
		return -EFAULT;

	/* Finish copy of previous record */
	if (*ppos != 0) {
		if (read_count < count) {
			len = chan->iter.len_left;
			read_offset = *ppos;
			goto skip_get_next;
		}
	}

	while (read_count < count) {
		size_t copy_len, space_left;

		len = lib_ring_buffer_get_next_record(chan, buf);
len_test:
		if (len < 0) {
			/*
			 * Check if buffer is finalized (end of file).
			 */
			if (len == -ENODATA) {
				/* A 0 read_count will tell about end of file */
				goto nodata;
			}
			if (filp->f_flags & O_NONBLOCK) {
				if (!read_count)
					read_count = -EAGAIN;
				goto nodata;
			} else {
				int error;

				/*
				 * No data available at the moment, return what
				 * we got.
				 */
				if (read_count)
					goto nodata;

				/*
				 * Wait for returned len to be >= 0 or -ENODATA.
				 */
				error = wait_event_interruptible(
					  trigger_group->read_wait,
					  ((len = lib_ring_buffer_get_next_record(
						  chan, buf)), len != -EAGAIN));
				CHAN_WARN_ON(chan, len == -EBUSY);
				if (error) {
					read_count = error;
					goto nodata;
				}
				CHAN_WARN_ON(chan, len < 0 && len != -ENODATA);
				goto len_test;
			}
		}
		read_offset = buf->iter.read_offset;
skip_get_next:
		space_left = count - read_count;
		if (len <= space_left) {
			copy_len = len;
			chan->iter.len_left = 0;
			*ppos = 0;
		} else {
			copy_len = space_left;
			chan->iter.len_left = len - copy_len;
			*ppos = read_offset + copy_len;
		}
		if (__lib_ring_buffer_copy_to_user(&buf->backend, read_offset,
					       &user_buf[read_count],
					       copy_len)) {
			/*
			 * Leave the len_left and ppos values at their current
			 * state, as we currently have a valid event to read.
			 */
			return -EFAULT;
		}
		read_count += copy_len;
	}
	goto put_record;

nodata:
	*ppos = 0;
	chan->iter.len_left = 0;

put_record:
	lib_ring_buffer_put_current_record(buf);
	return read_count;
}

/*
 * If the ring buffer is non empty (even just a partial subbuffer), return that
 * there is data available. Perform a ring buffer flush if we encounter a
 * non-empty ring buffer which does not have any consumeable subbuffer available.
 */
static
unsigned int lttng_trigger_group_notif_poll(struct file *filp,
		poll_table *wait)
{
	unsigned int mask = 0;
	struct lttng_trigger_group *trigger_group = filp->private_data;
	struct channel *chan = trigger_group->chan;
	struct lib_ring_buffer *buf = trigger_group->buf;
	const struct lib_ring_buffer_config *config = &chan->backend.config;
	int finalized, disabled;
	unsigned long consumed, offset;
	size_t subbuffer_header_size = config->cb.subbuffer_header_size();

	if (filp->f_mode & FMODE_READ) {
		poll_wait_set_exclusive(wait);
		poll_wait(filp, &trigger_group->read_wait, wait);

		finalized = lib_ring_buffer_is_finalized(config, buf);
		disabled = lib_ring_buffer_channel_is_disabled(chan);

		/*
		 * lib_ring_buffer_is_finalized() contains a smp_rmb() ordering
		 * finalized load before offsets loads.
		 */
		WARN_ON(atomic_long_read(&buf->active_readers) != 1);
retry:
		if (disabled)
			return POLLERR;

		offset = lib_ring_buffer_get_offset(config, buf);
		consumed = lib_ring_buffer_get_consumed(config, buf);

		/*
		 * If there is no buffer available to consume.
		 */
		if (subbuf_trunc(offset, chan) - subbuf_trunc(consumed, chan) == 0) {
			/*
			 * If there is a non-empty subbuffer, flush and try again.
			 */
			if (subbuf_offset(offset, chan) > subbuffer_header_size) {
				lib_ring_buffer_switch_remote(buf);
				goto retry;
			}

			if (finalized)
				return POLLHUP;
			else {
				/*
				 * The memory barriers
				 * __wait_event()/wake_up_interruptible() take
				 * care of "raw_spin_is_locked" memory ordering.
				 */
				if (raw_spin_is_locked(&buf->raw_tick_nohz_spinlock))
					goto retry;
				else
					return 0;
			}
		} else {
			if (subbuf_trunc(offset, chan) - subbuf_trunc(consumed, chan)
					>= chan->backend.buf_size)
				return POLLPRI | POLLRDBAND;
			else
				return POLLIN | POLLRDNORM;
		}
	}

	return mask;
}

/**
 *	lttng_trigger_group_notif_open - trigger ring buffer open file operation
 *	@inode: opened inode
 *	@file: opened file
 *
 *	Open implementation. Makes sure only one open instance of a buffer is
 *	done at a given moment.
 */
static int lttng_trigger_group_notif_open(struct inode *inode, struct file *file)
{
	struct lttng_trigger_group *trigger_group = inode->i_private;
	struct lib_ring_buffer *buf = trigger_group->buf;

	file->private_data = trigger_group;
	return lib_ring_buffer_open(inode, file, buf);
}

/**
 *	lttng_trigger_group_notif_release - trigger ring buffer release file operation
 *	@inode: opened inode
 *	@file: opened file
 *
 *	Release implementation.
 */
static int lttng_trigger_group_notif_release(struct inode *inode, struct file *file)
{
	struct lttng_trigger_group *trigger_group = file->private_data;
	struct lib_ring_buffer *buf = trigger_group->buf;
	int ret;

	ret = lib_ring_buffer_release(inode, file, buf);
	if (ret)
		return ret;
	fput(trigger_group->file);
	return 0;
}

static const struct file_operations lttng_trigger_group_notif_fops = {
	.owner = THIS_MODULE,
	.open = lttng_trigger_group_notif_open,
	.release = lttng_trigger_group_notif_release,
	.read = lttng_trigger_group_notif_read,
	.poll = lttng_trigger_group_notif_poll,
};

/**
 *	lttng_metadata_ring_buffer_poll - LTTng ring buffer poll file operation
 *	@filp: the file
 *	@wait: poll table
 *
 *	Handles the poll operations for the metadata channels.
 */
static
unsigned int lttng_metadata_ring_buffer_poll(struct file *filp,
		poll_table *wait)
{
	struct lttng_metadata_stream *stream = filp->private_data;
	struct lib_ring_buffer *buf = stream->priv;
	int finalized;
	unsigned int mask = 0;

	if (filp->f_mode & FMODE_READ) {
		poll_wait_set_exclusive(wait);
		poll_wait(filp, &stream->read_wait, wait);

		finalized = stream->finalized;

		/*
		 * lib_ring_buffer_is_finalized() contains a smp_rmb()
		 * ordering finalized load before offsets loads.
		 */
		WARN_ON(atomic_long_read(&buf->active_readers) != 1);

		if (finalized)
			mask |= POLLHUP;

		mutex_lock(&stream->metadata_cache->lock);
		if (stream->metadata_cache->metadata_written >
				stream->metadata_out)
			mask |= POLLIN;
		mutex_unlock(&stream->metadata_cache->lock);
	}

	return mask;
}

static
void lttng_metadata_ring_buffer_ioctl_put_next_subbuf(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	struct lttng_metadata_stream *stream = filp->private_data;

	stream->metadata_out = stream->metadata_in;
}

/*
 * Reset the counter of how much metadata has been consumed to 0. That way,
 * the consumer receives the content of the metadata cache unchanged. This is
 * different from the metadata_regenerate where the offset from epoch is
 * resampled, here we want the exact same content as the last time the metadata
 * was generated. This command is only possible if all the metadata written
 * in the cache has been output to the metadata stream to avoid corrupting the
 * metadata file.
 *
 * Return 0 on success, a negative value on error.
 */
static
int lttng_metadata_cache_dump(struct lttng_metadata_stream *stream)
{
	int ret;
	struct lttng_metadata_cache *cache = stream->metadata_cache;

	mutex_lock(&cache->lock);
	if (stream->metadata_out != cache->metadata_written) {
		ret = -EBUSY;
		goto end;
	}
	stream->metadata_out = 0;
	stream->metadata_in = 0;
	wake_up_interruptible(&stream->read_wait);
	ret = 0;

end:
	mutex_unlock(&cache->lock);
	return ret;
}

static
long lttng_metadata_ring_buffer_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	int ret;
	struct lttng_metadata_stream *stream = filp->private_data;
	struct lib_ring_buffer *buf = stream->priv;
	unsigned int rb_cmd;
	bool coherent;

	if (cmd == RING_BUFFER_GET_NEXT_SUBBUF_METADATA_CHECK)
		rb_cmd = RING_BUFFER_GET_NEXT_SUBBUF;
	else
		rb_cmd = cmd;

	switch (cmd) {
	case RING_BUFFER_GET_NEXT_SUBBUF:
	{
		struct lttng_metadata_stream *stream = filp->private_data;
		struct lib_ring_buffer *buf = stream->priv;
		struct channel *chan = buf->backend.chan;

		ret = lttng_metadata_output_channel(stream, chan, NULL);
		if (ret > 0) {
			lib_ring_buffer_switch_slow(buf, SWITCH_ACTIVE);
			ret = 0;
		} else if (ret < 0)
			goto err;
		break;
	}
	case RING_BUFFER_GET_SUBBUF:
	{
		/*
		 * Random access is not allowed for metadata channel.
		 */
		return -ENOSYS;
	}
	case RING_BUFFER_FLUSH_EMPTY:	/* Fall-through. */
	case RING_BUFFER_FLUSH:
	{
		struct lttng_metadata_stream *stream = filp->private_data;
		struct lib_ring_buffer *buf = stream->priv;
		struct channel *chan = buf->backend.chan;

		/*
		 * Before doing the actual ring buffer flush, write up to one
		 * packet of metadata in the ring buffer.
		 */
		ret = lttng_metadata_output_channel(stream, chan, NULL);
		if (ret < 0)
			goto err;
		break;
	}
	case RING_BUFFER_GET_METADATA_VERSION:
	{
		struct lttng_metadata_stream *stream = filp->private_data;

		return put_u64(stream->version, arg);
	}
	case RING_BUFFER_METADATA_CACHE_DUMP:
	{
		struct lttng_metadata_stream *stream = filp->private_data;

		return lttng_metadata_cache_dump(stream);
	}
	case RING_BUFFER_GET_NEXT_SUBBUF_METADATA_CHECK:
	{
		struct lttng_metadata_stream *stream = filp->private_data;
		struct lib_ring_buffer *buf = stream->priv;
		struct channel *chan = buf->backend.chan;

		ret = lttng_metadata_output_channel(stream, chan, &coherent);
		if (ret > 0) {
			lib_ring_buffer_switch_slow(buf, SWITCH_ACTIVE);
			ret = 0;
		} else if (ret < 0) {
			goto err;
		}
		break;
	}
	default:
		break;
	}
	/* PUT_SUBBUF is the one from lib ring buffer, unmodified. */

	/* Performing lib ring buffer ioctl after our own. */
	ret = lib_ring_buffer_ioctl(filp, rb_cmd, arg, buf);
	if (ret < 0)
		goto err;

	switch (cmd) {
	case RING_BUFFER_PUT_NEXT_SUBBUF:
	{
		lttng_metadata_ring_buffer_ioctl_put_next_subbuf(filp,
				cmd, arg);
		break;
	}
	case RING_BUFFER_GET_NEXT_SUBBUF_METADATA_CHECK:
	{
		return put_u32(coherent, arg);
	}
	default:
		break;
	}
err:
	return ret;
}

#ifdef CONFIG_COMPAT
static
long lttng_metadata_ring_buffer_compat_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	int ret;
	struct lttng_metadata_stream *stream = filp->private_data;
	struct lib_ring_buffer *buf = stream->priv;
	unsigned int rb_cmd;
	bool coherent;

	if (cmd == RING_BUFFER_GET_NEXT_SUBBUF_METADATA_CHECK)
		rb_cmd = RING_BUFFER_GET_NEXT_SUBBUF;
	else
		rb_cmd = cmd;

	switch (cmd) {
	case RING_BUFFER_GET_NEXT_SUBBUF:
	{
		struct lttng_metadata_stream *stream = filp->private_data;
		struct lib_ring_buffer *buf = stream->priv;
		struct channel *chan = buf->backend.chan;

		ret = lttng_metadata_output_channel(stream, chan, NULL);
		if (ret > 0) {
			lib_ring_buffer_switch_slow(buf, SWITCH_ACTIVE);
			ret = 0;
		} else if (ret < 0)
			goto err;
		break;
	}
	case RING_BUFFER_GET_SUBBUF:
	{
		/*
		 * Random access is not allowed for metadata channel.
		 */
		return -ENOSYS;
	}
	case RING_BUFFER_FLUSH_EMPTY:	/* Fall-through. */
	case RING_BUFFER_FLUSH:
	{
		struct lttng_metadata_stream *stream = filp->private_data;
		struct lib_ring_buffer *buf = stream->priv;
		struct channel *chan = buf->backend.chan;

		/*
		 * Before doing the actual ring buffer flush, write up to one
		 * packet of metadata in the ring buffer.
		 */
		ret = lttng_metadata_output_channel(stream, chan, NULL);
		if (ret < 0)
			goto err;
		break;
	}
	case RING_BUFFER_GET_METADATA_VERSION:
	{
		struct lttng_metadata_stream *stream = filp->private_data;

		return put_u64(stream->version, arg);
	}
	case RING_BUFFER_METADATA_CACHE_DUMP:
	{
		struct lttng_metadata_stream *stream = filp->private_data;

		return lttng_metadata_cache_dump(stream);
	}
	case RING_BUFFER_GET_NEXT_SUBBUF_METADATA_CHECK:
	{
		struct lttng_metadata_stream *stream = filp->private_data;
		struct lib_ring_buffer *buf = stream->priv;
		struct channel *chan = buf->backend.chan;

		ret = lttng_metadata_output_channel(stream, chan, &coherent);
		if (ret > 0) {
			lib_ring_buffer_switch_slow(buf, SWITCH_ACTIVE);
			ret = 0;
		} else if (ret < 0) {
			goto err;
		}
		break;
	}
	default:
		break;
	}
	/* PUT_SUBBUF is the one from lib ring buffer, unmodified. */

	/* Performing lib ring buffer ioctl after our own. */
	ret = lib_ring_buffer_compat_ioctl(filp, rb_cmd, arg, buf);
	if (ret < 0)
		goto err;

	switch (cmd) {
	case RING_BUFFER_PUT_NEXT_SUBBUF:
	{
		lttng_metadata_ring_buffer_ioctl_put_next_subbuf(filp,
				cmd, arg);
		break;
	}
	case RING_BUFFER_GET_NEXT_SUBBUF_METADATA_CHECK:
	{
		return put_u32(coherent, arg);
	}
	default:
		break;
	}
err:
	return ret;
}
#endif

/*
 * This is not used by anonymous file descriptors. This code is left
 * there if we ever want to implement an inode with open() operation.
 */
static
int lttng_metadata_ring_buffer_open(struct inode *inode, struct file *file)
{
	struct lttng_metadata_stream *stream = inode->i_private;
	struct lib_ring_buffer *buf = stream->priv;

	file->private_data = buf;
	/*
	 * Since life-time of metadata cache differs from that of
	 * session, we need to keep our own reference on the transport.
	 */
	if (!try_module_get(stream->transport->owner)) {
		printk(KERN_WARNING "LTT : Can't lock transport module.\n");
		return -EBUSY;
	}
	return lib_ring_buffer_open(inode, file, buf);
}

static
int lttng_metadata_ring_buffer_release(struct inode *inode, struct file *file)
{
	struct lttng_metadata_stream *stream = file->private_data;
	struct lib_ring_buffer *buf = stream->priv;

	mutex_lock(&stream->metadata_cache->lock);
	list_del(&stream->list);
	mutex_unlock(&stream->metadata_cache->lock);
	kref_put(&stream->metadata_cache->refcount, metadata_cache_destroy);
	module_put(stream->transport->owner);
	kfree(stream);
	return lib_ring_buffer_release(inode, file, buf);
}

static
ssize_t lttng_metadata_ring_buffer_splice_read(struct file *in, loff_t *ppos,
		struct pipe_inode_info *pipe, size_t len,
		unsigned int flags)
{
	struct lttng_metadata_stream *stream = in->private_data;
	struct lib_ring_buffer *buf = stream->priv;

	return lib_ring_buffer_splice_read(in, ppos, pipe, len,
			flags, buf);
}

static
int lttng_metadata_ring_buffer_mmap(struct file *filp,
		struct vm_area_struct *vma)
{
	struct lttng_metadata_stream *stream = filp->private_data;
	struct lib_ring_buffer *buf = stream->priv;

	return lib_ring_buffer_mmap(filp, vma, buf);
}

static
const struct file_operations lttng_metadata_ring_buffer_file_operations = {
	.owner = THIS_MODULE,
	.open = lttng_metadata_ring_buffer_open,
	.release = lttng_metadata_ring_buffer_release,
	.poll = lttng_metadata_ring_buffer_poll,
	.splice_read = lttng_metadata_ring_buffer_splice_read,
	.mmap = lttng_metadata_ring_buffer_mmap,
	.unlocked_ioctl = lttng_metadata_ring_buffer_ioctl,
	.llseek = vfs_lib_ring_buffer_no_llseek,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_metadata_ring_buffer_compat_ioctl,
#endif
};

static
int lttng_abi_create_stream_fd(struct file *channel_file, void *stream_priv,
		const struct file_operations *fops, const char *name)
{
	int stream_fd, ret;
	struct file *stream_file;

	stream_fd = lttng_get_unused_fd();
	if (stream_fd < 0) {
		ret = stream_fd;
		goto fd_error;
	}
	stream_file = anon_inode_getfile(name, fops, stream_priv, O_RDWR);
	if (IS_ERR(stream_file)) {
		ret = PTR_ERR(stream_file);
		goto file_error;
	}
	/*
	 * OPEN_FMODE, called within anon_inode_getfile/alloc_file, don't honor
	 * FMODE_LSEEK, FMODE_PREAD nor FMODE_PWRITE. We need to read from this
	 * file descriptor, so we set FMODE_PREAD here.
	 */
	stream_file->f_mode |= FMODE_PREAD;
	fd_install(stream_fd, stream_file);
	/*
	 * The stream holds a reference to the channel within the generic ring
	 * buffer library, so no need to hold a refcount on the channel and
	 * session files here.
	 */
	return stream_fd;

file_error:
	put_unused_fd(stream_fd);
fd_error:
	return ret;
}

static
int lttng_abi_open_stream(struct file *channel_file)
{
	struct lttng_channel *channel = channel_file->private_data;
	struct lib_ring_buffer *buf;
	int ret;
	void *stream_priv;

	buf = channel->ops->buffer_read_open(channel->chan);
	if (!buf)
		return -ENOENT;

	stream_priv = buf;
	ret = lttng_abi_create_stream_fd(channel_file, stream_priv,
			&lttng_stream_ring_buffer_file_operations,
			"[lttng_stream]");
	if (ret < 0)
		goto fd_error;

	return ret;

fd_error:
	channel->ops->buffer_read_close(buf);
	return ret;
}

static
int lttng_abi_open_metadata_stream(struct file *channel_file)
{
	struct lttng_channel *channel = channel_file->private_data;
	struct lttng_session *session = channel->session;
	struct lib_ring_buffer *buf;
	int ret;
	struct lttng_metadata_stream *metadata_stream;
	void *stream_priv;

	buf = channel->ops->buffer_read_open(channel->chan);
	if (!buf)
		return -ENOENT;

	metadata_stream = kzalloc(sizeof(struct lttng_metadata_stream),
			GFP_KERNEL);
	if (!metadata_stream) {
		ret = -ENOMEM;
		goto nomem;
	}
	metadata_stream->metadata_cache = session->metadata_cache;
	init_waitqueue_head(&metadata_stream->read_wait);
	metadata_stream->priv = buf;
	stream_priv = metadata_stream;
	metadata_stream->transport = channel->transport;
	/* Initial state is an empty metadata, considered as incoherent. */
	metadata_stream->coherent = false;

	/*
	 * Since life-time of metadata cache differs from that of
	 * session, we need to keep our own reference on the transport.
	 */
	if (!try_module_get(metadata_stream->transport->owner)) {
		printk(KERN_WARNING "LTT : Can't lock transport module.\n");
		ret = -EINVAL;
		goto notransport;
	}

	if (!lttng_kref_get(&session->metadata_cache->refcount)) {
		ret = -EOVERFLOW;
		goto kref_error;
	}

	ret = lttng_abi_create_stream_fd(channel_file, stream_priv,
			&lttng_metadata_ring_buffer_file_operations,
			"[lttng_metadata_stream]");
	if (ret < 0)
		goto fd_error;

	mutex_lock(&session->metadata_cache->lock);
	list_add(&metadata_stream->list,
		&session->metadata_cache->metadata_stream);
	mutex_unlock(&session->metadata_cache->lock);
	return ret;

fd_error:
	kref_put(&session->metadata_cache->refcount, metadata_cache_destroy);
kref_error:
	module_put(metadata_stream->transport->owner);
notransport:
	kfree(metadata_stream);
nomem:
	channel->ops->buffer_read_close(buf);
	return ret;
}

static
int lttng_abi_open_trigger_group_stream(struct file *notif_file)
{
	struct lttng_trigger_group *trigger_group = notif_file->private_data;
	struct channel *chan = trigger_group->chan;
	struct lib_ring_buffer *buf;
	int ret;
	void *stream_priv;

	buf = trigger_group->ops->buffer_read_open(chan);
	if (!buf)
		return -ENOENT;

	/* The trigger notification fd holds a reference on the trigger group */
	if (!atomic_long_add_unless(&notif_file->f_count, 1, LONG_MAX)) {
		ret = -EOVERFLOW;
		goto refcount_error;
	}
	trigger_group->buf = buf;
	stream_priv = trigger_group;
	ret = lttng_abi_create_stream_fd(notif_file, stream_priv,
			&lttng_trigger_group_notif_fops,
			"[lttng_trigger_stream]");
	if (ret < 0)
		goto fd_error;

	return ret;

fd_error:
	atomic_long_dec(&notif_file->f_count);
refcount_error:
	trigger_group->ops->buffer_read_close(buf);
	return ret;
}

static
int lttng_abi_create_event(struct file *channel_file,
			   struct lttng_kernel_event *event_param)
{
	struct lttng_channel *channel = channel_file->private_data;
	int event_fd, ret;
	struct file *event_file;
	void *priv;

	event_param->name[LTTNG_KERNEL_SYM_NAME_LEN - 1] = '\0';
	switch (event_param->instrumentation) {
	case LTTNG_KERNEL_KRETPROBE:
		event_param->u.kretprobe.symbol_name[LTTNG_KERNEL_SYM_NAME_LEN - 1] = '\0';
		break;
	case LTTNG_KERNEL_KPROBE:
		event_param->u.kprobe.symbol_name[LTTNG_KERNEL_SYM_NAME_LEN - 1] = '\0';
		break;
	case LTTNG_KERNEL_FUNCTION:
		WARN_ON_ONCE(1);
		/* Not implemented. */
		break;
	default:
		break;
	}
	event_fd = lttng_get_unused_fd();
	if (event_fd < 0) {
		ret = event_fd;
		goto fd_error;
	}
	event_file = anon_inode_getfile("[lttng_event]",
					&lttng_event_fops,
					NULL, O_RDWR);
	if (IS_ERR(event_file)) {
		ret = PTR_ERR(event_file);
		goto file_error;
	}
	/* The event holds a reference on the channel */
	if (!atomic_long_add_unless(&channel_file->f_count, 1, LONG_MAX)) {
		ret = -EOVERFLOW;
		goto refcount_error;
	}
	if (event_param->instrumentation == LTTNG_KERNEL_TRACEPOINT
			|| event_param->instrumentation == LTTNG_KERNEL_SYSCALL) {
		struct lttng_event_enabler *event_enabler;

		if (strutils_is_star_glob_pattern(event_param->name)) {
			/*
			 * If the event name is a star globbing pattern,
			 * we create the special star globbing enabler.
			 */
			event_enabler = lttng_event_enabler_create(LTTNG_ENABLER_FORMAT_STAR_GLOB,
				event_param, channel);
		} else {
			event_enabler = lttng_event_enabler_create(LTTNG_ENABLER_FORMAT_NAME,
				event_param, channel);
		}
		priv = event_enabler;
	} else {
		struct lttng_event *event;

		/*
		 * We tolerate no failure path after event creation. It
		 * will stay invariant for the rest of the session.
		 */
		event = lttng_event_create(channel, event_param,
				NULL, NULL,
				event_param->instrumentation);
		WARN_ON_ONCE(!event);
		if (IS_ERR(event)) {
			ret = PTR_ERR(event);
			goto event_error;
		}
		priv = event;
	}
	event_file->private_data = priv;
	fd_install(event_fd, event_file);
	return event_fd;

event_error:
	atomic_long_dec(&channel_file->f_count);
refcount_error:
	fput(event_file);
file_error:
	put_unused_fd(event_fd);
fd_error:
	return ret;
}

static
long lttng_trigger_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lttng_trigger *trigger;
	struct lttng_trigger_enabler *trigger_enabler;
	enum lttng_event_type *evtype = file->private_data;

	switch (cmd) {
	case LTTNG_KERNEL_ENABLE:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			trigger = file->private_data;
			return lttng_trigger_enable(trigger);
		case LTTNG_TYPE_ENABLER:
			trigger_enabler = file->private_data;
			return lttng_trigger_enabler_enable(trigger_enabler);
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	case LTTNG_KERNEL_DISABLE:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			trigger = file->private_data;
			return lttng_trigger_disable(trigger);
		case LTTNG_TYPE_ENABLER:
			trigger_enabler = file->private_data;
			return lttng_trigger_enabler_disable(trigger_enabler);
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	case LTTNG_KERNEL_FILTER:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			return -EINVAL;
		case LTTNG_TYPE_ENABLER:
			trigger_enabler = file->private_data;
			return lttng_trigger_enabler_attach_bytecode(trigger_enabler,
				(struct lttng_kernel_filter_bytecode __user *) arg);
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	case LTTNG_KERNEL_ADD_CALLSITE:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			trigger = file->private_data;
			return lttng_trigger_add_callsite(trigger,
				(struct lttng_kernel_event_callsite __user *) arg);
		case LTTNG_TYPE_ENABLER:
			return -EINVAL;
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	default:
		return -ENOIOCTLCMD;
	}
}

static
int lttng_trigger_release(struct inode *inode, struct file *file)
{
	struct lttng_trigger *trigger;
	struct lttng_trigger_enabler *trigger_enabler;
	enum lttng_event_type *evtype = file->private_data;

	if (!evtype)
		return 0;

	switch (*evtype) {
	case LTTNG_TYPE_EVENT:
		trigger = file->private_data;
		if (trigger)
			fput(trigger->group->file);
		break;
	case LTTNG_TYPE_ENABLER:
		trigger_enabler = file->private_data;
		if (trigger_enabler)
			fput(trigger_enabler->group->file);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}

	return 0;
}

static const struct file_operations lttng_trigger_fops = {
	.owner = THIS_MODULE,
	.release = lttng_trigger_release,
	.unlocked_ioctl = lttng_trigger_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_trigger_ioctl,
#endif
};

static
int lttng_abi_create_trigger(struct file *trigger_group_file,
		struct lttng_kernel_trigger *trigger_param)
{
	struct lttng_trigger_group *trigger_group = trigger_group_file->private_data;
	int trigger_fd, ret;
	struct file *trigger_file;
	void *priv;

	switch (trigger_param->instrumentation) {
	case LTTNG_KERNEL_TRACEPOINT:
	case LTTNG_KERNEL_UPROBE:
		break;
	case LTTNG_KERNEL_KPROBE:
		trigger_param->u.kprobe.symbol_name[LTTNG_KERNEL_SYM_NAME_LEN - 1] = '\0';
		break;
	case LTTNG_KERNEL_KRETPROBE:
		/* Placing a trigger on kretprobe is not supported. */
	case LTTNG_KERNEL_FUNCTION:
	case LTTNG_KERNEL_NOOP:
	case LTTNG_KERNEL_SYSCALL:
	default:
		ret = -EINVAL;
		goto inval_instr;
	}

	trigger_param->name[LTTNG_KERNEL_SYM_NAME_LEN - 1] = '\0';

	trigger_fd = lttng_get_unused_fd();
	if (trigger_fd < 0) {
		ret = trigger_fd;
		goto fd_error;
	}

	trigger_file = anon_inode_getfile("[lttng_trigger]",
					&lttng_trigger_fops,
					NULL, O_RDWR);
	if (IS_ERR(trigger_file)) {
		ret = PTR_ERR(trigger_file);
		goto file_error;
	}

	/* The trigger holds a reference on the trigger group. */
	if (!atomic_long_add_unless(&trigger_group_file->f_count, 1, LONG_MAX)) {
		ret = -EOVERFLOW;
		goto refcount_error;
	}

	if (trigger_param->instrumentation == LTTNG_KERNEL_TRACEPOINT
			|| trigger_param->instrumentation == LTTNG_KERNEL_SYSCALL) {
		struct lttng_trigger_enabler *enabler;

		if (strutils_is_star_glob_pattern(trigger_param->name)) {
			/*
			 * If the event name is a star globbing pattern,
			 * we create the special star globbing enabler.
			 */
			enabler = lttng_trigger_enabler_create(trigger_group,
				LTTNG_ENABLER_FORMAT_STAR_GLOB, trigger_param);
		} else {
			enabler = lttng_trigger_enabler_create(trigger_group,
				LTTNG_ENABLER_FORMAT_NAME, trigger_param);
		}
		priv = enabler;
	} else {
		struct lttng_trigger *trigger;

		/*
		 * We tolerate no failure path after trigger creation. It
		 * will stay invariant for the rest of the session.
		 */
		trigger = lttng_trigger_create(NULL, trigger_param->id,
			trigger_group, trigger_param, NULL,
			trigger_param->instrumentation);
		WARN_ON_ONCE(!trigger);
		if (IS_ERR(trigger)) {
			ret = PTR_ERR(trigger);
			goto trigger_error;
		}
		priv = trigger;
	}
	trigger_file->private_data = priv;
	fd_install(trigger_fd, trigger_file);
	return trigger_fd;

trigger_error:
	atomic_long_dec(&trigger_group_file->f_count);
refcount_error:
	fput(trigger_file);
file_error:
	put_unused_fd(trigger_fd);
fd_error:
inval_instr:
	return ret;
}

static
long lttng_trigger_group_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case LTTNG_KERNEL_TRIGGER_GROUP_NOTIFICATION_FD:
	{
		return lttng_abi_open_trigger_group_stream(file);
	}
	case LTTNG_KERNEL_TRIGGER_CREATE:
	{
		struct lttng_kernel_trigger utrigger_param;

		if (copy_from_user(&utrigger_param,
				(struct lttng_kernel_trigger __user *) arg,
				sizeof(utrigger_param)))
			return -EFAULT;
		return lttng_abi_create_trigger(file, &utrigger_param);
	}
	default:
		return -ENOIOCTLCMD;
	}
	return 0;
}

static
int lttng_trigger_group_release(struct inode *inode, struct file *file)
{
	struct lttng_trigger_group *trigger_group = file->private_data;

	if (trigger_group)
		lttng_trigger_group_destroy(trigger_group);
	return 0;
}

static const struct file_operations lttng_trigger_group_fops = {
	.owner = THIS_MODULE,
	.release = lttng_trigger_group_release,
	.unlocked_ioctl = lttng_trigger_group_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_trigger_group_ioctl,
#endif
};

/**
 *	lttng_channel_ioctl - lttng syscall through ioctl
 *
 *	@file: the file
 *	@cmd: the command
 *	@arg: command arg
 *
 *	This ioctl implements lttng commands:
 *      LTTNG_KERNEL_STREAM
 *              Returns an event stream file descriptor or failure.
 *              (typically, one event stream records events from one CPU)
 *	LTTNG_KERNEL_EVENT
 *		Returns an event file descriptor or failure.
 *	LTTNG_KERNEL_CONTEXT
 *		Prepend a context field to each event in the channel
 *	LTTNG_KERNEL_ENABLE
 *		Enable recording for events in this channel (weak enable)
 *	LTTNG_KERNEL_DISABLE
 *		Disable recording for events in this channel (strong disable)
 *
 * Channel and event file descriptors also hold a reference on the session.
 */
static
long lttng_channel_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lttng_channel *channel = file->private_data;

	switch (cmd) {
	case LTTNG_KERNEL_OLD_STREAM:
	case LTTNG_KERNEL_STREAM:
		return lttng_abi_open_stream(file);
	case LTTNG_KERNEL_OLD_EVENT:
	{
		struct lttng_kernel_event *uevent_param;
		struct lttng_kernel_old_event *old_uevent_param;
		int ret;

		uevent_param = kmalloc(sizeof(struct lttng_kernel_event),
				GFP_KERNEL);
		if (!uevent_param) {
			ret = -ENOMEM;
			goto old_event_end;
		}
		old_uevent_param = kmalloc(
				sizeof(struct lttng_kernel_old_event),
				GFP_KERNEL);
		if (!old_uevent_param) {
			ret = -ENOMEM;
			goto old_event_error_free_param;
		}
		if (copy_from_user(old_uevent_param,
				(struct lttng_kernel_old_event __user *) arg,
				sizeof(struct lttng_kernel_old_event))) {
			ret = -EFAULT;
			goto old_event_error_free_old_param;
		}

		memcpy(uevent_param->name, old_uevent_param->name,
				sizeof(uevent_param->name));
		uevent_param->instrumentation =
			old_uevent_param->instrumentation;

		switch (old_uevent_param->instrumentation) {
		case LTTNG_KERNEL_KPROBE:
			uevent_param->u.kprobe.addr =
				old_uevent_param->u.kprobe.addr;
			uevent_param->u.kprobe.offset =
				old_uevent_param->u.kprobe.offset;
			memcpy(uevent_param->u.kprobe.symbol_name,
				old_uevent_param->u.kprobe.symbol_name,
				sizeof(uevent_param->u.kprobe.symbol_name));
			break;
		case LTTNG_KERNEL_KRETPROBE:
			uevent_param->u.kretprobe.addr =
				old_uevent_param->u.kretprobe.addr;
			uevent_param->u.kretprobe.offset =
				old_uevent_param->u.kretprobe.offset;
			memcpy(uevent_param->u.kretprobe.symbol_name,
				old_uevent_param->u.kretprobe.symbol_name,
				sizeof(uevent_param->u.kretprobe.symbol_name));
			break;
		case LTTNG_KERNEL_FUNCTION:
			WARN_ON_ONCE(1);
			/* Not implemented. */
			break;
		default:
			break;
		}
		ret = lttng_abi_create_event(file, uevent_param);

old_event_error_free_old_param:
		kfree(old_uevent_param);
old_event_error_free_param:
		kfree(uevent_param);
old_event_end:
		return ret;
	}
	case LTTNG_KERNEL_EVENT:
	{
		struct lttng_kernel_event uevent_param;

		if (copy_from_user(&uevent_param,
				(struct lttng_kernel_event __user *) arg,
				sizeof(uevent_param)))
			return -EFAULT;
		return lttng_abi_create_event(file, &uevent_param);
	}
	case LTTNG_KERNEL_OLD_CONTEXT:
	{
		struct lttng_kernel_context *ucontext_param;
		struct lttng_kernel_old_context *old_ucontext_param;
		int ret;

		ucontext_param = kmalloc(sizeof(struct lttng_kernel_context),
				GFP_KERNEL);
		if (!ucontext_param) {
			ret = -ENOMEM;
			goto old_ctx_end;
		}
		old_ucontext_param = kmalloc(sizeof(struct lttng_kernel_old_context),
				GFP_KERNEL);
		if (!old_ucontext_param) {
			ret = -ENOMEM;
			goto old_ctx_error_free_param;
		}

		if (copy_from_user(old_ucontext_param,
				(struct lttng_kernel_old_context __user *) arg,
				sizeof(struct lttng_kernel_old_context))) {
			ret = -EFAULT;
			goto old_ctx_error_free_old_param;
		}
		ucontext_param->ctx = old_ucontext_param->ctx;
		memcpy(ucontext_param->padding, old_ucontext_param->padding,
				sizeof(ucontext_param->padding));
		/* only type that uses the union */
		if (old_ucontext_param->ctx == LTTNG_KERNEL_CONTEXT_PERF_COUNTER) {
			ucontext_param->u.perf_counter.type =
				old_ucontext_param->u.perf_counter.type;
			ucontext_param->u.perf_counter.config =
				old_ucontext_param->u.perf_counter.config;
			memcpy(ucontext_param->u.perf_counter.name,
				old_ucontext_param->u.perf_counter.name,
				sizeof(ucontext_param->u.perf_counter.name));
		}

		ret = lttng_abi_add_context(file,
				ucontext_param,
				&channel->ctx, channel->session);

old_ctx_error_free_old_param:
		kfree(old_ucontext_param);
old_ctx_error_free_param:
		kfree(ucontext_param);
old_ctx_end:
		return ret;
	}
	case LTTNG_KERNEL_CONTEXT:
	{
		struct lttng_kernel_context ucontext_param;

		if (copy_from_user(&ucontext_param,
				(struct lttng_kernel_context __user *) arg,
				sizeof(ucontext_param)))
			return -EFAULT;
		return lttng_abi_add_context(file,
				&ucontext_param,
				&channel->ctx, channel->session);
	}
	case LTTNG_KERNEL_OLD_ENABLE:
	case LTTNG_KERNEL_ENABLE:
		return lttng_channel_enable(channel);
	case LTTNG_KERNEL_OLD_DISABLE:
	case LTTNG_KERNEL_DISABLE:
		return lttng_channel_disable(channel);
	case LTTNG_KERNEL_SYSCALL_MASK:
		return lttng_channel_syscall_mask(channel,
			(struct lttng_kernel_syscall_mask __user *) arg);
	default:
		return -ENOIOCTLCMD;
	}
}

/**
 *	lttng_metadata_ioctl - lttng syscall through ioctl
 *
 *	@file: the file
 *	@cmd: the command
 *	@arg: command arg
 *
 *	This ioctl implements lttng commands:
 *      LTTNG_KERNEL_STREAM
 *              Returns an event stream file descriptor or failure.
 *
 * Channel and event file descriptors also hold a reference on the session.
 */
static
long lttng_metadata_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case LTTNG_KERNEL_OLD_STREAM:
	case LTTNG_KERNEL_STREAM:
		return lttng_abi_open_metadata_stream(file);
	default:
		return -ENOIOCTLCMD;
	}
}

/**
 *	lttng_channel_poll - lttng stream addition/removal monitoring
 *
 *	@file: the file
 *	@wait: poll table
 */
unsigned int lttng_channel_poll(struct file *file, poll_table *wait)
{
	struct lttng_channel *channel = file->private_data;
	unsigned int mask = 0;

	if (file->f_mode & FMODE_READ) {
		poll_wait_set_exclusive(wait);
		poll_wait(file, channel->ops->get_hp_wait_queue(channel->chan),
			  wait);

		if (channel->ops->is_disabled(channel->chan))
			return POLLERR;
		if (channel->ops->is_finalized(channel->chan))
			return POLLHUP;
		if (channel->ops->buffer_has_read_closed_stream(channel->chan))
			return POLLIN | POLLRDNORM;
		return 0;
	}
	return mask;

}

static
int lttng_channel_release(struct inode *inode, struct file *file)
{
	struct lttng_channel *channel = file->private_data;

	if (channel)
		fput(channel->session->file);
	return 0;
}

static
int lttng_metadata_channel_release(struct inode *inode, struct file *file)
{
	struct lttng_channel *channel = file->private_data;

	if (channel) {
		fput(channel->session->file);
		lttng_metadata_channel_destroy(channel);
	}

	return 0;
}

static const struct file_operations lttng_channel_fops = {
	.owner = THIS_MODULE,
	.release = lttng_channel_release,
	.poll = lttng_channel_poll,
	.unlocked_ioctl = lttng_channel_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_channel_ioctl,
#endif
};

static const struct file_operations lttng_metadata_fops = {
	.owner = THIS_MODULE,
	.release = lttng_metadata_channel_release,
	.unlocked_ioctl = lttng_metadata_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_metadata_ioctl,
#endif
};

/**
 *	lttng_event_ioctl - lttng syscall through ioctl
 *
 *	@file: the file
 *	@cmd: the command
 *	@arg: command arg
 *
 *	This ioctl implements lttng commands:
 *	LTTNG_KERNEL_CONTEXT
 *		Prepend a context field to each record of this event
 *	LTTNG_KERNEL_ENABLE
 *		Enable recording for this event (weak enable)
 *	LTTNG_KERNEL_DISABLE
 *		Disable recording for this event (strong disable)
 */
static
long lttng_event_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct lttng_event *event;
	struct lttng_event_enabler *event_enabler;
	enum lttng_event_type *evtype = file->private_data;

	switch (cmd) {
	case LTTNG_KERNEL_OLD_CONTEXT:
	{
		/* Not implemented */
		return -ENOSYS;
	}
	case LTTNG_KERNEL_CONTEXT:
	{
		/* Not implemented */
		return -ENOSYS;
	}
	case LTTNG_KERNEL_OLD_ENABLE:
	case LTTNG_KERNEL_ENABLE:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			event = file->private_data;
			return lttng_event_enable(event);
		case LTTNG_TYPE_ENABLER:
			event_enabler = file->private_data;
			return lttng_event_enabler_enable(event_enabler);
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	case LTTNG_KERNEL_OLD_DISABLE:
	case LTTNG_KERNEL_DISABLE:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			event = file->private_data;
			return lttng_event_disable(event);
		case LTTNG_TYPE_ENABLER:
			event_enabler = file->private_data;
			return lttng_event_enabler_disable(event_enabler);
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	case LTTNG_KERNEL_FILTER:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			return -EINVAL;
		case LTTNG_TYPE_ENABLER:
		{
			event_enabler = file->private_data;
			return lttng_event_enabler_attach_bytecode(event_enabler,
				(struct lttng_kernel_filter_bytecode __user *) arg);
		}
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	case LTTNG_KERNEL_ADD_CALLSITE:
		switch (*evtype) {
		case LTTNG_TYPE_EVENT:
			event = file->private_data;
			return lttng_event_add_callsite(event,
				(struct lttng_kernel_event_callsite __user *) arg);
		case LTTNG_TYPE_ENABLER:
			return -EINVAL;
		default:
			WARN_ON_ONCE(1);
			return -ENOSYS;
		}
	default:
		return -ENOIOCTLCMD;
	}
}

static
int lttng_event_release(struct inode *inode, struct file *file)
{
	struct lttng_event *event;
	struct lttng_event_enabler *event_enabler;
	enum lttng_event_type *evtype = file->private_data;

	if (!evtype)
		return 0;

	switch (*evtype) {
	case LTTNG_TYPE_EVENT:
		event = file->private_data;
		if (event)
			fput(event->chan->file);
		break;
	case LTTNG_TYPE_ENABLER:
		event_enabler = file->private_data;
		if (event_enabler)
			fput(event_enabler->chan->file);
		break;
	default:
		WARN_ON_ONCE(1);
		break;
	}

	return 0;
}

/* TODO: filter control ioctl */
static const struct file_operations lttng_event_fops = {
	.owner = THIS_MODULE,
	.release = lttng_event_release,
	.unlocked_ioctl = lttng_event_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = lttng_event_ioctl,
#endif
};

static int put_u64(uint64_t val, unsigned long arg)
{
	return put_user(val, (uint64_t __user *) arg);
}

static int put_u32(uint32_t val, unsigned long arg)
{
	return put_user(val, (uint32_t __user *) arg);
}

static long lttng_stream_ring_buffer_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	struct lib_ring_buffer *buf = filp->private_data;
	struct channel *chan = buf->backend.chan;
	const struct lib_ring_buffer_config *config = &chan->backend.config;
	const struct lttng_channel_ops *ops = chan->backend.priv_ops;
	int ret;

	if (atomic_read(&chan->record_disabled))
		return -EIO;

	switch (cmd) {
	case LTTNG_RING_BUFFER_GET_TIMESTAMP_BEGIN:
	{
		uint64_t ts;

		ret = ops->timestamp_begin(config, buf, &ts);
		if (ret < 0)
			goto error;
		return put_u64(ts, arg);
	}
	case LTTNG_RING_BUFFER_GET_TIMESTAMP_END:
	{
		uint64_t ts;

		ret = ops->timestamp_end(config, buf, &ts);
		if (ret < 0)
			goto error;
		return put_u64(ts, arg);
	}
	case LTTNG_RING_BUFFER_GET_EVENTS_DISCARDED:
	{
		uint64_t ed;

		ret = ops->events_discarded(config, buf, &ed);
		if (ret < 0)
			goto error;
		return put_u64(ed, arg);
	}
	case LTTNG_RING_BUFFER_GET_CONTENT_SIZE:
	{
		uint64_t cs;

		ret = ops->content_size(config, buf, &cs);
		if (ret < 0)
			goto error;
		return put_u64(cs, arg);
	}
	case LTTNG_RING_BUFFER_GET_PACKET_SIZE:
	{
		uint64_t ps;

		ret = ops->packet_size(config, buf, &ps);
		if (ret < 0)
			goto error;
		return put_u64(ps, arg);
	}
	case LTTNG_RING_BUFFER_GET_STREAM_ID:
	{
		uint64_t si;

		ret = ops->stream_id(config, buf, &si);
		if (ret < 0)
			goto error;
		return put_u64(si, arg);
	}
	case LTTNG_RING_BUFFER_GET_CURRENT_TIMESTAMP:
	{
		uint64_t ts;

		ret = ops->current_timestamp(config, buf, &ts);
		if (ret < 0)
			goto error;
		return put_u64(ts, arg);
	}
	case LTTNG_RING_BUFFER_GET_SEQ_NUM:
	{
		uint64_t seq;

		ret = ops->sequence_number(config, buf, &seq);
		if (ret < 0)
			goto error;
		return put_u64(seq, arg);
	}
	case LTTNG_RING_BUFFER_INSTANCE_ID:
	{
		uint64_t id;

		ret = ops->instance_id(config, buf, &id);
		if (ret < 0)
			goto error;
		return put_u64(id, arg);
	}
	default:
		return lib_ring_buffer_file_operations.unlocked_ioctl(filp,
				cmd, arg);
	}

error:
	return -ENOSYS;
}

#ifdef CONFIG_COMPAT
static long lttng_stream_ring_buffer_compat_ioctl(struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	struct lib_ring_buffer *buf = filp->private_data;
	struct channel *chan = buf->backend.chan;
	const struct lib_ring_buffer_config *config = &chan->backend.config;
	const struct lttng_channel_ops *ops = chan->backend.priv_ops;
	int ret;

	if (atomic_read(&chan->record_disabled))
		return -EIO;

	switch (cmd) {
	case LTTNG_RING_BUFFER_COMPAT_GET_TIMESTAMP_BEGIN:
	{
		uint64_t ts;

		ret = ops->timestamp_begin(config, buf, &ts);
		if (ret < 0)
			goto error;
		return put_u64(ts, arg);
	}
	case LTTNG_RING_BUFFER_COMPAT_GET_TIMESTAMP_END:
	{
		uint64_t ts;

		ret = ops->timestamp_end(config, buf, &ts);
		if (ret < 0)
			goto error;
		return put_u64(ts, arg);
	}
	case LTTNG_RING_BUFFER_COMPAT_GET_EVENTS_DISCARDED:
	{
		uint64_t ed;

		ret = ops->events_discarded(config, buf, &ed);
		if (ret < 0)
			goto error;
		return put_u64(ed, arg);
	}
	case LTTNG_RING_BUFFER_COMPAT_GET_CONTENT_SIZE:
	{
		uint64_t cs;

		ret = ops->content_size(config, buf, &cs);
		if (ret < 0)
			goto error;
		return put_u64(cs, arg);
	}
	case LTTNG_RING_BUFFER_COMPAT_GET_PACKET_SIZE:
	{
		uint64_t ps;

		ret = ops->packet_size(config, buf, &ps);
		if (ret < 0)
			goto error;
		return put_u64(ps, arg);
	}
	case LTTNG_RING_BUFFER_COMPAT_GET_STREAM_ID:
	{
		uint64_t si;

		ret = ops->stream_id(config, buf, &si);
		if (ret < 0)
			goto error;
		return put_u64(si, arg);
	}
	case LTTNG_RING_BUFFER_GET_CURRENT_TIMESTAMP:
	{
		uint64_t ts;

		ret = ops->current_timestamp(config, buf, &ts);
		if (ret < 0)
			goto error;
		return put_u64(ts, arg);
	}
	case LTTNG_RING_BUFFER_COMPAT_GET_SEQ_NUM:
	{
		uint64_t seq;

		ret = ops->sequence_number(config, buf, &seq);
		if (ret < 0)
			goto error;
		return put_u64(seq, arg);
	}
	case LTTNG_RING_BUFFER_COMPAT_INSTANCE_ID:
	{
		uint64_t id;

		ret = ops->instance_id(config, buf, &id);
		if (ret < 0)
			goto error;
		return put_u64(id, arg);
	}
	default:
		return lib_ring_buffer_file_operations.compat_ioctl(filp,
				cmd, arg);
	}

error:
	return -ENOSYS;
}
#endif /* CONFIG_COMPAT */

static void lttng_stream_override_ring_buffer_fops(void)
{
	lttng_stream_ring_buffer_file_operations.owner = THIS_MODULE;
	lttng_stream_ring_buffer_file_operations.open =
		lib_ring_buffer_file_operations.open;
	lttng_stream_ring_buffer_file_operations.release =
		lib_ring_buffer_file_operations.release;
	lttng_stream_ring_buffer_file_operations.poll =
		lib_ring_buffer_file_operations.poll;
	lttng_stream_ring_buffer_file_operations.splice_read =
		lib_ring_buffer_file_operations.splice_read;
	lttng_stream_ring_buffer_file_operations.mmap =
		lib_ring_buffer_file_operations.mmap;
	lttng_stream_ring_buffer_file_operations.unlocked_ioctl =
		lttng_stream_ring_buffer_ioctl;
	lttng_stream_ring_buffer_file_operations.llseek =
		lib_ring_buffer_file_operations.llseek;
#ifdef CONFIG_COMPAT
	lttng_stream_ring_buffer_file_operations.compat_ioctl =
		lttng_stream_ring_buffer_compat_ioctl;
#endif
}

int __init lttng_abi_init(void)
{
	int ret = 0;

	wrapper_vmalloc_sync_mappings();
	lttng_clock_ref();

	ret = lttng_tp_mempool_init();
	if (ret) {
		goto error;
	}

	lttng_proc_dentry = proc_create_data("lttng", S_IRUSR | S_IWUSR, NULL,
					&lttng_proc_ops, NULL);

	if (!lttng_proc_dentry) {
		printk(KERN_ERR "Error creating LTTng control file\n");
		ret = -ENOMEM;
		goto error;
	}
	lttng_stream_override_ring_buffer_fops();
	return 0;

error:
	lttng_tp_mempool_destroy();
	lttng_clock_unref();
	return ret;
}

/* No __exit annotation because used by init error path too. */
void lttng_abi_exit(void)
{
	lttng_tp_mempool_destroy();
	lttng_clock_unref();
	if (lttng_proc_dentry)
		remove_proc_entry("lttng", NULL);
}
