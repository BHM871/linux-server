
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

// ---------------- module parameters ----------------
static unsigned int on_ms = 10; // collect window in ms
static unsigned int off_ms = 90; // idle window in ms
static unsigned long delta_ns = 10000000UL; // 10 ms default
static unsigned int sample_rate = 1; // keep every Nth event (1 = no sampling)
static unsigned int buf_len = 1024; // per-cpu buffer size (events)
static unsigned int pair_hash_bits = 12; // hashtable size 2^bits
static unsigned int max_report = 20000;

// ----------------- constants & helpers -----------------
#define ID_IS_PFN_FLAG (1ULL << 63)
#ifndef MODULE_NAME
#error "MODULE_NAME must be define before include"
#endif // !MODULE_NAME

struct page_event {
	u64 ts_ns;
	pid_t pid;
	// either (ID_IS_PFN_FLAG | pfn) or (vaddr >> PAGE_SHIFT)
	u64 id;
};

struct recent_buf {
	// allocated at init size buf_len
	struct page_event *buf;
	// next write index [0..buf_len-1]
	unsigned int head;
	// number of valid items currently in buffer (<= buf_len)
	unsigned int count;
	spinlock_t lock;
};

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

/* per-cpu recent buffers */
static struct recent_buf __percpu *pcpu_bufs = NULL;

static DEFINE_HASHTABLE(pair_table, 12); // initialize later with param
static spinlock_t *bucket_locks = NULL; // array of locks for hash buckets
static unsigned int bucket_lock_count = 256; // will be tuned

// stats
static atomic64_t stat_events = ATOMIC64_INIT(0);
static atomic64_t stat_dropped = ATOMIC64_INIT(0);
static atomic64_t stat_pairs = ATOMIC64_INIT(0);

// #  Hashs

static inline u32 pair_hash_u64(u64 a, u64 b)
{
	u64 h = a;

	h ^= b + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);

	h ^= h >> 33;
	h *= 0xff51afd7ed558ccdULL;
	h ^= h >> 33;
	h *= 0xc4ceb9fe1a85ec53ULL;
	h ^= h >> 33;

	return (u32)h;
}

/* helper: map hashbucket -> lock index */
static inline unsigned int lock_index_for_bucket(u32 bucket)
{
	// bucket_lock_count must be power of two
	return bucket & (bucket_lock_count - 1);
}

// #  Buffers

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
		pr_warn("%s: could not allocate bucket locks, using 1\n",
			MODULE_NAME);
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

// #  Events

// ---------------- push event (hot path) ----------------
static void push_event_to_percpu(u64 id)
{
	struct recent_buf *rb = this_cpu_ptr(pcpu_bufs);
	unsigned long flags;

	if (sample_rate > 1) {
		// simple sampling based on total events (not perfect but ok)
		if ((atomic64_read(&stat_events) % sample_rate) != 0) {
			atomic64_inc(&stat_dropped);
			atomic64_inc(&stat_events);
			return;
		}
	}

	// Very small fast critical section
	spin_lock_irqsave(&rb->lock, flags);

	rb->buf[rb->head].ts_ns = ktime_get_ns();
	rb->buf[rb->head].pid = current->pid;
	rb->buf[rb->head].id = id;
	rb->head = (rb->head + 1) % buf_len;
	rb->count++;

	if (rb->count < buf_len)
		rb->count++;

	spin_unlock_irqrestore(&rb->lock, flags);

	atomic64_inc(&stat_events);
}

// ---------------- snapshot helper ----------------
/*
 * Copy up to max_items from per-cpu buffer into out_buf.
 * Returns number of items copied.
 */
static unsigned int snapshot_percpu_buffer(int cpu, struct page_event *out_buf,
					   unsigned int max_items)
{
	struct recent_buf *rb = per_cpu_ptr(pcpu_bufs, cpu);
	unsigned long flags;
	unsigned int cnt, idx, i;

	spin_lock_irqsave(&rb->lock, flags);
	cnt = rb->count;
	if (cnt > max_items)
		cnt = max_items;

	// oldest idx:
	idx = (rb->head - rb->count + buf_len) % buf_len;
	for (i = 0; i < cnt; ++i)
		out_buf[i] = rb->buf[(idx + i) % buf_len];

	spin_unlock_irqrestore(&rb->lock, flags);
	return cnt;
}

// #  Pair Table

// ---------------- pair table manipulation ----------------
static void pair_table_inc(u64 a, u64 b)
{
	struct pair_entry *entry;
	u32 h32 = pair_hash_u64(a, b);
	u32 bucket = h32 & ((1u << pair_hash_bits) - 1);
	unsigned int li = lock_index_for_bucket(bucket);

	if (!bucket_locks)
		return;

	spin_lock(&bucket_locks[li]);
	hash_for_each_possible(pair_table, entry, node, h32) {
		if (entry->key.a == a && entry->key.b == b) {
			atomic64_inc(&entry->count);
			spin_unlock(&bucket_locks[li]);
			atomic64_inc(&stat_pairs);
			return;
		}
	}

	// not found -> create
	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		spin_unlock(&bucket_locks[li]);
	}

	entry->key.a = a;
	entry->key.b = b;
	atomic64_set(&entry->count, 1);
	hash_add(pair_table, &entry->node, h32);

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

// #  Handles

static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs);

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
		pr_warn("%s: failed to register kprobe mark_page_accessed: %d\n",
			MODULE_NAME, ret);
		// continue attempt for the other
	}
	ret = register_kprobe(&kp_handle_fault);
	if (ret < 0) {
		pr_warn("%s: failed to register kprobe handle_mm_fault: %d\n",
			MODULE_NAME, ret);
		// still ok to run with only one probe
	}
	return 0;
}

static void unregister_my_kprobes(void)
{
	unregister_kprobe(&kp_mark_page);
	unregister_kprobe(&kp_handle_fault);
}

// # /proc

// ---------------- proc read ----------------
static int proc_show(struct seq_file *m, void *v);

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

// # Module

static int __init pw_init(void);

static void __exit pw_exit(void);
