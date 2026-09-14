// page_rate_collector.c
// Kernel module for lightweight observation of page-access sequences.
//
// Tested conceptually for x86_64 kernels (target: 6.12.x).

#define MODULE_NAME "page_rate"

#include "page_collector.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TCC - Instrumentation");
MODULE_DESCRIPTION(
	"Lightweight page access pair collector (mark_page_accessed + handle_mm_fault)");
MODULE_VERSION("0.1");

// ----------------- module parameters -----------------
module_param(delta_ns, ulong, 0644);
MODULE_PARM_DESC(delta_ns, "Delta time in ns for pairing (default 10ms)");

module_param(buf_len, uint, 0644);
MODULE_PARM_DESC(buf_len, "Per-CPU recent events buffer length");

module_param(sample_rate, uint, 0644);
MODULE_PARM_DESC(sample_rate,
		 "Sampling rate: keep 1 every N events (default 1)");

module_param(pair_hash_bits, uint, 0644);
MODULE_PARM_DESC(pair_hash_bits, "Hash bits for pair table (2^bits buckets)");

module_param(max_report, uint, 0644);
MODULE_PARM_DESC(max_report, "Max number of pair entries to report from /proc");

/* workqueue to process events in batch */
static struct workqueue_struct *pm_wq;
static struct delayed_work process_work;
// periodic worker interval fallback
static unsigned int process_interval_ms = 50;

// proc
#define PROC_NAME "page_rate_pairs"
static struct proc_dir_entry *proc_entry = NULL;

// ----------------- worker: process snapshots and form pairs -----------------
static void process_all_buffers(struct work_struct *work);

static void schedule_processing(unsigned long delay_ms)
{
	if (!pm_wq)
		return;
	queue_delayed_work(pm_wq, &process_work, msecs_to_jiffies(delay_ms));
}

static void process_work_fn(struct work_struct *work)
{
	process_all_buffers(work);
	// reschedule periodic processing (keeps sampling up-to-date)
	schedule_processing(process_interval_ms);
}

static void process_all_buffers(struct work_struct *work)
{
	int cpu;
	struct page_event *snapshot = NULL;
	unsigned int max_items = buf_len;
	unsigned int i, j;

	snapshot =
		kmalloc_array(max_items, sizeof(struct page_event), GFP_KERNEL);
	if (!snapshot)
		return;

	// for each CPU, snapshot buffer and find pairs within delta_ns
	for_each_possible_cpu(cpu) {
		unsigned int cnt =
			snapshot_percpu_buffer(cpu, snapshot, max_items);
		if (cnt == 0)
			continue;

		// For each event i, look ahead j (i < j) until time delta exceeded
		for (i = 0; i < cnt; ++i) {
			u64 ts_i = snapshot[i].ts_ns;
			u64 id_i = snapshot[i].id;
			for (j = i + 1; j < cnt; ++j) {
				u64 ts_j = snapshot[j].ts_ns;
				if (ts_j < ts_i)
					continue;
				if (ts_j - ts_i > delta_ns)
					break;
				u64 id_j = snapshot[j].id;
				// increment pair id_i -> id_j
				pair_table_inc(id_i, id_j);
			}
		}
	}

	kfree(snapshot);
}

// ----------------- kprobe handlers -----------------
static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	u64 id = 0;
	// note: this handler runs on x86_64 assumptions (first arg in regs->di, second in regs->si)
#ifdef CONFIG_X86_64
	if (p->symbol_name &&
	    strcmp(p->symbol_name, "mark_page_accessed") == 0) {
		// mark_page_accessed(struct page *page)
		void *page_ptr = (void *)regs->di;
		if (page_ptr) {
			// page_to_pfn is macro, but we use page_to_pfn(page_ptr)
			// cast: 'struct page *' from void*
			struct page *pg = (struct page *)page_ptr;
			unsigned long pfn = page_to_pfn(pg);
			id = ID_IS_PFN_FLAG | (u64)pfn;
			push_event_to_percpu(id);
		}
	} else if (p->symbol_name &&
		   strcmp(p->symbol_name, "handle_mm_fault") == 0) {
		// handle_mm_fault(struct vm_area_struct *vma, unsigned long address, unsigned int flags)
		unsigned long address = (unsigned long)regs->si;
		// we store page-index of address (vaddr >> PAGE_SHIFT)
		u64 page_idx = (u64)(address >> PAGE_SHIFT);
		id = page_idx; // MSB = 0 indicates vaddr-page-index
		push_event_to_percpu(id);
	}
#endif
	return 0;
}

// ----------------- /proc read -----------------
static int proc_show(struct seq_file *m, void *v)
{
	struct pair_entry *entry;
	unsigned int bkt;
	unsigned long printed = 0;

	// iterate and print lines "idA,idB,count\n"
	hash_for_each(pair_table, bkt, entry, node) {
		long long cnt = (long long)atomic64_read(&entry->count);
		seq_printf(m, "%llu,%llu,%lld\n",
			   (unsigned long long)entry->key.a,
			   (unsigned long long)entry->key.b, cnt);
		printed++;
		if (printed >= max_report)
			break;
	}
	seq_printf(m,
		   "# stats: total_events=%lld total_pairs=%lld dropped=%lld\n",
		   (long long)atomic64_read(&stat_events),
		   (long long)atomic64_read(&stat_pairs),
		   (long long)atomic64_read(&stat_dropped));
	return 0;
}

// ----------------- init/exit -----------------
static int __init pw_init(void)
{
	int ret;
	unsigned int buckets = 1u << pair_hash_bits;

	pr_info("%s: init (delta_ns=%lu buf_len=%u sample=%u hash_bits=%u)\n",
		MODULE_NAME, delta_ns, buf_len, sample_rate, pair_hash_bits);

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
	pm_wq = create_singlethread_workqueue("page_rate_wq");
	if (!pm_wq) {
		pr_err("page_rate: failed to create workqueue\n");
		free_percpu_buffers();
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&process_work, process_work_fn);
	schedule_processing(process_interval_ms);

	// create proc entry
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("%s: failed to create /proc/%s\n", MODULE_NAME,
			PROC_NAME);
		// not fatal
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
	if (pm_wq) {
		cancel_delayed_work_sync(&process_work);
		destroy_workqueue(pm_wq);
		pm_wq = NULL;
	}

	// remove proc
	if (proc_entry) {
		proc_remove(proc_entry);
		proc_entry = NULL;
	}

	// free hashtable
	free_pair_table();

	// free buffers
	free_percpu_buffers();

	// free locks
	free_bucket_locks();

	pr_info("%s: module unloaded\n", MODULE_NAME);
}

module_init(pw_init);
module_exit(pw_exit);
