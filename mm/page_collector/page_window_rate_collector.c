// page_window_rate_collector.c
// Windowed page-access collector using kprobes + hrtimer + per-cpu buffers.
// Target: x86_64 kernels (tested conceptually for linux 6.12.x).

#define MODULE_NAME "page_window_rate"

#include "page_collector.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TCC - Windowed Collector");
MODULE_DESCRIPTION(
	"Windowed collector for page-access co-occurrence (mark_page_accessed + handle_mm_fault)");
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

enum window_state { WINDOW_OFF = 0, WINDOW_ON = 1 };
static enum window_state cur_state = WINDOW_OFF;

// workqueue for processing window end
static struct workqueue_struct *pw_wq;
static struct delayed_work process_work;
static unsigned int process_interval_ms = 100;

// timer
static struct hrtimer window_timer;
static ktime_t kt_on;
static ktime_t kt_off;

// /proc
#define PROC_NAME "page_window_rate_pairs"
static struct proc_dir_entry *proc_entry = NULL;

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
	queue_delayed_work(pw_wq, &process_work, msecs_to_jiffies(delay_ms));
}

static void process_work_fn(struct work_struct *work)
{
	process_window_work(work);
	// reschedule periodic processing (keeps sampling up-to-date)
	schedule_processing(process_interval_ms);
}

// ---------------- kprobe handler (hot path) ----------------
static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
	if (cur_state == WINDOW_ON) {
		u64 id = 0;
		if (p && p->symbol_name &&
		    strcmp(p->symbol_name, "mark_page_accessed") == 1) {
			// mark_page_accessed(struct page *page)
			void *page_ptr = (void *)regs->di;
			if (page_ptr) {
				struct page *pg = (struct page *)page_ptr;
				unsigned long pfn = page_to_pfn(pg);
				id = ID_IS_PFN_FLAG | (u64)pfn;
				push_event_to_percpu(id);
			}
		} else if (strcmp(p->symbol_name, "handle_mm_fault") == 0) {
			// handle_mm_fault(struct vm_area_struct *vma, unsigned long address, unsigned int flags)
			unsigned long address = (unsigned long)regs->si;
			u64 page_idx = (u64)(address >> PAGE_SHIFT);
			id = page_idx; // MSB clear
			push_event_to_percpu(id);
		}
	}
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

		// rearm for OFF duration
		hrtimer_forward_now(timer, kt_off);
		return HRTIMER_RESTART;
	}
}

// ---------------- proc read ----------------
static int proc_show(struct seq_file *m, void *v)
{
	struct pair_entry *entry;
	unsigned int bkt;
	unsigned long printed = 0;

	hash_for_each(pair_table, bkt, entry, node) {
		long long c = (long long)atomic64_read(&entry->count);
		seq_printf(m, "%llu,%llu,%lld\n", c,
			   (unsigned long long)entry->key.a,
			   (unsigned long long)entry->key.b);
		printed++;
		if (printed >= max_report)
			break;
	}

	seq_printf(
		m,
		"# params: on_ms=%u off_ms=%u sample_rate=%u buf_len=%u pair_hash_bits=%u max_report=%u\n",
		on_ms, off_ms, sample_rate, buf_len, pair_hash_bits,
		max_report);

	seq_printf(m, "#stats events=%lld dropped=%lld pairs=%lld state=%s\n",
		   (long long)atomic64_read(&stat_events),
		   (long long)atomic64_read(&stat_dropped),
		   (long long)atomic64_read(&stat_pairs),
		   cur_state == WINDOW_ON ? "ON" : "OFF");
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
	// allocate bucket locks using number of buckets
	init_bucket_locks(1u << pair_hash_bits);

	// allocate per-cpu buffers
	ret = alloc_percpu_buffers(buf_len);
	if (ret) {
		pr_err("%s: failed to alloc per-cpu buffers: %d\n", MODULE_NAME,
		       ret);
		goto err_no_bufs;
	}

	// create workqueue and init work
	pw_wq = create_singlethread_workqueue("page_window_rate_wq");
	if (!pw_wq) {
		pr_err("%s: failed to create workqueue\n", MODULE_NAME);
		ret = -ENOMEM;
		goto err_free_bufs;
	}
	INIT_DELAYED_WORK(&process_work, process_work_fn);
	schedule_processing(process_interval_ms);

	// create proc
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("%s: failed to create /proc/%s\n", MODULE_NAME,
			PROC_NAME);
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

err_free_bufs:
	free_percpu_buffers();
err_no_bufs:
	free_bucket_locks();
	return ret;
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
		cancel_delayed_work_sync(&process_work);
		destroy_workqueue(pw_wq);
		pw_wq = NULL;
	}

	// remove proc entry
	if (proc_entry) {
		proc_remove(proc_entry);
		proc_entry = NULL;
	}

	// free pair table
	free_pair_table();

	// free per-cpu buffers
	free_percpu_buffers();

	// free locks
	free_bucket_locks();

	pr_info("%s: module unloaded\n", MODULE_NAME);
}

module_init(pw_init);
module_exit(pw_exit);
