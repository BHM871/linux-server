// page_window_rate_collector.c
// Windowed page-access collector using kprobes + hrtimer + per-cpu buffers.
// Target: x86_64 kernels (tested conceptually for linux 6.12.x).

#include <linux/pid.h>
#include <linux/mm.h>
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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("TCC - Windowed Collector");
MODULE_DESCRIPTION(
	"Windowed collector for page-access co-occurrence (folio_mark_accessed + handle_mm_fault)");
MODULE_VERSION("0.4");

// ---------------- module parameters ----------------
static unsigned int on_ms = 10; // collect window in ms
module_param(on_ms, uint, 0644);
MODULE_PARM_DESC(on_ms, "Collecting window duration in ms (default 10ms)");

static unsigned int off_ms = 90; // idle window in ms
module_param(off_ms, uint, 0644);
MODULE_PARM_DESC(off_ms, "Idle window duration in ms (default 90ms)");

static unsigned long delta_ns = 10000000UL; // 10 ms default
module_param(delta_ns, ulong, 0644);
MODULE_PARM_DESC(delta_ns, "Idle window duration in ms (default 90ms)");

static unsigned int sample_rate = 2; // keep every Nth event (1 = no sampling)
module_param(sample_rate, uint, 0644);
MODULE_PARM_DESC(sample_rate,
		 "Sampling rate: keep 1 every N events (default 1)");

static unsigned int buf_len = 1024; // per-cpu buffer size (events)
module_param(buf_len, uint, 0444);
MODULE_PARM_DESC(buf_len,
		 "Per-CPU buffer length (max events per window) (default 1024)");

static unsigned int hash_bits = 12; // hashtable size 2^bits
module_param(hash_bits, uint, 0444);
MODULE_PARM_DESC(hash_bits, "Bit to define hashtables size (default 12)");

static unsigned short segments_percentil = 70;
module_param(segments_percentil, ushort, 0644);
MODULE_PARM_DESC(segments_percentil,
		 "Percentil used to create segments (default p70)");

static unsigned short max_segment_size = 100;
module_param(max_segment_size, ushort, 0444);
MODULE_PARM_DESC(max_segment_size,
		 "Max of pages that a segment can have (default 100)");

static unsigned int max_report = 20000;
module_param(max_report, uint, 0444);
MODULE_PARM_DESC(max_report, "Max entries printed via /proc");

static unsigned short active = 15; // bit set to define features active
module_param(active, ushort, 0444);
MODULE_PARM_DESC(
	active,
	"Bit set to define if feature are active (segments_load, segments_count, process_count and events_file) (default 00001111)");

// ----------------- constants & helpers -----------------

// module

/* constants: module name */
#define MODULE_NAME "page_window_rate"
/* constants: module events file name */
#define MODULE_EVENTS "/tmp/page_window_rate_events.bin"

// /proc

/* constants: /proc file name */
#define PROC_NAME "page_window_rate_pairs"
/* constants: /proc file pointer */
static struct proc_dir_entry *proc_entry = NULL;

// generic

#define ID_IS_PFN_FLAG (1ULL << 63)

// ----------------- enums & structs -----------------

enum init_errors {
	ELOCKS = 1,
	EBUFFERS = 2,
	EMIN_HEAP = 4,
	EQUEUE_WORK = 8,
	EWORKERS = 16,
	EKPROBES = 32,
	EWINDOW = 64,
};

enum features {
	EVENTS_FILE = 1,
	EVENTS_FILLED = 2,
	EVENTS_DELAYED = 4,
	PROCESS_COUNT = 8,
	SEGMENT_COUNT = 16,
	SEGMENT_LOAD = 32,
};

enum event_type { ACCESSED = 'F', FAULT = 'H' };

enum window_state { WINDOW_OFF = 0, WINDOW_ON = 1 };

struct page_event {
	u64 ts_ns;
	pid_t pid;
	pid_t tgid;
	// either (ID_IS_PFN_FLAG | pfn) or (vaddr & PAGE_MASK)
	u64 id;
	char type;
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
	bool loaded;
	pid_t pid;
	pid_t tgid;
	u64 id;
	u64 *pages;
	unsigned int head;
	unsigned int count;
};

struct heap_pair_entry {
	struct pair_key key;
	u64 count;
};

struct heap_pair {
	struct heap_pair_entry *items;
	int size;
	int capacity;
};

// ----------------- variables -----------------

static enum window_state cur_state = WINDOW_OFF;

// hashtables
static DEFINE_HASHTABLE(pair_table, 12);
static DEFINE_HASHTABLE(processors_table, 12);
static DEFINE_HASHTABLE(segments_table, 12);

// timer
static struct hrtimer window_timer;
static ktime_t kt_on;
static ktime_t kt_off;

// per-cpu recent buffers
static struct recent_buf __percpu *pcpu_bufs = NULL;

// locks
static spinlock_t *bucket_locks = NULL; // array of locks for hash buckets
static unsigned int bucket_lock_count = 256; // will be tuned

// stats
static atomic64_t stat_events = ATOMIC64_INIT(0);
static atomic64_t stat_dropped = ATOMIC64_INIT(0);
static atomic64_t stat_pairs = ATOMIC64_INIT(0);

static atomic64_t folio_mark_hits = ATOMIC_INIT(0);
static atomic64_t handle_fault_hits = ATOMIC_INIT(0);

static atomic64_t stat_segments = ATOMIC64_INIT(0);
static atomic64_t stat_segments_loaded = ATOMIC64_INIT(0);

static atomic_long_t allocated_bytes = ATOMIC_LONG_INIT(0);
static atomic_long_t allocated_count = ATOMIC_LONG_INIT(0);

// workqueue for processing window
static struct workqueue_struct *pw_wq;
static struct delayed_work process_work_events_delayed;
static struct work_struct process_work_events_filled;
static struct work_struct process_work_segments;
static unsigned int process_interval_ms = 100;

// events file
static struct file *events_file;
static loff_t events_file_pos;

// ----------------- utils -----------------

// allocation & deallocation

/* Header to manage allocations */
struct allocation_header {
	size_t size;
};

/* utils: tracke allocations with head */
static void *tracked_alloc(size_t size, gfp_t flags)
{
	struct allocation_header *header;

	header = kmalloc(sizeof(*header) + size, flags);
	if (!header)
		return NULL;

	header->size = size;

	atomic_long_add(size, &allocated_bytes);
	atomic_long_inc(&allocated_count);

	return header + 1;
}

/* utils: alloc with GFP_ATOMIC */
static void *allocation(size_t size)
{
	return tracked_alloc(size, GFP_ATOMIC);
}

/* utils: alloc a array with GFP_KERNEL */
static void *aallocation(u64 count, size_t size)
{
	return tracked_alloc(count * size, GFP_KERNEL);
}

/* utils: dealloc a poiter usding header */
static void deallocation(void *ptr)
{
	struct allocation_header *header;

	if (!ptr)
		return;

	header = (struct allocation_header *)ptr - 1;

	atomic_long_sub(header->size, &allocated_bytes);
	atomic_long_dec(&allocated_count);

	kfree(header);
}

// features

/* utils: is active */
static bool is_active(enum features feat)
{
	return active & feat;
}

/* utils: is not active */
static bool is_not_active(enum features feat)
{
	return !is_active(feat);
}

// hashtables

/* utils: calculate hashtable key hash with two values */
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

/* utils: map hashbucket -> lock index */
static inline unsigned int lock_index_for_bucket(u32 bucket)
{
	// bucket_lock_count must be power of two
	return bucket & (bucket_lock_count - 1);
}

// segments

/* utils: define if segment is loaded or not */
static void mark_segment_as(struct segment_pages *segment, bool loaded,
			    spinlock_t *lock)
{
	if (is_not_active(SEGMENT_LOAD))
		return;

	if (segment->loaded == loaded)
		return;

	segment->loaded = loaded;
	if (loaded)
		atomic64_inc(&stat_segments_loaded);
	else
		atomic64_dec(&stat_segments_loaded);
}

/* utils: find a segment and define if it is loaded or not */
static void mark_segment_by_id_as(pid_t tgid, u64 id, bool loaded,
				  spinlock_t *lock)
{
	if (is_not_active(SEGMENT_LOAD))
		return;

	struct segment_pages *entry;
	u32 h32 = pair_hash_u64(tgid, id);

	// already has lock to asscess
	hash_for_each_possible(segments_table, entry, node, h32) {
		if (entry->tgid == tgid && entry->id == id) {
			mark_segment_as(entry, loaded, lock);
			return;
		}
	}
}

/* utils: calculate percentil */
static u64 calculate_percentil(int percentil, u64 elements)
{
	u64 per = (elements * percentil) / 100;
	return clamp(per, 0, elements - 1);
}

// ----------------- heap tree -----------------

/* heap: struct object */
static struct heap_pair heap;

/* heap: swap two heap.items */
static void heap_swap(struct heap_pair *heap, int a, int b)
{
	struct heap_pair_entry tmp = heap->items[a];
	heap->items[a] = heap->items[b];
	heap->items[b] = tmp;
}

/* heap: shift up element of 'index' */
static void heap_shift_up(struct heap_pair *heap, int index)
{
	while (index > 0) {
		int parent = (index - 1) / 2;

		if (heap->items[parent].count <= heap->items[index].count)
			break;

		heap_swap(heap, parent, index);
		index = parent;
	}
}

/* heap: shift down element of 'index' */
static void heap_shift_down(struct heap_pair *heap, int index)
{
	while (index <= heap->capacity) {
		int left = index * 2 + 1;
		int rigth = index * 2 + 2;
		int smallest = index;

		if (left < heap->size &&
		    heap->items[left].count < heap->items[smallest].count)
			smallest = left;

		if (rigth < heap->size &&
		    heap->items[rigth].count < heap->items[smallest].count)
			smallest = rigth;

		if (smallest == index)
			break;

		heap_swap(heap, smallest, index);
		index = smallest;
	}
}

/* heap: insert a element in heap */
static void heap_push(struct heap_pair *heap, struct heap_pair_entry entry)
{
	if (heap->size < heap->capacity) {
		heap->items[heap->size] = entry;
		heap_shift_up(heap, heap->size);
		heap->size++;

		return;
	}

	if (entry.count <= heap->items[0].count)
		return;

	heap->items[0] = entry;
	heap_shift_down(heap, 0);
}

/* heap: walk through pair table and insert heap  */
static void fill_heap_pair_entry(void)
{
	struct pair_entry *entry;
	unsigned int bkt;

	heap.size = 0;

	hash_for_each(pair_table, bkt, entry, node) {
		struct heap_pair_entry item = {
			.key = entry->key,
			.count = atomic64_read(&entry->count),
		};
		heap_push(&heap, item);
	}
}

// ----------------- buffers -----------------

/* buffer: alloc buffers */
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

/* buffer: free buffers */
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

/* lock: init locks */
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

/* lock: free locks */
static void free_bucket_locks(void)
{
	if (bucket_locks) {
		kfree(bucket_locks);
		bucket_locks = NULL;
	}
}

// ---------------- hashtables ----------------

// pair table

/* pair_table: incrementation */
static void pair_table_inc(pid_t pid, pid_t tgid, u64 a, u64 b)
{
	struct pair_entry *entry;
	u32 h32 = pair_hash_u64(tgid, a);
	u32 bucket = hash_min(h32, HASH_BITS(pair_table));
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
	entry = (struct pair_entry *)allocation(sizeof(*entry));
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

	mark_segment_by_id_as(tgid, a, false, &bucket_locks[li]);

	spin_unlock(&bucket_locks[li]);
	atomic64_inc(&stat_pairs);
}

/* pair_table: free */
static void free_pair_table(void)
{
	struct pair_entry *entry;
	struct hlist_node *tmp;
	unsigned bkt;

	hash_for_each_safe(pair_table, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		deallocation(entry);
	}
}

// processor table

/* processor_table: incrementation
 * only if feature PROCESS_COUNT is active
 * */
static void processors_table_inc(pid_t pid, pid_t tgid, u64 page, bool dropped)
{
	if (is_not_active(PROCESS_COUNT))
		return;

	struct processor_count *entry;
	u32 h32 = pair_hash_u64(pid, tgid);
	u32 bucket = hash_min(h32, HASH_BITS(processors_table));
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
	entry = (struct processor_count *)allocation(sizeof(*entry));
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

/* processor_table: incrementation */
static void free_processors_table(void)
{
	struct processor_count *entry;
	struct hlist_node *tmp;
	unsigned bkt;
	hash_for_each_safe(processors_table, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		deallocation(entry);
	}
}

// segments table

/* segments_table: incrementation
 * only if feature SEGMENT_COUNT is active
 * */
static void segments_table_inc(pid_t pid, pid_t tgid, u64 id, u64 page)
{
	if (is_not_active(SEGMENT_COUNT))
		return;

	struct segment_pages *entry;
	u32 h32 = pair_hash_u64(tgid, id);
	u32 bucket = hash_min(h32, HASH_BITS(segments_table));
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
	entry = (struct segment_pages *)allocation(sizeof(*entry));
	if (!entry) {
		spin_unlock(&bucket_locks[li]);
		return;
	}

	entry->pages = (u64 *)aallocation(max_segment_size, sizeof(u64));
	if (!entry->pages) {
		deallocation(entry);
		spin_unlock(&bucket_locks[li]);
		return;
	}

	entry->pid = pid;
	entry->tgid = tgid;
	entry->id = id;
	entry->pages[0] = page;
	entry->head = 1;
	entry->count = 1;
	entry->loaded = false;
	atomic64_inc(&stat_segments);

	hash_add(segments_table, &entry->node, h32);

	spin_unlock(&bucket_locks[li]);
}

/* segments_table: free */
static void free_segments_table(void)
{
	struct segment_pages *entry;
	struct hlist_node *tmp;
	unsigned bkt;
	hash_for_each_safe(segments_table, bkt, tmp, entry, node) {
		hash_del(&entry->node);
		deallocation(entry->pages);
		deallocation(entry);
	}
}

// ---------------- events ----------------

/* events: push events in temporary buffer */
static void push_event_to_percpu(u64 id, enum event_type type)
{
	struct recent_buf *rb = this_cpu_ptr(pcpu_bufs);
	unsigned long flags;

	if (is_active(EVENTS_FILLED)) {
		if (rb->head == 0 && rb->count != 0 && pw_wq) {
			queue_work(pw_wq, &process_work_events_filled);
		}
	}

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

/* events:
 *	Copy up to max_items from per-cpu buffer into out_buf.
 *	Returns number of items copied.
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
		idx = (idx + 1) % buf_len;
		struct page_event p = rb->buf[idx];

		if (!p.pid || !p.id)
			continue;

		out_buf[cnt++] = p;

		// invalidate event
		rb->buf[idx].pid = 0;
		rb->buf[idx].id = 0;
		rb->count--;
	}

	spin_unlock_irqrestore(&rb->lock, flags);
	return cnt;
}

// ---------------- file helper ----------------

/* file: open event file stream
 * only if feature EVENTS_FILE is active
 * */
static int open_events_file(void)
{
	if (is_not_active(EVENTS_FILE))
		return -1;

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

/* file: write events in file if file was opened */
static void write_events(struct page_event *snap, int cnt)
{
	if (!events_file || cnt == 0)
		return;

	kernel_write(events_file, snap, cnt * sizeof(struct page_event),
		     &events_file_pos);
}

/* file: close events file stream */
static void close_events_file(void)
{
	if (events_file) {
		filp_close(events_file, NULL);
		events_file = NULL;
	}
}

// ---------------- processors ----------------

// events

/* processors: consume snapshots and compute pairs */
static void process_events(struct work_struct *work)
{
	int cpu;
	unsigned int max_items = buf_len;
	struct page_event *snap = NULL;

	// allocate snapshot buffer once (per CPU we reuse)
	snap = (struct page_event *)aallocation(max_items,
						sizeof(struct page_event));
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
				if (snap[j].type != FAULT ||
				    snap[i].tgid != snap[j].tgid || ts_j < ts_i)
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

	deallocation(snap);

	pr_debug("%s: processed window (pairs total ~ %lld)\n", MODULE_NAME,
		 (long long)atomic64_read(&stat_pairs));
}

/* processors: schedule process_work_events */
static void schedule_processing(unsigned long delay_ms)
{
	if (!pw_wq)
		return;
	queue_delayed_work(pw_wq, &process_work_events_delayed, delay_ms);
}

/* processors: wrapper for process events and schedule next */
static void process_events_fn(struct work_struct *work)
{
	process_events(work);
	// reschedule periodic processing (keeps sampling up-to-date)
	schedule_processing(process_interval_ms);
}

// segments

/* processors: find task and load a segment if feature is active
 * only if feature SEGMENT_LOAD is active
 * */
static void prefetch_segment(struct segment_pages *segment, spinlock_t *lock)
{
	if (is_not_active(SEGMENT_LOAD))
		return;

	if (segment->loaded)
		return;

	struct pid *pid;
	struct task_struct *task;
	u8 value;
	unsigned int idx, i, out;

	pid = find_get_pid(segment->pid);
	if (!pid)
		return;

	task = get_pid_task(pid, PIDTYPE_PID);
	put_pid(pid);

	if (!task)
		return;

	idx = (segment->head - segment->count + max_segment_size) %
	      max_segment_size;

	for (i = 0; i < segment->count; i++) {
		idx = (idx + 1) % max_segment_size;
		unsigned long long addr = segment->pages[idx];

		if ((addr & ID_IS_PFN_FLAG))
			continue;

		out = access_process_vm(task, addr, &value, sizeof(value), 0);
		if (out <= 0) {
			pr_debug(
				"%s: prefetch failed pid=%d tgid=%d addr=%llx\n",
				MODULE_NAME, segment->pid, segment->tgid, addr);
		}
	}

	put_task_struct(task);
	mark_segment_as(segment, true, lock);
}

/*
 * processors: walk through segments_table and load segments
 * only if feature SEGMENT_LOAD is active
 */
static void prefetch_segments(void)
{
	if (is_not_active(SEGMENT_LOAD))
		return;

	struct segment_pages *entry;
	unsigned int bucket;

	hash_for_each(segments_table, bucket, entry, node) {
		unsigned int li = lock_index_for_bucket(bucket);
		spin_lock(&bucket_locks[li]);
		prefetch_segment(entry, &bucket_locks[li]);
		spin_unlock(&bucket_locks[li]);
	}
}

/*
 * processors: consume heap with pair_table elements and create segments
 * only if feature SEGMENT_LOAD is active
 */
static void process_segments(struct work_struct *work)
{
	if (is_not_active(SEGMENT_COUNT))
		return;

	struct heap_pair_entry *entry;
	int per, size;
	u64 limit;

	fill_heap_pair_entry();
	if (heap.size <= 0)
		return;

	size = min(heap.size, heap.capacity);
	per = calculate_percentil(segments_percentil, size);

	limit = heap.items[size - 1].count;
	limit = calculate_percentil(segments_percentil, limit);

	for (int i = size - 1; i >= per; i--) {
		entry = &heap.items[i];
		if (entry->count < limit)
			break;

		segments_table_inc(entry->key.pid, entry->key.tgid,
				   entry->key.a, entry->key.b);
	}

	prefetch_segments();
}

// ---------------- handles ----------------

/* handles: function to initialize events registration */
static int kp_pre_handler(struct kprobe *p, struct pt_regs *regs);

/* handles: kproble to register access */
static struct kprobe kp_folio_mark = {
	.symbol_name = "folio_mark_accessed",
	.pre_handler = kp_pre_handler,
};

/* handles: kproble to register faults */
static struct kprobe kp_handle_fault = {
	.symbol_name = "handle_mm_fault",
	.pre_handler = kp_pre_handler,
};

/*
 * handles: register both kprobles
 * Return registration status
 * */
static int register_my_kprobes(int *errors)
{
	int ret;
	ret = register_kprobe(&kp_folio_mark);
	if (ret) {
		pr_warn("%s: failed to register kprobe folio_mark_accessed: %d\n",
			MODULE_NAME, ret);
		*errors |= EKPROBES;
	}
	ret |= register_kprobe(&kp_handle_fault);
	if (ret) {
		pr_warn("%s: failed to register kprobe handle_mm_fault: %d\n",
			MODULE_NAME, ret);
		*errors |= EKPROBES;
	}
	return ret;
}

/*
 * handles: unregister both of kprobles
 * */
static void unregister_my_kprobes(void)
{
	unregister_kprobe(&kp_folio_mark);
	unregister_kprobe(&kp_handle_fault);
}

/* handles: function to initialize events registration */
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

// ---------------- /proc ----------------

/* proc: logic to print /proc file */
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
		"# features SEGMENT_LOAD=%d SEGMENT_COUNT=%d PROCESS_COUNT=%d EVENTS_DELAYED=%d EVENTS_FILLED=%d EVENTS_FILE=%d\n",
		is_active(SEGMENT_LOAD), is_active(SEGMENT_COUNT),
		is_active(PROCESS_COUNT), is_active(EVENTS_DELAYED),
		is_active(EVENTS_FILLED), is_active(EVENTS_FILE));

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

/* proc: logic open /proc file */
static int proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, proc_show, NULL);
}

/* proc: register /proc struct */
static const struct proc_ops proc_fops = {
	.proc_open = proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

// ---------------- module ----------------

// params

/* module: validate features active values */
static void verify_features(void)
{
	if (is_active(EVENTS_DELAYED) && is_active(EVENTS_FILLED)) {
		pr_warn("%s: EVENTS_DELAYED and EVENTS_FILLED feature can't be active together, using EVENTS_FILLED",
			MODULE_NAME);
		active &= ~EVENTS_DELAYED;
	}

	if (is_not_active(EVENTS_DELAYED) && is_not_active(EVENTS_FILLED)) {
		pr_warn("%s: EVENTS_DELAYED and EVENTS_FILLED feature are disactive, using EVENTS_FILLED",
			MODULE_NAME);
		active |= EVENTS_FILLED;
	}

	if (is_active(SEGMENT_LOAD) && is_not_active(SEGMENT_COUNT)) {
		pr_warn("%s: forcing SEGMENT_COUNT feature because SEGMENT_LOAD is active\n",
			MODULE_NAME);
		active |= SEGMENT_COUNT;
	}
}

/* module: validate params values */
static void verify_params(void)
{
	if (sample_rate <= 1) {
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

	INIT_DELAYED_WORK(&process_work_events_delayed, process_events_fn);
	INIT_WORK(&process_work_events_filled, process_events);
	INIT_WORK(&process_work_segments, process_segments);

	if (!process_work_events_delayed.work.func ||
	    !process_work_segments.func) {
		pr_err("%s: failed create workers\n", MODULE_NAME);
		*errors |= EWORKERS;
		return 1;
	}

	if (is_active(EVENTS_DELAYED))
		schedule_processing(process_interval_ms);

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
	hrtimer_setup(&window_timer, window_timer_cb, CLOCK_MONOTONIC,
		      HRTIMER_MODE_REL);

	if (!window_timer.function) {
		*errors |= EWINDOW;
		return 1;
	}

	if (on_ms <= 0 && off_ms <= 0) {
		cur_state = WINDOW_OFF;
		return 0;
	}

	cur_state = WINDOW_ON;
	if (off_ms <= 0)
		return 0;

	kt_on = ms_to_ktime(on_ms);
	kt_off = ms_to_ktime(off_ms);

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
		cancel_delayed_work_sync(&process_work_events_delayed);
		cancel_work_sync(&process_work_events_filled);
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

	return 1;
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

	pr_info("%s: features actived (SEGMENT_LOAD=%d SEGMENT_COUNT=%d PROCESS_COUNT=%d EVENTS_DELAYED=%d EVENTS_FILLED=%d EVENTS_FILE=%d)\n",
		MODULE_NAME, is_active(SEGMENT_LOAD), is_active(SEGMENT_COUNT),
		is_active(PROCESS_COUNT), is_active(EVENTS_DELAYED),
		is_active(EVENTS_FILLED), is_active(EVENTS_FILE));
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
		cancel_delayed_work_sync(&process_work_events_delayed);
		cancel_work_sync(&process_work_events_filled);
		cancel_work_sync(&process_work_segments);

		destroy_workqueue(pw_wq);
		pw_wq = NULL;
	}

	close_events_file();

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
