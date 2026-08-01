// page_window_collector.c
// Windowed page-access collector using kprobes + hrtimer + per-cpu buffers.
// Target: x86_64 kernels (tested conceptually for linux 6.12.x).
//
// WARNING: test in VM. This module instruments kernel functions and allocates
// kernel memory. Use with care.

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kprobes.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>
#include <linux/hashtable.h>
#include <linux/spinlock.h>
#include <linux/percpu.h>
#include <linux/workqueue.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/slab.h>
#include <linux/atomic.h>
#include <linux/smp.h>
#include <linux/jiffies.h>
#include <linux/page-flags.h>
#include <linux/mm.h>
#include <linux/uaccess.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TCC - Windowed Collector");
MODULE_DESCRIPTION(
	"Windowed collector for page-access co-occurrence (mark_page_accessed + handle_mm_fault)");
MODULE_VERSION("0.2");

// ---------------- module parameters ----------------
static unsigned int on_ms = 10; // collect window in ms
module_param(on_ms, uint, 0644);
MODULE_PARM_DESC(on_ms, "Collecting window duration in ms (default 10ms)");

static unsigned int off_ms = 90; // idle window in ms
module_param(off_ms, uint, 0644);
MODULE_PARM_DESC(off_ms, "Idle window duration in ms (default 90ms)");

static unsigned int buf_len = 16384; // per-cpu buffer size (events)
module_param(buf_len, uint, 0644);
MODULE_PARM_DESC(buf_len,
		 "Per-CPU buffer length (max events per window). Default 16k");

static unsigned int pair_hash_bits = 12; // hashtable size 2^bits
module_param(pair_hash_bits, uint, 0644);
MODULE_PARM_DESC(pair_hash_bits,
		 "Hash bits for pair table (2^bits buckets). Default 12");

static unsigned int max_report = 20000;
module_param(max_report, uint, 0644);
MODULE_PARM_DESC(max_report, "Max entries printed via /proc");

// ---------------- constants ----------------
#define ID_IS_PFN_FLAG (1ULL << 63)

// ---------------- data structures ----------------
struct page_event {
	u64 ts_ns;
	pid_t pid;
	u64 id; // ID_IS_PFN_FLAG | pfn  OR  page_index (vaddr >> PAGE_SHIFT)
};

struct recent_buf {
	struct page_event *buf; // allocated size buf_len
	unsigned int head; // next write index
	unsigned int count; // number valid (<= buf_len)
	spinlock_t lock; // protects head/count and writes
};

static struct recent_buf __percpu *pcpu_bufs = NULL;

enum window_state { WINDOW_OFF = 0, WINDOW_ON = 1 };
static enum window_state cur_state = WINDOW_OFF;

// stats
static atomic64_t stat_events = ATOMIC64_INIT(0);
static atomic64_t stat_dropped = ATOMIC64_INIT(0);
static atomic64_t stat_pairs = ATOMIC64_INIT(0);

// pair table
struct pair_key {
	u64 a;
	u64 b;
};

struct pair_entry {
	struct hlist_node node;
	struct pair_key key;
	atomic64_t count;
};

static DEFINE_HASHTABLE(pair_table, 12); // will use pair_hash_bits in code
static spinlock_t *bucket_locks = NULL;
static unsigned int bucket_lock_count = 256;

// workqueue for processing window end
static struct workqueue_struct *pw_wq;
static struct work_struct process_work;

// timer
static struct hrtimer window_timer;
static ktime_t kt_on;
static ktime_t kt_off;

// /proc
#define PROC_NAME "page_window_pairs"
static struct proc_dir_entry *proc_entry = NULL;

// ---------------- helpers ----------------
static inline u32 mix64(u64 x)
{
	x = (~x) + (x << 21);
	x = x ^ (x >> 24);
	x = (x + (x << 3)) + (x << 8);
	x = x ^ (x >> 14);
	x = (x + (x << 2)) + (x << 4);
	x = x ^ (x >> 28);
	x = x + (x << 31);
	return (u32)x;
}

static inline u32 pair_hash_u64(u64 a, u64 b)
{
	return mix64(a ^ (b << 1));
}

static inline unsigned int lock_index_for_bucket(u32 bucket)
{
	return bucket & (bucket_lock_count - 1);
}

// ---------------- per-cpu buffer allocation ----------------
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

static void free_percpu_buffers(void)
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

// ---------------- bucket locks ----------------
static void init_bucket_locks(unsigned int buckets)
{
	unsigned int p = 1;
	while (p < 256 && p < buckets)
		p <<= 1;
	bucket_lock_count = p;
	bucket_locks =
		kcalloc(bucket_lock_count, sizeof(spinlock_t), GFP_KERNEL);
	if (!bucket_locks) {
		bucket_lock_count = 1;
		pr_warn("page_window: could not allocate bucket locks, using 1\n");
		return;
	}
	for (unsigned int i = 0; i < bucket_lock_count; ++i)
		spin_lock_init(&bucket_locks[i]);
}

static void free_bucket_locks(void)
{
	if (bucket_locks) {
		kfree(bucket_locks);
		bucket_locks = NULL;
	}
}

// ---------------- push event (hot path) ----------------
static void push_event_to_percpu(u64 id)
{
	struct recent_buf *rb = this_cpu_ptr(pcpu_bufs);
	unsigned long flags;

	// Very small fast critical section
	spin_lock_irqsave(&rb->lock, flags);

	if (rb->count < buf_len) {
		rb->buf[rb->head].ts_ns = ktime_get_ns();
		rb->buf[rb->head].pid = current->pid;
		rb->buf[rb->head].id = id;
		rb->head = (rb->head + 1) % buf_len;
		rb->count++;
	} else {
		// buffer full: drop (we keep oldest by overwriting; here we choose drop to avoid advancing head)
		// Alternative: advance head and overwrite oldest. Simpler: overwrite oldest
		rb->buf[rb->head].ts_ns = ktime_get_ns();
		rb->buf[rb->head].pid = current->pid;
		rb->buf[rb->head].id = id;
		rb->head = (rb->head + 1) % buf_len;
		// count stays buf_len
	}

	spin_unlock_irqrestore(&rb->lock, flags);

	atomic64_inc(&stat_events);
}

// ---------------- snapshot helper ----------------
/*
 * Copy up to max_items from per-cpu buffer into out_buf.
 * Returns number of items copied.
 */
static unsigned int snapshot_percpu_to_array(int cpu,
					     struct page_event *out_buf,
					     unsigned int max_items)
{
	struct recent_buf *rb = per_cpu_ptr(pcpu_bufs, cpu);
	unsigned long flags;
	unsigned int cnt, start_idx, i;

	spin_lock_irqsave(&rb->lock, flags);
	cnt = rb->count;
	if (cnt > max_items)
		cnt = max_items;
	// oldest idx:
	start_idx = (rb->head + buf_len - rb->count) % buf_len;
	for (i = 0; i < cnt; ++i)
		out_buf[i] = rb->buf[(start_idx + i) % buf_len];
	spin_unlock_irqrestore(&rb->lock, flags);
	return cnt;
}

// ---------------- pair table manipulation ----------------
static void pair_table_inc(u64 a, u64 b)
{
	u32 h32 = pair_hash_u64(a, b);
	unsigned int buckets = 1u << pair_hash_bits;
	u32 bucket = h32 & (buckets - 1);
	unsigned int li = lock_index_for_bucket(bucket);
	struct pair_entry *entry;
	bool found = false;

	if (!bucket_locks)
		return;

	spin_lock(&bucket_locks[li]);
	hash_for_each_possible(pair_table, entry, node, (u32)h32) {
		if (entry->key.a == a && entry->key.b == b) {
			atomic64_inc(&entry->count);
			found = true;
			break;
		}
	}
	if (!found) {
		entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
		if (entry) {
			entry->key.a = a;
			entry->key.b = b;
			atomic64_set(&entry->count, 1);
			hash_add(pair_table, &entry->node, (u32)h32);
		} else {
			// allocation fail -> drop silently
		}
	}
	spin_unlock(&bucket_locks[li]);
	atomic64_inc(&stat_pairs);
}

static void free_pair_table(void)
{
	struct pair_entry *entry;
	unsigned bkt;
	hash_for_each(pair_table, bkt, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
}

// ---------------- processing: consume snapshots and compute pairs ----------------
static void process_window_work(struct work_struct *work)
{
	int cpu;
	unsigned int max_items = buf_len;
	struct page_event *snap = NULL;

	// allocate snapshot buffer once (per CPU we reuse)
	snap = kmalloc_array(max_items, sizeof(struct page_event), GFP_KERNEL);
	if (!snap) {
		pr_warn("page_window: cannot allocate snapshot buffer\n");
		return;
	}

	// For each CPU, copy its buffer and then for each pair (i<j) within the snapshot,
	// if timestamps within window (they should already be), increment pair.
	for_each_possible_cpu(cpu) {
		unsigned int cnt =
			snapshot_percpu_to_array(cpu, snap, max_items);
		if (cnt == 0)
			continue;

		// nested loops: for each i, j>i, increment pair if within on_ms (they are from same window)
		// We assume these events are all from the same window, so we pair all i<j
		unsigned int i, j;
		for (i = 0; i < cnt; ++i) {
			u64 id_i = snap[i].id;
			for (j = i + 1; j < cnt; ++j) {
				u64 id_j = snap[j].id;
				pair_table_inc(id_i, id_j);
			}
		}
	}

	kfree(snap);

	pr_debug("page_window: processed window (pairs total ~ %lld)\n",
		 (long long)atomic64_read(&stat_pairs));
}

// ---------------- kprobe handler (hot path) ----------------
static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
#ifdef CONFIG_X86_64
	if (cur_state == WINDOW_ON) {
		u64 id = 0;
		if (p && p->symbol_name) {
			if (strcmp(p->symbol_name, "mark_page_accessed") == 0) {
				// mark_page_accessed(struct page *page)
				void *page_ptr = (void *)regs->di;
				if (page_ptr) {
					struct page *pg =
						(struct page *)page_ptr;
					unsigned long pfn = page_to_pfn(pg);
					id = ID_IS_PFN_FLAG | (u64)pfn;
					push_event_to_percpu(id);
				}
			} else if (strcmp(p->symbol_name, "handle_mm_fault") ==
				   0) {
				// handle_mm_fault(struct vm_area_struct *vma, unsigned long address, unsigned int flags)
				unsigned long address = (unsigned long)regs->si;
				u64 page_idx = (u64)(address >> PAGE_SHIFT);
				id = page_idx; // MSB clear
				push_event_to_percpu(id);
			}
		}
	}
#endif
	return 0;
}

// register/unregister kprobes
static struct kprobe kp_mark_page = {
	.symbol_name = "mark_page_accessed",
	.pre_handler = kp_pre_handler,
};

static struct kprobe kp_handle_fault = {
	.symbol_name = "handle_mm_fault",
	.pre_handler = kp_pre_handler,
};

static int register_my_kprobes(void)
{
	int ret;
	ret = register_kprobe(&kp_mark_page);
	if (ret < 0) {
		pr_warn("page_map: failed to register kprobe mark_page_accessed: %d\n",
			ret);
		// continue attempt for the other
	}
	ret = register_kprobe(&kp_handle_fault);
	if (ret < 0) {
		pr_warn("page_map: failed to register kprobe handle_mm_fault: %d\n",
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

// ---------------- timer callback (switch windows) ----------------
static enum hrtimer_restart window_timer_cb(struct hrtimer *timer)
{
	// toggle state
	if (cur_state == WINDOW_OFF) {
		// turn ON: start on timer and set state
		cur_state = WINDOW_ON;
		pr_debug("page_window: window ON\n");
		// start timer for ON duration: rearm for ON -> OFF
		hrtimer_forward_now(timer, kt_on);
		return HRTIMER_RESTART;
	} else {
		// turning OFF: schedule processing job and set state
		cur_state = WINDOW_OFF;
		pr_debug("page_window: window OFF -> scheduling processing\n");

		// schedule processing on workqueue
		if (pw_wq)
			queue_work(pw_wq, &process_work);

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
		seq_printf(m, "%llu,%llu,%lld\n",
			   (unsigned long long)entry->key.a,
			   (unsigned long long)entry->key.b, c);
		printed++;
		if (printed >= max_report)
			break;
	}

	seq_printf(m, "#stats events=%lld dropped=%lld pairs=%lld state=%s\n",
		   (long long)atomic64_read(&stat_events),
		   (long long)atomic64_read(&stat_dropped),
		   (long long)atomic64_read(&stat_pairs),
		   cur_state == WINDOW_ON ? "ON" : "OFF");
	return 0;
}

static int proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_show, NULL);
}

static const struct proc_ops proc_fops = {
	.proc_open = proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

// ---------------- init / exit ----------------
static int __init pw_init(void)
{
	int ret;

	pr_info("page_window: init (on_ms=%u off_ms=%u buf_len=%u hash_bits=%u)\n",
		on_ms, off_ms, buf_len, pair_hash_bits);

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
		pr_err("page_window: failed to alloc per-cpu buffers: %d\n",
		       ret);
		goto err_no_bufs;
	}

	// create workqueue and init work
	pw_wq = create_singlethread_workqueue("page_window_wq");
	if (!pw_wq) {
		pr_err("page_window: failed to create workqueue\n");
		ret = -ENOMEM;
		goto err_free_bufs;
	}
	INIT_WORK(&process_work, process_window_work);

	// create proc
	proc_entry = proc_create(PROC_NAME, 0444, NULL, &proc_fops);
	if (!proc_entry) {
		pr_warn("page_window: failed to create /proc/%s\n", PROC_NAME);
	}

	register_my_kprobes();

	// init timers
	kt_on = ms_to_ktime(on_ms);
	kt_off = ms_to_ktime(off_ms);

	// But MS_TO_NS macro not included? Use multiplies directly:
	kt_on = ktime_set(0, (s64)on_ms * 1000000LL);
	kt_off = ktime_set(0, (s64)off_ms * 1000000LL);

	hrtimer_start(&window_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	window_timer.function = window_timer_cb;

	// Start with ON state by scheduling timer for ON duration to flip to OFF after ON completes.
	// We'll start timer such that first expiry toggles to OFF and schedules processing.
	cur_state = WINDOW_ON;
	hrtimer_start(&window_timer, kt_on, HRTIMER_MODE_REL);

	pr_info("page_window: module loaded\n");
	return 0;

err_free_bufs:
	free_percpu_buffers();
err_no_bufs:
	free_bucket_locks();
	return ret;
}

static void __exit pw_exit(void)
{
	pr_info("page_window: unloading module\n");

	// stop timer
	hrtimer_cancel(&window_timer);

	// unregister kprobes
	unregister_my_kprobes();

	// cancel work and destroy wq
	if (pw_wq) {
		cancel_work_sync(&process_work);
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

	pr_info("page_window: module unloaded\n");
}

module_init(pw_init);
module_exit(pw_exit);
