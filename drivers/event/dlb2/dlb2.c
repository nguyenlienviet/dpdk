/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright(c) 2016-2020 Intel Corporation
 */

#include <assert.h>
#include <errno.h>
#include <nmmintrin.h>
#include <pthread.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/fcntl.h>

#include <rte_common.h>
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_debug.h>
#include <rte_dev.h>
#include <rte_errno.h>
#include <rte_eventdev.h>
#include <rte_eventdev_pmd.h>
#include <rte_io.h>
#include <rte_kvargs.h>
#include <rte_log.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_prefetch.h>
#include <rte_ring.h>
#include <rte_string_fns.h>

#include "dlb2_priv.h"
#include "dlb2_iface.h"
#include "dlb2_inline_fns.h"

/*
 * Resources exposed to eventdev. Some values overridden at runtime using
 * values returned by the DLB kernel driver.
 */
#if (RTE_EVENT_MAX_QUEUES_PER_DEV > UINT8_MAX)
#error "RTE_EVENT_MAX_QUEUES_PER_DEV cannot fit in member max_event_queues"
#endif
static struct rte_event_dev_info evdev_dlb2_default_info = {
	.driver_name = "", /* probe will set */
	.min_dequeue_timeout_ns = DLB2_MIN_DEQUEUE_TIMEOUT_NS,
	.max_dequeue_timeout_ns = DLB2_MAX_DEQUEUE_TIMEOUT_NS,
#if (RTE_EVENT_MAX_QUEUES_PER_DEV < DLB2_MAX_NUM_LDB_QUEUES)
	.max_event_queues = RTE_EVENT_MAX_QUEUES_PER_DEV,
#else
	.max_event_queues = DLB2_MAX_NUM_LDB_QUEUES,
#endif
	.max_event_queue_flows = DLB2_MAX_NUM_FLOWS,
	.max_event_queue_priority_levels = DLB2_QID_PRIORITIES,
	.max_event_priority_levels = DLB2_QID_PRIORITIES,
	.max_event_ports = DLB2_MAX_NUM_LDB_PORTS,
	.max_event_port_dequeue_depth = DLB2_MAX_CQ_DEPTH,
	.max_event_port_enqueue_depth = DLB2_MAX_ENQUEUE_DEPTH,
	.max_event_port_links = DLB2_MAX_NUM_QIDS_PER_LDB_CQ,
	.max_num_events = DLB2_MAX_NUM_LDB_CREDITS,
	.max_single_link_event_port_queue_pairs = DLB2_MAX_NUM_DIR_PORTS,
	.event_dev_cap = (RTE_EVENT_DEV_CAP_QUEUE_QOS |
			  RTE_EVENT_DEV_CAP_EVENT_QOS |
			  RTE_EVENT_DEV_CAP_BURST_MODE |
			  RTE_EVENT_DEV_CAP_DISTRIBUTED_SCHED |
			  RTE_EVENT_DEV_CAP_IMPLICIT_RELEASE_DISABLE |
			  RTE_EVENT_DEV_CAP_QUEUE_ALL_TYPES),
};

struct process_local_port_data
dlb2_port[DLB2_MAX_NUM_PORTS][DLB2_NUM_PORT_TYPES];

/* override defaults with value(s) provided on command line */
static void
dlb2_init_queue_depth_thresholds(struct dlb2_eventdev *dlb2,
				 int *qid_depth_thresholds)
{
	int q;

	for (q = 0; q < DLB2_MAX_NUM_QUEUES; q++) {
		if (qid_depth_thresholds[q] != 0)
			dlb2->ev_queues[q].depth_threshold =
				qid_depth_thresholds[q];
	}
}

static int
dlb2_hw_query_resources(struct dlb2_eventdev *dlb2)
{
	struct dlb2_hw_dev *handle = &dlb2->qm_instance;
	struct dlb2_hw_resource_info *dlb2_info = &handle->info;
	int ret;

	/* Query driver resources provisioned for this device */

	ret = dlb2_iface_get_num_resources(handle,
					   &dlb2->hw_rsrc_query_results);
	if (ret) {
		DLB2_LOG_ERR("ioctl get dlb2 num resources, err=%d\n", ret);
		return ret;
	}

	/* Complete filling in device resource info returned to evdev app,
	 * overriding any default values.
	 * The capabilities (CAPs) were set at compile time.
	 */

	evdev_dlb2_default_info.max_event_queues =
		dlb2->hw_rsrc_query_results.num_ldb_queues;

	evdev_dlb2_default_info.max_event_ports =
		dlb2->hw_rsrc_query_results.num_ldb_ports;

	evdev_dlb2_default_info.max_num_events =
		dlb2->hw_rsrc_query_results.num_ldb_credits;

	/* Save off values used when creating the scheduling domain. */

	handle->info.num_sched_domains =
		dlb2->hw_rsrc_query_results.num_sched_domains;

	handle->info.hw_rsrc_max.nb_events_limit =
		dlb2->hw_rsrc_query_results.num_ldb_credits;

	handle->info.hw_rsrc_max.num_queues =
		dlb2->hw_rsrc_query_results.num_ldb_queues +
		dlb2->hw_rsrc_query_results.num_dir_ports;

	handle->info.hw_rsrc_max.num_ldb_queues =
		dlb2->hw_rsrc_query_results.num_ldb_queues;

	handle->info.hw_rsrc_max.num_ldb_ports =
		dlb2->hw_rsrc_query_results.num_ldb_ports;

	handle->info.hw_rsrc_max.num_dir_ports =
		dlb2->hw_rsrc_query_results.num_dir_ports;

	handle->info.hw_rsrc_max.reorder_window_size =
		dlb2->hw_rsrc_query_results.num_hist_list_entries;

	rte_memcpy(dlb2_info, &handle->info.hw_rsrc_max, sizeof(*dlb2_info));

	return 0;
}

#define DLB2_BASE_10 10

static int
dlb2_string_to_int(int *result, const char *str)
{
	long ret;
	char *endptr;

	if (str == NULL || result == NULL)
		return -EINVAL;

	errno = 0;
	ret = strtol(str, &endptr, DLB2_BASE_10);
	if (errno)
		return -errno;

	/* long int and int may be different width for some architectures */
	if (ret < INT_MIN || ret > INT_MAX || endptr == str)
		return -EINVAL;

	*result = ret;
	return 0;
}

static int
set_numa_node(const char *key __rte_unused, const char *value, void *opaque)
{
	int *socket_id = opaque;
	int ret;

	ret = dlb2_string_to_int(socket_id, value);
	if (ret < 0)
		return ret;

	if (*socket_id > RTE_MAX_NUMA_NODES)
		return -EINVAL;
	return 0;
}

static int
set_max_num_events(const char *key __rte_unused,
		   const char *value,
		   void *opaque)
{
	int *max_num_events = opaque;
	int ret;

	if (value == NULL || opaque == NULL) {
		DLB2_LOG_ERR("NULL pointer\n");
		return -EINVAL;
	}

	ret = dlb2_string_to_int(max_num_events, value);
	if (ret < 0)
		return ret;

	if (*max_num_events < 0 || *max_num_events >
			DLB2_MAX_NUM_LDB_CREDITS) {
		DLB2_LOG_ERR("dlb2: max_num_events must be between 0 and %d\n",
			     DLB2_MAX_NUM_LDB_CREDITS);
		return -EINVAL;
	}

	return 0;
}

static int
set_num_dir_credits(const char *key __rte_unused,
		    const char *value,
		    void *opaque)
{
	int *num_dir_credits = opaque;
	int ret;

	if (value == NULL || opaque == NULL) {
		DLB2_LOG_ERR("NULL pointer\n");
		return -EINVAL;
	}

	ret = dlb2_string_to_int(num_dir_credits, value);
	if (ret < 0)
		return ret;

	if (*num_dir_credits < 0 ||
	    *num_dir_credits > DLB2_MAX_NUM_DIR_CREDITS) {
		DLB2_LOG_ERR("dlb2: num_dir_credits must be between 0 and %d\n",
			     DLB2_MAX_NUM_DIR_CREDITS);
		return -EINVAL;
	}

	return 0;
}

static int
set_dev_id(const char *key __rte_unused,
	   const char *value,
	   void *opaque)
{
	int *dev_id = opaque;
	int ret;

	if (value == NULL || opaque == NULL) {
		DLB2_LOG_ERR("NULL pointer\n");
		return -EINVAL;
	}

	ret = dlb2_string_to_int(dev_id, value);
	if (ret < 0)
		return ret;

	return 0;
}

static int
set_cos(const char *key __rte_unused,
	const char *value,
	void *opaque)
{
	enum dlb2_cos *cos_id = opaque;
	int x = 0;
	int ret;

	if (value == NULL || opaque == NULL) {
		DLB2_LOG_ERR("NULL pointer\n");
		return -EINVAL;
	}

	ret = dlb2_string_to_int(&x, value);
	if (ret < 0)
		return ret;

	if (x != DLB2_COS_DEFAULT && (x < DLB2_COS_0 || x > DLB2_COS_3)) {
		DLB2_LOG_ERR(
			"COS %d out of range, must be DLB2_COS_DEFAULT or 0-3\n",
			x);
		return -EINVAL;
	}

	*cos_id = x;

	return 0;
}


static int
set_qid_depth_thresh(const char *key __rte_unused,
		     const char *value,
		     void *opaque)
{
	struct dlb2_qid_depth_thresholds *qid_thresh = opaque;
	int first, last, thresh, i;

	if (value == NULL || opaque == NULL) {
		DLB2_LOG_ERR("NULL pointer\n");
		return -EINVAL;
	}

	/* command line override may take one of the following 3 forms:
	 * qid_depth_thresh=all:<threshold_value> ... all queues
	 * qid_depth_thresh=qidA-qidB:<threshold_value> ... a range of queues
	 * qid_depth_thresh=qid:<threshold_value> ... just one queue
	 */
	if (sscanf(value, "all:%d", &thresh) == 1) {
		first = 0;
		last = DLB2_MAX_NUM_QUEUES - 1;
	} else if (sscanf(value, "%d-%d:%d", &first, &last, &thresh) == 3) {
		/* we have everything we need */
	} else if (sscanf(value, "%d:%d", &first, &thresh) == 2) {
		last = first;
	} else {
		DLB2_LOG_ERR("Error parsing qid depth devarg. Should be all:val, qid-qid:val, or qid:val\n");
		return -EINVAL;
	}

	if (first > last || first < 0 || last >= DLB2_MAX_NUM_QUEUES) {
		DLB2_LOG_ERR("Error parsing qid depth devarg, invalid qid value\n");
		return -EINVAL;
	}

	if (thresh < 0 || thresh > DLB2_MAX_QUEUE_DEPTH_THRESHOLD) {
		DLB2_LOG_ERR("Error parsing qid depth devarg, threshold > %d\n",
			     DLB2_MAX_QUEUE_DEPTH_THRESHOLD);
		return -EINVAL;
	}

	for (i = first; i <= last; i++)
		qid_thresh->val[i] = thresh; /* indexed by qid */

	return 0;
}

static void
dlb2_entry_points_init(struct rte_eventdev *dev)
{
	RTE_SET_USED(dev);

	/* Eventdev PMD entry points */
}

int
dlb2_primary_eventdev_probe(struct rte_eventdev *dev,
			    const char *name,
			    struct dlb2_devargs *dlb2_args)
{
	struct dlb2_eventdev *dlb2;
	int err;

	dlb2 = dev->data->dev_private;

	dlb2->event_dev = dev; /* backlink */

	evdev_dlb2_default_info.driver_name = name;

	dlb2->max_num_events_override = dlb2_args->max_num_events;
	dlb2->num_dir_credits_override = dlb2_args->num_dir_credits_override;
	dlb2->qm_instance.cos_id = dlb2_args->cos_id;

	err = dlb2_iface_open(&dlb2->qm_instance, name);
	if (err < 0) {
		DLB2_LOG_ERR("could not open event hardware device, err=%d\n",
			     err);
		return err;
	}

	err = dlb2_iface_get_device_version(&dlb2->qm_instance,
					    &dlb2->revision);
	if (err < 0) {
		DLB2_LOG_ERR("dlb2: failed to get the device version, err=%d\n",
			     err);
		return err;
	}

	err = dlb2_hw_query_resources(dlb2);
	if (err) {
		DLB2_LOG_ERR("get resources err=%d for %s\n",
			     err, name);
		return err;
	}

	dlb2_iface_hardware_init(&dlb2->qm_instance);

	err = dlb2_iface_get_cq_poll_mode(&dlb2->qm_instance, &dlb2->poll_mode);
	if (err < 0) {
		DLB2_LOG_ERR("dlb2: failed to get the poll mode, err=%d\n",
			     err);
		return err;
	}

	rte_spinlock_init(&dlb2->qm_instance.resource_lock);

	dlb2_iface_low_level_io_init();

	dlb2_entry_points_init(dev);

	dlb2_init_queue_depth_thresholds(dlb2,
					 dlb2_args->qid_depth_thresholds.val);

	return 0;
}

int
dlb2_secondary_eventdev_probe(struct rte_eventdev *dev,
			      const char *name)
{
	struct dlb2_eventdev *dlb2;
	int err;

	dlb2 = dev->data->dev_private;

	evdev_dlb2_default_info.driver_name = name;

	err = dlb2_iface_open(&dlb2->qm_instance, name);
	if (err < 0) {
		DLB2_LOG_ERR("could not open event hardware device, err=%d\n",
			     err);
		return err;
	}

	err = dlb2_hw_query_resources(dlb2);
	if (err) {
		DLB2_LOG_ERR("get resources err=%d for %s\n",
			     err, name);
		return err;
	}

	dlb2_iface_low_level_io_init();

	dlb2_entry_points_init(dev);

	return 0;
}

int
dlb2_parse_params(const char *params,
		  const char *name,
		  struct dlb2_devargs *dlb2_args)
{
	int ret = 0;
	static const char * const args[] = { NUMA_NODE_ARG,
					     DLB2_MAX_NUM_EVENTS,
					     DLB2_NUM_DIR_CREDITS,
					     DEV_ID_ARG,
					     DLB2_QID_DEPTH_THRESH_ARG,
					     DLB2_COS_ARG,
					     NULL };

	if (params != NULL && params[0] != '\0') {
		struct rte_kvargs *kvlist = rte_kvargs_parse(params, args);

		if (kvlist == NULL) {
			RTE_LOG(INFO, PMD,
				"Ignoring unsupported parameters when creating device '%s'\n",
				name);
		} else {
			int ret = rte_kvargs_process(kvlist, NUMA_NODE_ARG,
						     set_numa_node,
						     &dlb2_args->socket_id);
			if (ret != 0) {
				DLB2_LOG_ERR("%s: Error parsing numa node parameter",
					     name);
				rte_kvargs_free(kvlist);
				return ret;
			}

			ret = rte_kvargs_process(kvlist, DLB2_MAX_NUM_EVENTS,
						 set_max_num_events,
						 &dlb2_args->max_num_events);
			if (ret != 0) {
				DLB2_LOG_ERR("%s: Error parsing max_num_events parameter",
					     name);
				rte_kvargs_free(kvlist);
				return ret;
			}

			ret = rte_kvargs_process(kvlist,
					DLB2_NUM_DIR_CREDITS,
					set_num_dir_credits,
					&dlb2_args->num_dir_credits_override);
			if (ret != 0) {
				DLB2_LOG_ERR("%s: Error parsing num_dir_credits parameter",
					     name);
				rte_kvargs_free(kvlist);
				return ret;
			}

			ret = rte_kvargs_process(kvlist, DEV_ID_ARG,
						 set_dev_id,
						 &dlb2_args->dev_id);
			if (ret != 0) {
				DLB2_LOG_ERR("%s: Error parsing dev_id parameter",
					     name);
				rte_kvargs_free(kvlist);
				return ret;
			}

			ret = rte_kvargs_process(
					kvlist,
					DLB2_QID_DEPTH_THRESH_ARG,
					set_qid_depth_thresh,
					&dlb2_args->qid_depth_thresholds);
			if (ret != 0) {
				DLB2_LOG_ERR("%s: Error parsing qid_depth_thresh parameter",
					     name);
				rte_kvargs_free(kvlist);
				return ret;
			}

			ret = rte_kvargs_process(kvlist, DLB2_COS_ARG,
						 set_cos,
						 &dlb2_args->cos_id);
			if (ret != 0) {
				DLB2_LOG_ERR("%s: Error parsing cos parameter",
					     name);
				rte_kvargs_free(kvlist);
				return ret;
			}

			rte_kvargs_free(kvlist);
		}
	}
	return ret;
}
RTE_LOG_REGISTER(eventdev_dlb2_log_level, pmd.event.dlb2, NOTICE);
