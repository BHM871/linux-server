
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
static unsigned int sample_rate = 2; // keep every Nth event (1 = no sampling)
static unsigned int buf_len = 1024; // per-cpu buffer size (events)
static unsigned int hash_bits = 12; // hashtable size 2^bits
static unsigned short segments_percentil = 70;
static unsigned short max_segment_size = 100;
static unsigned int max_report = 20000;

// ----------------- constants & helpers -----------------
#define ID_IS_PFN_FLAG (1ULL << 63)
#ifndef MODULE_NAME
#error "MODULE_NAME must be define before include"
#endif // !MODULE_NAME

#ifndef MODULE_EVENTS
#error "MODULE_EVENTS must be define before include"
#endif // !MODULE_EVENTS

enum event_type { ACCESSED = 'F', FAULT = 'H' };

struct page_event {
	u64 ts_ns;
	pid_t pid;
	pid_t tgid;
	// either (ID_IS_PFN_FLAG | pfn) or (vaddr >> PAGE_SHIFT)
	u64 id;
	enum event_type type;
	char comm[TASK_COMM_LEN];
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
	pid_t pid;
	pid_t tgid;
	u64 a;
	u64 b;
};

struct pair_entry {
	struct hlist_node node;
	struct pair_key key;
	atomic64_t count;
};

struct processor_count {
	struct hlist_node node;
	pid_t pid;
	pid_t tgid;
	atomic64_t total;
	atomic64_t dropped;
};

struct segment_pages {
	struct hlist_node node;
	pid_t pid;
	pid_t tgid;
	u64 id;
	u64 *pages;
	unsigned int head;
	unsigned int count;
};

struct heap_pair_entry {
	struct pair_entry *items;
	int size;
	int capacity;
};

/* per-cpu recent buffers */
static struct recent_buf __percpu *pcpu_bufs = NULL;

static DEFINE_HASHTABLE(pair_table, 12); // initialize later with param
static DEFINE_HASHTABLE(processors_table, 12);
static DEFINE_HASHTABLE(segments_table, 12);

static spinlock_t *bucket_locks = NULL; // array of locks for hash buckets
static unsigned int bucket_lock_count = 256; // will be tuned

// stats
static atomic64_t stat_events = ATOMIC64_INIT(0);
static atomic64_t stat_dropped = ATOMIC64_INIT(0);
static atomic64_t stat_pairs = ATOMIC64_INIT(0);

// pair heap
static struct heap_pair_entry heap;

// workqueue for processing window end
static struct file *events_file;
static loff_t events_file_pos;
static struct workqueue_struct *pw_wq;
static struct delayed_work process_work_events;
static struct work_struct process_work_segments;
static unsigned int process_interval_ms = 100;

// # Heap Tree

static void heap_swap(struct heap_pair_entry *heap, int a, int b)
{
	struct pair_entry tmp = heap->items[a];
	heap->items[a] = heap->items[b];
	heap->items[b] = tmp;
}

static void heap_shift_up(struct heap_pair_entry *heap, int index)
{
	while (index > 0) {
		int parent = (index - 1) / 2;

		if (atomic64_read(&heap->items[parent].count) <=
		    atomic64_read(&heap->items[index].count))
			break;

		heap_swap(heap, parent, index);
		index = parent;
	}
}

static void heap_shift_down(struct heap_pair_entry *heap, int index)
{
	while (index <= heap->capacity) {
		int left = index * 2 + 1;
		int rigth = index * 2 + 2;
		int smallest = index;

		if (left < heap->size &&
		    atomic64_read(&heap->items[left].count) <
			    atomic64_read(&heap->items[smallest].count))
			smallest = left;

		if (rigth < heap->size &&
		    atomic64_read(&heap->items[rigth].count) <
			    atomic64_read(&heap->items[smallest].count))
			smallest = rigth;

		if (smallest == index)
			break;

		heap_swap(heap, smallest, index);
		index = smallest;
	}
}

static void heap_push(struct heap_pair_entry *heap, struct pair_entry entry)
{
	if (heap->size < heap->capacity) {
		heap->items[heap->size] = entry;
		heap_shift_up(heap, heap->size);
		heap->size++;

		return;
	}

	if (atomic64_read(&entry.count) <= atomic64_read(&heap->items[0].count))
		return;

	heap->items[0] = entry;
	heap_shift_down(heap, 0);
}

static void fill_heap_pair_entry(void)
{
	struct pair_entry *entry;
	unsigned int bkt;

	hash_for_each(pair_table, bkt, entry, node) {
		heap_push(&heap, *entry);
	}
}

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

// #  Hashtables

// ---------------- pair table manipulation ----------------
static void pair_table_inc(pid_t pid, pid_t tgid, u64 a, u64 b)
{
	struct pair_entry *entry;
	u32 h32 = pair_hash_u64(tgid, a);
	u32 bucket = h32 & ((1u << hash_bits) - 1);
	unsigned int li = lock_index_for_bucket(bucket);

	if (!bucket_locks)
		return;

	spin_lock(&bucket_locks[li]);
	hash_for_each_possible(pair_table, entry, node, h32) {
		if (entry->key.tgid == tgid && entry->key.a == a &&
		    entry->key.b == b) {
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
		return;
	}

	entry->key.pid = pid;
	entry->key.tgid = tgid;
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

// ---------------- processer table manipulation ----------------
static void processors_table_inc(pid_t pid, pid_t tgid, u64 page, bool dropped)
{
	struct processor_count *entry;
	u32 h32 = pair_hash_u64(pid, tgid);
	u32 bucket = h32 & ((1u << hash_bits) - 1);
	unsigned int li = lock_index_for_bucket(bucket);

	if (!bucket_locks)
		return;

	spin_lock(&bucket_locks[li]);
	hash_for_each_possible(processors_table, entry, node, h32) {
		if (entry->pid == pid && entry->tgid == tgid) {
			if (dropped)
				atomic64_inc(&entry->dropped);
			atomic64_inc(&entry->total);
			spin_unlock(&bucket_locks[li]);
			return;
		}
	}

	// not found -> create
	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		spin_unlock(&bucket_locks[li]);
		return;
	}

	entry->pid = pid;
	entry->tgid = tgid;
	atomic64_set(&entry->total, 1);
	atomic64_set(&entry->dropped, dropped);

	hash_add(processors_table, &entry->node, h32);

	spin_unlock(&bucket_locks[li]);
}

static void free_processors_table(void)
{
	struct processor_count *entry;
	unsigned bkt;
	hash_for_each(processors_table, bkt, entry, node) {
		hash_del(&entry->node);
		kfree(entry);
	}
}

// ---------------- segments table manipulation ----------------
static void segments_table_inc(pid_t pid, pid_t tgid, u64 id, u64 page)
{
	struct segment_pages *entry;
	u32 h32 = pair_hash_u64(tgid, id);
	u32 bucket = h32 & ((1u << hash_bits) - 1);
	unsigned int li = lock_index_for_bucket(bucket);

	if (!bucket_locks)
		return;

	spin_lock(&bucket_locks[li]);
	hash_for_each_possible(segments_table, entry, node, h32) {
		if (entry->tgid == tgid && entry->id == id) {
			entry->pages[entry->head] = page;
			entry->head = (entry->head + 1) % max_segment_size;
			if (entry->count < max_segment_size)
				entry->count++;

			spin_unlock(&bucket_locks[li]);
			return;
		}
	}

	// not found -> create
	entry = kmalloc(sizeof(*entry), GFP_ATOMIC);
	if (!entry) {
		spin_unlock(&bucket_locks[li]);
		return;
	}

	entry->pages = kmalloc_array(max_segment_size, sizeof(u64), GFP_KERNEL);
	if (!entry->pages) {
		kfree(entry);
		spin_unlock(&bucket_locks[li]);
		return;
	}

	entry->pid = pid;
	entry->tgid = tgid;
	entry->id = id;
	entry->pages[0] = page;
	entry->head = 1;
	entry->count = 1;

	hash_add(segments_table, &entry->node, h32);

	spin_unlock(&bucket_locks[li]);
}

static void free_segments_table(void)
{
	struct segment_pages *entry;
	unsigned bkt;
	hash_for_each(segments_table, bkt, entry, node) {
		hash_del(&entry->node);
		kfree(entry->pages);
		kfree(entry);
	}
}

// #  Events

// ---------------- push event (hot path) ----------------
static void push_event_to_percpu(u64 id, enum event_type type)
{
	struct recent_buf *rb = this_cpu_ptr(pcpu_bufs);
	unsigned long flags;

	// simple sampling based on total events (not perfect but ok)
	if ((atomic64_read(&stat_events) % sample_rate) != 0) {
		atomic64_inc(&stat_dropped);
		atomic64_inc(&stat_events);

		processors_table_inc(current->pid, current->tgid, id, 1);
		return;
	}

	// Very small fast critical section
	spin_lock_irqsave(&rb->lock, flags);

	rb->buf[rb->head].ts_ns = ktime_get_ns();
	rb->buf[rb->head].pid = current->pid;
	rb->buf[rb->head].tgid = current->tgid;
	memcpy(rb->buf[rb->head].comm, current->comm, TASK_COMM_LEN);

	rb->buf[rb->head].id = id;
	rb->buf[rb->head].type = type;
	rb->head = (rb->head + 1) % buf_len;

	if (rb->count < buf_len)
		rb->count++;

	processors_table_inc(current->pid, current->tgid, id, 0);

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
	unsigned int cmax, cnt = 0, idx, i;

	spin_lock_irqsave(&rb->lock, flags);
	cmax = rb->count;
	if (cmax > max_items)
		cmax = max_items;

	// oldest idx:
	idx = (rb->head - rb->count + buf_len) % buf_len;
	for (i = 0; i < cmax; ++i) {
		struct page_event p = rb->buf[(idx + i) % buf_len];
		if (!p.id)
			continue;

		out_buf[cnt++] = p;
	}

	spin_unlock_irqrestore(&rb->lock, flags);
	return cnt;
}

// ---------------- file helper ----------------

static int open_events_file(void)
{
	events_file =
		filp_open(MODULE_EVENTS, O_WRONLY | O_CREAT | O_TRUNC, 0644);

	if (IS_ERR(events_file)) {
		int ret = PTR_ERR(events_file);
		events_file = NULL;
		return ret;
	}

	events_file_pos = 0;
	return 0;
}

static void write_events(struct page_event *snap, int cnt)
{
	if (!events_file) {
		return;
	}

	if (cnt == 0)
		return;

	kernel_write(events_file, snap, cnt * sizeof(struct page_event),
		     &events_file_pos);
}

static void close_events_file(void)
{
	if (events_file) {
		filp_close(events_file, NULL);
		events_file = NULL;
	}
}

// ---------------- processing: consume snapshots and compute pairs ----------------
static void process_events(struct work_struct *work)
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
		u64 ts_i;
		u64 id_i;
		u64 ts_j;
		u64 id_j;
		for (i = 0; i < cnt; ++i) {
			if (snap[i].type != FAULT)
				continue;

			ts_i = snap[i].ts_ns;
			id_i = snap[i].id;
			for (j = i + 1; j < cnt; ++j) {
				ts_j = snap[j].ts_ns;
				if (snap[i].tgid != snap[j].tgid ||
				    snap[j].type != FAULT || ts_j < ts_i)
					continue;
				if (ts_j - ts_i > delta_ns)
					break;
				id_j = snap[j].id;

				// increment pair id_i -> id_j
				pair_table_inc(snap[i].pid, snap[i].tgid, id_i,
					       id_j);
			}
		}

		write_events(snap, cnt);
	}

	kfree(snap);

	pr_debug("%s: processed window (pairs total ~ %lld)\n", MODULE_NAME,
		 (long long)atomic64_read(&stat_pairs));
}

static void schedule_processing(unsigned long delay_ms)
{
	if (!pw_wq)
		return;
	queue_delayed_work(pw_wq, &process_work_events, delay_ms);
}

static void process_events_fn(struct work_struct *work)
{
	process_events(work);
	// reschedule periodic processing (keeps sampling up-to-date)
	schedule_processing(process_interval_ms);
}

// #  Segments

static u64 calculate_percentil(int percentil, u64 elements)
{
	u64 per = (elements * percentil) / 100;
	return clamp(per, 0, elements - 1);
}

static void process_segments(struct work_struct *work)
{
	struct pair_entry *entry;
	int per, size;
	u64 limit;

	fill_heap_pair_entry();

	size = min(heap.size, heap.capacity);
	per = calculate_percentil(segments_percentil, size);

	limit = (unsigned long long)atomic64_read(&heap.items[size - 1].count);
	limit = calculate_percentil(100 - segments_percentil, limit);

	for (int i = size - 1; i >= per; i--) {
		entry = &heap.items[i];
		if (atomic64_read(&entry->count) < limit)
			break;

		segments_table_inc(entry->key.pid, entry->key.tgid,
				   entry->key.a, entry->key.b);
	}
}

// #  Handles

static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs);

// register/unregister kprobes
static struct kprobe kp_folio_mark = {
	.symbol_name = "folio_mark_accessed",
	.pre_handler = kp_pre_handler,
};

static struct kprobe kp_handle_fault = {
	.symbol_name = "handle_mm_fault",
	.pre_handler = kp_pre_handler,
};

static int register_my_kprobes(void)
{
	int ret;
	ret = register_kprobe(&kp_folio_mark);
	if (ret) {
		pr_warn("%s: failed to register kprobe folio_mark_accessed: %d\n",
			MODULE_NAME, ret);
	}
	ret = register_kprobe(&kp_handle_fault);
	if (ret) {
		pr_warn("%s: failed to register kprobe handle_mm_fault: %d\n",
			MODULE_NAME, ret);
	}
	return 0;
}

static void unregister_my_kprobes(void)
{
	unregister_kprobe(&kp_folio_mark);
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
