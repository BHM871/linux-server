// page_rate_collector.c
// Kernel module for lightweight observation of page-access sequences.
//
// Tested conceptually for x86_64 kernels (target: 6.12.x).

#define MODULE_NAME "page_rate"
#define MODULE_EVENTS "/tmp/page_rate_events.bin"

#include "page_collector.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TCC - Instrumentation");
MODULE_DESCRIPTION(
	"Lightweight page access pair collector (folio_mark_accessed + handle_mm_fault)");
MODULE_VERSION("0.1");

// ----------------- module parameters -----------------
module_param(delta_ns, ulong, 0644);
MODULE_PARM_DESC(delta_ns, "Delta time in ns for pairing (default 10ms)");

module_param(buf_len, uint, 0644);
MODULE_PARM_DESC(buf_len, "Per-CPU recent events buffer length");

module_param(sample_rate, uint, 0644);
MODULE_PARM_DESC(sample_rate,
		 "Sampling rate: keep 1 every N events (default 1)");

module_param(max_report, uint, 0644);
MODULE_PARM_DESC(max_report, "Max number of pair entries to report from /proc");

// proc
#define PROC_NAME "page_rate_pairs"
static struct proc_dir_entry *proc_entry = NULL;

// ----------------- kprobe handlers -----------------
static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
	u64 id = 0;
	enum event_type type = '-';
	if (p == &kp_folio_mark) {
		type = ACCESSED;
		struct folio *folio = (struct folio *)regs->di;
		if (folio) {
			unsigned long pfn = folio_pfn(folio);
			id = ID_IS_PFN_FLAG | (u64)pfn;
		}
	} else if (p == &kp_handle_fault) {
		type = FAULT;
		unsigned long address = (unsigned long)regs->si;
		id = (u64)(address & PAGE_MASK);
	}
	push_event_to_percpu(id, type);
#endif
	return 0;
}

// ----------------- /proc read -----------------
static int proc_show(struct seq_file *m, void *v)
{
	struct pair_entry entry;

	fill_heap_pair_entry();

	seq_printf(
		m,
		"# params: delta_ns=%ld sample_rate=%u buf_len=%u pair_hash_bits=%u max_report=%u\n",
		delta_ns, sample_rate, buf_len, hash_bits, max_report);

	seq_printf(m,
		   "# stats: total_events=%lld total_pairs=%lld dropped=%lld\n",
		   (long long)atomic64_read(&stat_events),
		   (long long)atomic64_read(&stat_pairs),
		   (long long)atomic64_read(&stat_dropped));

	for (int i = 0; i < max_report; i++) {
		entry = heap.items[i];

		long long cnt = (long long)atomic64_read(&entry.count);
		seq_printf(m, "%lld,%llu,%llu\n", cnt,
			   (unsigned long long)entry.key.a,
			   (unsigned long long)entry.key.b);
	}

	return 0;
}

// ----------------- init/exit -----------------
static int __init pw_init(void)
{
	int ret;
	unsigned int buckets = 1u << hash_bits;

	pr_info("%s: init (delta_ns=%lu buf_len=%u sample=%u hash_bits=%u)\n",
		MODULE_NAME, delta_ns, buf_len, sample_rate, hash_bits);

	// sanity checks and limits
	if (buf_len < 16)
		buf_len = 16;
	if (buf_len > 65536)
		buf_len = 65536;

	// init hashtable with param bits
	hash_init(pair_table);
	init_bucket_locks(buckets);

	// allocate per-cpu buffers
	ret = alloc_percpu_buffers(buf_len);
	if (ret) {
		pr_err("%s: failed to alloc per-cpu buffers: %d\n", MODULE_NAME,
		       ret);
		return ret;
	}

	// create workqueue
	pw_wq = create_singlethread_workqueue("page_rate_wq");
	if (!pw_wq) {
		pr_err("page_rate: failed to create workqueue\n");
		free_percpu_buffers();
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&process_work_events, process_events_fn);
	schedule_processing(process_interval_ms);

	// create proc entry
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("%s: failed to create /proc/%s\n", MODULE_NAME,
			PROC_NAME);
		// not fatal
	} else {
		heap.items = kmalloc_array(
			max_report, sizeof(struct pair_entry), GFP_KERNEL);
		heap.size = 0;
		heap.capacity = max_report;
	}

	// register kprobes
	register_my_kprobes();

	pr_info("%s: module loaded\n", MODULE_NAME);
	return 0;
}

static void __exit pw_exit(void)
{
	// unregister probes
	unregister_my_kprobes();

	// cancel work and destroy wq
	if (pw_wq) {
		cancel_delayed_work_sync(&process_work_events);
		destroy_workqueue(pw_wq);
		pw_wq = NULL;
	}

	// remove proc
	if (proc_entry) {
		proc_remove(proc_entry);
		proc_entry = NULL;
		kfree(heap.items);
	}

	// free hashtable
	free_pair_table();
	free_processors_table();
	free_segments_table();

	// free buffers
	free_percpu_buffers();

	// free locks
	free_bucket_locks();

	pr_info("%s: module unloaded\n", MODULE_NAME);
}

module_init(pw_init);
module_exit(pw_exit);
