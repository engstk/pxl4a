/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_MM_EVENT_H
#define _LINUX_MM_EVENT_H

/*
 * These enums are exposed to userspace via the ftrace trace_pipe_raw endpoint
 * and are an ABI. Userspace tools depend on them.
 */
enum mm_event_type {
	MM_MIN_FAULT = 0,
	MM_MAJ_FAULT = 1,
	MM_READ_IO = 2,
	MM_COMPACTION = 3,
	MM_RECLAIM = 4,
	MM_SWP_FAULT = 5,
	MM_KERN_ALLOC = 6,
	BLK_READ_SUBMIT_BIO = 7,
	UFS_READ_QUEUE_CMD = 8,
	UFS_READ_SEND_CMD = 9,
	UFS_READ_COMPL_CMD = 10,
	F2FS_READ_DATA = 11,
	MM_TYPE_NUM = 12,
};

struct mm_event_task {
	unsigned int count;
	unsigned int max_lat;
	u64 accm_lat;
} __attribute__ ((packed));

struct mm_event_vmstat {
	unsigned long free;
	unsigned long file;
	unsigned long anon;
	unsigned long ion;
	unsigned long slab;
	unsigned long ws_refault;
	unsigned long ws_activate;
	unsigned long mapped;
	unsigned long pgin;
	unsigned long pgout;
	unsigned long swpin;
	unsigned long swpout;
	unsigned long reclaim_steal;
	unsigned long reclaim_scan;
	unsigned long compact_scan;
};

#ifdef CONFIG_MM_EVENT_STAT
void mm_event_task_init(struct task_struct *tsk);
void mm_event_record(enum mm_event_type event, unsigned long s_jiffies);
void mm_event_count(enum mm_event_type event, int count);
#else
static inline void mm_event_task_init(struct task_struct *tsk) {}
static inline void mm_event_record(enum mm_event_type event, unsigned long s_jiffies) {}
static inline void mm_event_count(enum mm_event_type event, int count) {}
#endif /* _LINUX_MM_EVENT_H */
#endif
