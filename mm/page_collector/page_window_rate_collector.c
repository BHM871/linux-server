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

// ---------------- timer callback (switch windows) ----------------
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

// ---------------- init / exit ----------------
static int __init pw_init(void)
{
	int ret;

	pr_info("%s: init (on_ms=%u off_ms=%u sample_rate=%u segments_percentil=%u max_segment_size=%u buf_len=%u hash_bits=%u max_report=%u)\n",
		MODULE_NAME, on_ms, off_ms, sample_rate, segments_percentil,
		max_segment_size, buf_len, hash_bits, max_report);
	pr_info("%s: features actived (SEGMENT_LOAD=%d SEGMENT_COUNT=%d PROCESS_COUNT=%d EVENTS_FILE=%d)\n",
		MODULE_NAME, is_active(SEGMENT_LOAD), is_active(SEGMENT_COUNT),
		is_active(PROCESS_COUNT), is_active(EVENTS_FILE));

	if (is_active(SEGMENT_LOAD) && is_not_active(SEGMENT_COUNT)) {
		pr_info("%s: forcing SEGMENT_COUNT feature because SEGMENT_LOAD is enabled\n",
			MODULE_NAME);
		active |= SEGMENT_COUNT;
	}

	if (buf_len < 128)
		buf_len = 128;
	if (buf_len > 131072)
		buf_len = 131072;

	// init hash table with chosen bits
	hash_init(pair_table);

	if (is_active(PROCESS_COUNT))
		hash_init(processors_table);

	if (is_active(SEGMENT_COUNT))
		hash_init(segments_table);

	// allocate bucket locks using number of buckets
	init_bucket_locks(1u << hash_bits);
	if (bucket_lock_count == 1) {
		pr_err("%s: failed to alloc bucket locks\n", MODULE_NAME);
		return -ENOMEM;
	}

	// allocate per-cpu buffers
	ret = alloc_percpu_buffers(buf_len);
	if (ret) {
		pr_err("%s: failed to alloc per-cpu buffers: %d\n", MODULE_NAME,
		       ret);
		free_percpu_buffers();
		free_bucket_locks();
		return ret;
	}

	// create workqueue and init work
	pw_wq = create_singlethread_workqueue("page_window_rate_wq");
	if (!pw_wq) {
		pr_err("%s: failed to create workqueue\n", MODULE_NAME);
		free_percpu_buffers();
		free_bucket_locks();
		return -ENOMEM;
	}
	if (open_events_file()) {
		pr_err("%s: failed to open events file\n", MODULE_NAME);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&process_work_events, process_events_fn);
	INIT_WORK(&process_work_segments, process_segments);
	schedule_processing(process_interval_ms);

	// create proc
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("%s: failed to create /proc/%s\n", MODULE_NAME,
			PROC_NAME);
	}

	heap.size = 0;
	heap.capacity = max_report;
	heap.items = (struct heap_pair_entry *)aallocation(
		max_report, sizeof(struct heap_pair_entry));
	if (!heap.items) {
		pr_err("%s: failed to create heap list\n", MODULE_NAME);
		return -ENOMEM;
	}

	ret = register_my_kprobes();
	if (ret) {
		pr_err("%s: failed to registry kprobes\n", MODULE_NAME);
		return -ENOMEM;
	}

	// init timers
	kt_on = ms_to_ktime(on_ms);
	kt_off = ms_to_ktime(off_ms);

	hrtimer_setup(&window_timer, window_timer_cb, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	// Start with ON state by scheduling timer for ON duration to flip to OFF after ON completes.
	// We'll start timer such that first expiry toggles to OFF and schedules processing.
	cur_state = WINDOW_ON;
	hrtimer_start(&window_timer, kt_on, HRTIMER_MODE_REL);

	pr_info("%s: module loaded\n", MODULE_NAME);
	return 0;
}

static void __exit pw_exit(void)
{
	pr_info("%s: unloading module\n", MODULE_NAME);
	pr_info("%s: mem allocated_bytes=%llu allocated_objects=%llu\n",
		MODULE_NAME,
		(unsigned long long)atomic_long_read(&allocated_bytes),
		(unsigned long long)atomic_long_read(&allocated_count));

	// stop timer
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
