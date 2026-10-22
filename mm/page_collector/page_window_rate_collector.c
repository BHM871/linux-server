// page_window_rate_collector.c
// Windowed page-access collector using kprobes + hrtimer + per-cpu buffers.
// Target: x86_64 kernels (tested conceptually for linux 6.12.x).

#define MODULE_NAME "page_window_rate"

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

module_param(buf_len, uint, 0644);
MODULE_PARM_DESC(buf_len,
		 "Per-CPU buffer length (max events per window). Default 16k");

module_param(pair_hash_bits, uint, 0644);
MODULE_PARM_DESC(pair_hash_bits,
		 "Hash bits for pair table (2^bits buckets). Default 12");

module_param(max_report, uint, 0644);
MODULE_PARM_DESC(max_report, "Max entries printed via /proc");

enum window_state { WINDOW_OFF = 0, WINDOW_ON = 1, WINDOW_LOCK = 2 };
static enum window_state cur_state = WINDOW_OFF;

// workqueue for processing window end
static struct file *events_file;
static loff_t events_file_pos;
static struct workqueue_struct *pw_wq;
static struct work_struct process_work;
static unsigned int process_interval_ms = 100;

// timer
static struct hrtimer window_timer;
static ktime_t kt_on;
static ktime_t kt_off;

static atomic64_t folio_mark_hits = ATOMIC_INIT(0);
static atomic64_t handle_fault_hits = ATOMIC_INIT(0);

// /proc
#define PROC_NAME "page_window_rate_pairs"
static struct proc_dir_entry *proc_entry = NULL;

// #  Events

// ---------------- push event ----------------
static void push_event_if_possible(u64 id, char type)
{
	struct recent_buf *rb = this_cpu_ptr(pcpu_bufs);

	if (rb->head == 0 && rb->count != 0) {
		cur_state = WINDOW_LOCK;

		// schedule processing on workqueue
		if (pw_wq)
			queue_work(pw_wq, &process_work);
	}

	push_event_to_percpu(id, type);
}

// ---------------- file helper ----------------

static int open_events_file(void)
{
	events_file = filp_open("/tmp/page_window_rate_events.bin",
				O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (IS_ERR(events_file)) {
		int ret = PTR_ERR(events_file);
		events_file = NULL;
		return ret;
	}

	events_file_pos = 0;
	return 0;
}

static void write_events(struct work_struct *work)
{
	if (!events_file) {
		return;
	}

	int cpu;
	unsigned int max_items = buf_len;
	struct page_event *snap = NULL;

	// allocate snapshot buffer once (per CPU we reuse)
	snap = kmalloc_array(max_items, sizeof(struct page_event), GFP_KERNEL);
	if (!snap) {
		pr_warn("%s: cannot allocate snapshot buffer\n", MODULE_NAME);
		return;
	}

	for_each_possible_cpu(cpu) {
		unsigned int cnt = snapshot_percpu_buffer(cpu, snap, max_items);
		if (cnt == 0)
			continue;

		kernel_write(events_file, snap, cnt * sizeof(struct page_event),
			     &events_file_pos);
	}

	kfree(snap);

	cur_state = WINDOW_ON;
}

static void close_events_file(void)
{
	if (events_file) {
		filp_close(events_file, NULL);
		events_file = NULL;
	}
}

/* ------------- UNUSED EXCERPT START -------------
// ---------------- processing: consume snapshots and compute pairs ----------------
static void process_window_work(struct work_struct *work)
{
	int cpu;
	unsigned int max_items = buf_len;
	struct page_event *snap = NULL;

	// allocate snapshot buffer once (per CPU we reuse)
	snap = kmalloc_array(max_items, sizeof(struct page_event), GFP_KERNEL);
	if (!snap) {
		pr_warn("%s: cannot allocate snapshot buffer\n", MODULE_NAME);
		return;
	}

	// For each CPU, copy its buffer and then for each pair (i<j) within the snapshot,
	// if timestamps within window (they should already be), increment pair.
	unsigned int i, j;
	for_each_possible_cpu(cpu) {
		unsigned int cnt = snapshot_percpu_buffer(cpu, snap, max_items);
		if (cnt == 0)
			continue;

		// nested loops: for each i, j>i, increment pair if within on_ms (they are from same window)
		// We assume these events are all from the same window, so we pair all i<j
		for (i = 0; i < cnt; ++i) {
			u64 id_i = snap[i].id;
			for (j = i + 1; j < cnt; ++j) {
				u64 id_j = snap[j].id;
				pair_table_inc(id_i, id_j);
			}
		}
	}

	kfree(snap);

	pr_debug("%s: processed window (pairs total ~ %lld)\n", MODULE_NAME,
		 (long long)atomic64_read(&stat_pairs));
}

static void schedule_processing(unsigned long delay_ms)
{
	if (!pw_wq)
		return;
	queue_work(pw_wq, &process_work);
}

static void process_work_fn(struct work_struct *work)
{
	process_window_work(work);
	// reschedule periodic processing (keeps sampling up-to-date)
	schedule_processing(process_interval_ms);
}
// ------------- UNUSED EXCERPT END -------------*/

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
		type = 'F';

		struct folio *folio = (struct folio *)regs->di;
		if (folio) {
			unsigned long pfn = folio_pfn(folio);
			id = ID_IS_PFN_FLAG | (u64)pfn;
		}
	} else if (p == &kp_handle_fault) {
		atomic64_inc(&handle_fault_hits);
		type = 'H';

		unsigned long address = (unsigned long)regs->si;
		id = (u64)(address & PAGE_MASK);
	}

	push_event_if_possible(id, type);
#endif
	return 0;
}

// ---------------- timer callback (switch windows) ----------------
static enum hrtimer_restart window_timer_cb(struct hrtimer *timer)
{
	if (cur_state == WINDOW_LOCK)
		return HRTIMER_RESTART;

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
		"# params: on_ms=%u off_ms=%u sample_rate=%u buf_len=%u pair_hash_bits=%u max_report=%u\n",
		on_ms, off_ms, sample_rate, buf_len, pair_hash_bits,
		max_report);

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

	struct processor_count *entry;
	unsigned bkt;
	hash_for_each(processors_table, bkt, entry, node) {
		seq_printf(m, "pid=%i total=%lld dropped=%lld\n",
			   (unsigned int)entry->pid,
			   (long long)atomic64_read(&entry->total),
			   (long long)atomic64_read(&entry->dropped));
	}

	return 0;
}

// ---------------- init / exit ----------------
static int __init pw_init(void)
{
	int ret;

	pr_info("%s: init (on_ms=%u off_ms=%u sample_rate=%u buf_len=%u hash_bits=%u)\n",
		MODULE_NAME, on_ms, off_ms, sample_rate, buf_len,
		pair_hash_bits);

	if (buf_len < 128)
		buf_len = 128;
	if (buf_len > 131072)
		buf_len = 131072;

	// init hash table with chosen bits
	hash_init(pair_table);
	hash_init(processors_table);

	// allocate bucket locks using number of buckets
	init_bucket_locks(1u << pair_hash_bits);

	// allocate per-cpu buffers
	ret = alloc_percpu_buffers(buf_len);
	if (ret) {
		pr_err("%s: failed to alloc per-cpu buffers: %d\n", MODULE_NAME,
		       ret);
		free_bucket_locks();
		return ret;
	}

	// create workqueue and init work
	pw_wq = create_singlethread_workqueue("page_window_rate_wq");
	if (!pw_wq) {
		pr_err("%s: failed to create workqueue\n", MODULE_NAME);
		ret = -ENOMEM;
		free_percpu_buffers();
	}
	if (pw_wq && open_events_file()) {
		pr_err("%s: failed to open events file\n", MODULE_NAME);
		ret = -ENOMEM;
	}
	INIT_WORK(&process_work, write_events);
	// schedule_processing(process_interval_ms);

	// create proc
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("%s: failed to create /proc/%s\n", MODULE_NAME,
			PROC_NAME);
	} else {
		heap.items = kmalloc_array(
			max_report, sizeof(struct pair_entry), GFP_KERNEL);
		heap.size = 0;
		heap.capacity = max_report;
	}

	register_my_kprobes();

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

	// stop timer
	hrtimer_cancel(&window_timer);

	// unregister kprobes
	unregister_my_kprobes();

	// cancel work and destroy wq
	if (pw_wq) {
		cancel_work_sync(&process_work);
		destroy_workqueue(pw_wq);
		pw_wq = NULL;
		close_events_file();
	}

	// remove proc entry
	if (proc_entry) {
		proc_remove(proc_entry);
		proc_entry = NULL;
		kfree(heap.items);
	}

	// free pair table
	free_pair_table();
	free_processors_table();

	// free per-cpu buffers
	free_percpu_buffers();

	// free locks
	free_bucket_locks();

	pr_info("%s: module unloaded\n", MODULE_NAME);
}

module_init(pw_init);
module_exit(pw_exit);
