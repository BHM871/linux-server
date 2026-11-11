// page_window_rate_collector.c
// Windowed page-access collector using kprobes + hrtimer + per-cpu buffers.
// Target: x86_64 kernels (tested conceptually for linux 6.12.x).

#define MODULE_NAME "page_window_rate"
#define MODULE_EVENTS "/tmp/page_window_rate_events.bin"

#include "page_collector.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TCC - Windowed Collector");
MODULE_DESCRIPTION(
	"Windowed collector for page-access co-occurrence (folio_mark_accessed + handle_mm_fault)");
MODULE_VERSION("0.3");

// ---------------- module parameters ----------------
module_param(on_ms, uint, 0644);
MODULE_PARM_DESC(on_ms, "Collecting window duration in ms (default 10ms)");

module_param(off_ms, uint, 0644);
MODULE_PARM_DESC(off_ms, "Idle window duration in ms (default 90ms)");

module_param(sample_rate, uint, 0644);
MODULE_PARM_DESC(sample_rate,
		 "Sampling rate: keep 1 every N events (default 1)");

module_param(buf_len, uint, 0444);
MODULE_PARM_DESC(buf_len,
		 "Per-CPU buffer length (max events per window). Default 16k");

module_param(segments_percentil, ushort, 0644);
MODULE_PARM_DESC(segments_percentil,
		 "Percentil used to create segments. Default p70");

module_param(max_segment_size, ushort, 0444);
MODULE_PARM_DESC(max_segment_size,
		 "Max of pages that a segment can have. Default 100");

module_param(max_report, uint, 0444);
MODULE_PARM_DESC(max_report, "Max entries printed via /proc");

module_param(active, ushort, 0444);
MODULE_PARM_DESC(
	active,
	"Bit set to define if feature are active (segments_load, segments_count, process_count and events_file. Default (00001111)");

enum window_state { WINDOW_OFF = 0, WINDOW_ON = 1 };
static enum window_state cur_state = WINDOW_OFF;

// timer
static struct hrtimer window_timer;
static ktime_t kt_on;
static ktime_t kt_off;

static atomic64_t folio_mark_hits = ATOMIC_INIT(0);
static atomic64_t handle_fault_hits = ATOMIC_INIT(0);

// /proc
#define PROC_NAME "page_window_rate_pairs"
static struct proc_dir_entry *proc_entry = NULL;

// ---------------- kprobe handler (hot path) ----------------
static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
	if (READ_ONCE(cur_state) != WINDOW_ON)
		return 0;

	u64 id = 0;
	char type = '-';
	if (p == &kp_folio_mark) {
		atomic64_inc(&folio_mark_hits);
		type = ACCESSED;

		struct folio *folio = (struct folio *)regs->di;
		if (folio) {
			unsigned long pfn = folio_pfn(folio);
			id = ID_IS_PFN_FLAG | (u64)pfn;
		}
	} else if (p == &kp_handle_fault) {
		atomic64_inc(&handle_fault_hits);
		type = FAULT;

		unsigned long address = (unsigned long)regs->si;
		id = (u64)(address & PAGE_MASK);
	}

	push_event_to_percpu(id, type);
#endif
	return 0;
}

// ---------------- proc read ----------------
static int proc_show(struct seq_file *m, void *v)
{
	struct recent_buf *rb = this_cpu_ptr(pcpu_bufs);

	seq_printf(
		m,
		"# params: on_ms=%u off_ms=%u sample_rate=%u segments_percentil=%u max_segment_size=%u buf_len=%u hash_bits=%u max_report=%u\n",
		on_ms, off_ms, sample_rate, segments_percentil,
		max_segment_size, buf_len, hash_bits, max_report);

	seq_printf(
		m,
		"# features SEGMENT_LOAD=%d SEGMENT_COUNT=%d PROCESS_COUNT=%d EVENTS_FILE=%d\n",
		is_active(SEGMENT_LOAD), is_active(SEGMENT_COUNT),
		is_active(PROCESS_COUNT), is_active(EVENTS_FILE));

	seq_printf(m, "# mem allocated_bytes=%llu allocated_objects=%llu\n",
		   (unsigned long long)atomic_long_read(&allocated_bytes),
		   (unsigned long long)atomic_long_read(&allocated_count));

	seq_printf(
		m,
		"# stats events=%lld dropped=%lld pairs=%lld state=%d (0=off 1=on 2=lock)\n",
		(long long)atomic64_read(&stat_events),
		(long long)atomic64_read(&stat_dropped),
		(long long)atomic64_read(&stat_pairs), cur_state);

	seq_printf(m, "# this cpu buffer: head=%d count=%d\n", rb->head,
		   rb->count);

	seq_printf(m, "# kprobes: folio_mark=%lld handle_fault=%lld\n",
		   (long long)atomic64_read(&folio_mark_hits),
		   (long long)atomic64_read(&handle_fault_hits));

	seq_printf(m, "\n");

	struct segment_pages *entrys;
	unsigned bkt;
	seq_printf(m, "# segments segments_count=%lld segments_loaded=%lld\n",
		   (long long)atomic64_read(&stat_segments),
		   (long long)atomic64_read(&stat_segments_loaded));
	hash_for_each(segments_table, bkt, entrys, node) {
		seq_printf(m, "pid=%i tgid=%i id=%llu pages=",
			   (unsigned int)entrys->pid,
			   (unsigned int)entrys->tgid,
			   (unsigned long long)entrys->id);

		int idx = (entrys->head - entrys->count + max_segment_size) %
			  max_segment_size;
		for (int i = 0; i < max_segment_size && i < entrys->count;
		     i++) {
			seq_printf(
				m, "%llu;",
				(unsigned long long)entrys
					->pages[(idx + i) % max_segment_size]);
		}

		seq_printf(m, "\n");
	}

	seq_printf(m, "\n");

	struct processor_count *entryp;
	seq_printf(m, "# process\n");
	hash_for_each(processors_table, bkt, entryp, node) {
		seq_printf(m, "pid=%i tgid=%i total=%lld dropped=%lld\n",
			   (unsigned int)entryp->pid,
			   (unsigned int)entryp->tgid,
			   (long long)atomic64_read(&entryp->total),
			   (long long)atomic64_read(&entryp->dropped));
	}

	return 0;
}

// ---------------- module ----------------

// params

/* module: validate features active values */
static void verify_features(void)
{
	if (is_active(SEGMENT_LOAD) && is_not_active(SEGMENT_COUNT)) {
		pr_warn("%s: forcing SEGMENT_COUNT feature because SEGMENT_LOAD is active\n",
			MODULE_NAME);
		active |= SEGMENT_COUNT;
	}
}

/* module: validate params values */
static void verify_params(void)
{
	if (sample_rate <= 0) {
		pr_warn("%s: 'sample_rate' value is invalid, setupping sample_rate=2\n",
			MODULE_NAME);
		sample_rate = 2;
	}

	if (buf_len < 128 || buf_len > 4096) {
		buf_len = max(buf_len, 128);
		buf_len = min(buf_len, 4096);

		pr_warn("%s: 'buf_len' value is invalid, setupping buf_len=%d\n",
			MODULE_NAME, buf_len);
	}

	if (segments_percentil < 10 || segments_percentil > 99) {
		segments_percentil = max(segments_percentil, 10);
		segments_percentil = min(segments_percentil, 99);

		pr_warn("%s: 'segments_percentil' value is invalid, setupping segments_percentil=%d\n",
			MODULE_NAME, segments_percentil);
	}

	if (max_segment_size < 10 || max_segment_size > 2048) {
		max_segment_size = max(max_segment_size, 10);
		max_segment_size = min(max_segment_size, 2048);

		pr_warn("%s: 'max_segment_size' value is invalid, setupping max_segment_size=%d\n",
			MODULE_NAME, max_segment_size);
	}

	if (max_report < 100 || max_report > 30000) {
		max_report = max(max_report, 100);
		max_report = min(max_report, 30000);

		pr_warn("%s: 'max_report' value is invalid, setupping max_report=%d\n",
			MODULE_NAME, max_report);
	}
}

// allocations

/* module: initialize hashtables */
static void init_hashtables(void)
{
	// init hash table with chosen bits
	hash_init(pair_table);
	hash_init(processors_table);
	hash_init(segments_table);
}

/* module: alloc buffers and locks */
static int alloc_dependencies(int *errors)
{
	// allocate bucket locks using number of buckets
	init_bucket_locks(1u << hash_bits);
	if (bucket_lock_count == 1) {
		pr_err("%s: failed to alloc bucket locks: %d\n", MODULE_NAME,
		       -ENOMEM);
		*errors |= ELOCKS;
		return -ENOMEM;
	}

	// allocate per-cpu buffers
	int ret = alloc_percpu_buffers(buf_len);
	if (ret) {
		pr_err("%s: failed to alloc per-cpu buffers: %d\n", MODULE_NAME,
		       ret);
		*errors |= EBUFFERS;
		return ret;
	}

	heap.size = 0;
	heap.capacity = max_report;
	heap.items = (struct heap_pair_entry *)aallocation(
		max_report, sizeof(struct heap_pair_entry));

	if (!heap.items) {
		pr_err("%s: failed to create heap list\n", MODULE_NAME);
		*errors |= EMIN_HEAP;
		return -ENOMEM;
	}

	return 0;
}

/* module: initialize queue and workers */
static int start_workers(int *errors)
{
	// create workqueue and init work
	pw_wq = create_singlethread_workqueue("page_window_rate_wq");
	if (!pw_wq) {
		pr_err("%s: failed to create workqueue\n", MODULE_NAME);
		*errors |= EQUEUE_WORK;
		return -ENOMEM;
	}

	INIT_DELAYED_WORK(&process_work_events, process_events_fn);
	schedule_processing(process_interval_ms);

	INIT_WORK(&process_work_segments, process_segments);

	if (!process_work_events.wq || !process_work_segments.func) {
		pr_err("%s: failed create workers\n", MODULE_NAME);
		*errors |= EWORKERS;
		return -1;
	}

	return 0;
}

// window

/* module: register time to toggle window status */
static enum hrtimer_restart window_timer_cb(struct hrtimer *timer)
{
	// toggle state
	if (cur_state == WINDOW_OFF) {
		// turn ON: start on timer and set state
		cur_state = WINDOW_ON;
		pr_debug("page_window_rate: window ON\n");

		// start timer for ON duration: rearm for ON -> OFF
		hrtimer_forward_now(timer, kt_on);
		return HRTIMER_RESTART;
	} else {
		// turning OFF: schedule processing job and set state
		cur_state = WINDOW_OFF;
		pr_debug("page_window_rate: window OFF\n");

		if (pw_wq)
			queue_work(pw_wq, &process_work_segments);

		// rearm for OFF duration
		hrtimer_forward_now(timer, kt_off);
		return HRTIMER_RESTART;
	}
}

/* module: initialilze window timer */
static int init_window_timer(int *errors)
{
	if (on_ms <= 0 && off_ms <= 0) {
		cur_state = WINDOW_OFF;
		return 0;
	}

	cur_state = WINDOW_ON;
	if (off_ms <= 0)
		return 0;

	kt_on = ms_to_ktime(on_ms);
	kt_off = ms_to_ktime(off_ms);

	hrtimer_setup(&window_timer, window_timer_cb, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	if (!window_timer.function) {
		*errors |= EWINDOW;
		return -1;
	}

	hrtimer_start(&window_timer, kt_on, HRTIMER_MODE_REL);

	return 0;
}

// errors

/* module: handle possible errors and return final result */
static int handle_errors(int errors, int *returns)
{
	if (!errors)
		return 0;

	errors >>= 1;
	// has some error after ELOCK so free locks
	if (errors) {
		free_bucket_locks();
	}

	errors >>= 1;
	// has some error after EBUFFERS so free buffers
	if (errors) {
		free_percpu_buffers();
	}

	errors >>= 1;
	// has some error after EMIN_HEAP so free heap
	if (errors) {
		deallocation(heap.items);
	}

	errors >>= 1;
	// has some error after EQUEUE_WORK so destroy workqueue
	if (errors) {
		destroy_workqueue(pw_wq);
		pw_wq = NULL;
	}

	errors >>= 1;
	// has some error after EWORKERS so cancel workers
	if (errors) {
		cancel_delayed_work_sync(&process_work_events);
		cancel_work_sync(&process_work_segments);
	}

	errors >>= 1;
	// has some error after EKPROBES so cancel timer
	if (errors) {
		hrtimer_cancel(&window_timer);
	}

	unregister_my_kprobes();

	for (int i = 0; i < 7; i++)
		if (returns[i])
			return returns[i];

	return -1;
}

// ---------------- init / exit ----------------
static int __init pw_init(void)
{
	int errors = 0;
	int ret[7];

	pr_info("%s: init (on_ms=%u off_ms=%u sample_rate=%u segments_percentil=%u max_segment_size=%u buf_len=%u hash_bits=%u max_report=%u)\n",
		MODULE_NAME, on_ms, off_ms, sample_rate, segments_percentil,
		max_segment_size, buf_len, hash_bits, max_report);

	verify_features();
	verify_params();

	pr_info("%s: features actived (SEGMENT_LOAD=%d SEGMENT_COUNT=%d PROCESS_COUNT=%d EVENTS_FILE=%d)\n",
		MODULE_NAME, is_active(SEGMENT_LOAD), is_active(SEGMENT_COUNT),
		is_active(PROCESS_COUNT), is_active(EVENTS_FILE));

	init_hashtables();
	ret[0] = alloc_dependencies(&errors);
	ret[1] = start_workers(&errors);

	if (open_events_file() > 0) {
		pr_warn("%s: failed to open events file\n", MODULE_NAME);
	}

	// create proc
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("%s: failed to create /proc/%s\n", MODULE_NAME,
			PROC_NAME);
	}

	ret[2] = register_my_kprobes(&errors);

	ret[3] = init_window_timer(&errors);

	return handle_errors(errors, ret);
}

static void __exit pw_exit(void)
{
	pr_info("%s: unloading module\n", MODULE_NAME);
	pr_info("%s: mem allocated_bytes=%llu allocated_objects=%llu\n",
		MODULE_NAME,
		(unsigned long long)atomic_long_read(&allocated_bytes),
		(unsigned long long)atomic_long_read(&allocated_count));

	// stop timer
	if (window_timer.function)
		hrtimer_cancel(&window_timer);

	// unregister kprobes
	unregister_my_kprobes();

	// cancel work and destroy wq
	if (pw_wq) {
		cancel_delayed_work_sync(&process_work_events);
		cancel_work_sync(&process_work_segments);
		destroy_workqueue(pw_wq);
		pw_wq = NULL;
		close_events_file();
	}

	// remove proc entry
	if (proc_entry) {
		proc_remove(proc_entry);
		proc_entry = NULL;
	}

	deallocation(heap.items);

	// free tables
	free_pair_table();
	free_processors_table();
	free_segments_table();

	// free per-cpu buffers
	free_percpu_buffers();

	// free locks
	free_bucket_locks();

	pr_info("%s: mem freeded allocated_bytes=%llu allocated_objects=%llu\n",
		MODULE_NAME,
		(unsigned long long)atomic_long_read(&allocated_bytes),
		(unsigned long long)atomic_long_read(&allocated_count));
	pr_info("%s: module unloaded\n", MODULE_NAME);
}

module_init(pw_init);
module_exit(pw_exit);
