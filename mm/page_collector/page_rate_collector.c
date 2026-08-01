// page_rate_collector.c
// Kernel module for lightweight observation of page-access sequences.
//
// Tested conceptually for x86_64 kernels (target: 6.12.x).
// WARNING: test in VM before using on real systems.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/uaccess.h>
#include <linux/smp.h>
#include <linux/jiffies.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TCC - Instrumentation");
MODULE_DESCRIPTION(
	"Lightweight page access pair collector (mark_page_accessed + handle_mm_fault)");
MODULE_VERSION("0.1");

// ----------------- module parameters -----------------
static unsigned long delta_ns = 10000000UL; // 10 ms default
module_param(delta_ns, ulong, 0644);
MODULE_PARM_DESC(delta_ns, "Delta time in ns for pairing (default 10ms)");

static unsigned int buf_len = 1024; // per-cpu buffer size (events)
module_param(buf_len, uint, 0644);
MODULE_PARM_DESC(buf_len, "Per-CPU recent events buffer length");

static unsigned int sample_rate = 1; // keep every Nth event (1 = no sampling)
module_param(sample_rate, uint, 0644);
MODULE_PARM_DESC(sample_rate,
		 "Sampling rate: keep 1 every N events (default 1)");

static unsigned int pair_hash_bits = 12; // hashtable size 2^12
module_param(pair_hash_bits, uint, 0644);
MODULE_PARM_DESC(pair_hash_bits, "Hash bits for pair table (2^bits buckets)");

static unsigned int max_report = 10000; // limit for /proc printing
module_param(max_report, uint, 0644);
MODULE_PARM_DESC(max_report, "Max number of pair entries to report from /proc");

// ----------------- constants & helpers -----------------
#define ID_IS_PFN_FLAG (1ULL << 63)

struct page_event {
	u64 ts_ns;
	pid_t pid;
	u64 id; // either (ID_IS_PFN_FLAG | pfn) or (vaddr >> PAGE_SHIFT)
};

struct recent_buf {
	struct page_event *buf; // allocated at init size buf_len
	unsigned int head; // next write index [0..buf_len-1]
	unsigned int
		count; // number of valid items currently in buffer (<= buf_len)
	spinlock_t lock;
};

/* per-cpu recent buffers */
static struct recent_buf __percpu *pcpu_bufs = NULL;

/* workqueue to process events in batch */
static struct workqueue_struct *pm_wq;
static struct delayed_work process_work;
static unsigned int process_interval_ms =
	50; // periodic worker interval fallback

// pair hashtable
struct pair_key {
	u64 a;
	u64 b;
};

struct pair_entry {
	struct hlist_node node;
	struct pair_key key;
	atomic64_t count;
};

static DEFINE_HASHTABLE(pair_table, 12); // initialize later with param
static spinlock_t *bucket_locks = NULL; // array of locks for hash buckets
static unsigned int bucket_lock_count = 256; // will be tuned

// stats
static atomic64_t total_events = ATOMIC64_INIT(0);
static atomic64_t total_pairs = ATOMIC64_INIT(0);
static atomic64_t total_dropped = ATOMIC64_INIT(0);

// proc
#define PROC_NAME "page_rate_pairs"
static struct proc_dir_entry *proc_entry = NULL;

/* helper: simple 64-bit mix hash for two u64 */
static inline u32 pair_hash_u64(u64 a, u64 b)
{
	u64 x = a;
	x = x * 11400714819323198485ULL;
	x ^= (b << 1);
	x = x ^ (x >> 33);
	x = x * 14029467366897019727ULL;
	return (u32)x;
}

/* helper: map hashbucket -> lock index */
static inline unsigned int lock_index_for_bucket(u32 bucket)
{
	return bucket & (bucket_lock_count -
			 1); // bucket_lock_count must be power of two
}

// ----------------- buffer helpers -----------------
static int alloc_percpu_buffers(unsigned int len)
{
	int cpu;
	pcpu_bufs = alloc_percpu(struct recent_buf);
	if (!pcpu_bufs)
		return -ENOMEM;

	for_each_possible_cpu(cpu) {
		struct recent_buf *rb = per_cpu_ptr(pcpu_bufs, cpu);
		rb->buf = kzalloc(sizeof(struct page_event) * len, GFP_KERNEL);
		if (!rb->buf)
			return -ENOMEM;
		rb->head = 0;
		rb->count = 0;
		spin_lock_init(&rb->lock);
	}
	return 0;
}

static void free_percpu_buffers(unsigned int len)
{
	int cpu;
	if (!pcpu_bufs)
		return;
	for_each_possible_cpu(cpu) {
		struct recent_buf *rb = per_cpu_ptr(pcpu_bufs, cpu);
		if (rb && rb->buf) {
			kfree(rb->buf);
			rb->buf = NULL;
		}
	}
	free_percpu(pcpu_bufs);
	pcpu_bufs = NULL;
}

/* push event to per-cpu ring buffer (fast path) */
static void push_event_to_percpu(u64 id)
{
	struct recent_buf *rb = this_cpu_ptr(pcpu_bufs);
	unsigned long flags;

	if (sample_rate > 1) {
		// simple sampling based on total events (not perfect but ok)
		if ((atomic64_read(&total_events) % sample_rate) != 0) {
			atomic64_inc(&total_dropped);
			atomic64_inc(&total_events);
			return;
		}
	}

	spin_lock_irqsave(&rb->lock, flags);

	rb->buf[rb->head].ts_ns = ktime_get_ns();
	rb->buf[rb->head].pid = current->pid;
	rb->buf[rb->head].id = id;

	rb->head = (rb->head + 1) % buf_len;

	if (rb->count < buf_len)
		rb->count++;
	// else overwrite oldest

	spin_unlock_irqrestore(&rb->lock, flags);

	atomic64_inc(&total_events);
}

/* Snapshot copy of a per-cpu buffer into given array (caller must kfree) */
static unsigned int snapshot_percpu_buffer(int cpu, struct page_event *out,
					   unsigned int max_items)
{
	struct recent_buf *rb = per_cpu_ptr(pcpu_bufs, cpu);
	unsigned long flags;
	unsigned int i, idx, cnt;

	spin_lock_irqsave(&rb->lock, flags);
	cnt = rb->count;
	if (cnt > max_items)
		cnt = max_items;

	// buffer contains newest data but head points to next write pos.
	// oldest index = (head - count + buf_len) % buf_len
	idx = (rb->head + buf_len - rb->count) % buf_len;
	for (i = 0; i < cnt; ++i) {
		out[i] = rb->buf[(idx + i) % buf_len];
	}
	spin_unlock_irqrestore(&rb->lock, flags);
	return cnt;
}

// ----------------- hashtable helpers -----------------
static void ensure_bucket_locks(unsigned int buckets)
{
	// choose number of locks as power of two <= buckets, at most 4096
	unsigned int p = 1;
	while (p < 256 && p < buckets)
		p <<= 1;
	bucket_lock_count = p;
	bucket_locks =
		kcalloc(bucket_lock_count, sizeof(spinlock_t), GFP_KERNEL);
	if (!bucket_locks)
		bucket_lock_count = 1;
	else {
		unsigned int i;
		for (i = 0; i < bucket_lock_count; ++i)
			spin_lock_init(&bucket_locks[i]);
	}
}

/* increment pair count (a->b), create entry if needed */
static void pair_table_inc(u64 a, u64 b)
{
	struct pair_entry *entry;
	u32 h = pair_hash_u64(a, b);
	u32 bucket = h & ((1u << pair_hash_bits) - 1);
	unsigned int li = lock_index_for_bucket(bucket);

	spin_lock(&bucket_locks[li]);

	hash_for_each_possible(pair_table, entry, node, h) {
		if (entry->key.a == a && entry->key.b == b) {
			atomic64_inc(&entry->count);
			spin_unlock(&bucket_locks[li]);
			atomic64_inc(&total_pairs);
			return;
		}
	}

	// not found -> create
	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		spin_unlock(&bucket_locks[li]);
		return;
	}
	entry->key.a = a;
	entry->key.b = b;
	atomic64_set(&entry->count, 1);
	hash_add(pair_table, &entry->node, h);
	spin_unlock(&bucket_locks[li]);
	atomic64_inc(&total_pairs);
}

// free hashtable entries
static void free_pair_table(void)
{
	struct pair_entry *entry;
	unsigned bkt;
	// iterate over hashtable and free entries
	hash_for_each(pair_table, bkt, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
}

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

static struct kprobe kp_mark_page = {
	.symbol_name = "mark_page_accessed",
	.pre_handler = kp_pre_handler,
};

static struct kprobe kp_handle_fault = {
	.symbol_name = "handle_mm_fault",
	.pre_handler = kp_pre_handler,
};

// register/unregister kprobes
static int register_my_kprobes(void)
{
	int ret;
	ret = register_kprobe(&kp_mark_page);
	if (ret < 0) {
		pr_warn("page_rate: failed to register kprobe mark_page_accessed: %d\n",
			ret);
		// continue attempt for the other
	}
	ret = register_kprobe(&kp_handle_fault);
	if (ret < 0) {
		pr_warn("page_rate: failed to register kprobe handle_mm_fault: %d\n",
			ret);
		// still ok to run with only one probe
	}
	return 0;
}

static void unregister_my_kprobes(void)
{
	unregister_kprobe(&kp_mark_page);
	unregister_kprobe(&kp_handle_fault);
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
		   (long long)atomic64_read(&total_events),
		   (long long)atomic64_read(&total_pairs),
		   (long long)atomic64_read(&total_dropped));
	return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_show, NULL);
}

static const struct proc_ops proc_fops = {
	.proc_open = proc_open,
	.proc_read = seq_read,
	.proc_release = single_release,
	.proc_lseek = seq_lseek,
};

// ----------------- init/exit -----------------
static int __init pm_init(void)
{
	int ret;
	unsigned int buckets = 1u << pair_hash_bits;

	pr_info("page_rate: init (delta_ns=%lu buf_len=%u sample=%u hash_bits=%u)\n",
		delta_ns, buf_len, sample_rate, pair_hash_bits);

	// sanity checks and limits
	if (buf_len < 16)
		buf_len = 16;
	if (buf_len > 65536)
		buf_len = 65536;

	// init hashtable with param bits
	hash_init(pair_table);
	// NOTE: hash_init ignores bits param here; we'll just trust pair_hash_bits for hashing
	ensure_bucket_locks(buckets);

	// allocate per-cpu buffers
	ret = alloc_percpu_buffers(buf_len);
	if (ret) {
		pr_err("page_rate: failed to alloc per-cpu buffers: %d\n", ret);
		return ret;
	}

	// create workqueue
	pm_wq = create_singlethread_workqueue("page_rate_wq");
	if (!pm_wq) {
		pr_err("page_rate: failed to create workqueue\n");
		free_percpu_buffers(buf_len);
		return -ENOMEM;
	}
	INIT_DELAYED_WORK(&process_work, process_work_fn);
	schedule_processing(process_interval_ms);

	// create proc entry
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("page_rate: failed to create /proc/%s\n", PROC_NAME);
		// not fatal
	}

	// register kprobes
	register_my_kprobes();

	pr_info("page_rate: module loaded\n");
	return 0;
}

static void __exit pm_exit(void)
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
	free_percpu_buffers(buf_len);

	// free locks
	if (bucket_locks) {
		kfree(bucket_locks);
		bucket_locks = NULL;
	}

	pr_info("page_rate: module unloaded\n");
}

module_init(pm_init);
module_exit(pm_exit);
