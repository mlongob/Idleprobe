/*  
 * idleprobe.c - Idle times track probe
 */

#include <linux/module.h> /* Base */
#include <linux/moduleparam.h> /* Base */
#include <linux/kernel.h> /* Base */
#include <linux/param.h> /* Base */
#include <linux/init.h> /* Base */
#include <linux/types.h> /* Fixed-size types */
#include <linux/sched.h> /* For process info fs */
#include <linux/kprobes.h> /* For kprobes fs */
#include <linux/smp.h> /* For CPU identification */
#include <linux/kallsyms.h> /* For kernel symbols fs */
#include <linux/proc_fs.h> /* For proc fs */
#include <linux/seq_file.h> /* For proc fs interface */
#include <linux/list.h> /* For linked lists */
#include <linux/slab.h> /* For kmalloc and kfree */
#include <linux/gfp.h> /* For memory flags */
#include <linux/spinlock.h> /* For spinlocks */
#include <linux/time.h> /* For low level timing */
#include <linux/ktime.h> /* For low level timing */

#define DRIVER_AUTHOR "Mario Longobardi <longob@umich.edu>"
#define DRIVER_DESC "Idle times track prober"
#define PROCFS_NAME "idleprobe"
#define FETCH_TIMEOUT 120 /* Wait time before discarding non-fetched data */
/* #define IP_DEBUG */

typedef struct delta_period delta_period_t;
static void begin_idle(int cpu);
static void end_idle(int cpu);
static int init_jprobe(struct jprobe *jp);
static void remove_jprobe(struct jprobe *jp);
static int init_procfs(void);
static void remove_procfs(void);
static void init_capture(void);
static void cleanup_capture(void);
static void* IP_seq_start(struct seq_file *s, loff_t *pos);
static void* IP_seq_next(struct seq_file *s, void *v, loff_t *pos);
static void IP_seq_stop(struct seq_file *s, void *v);
static int IP_seq_show(struct seq_file *s, void *v);
static int IP_open(struct inode *inode, struct file *file);
static void IP_tick_nohz_stop_sched_tick(int a);
static void IP_tick_nohz_restart_sched_tick(void);
static u64 delta_to_ns(const delta_period_t* delta);

struct delta_period
{
	/* 
	 * One delta entry
	 */
	
	struct timespec begin;
	struct timespec end;
};

typedef struct capture_entry
{
	/* 
	 * Entry data for one idle period
	 */
	
	int cpu;					/* CPU number */
	struct timespec timestamp;	/* Timestamp of when the period begun */
	delta_period_t highRes;		/* Based on CPU cycles */
} capture_entry_t;

struct capture_list
{
	/* 
	 * Linked-list of idle times entries
	 */
	
	capture_entry_t entry;
	int count;					/* Number of entry */
	struct list_head list;
};

struct list_head *IP_list; /* Main list */
DEFINE_SPINLOCK(IP_list_lock); /* SpinLock for IP_list */

static struct jprobe jp_begin = {
	/* 
	 * Begin Jprobe
	 */
	
	.entry			= IP_tick_nohz_stop_sched_tick,
	.kp = {
		.symbol_name	= "tick_nohz_stop_sched_tick",
	},
};

static struct jprobe jp_end = {
	/* 
	 * End Jprobe
	 */
	
	.entry			= IP_tick_nohz_restart_sched_tick,
	.kp = {
		.symbol_name	= "tick_nohz_restart_sched_tick",
	},
};

static struct file_operations IP_file_ops = {
	.open    = IP_open,
	.read    = seq_read,
	.llseek  = seq_lseek,
	.release = seq_release
};

static struct seq_operations IP_seq_ops = {
	.start = IP_seq_start,
	.next  = IP_seq_next,
	.stop  = IP_seq_stop,
	.show  = IP_seq_show
};

static int entry_count = 0;			/* Counter for vaild entries */
static capture_entry_t* idle_store;	/* Partial result storage */
u64 last_fetch_timestamp;			/* Timestamp of last fetch */

static void begin_idle(int cpu)
{
	/* 
	 * Beginning of idle time on "cpu"
	 */
	
	getrawmonotonic(&(idle_store[cpu].highRes.begin));
	getnstimeofday(&idle_store[cpu].timestamp);
}

static void end_idle(int cpu)
{
	/* 
	 * End of idle time on "cpu"
	 */
	
	struct capture_list* tmp;
	struct timespec highRes_end;
	if(idle_store[cpu].timestamp.tv_sec == 0)
	{
		/* When the module is started, the fist call to end_idle
		   can be corrupted */
		return;
	}
	getrawmonotonic(&highRes_end); /* kmalloc is slow. Fetching time beforehand */
	tmp = (struct capture_list*) kmalloc(sizeof(struct capture_list), GFP_ATOMIC);
	tmp->entry = idle_store[cpu];
	idle_store[cpu].timestamp.tv_sec = 0;
	
	tmp->entry.highRes.end = highRes_end;
	
	spin_lock(&IP_list_lock);
	if(!list_empty(IP_list) && 
	  (tmp->entry.timestamp.tv_sec > last_fetch_timestamp + FETCH_TIMEOUT))
	{
		/* Discard oldest record */
		list_del(IP_list->next);
	}
	tmp->count = entry_count++;
	list_add_tail(&tmp->list, IP_list);
	spin_unlock(&IP_list_lock);
}

static void init_capture(void)
{
	int i;
	struct timespec last_fetch;
	idle_store = (capture_entry_t*) kmalloc(
				 sizeof(capture_entry_t)*NR_CPUS, GFP_KERNEL);
	for(i = 0; i < NR_CPUS; ++i)
	{
		idle_store[i].cpu = i;
		idle_store[i].timestamp.tv_sec = 0;
	}
	IP_list = (struct list_head*) kmalloc(sizeof(struct list_head), GFP_KERNEL);
	INIT_LIST_HEAD(IP_list);
	getnstimeofday(&last_fetch);
	last_fetch_timestamp = last_fetch.tv_sec;
}

static void cleanup_capture(void)
{
	struct list_head *cur, *next;
	struct capture_list* tmp;
	list_for_each_safe(cur, next, IP_list)
	{
		tmp = list_entry(cur, struct capture_list, list);
		list_del(cur);
		kfree(tmp);
	}
	kfree(IP_list);
	kfree(idle_store);
}

static void IP_tick_nohz_stop_sched_tick(int a)
{
	int cpu;
	if(a == 1)
	{
		cpu = smp_processor_id();
		#ifdef IP_DEBUG
		printk(KERN_INFO "idleprobe: tick_nohz_stop_sched_tick - %d [\"%s\" (pid %i) ON CPU%d]\n", a, current->comm, current->pid, cpu);
		#endif /* IP_DEBUG */
		begin_idle(cpu);
	}
	jprobe_return();
}

static void IP_tick_nohz_restart_sched_tick(void)
{
	int cpu;
	cpu = smp_processor_id();
	#ifdef IP_DEBUG
	printk(KERN_INFO "idleprobe: tick_nohz_restart_sched_tick [\"%s\" (pid %i) ON CPU%d]\n", current->comm, current->pid, cpu);
	#endif /* IP_DEBUG */
	end_idle(cpu);
	jprobe_return();
}

static int init_jprobe(struct jprobe *jp)
{
	/* 
	 * Initializes a jprobe
	 */
	
	int ret = register_jprobe(jp);
	if (ret < 0) {
		printk(KERN_INFO "idleprobe: register_jprobe failed, returned %d\n", ret);
		return -1;
	}
	#ifdef IP_DEBUG
	printk(KERN_INFO "idleprobe: Planted jprobe at %p, handler addr %p\n",
		   jp->kp.addr, jp->entry);
	#endif /* IP_DEBUG */
	return 0;
}

static void remove_jprobe(struct jprobe *jp)
{
	unregister_jprobe(jp);
	#ifdef IP_DEBUG
	printk(KERN_INFO "idleprobe: jprobe at %p unregistered\n", jp->kp.addr);
	#endif /* IP_DEBUG */
}

static int init_procfs(void)
{
	/* 
	 * Initializes the proc file
	 */
	
	
	struct proc_dir_entry *proc_file = create_proc_entry(PROCFS_NAME, 0, NULL);
	
	if (proc_file == NULL) {
		remove_proc_entry(PROCFS_NAME, NULL);
		printk(KERN_ALERT "idleprobe: Error - Could not initialize /proc/%s\n",
		       PROCFS_NAME);
		return -1;
	}
	
	proc_file->proc_fops = &IP_file_ops;

	#ifdef IP_DEBUG
	printk(KERN_INFO "idleprobe: /proc/%s created\n", PROCFS_NAME);
	#endif
	return 0;
}

static void remove_procfs(void)
{
	remove_proc_entry(PROCFS_NAME, NULL);
	#ifdef IP_DEBUG
	printk(KERN_INFO "idleprobe: /proc/%s removed\n", PROCFS_NAME);
	#endif
}

static void* IP_seq_start(struct seq_file *s, loff_t *pos)
{
	struct list_head *list;
	
	if(s->private == NULL)
	{
		/* Beginning of reading session */
		struct timespec last_fetch;
		getnstimeofday(&last_fetch);
		spin_lock(&IP_list_lock);
		list = IP_list;
		IP_list = (struct list_head*) kmalloc(sizeof(struct list_head), GFP_KERNEL);
		INIT_LIST_HEAD(IP_list);
		last_fetch_timestamp = last_fetch.tv_sec;
		spin_unlock(&IP_list_lock);
		s->private = list;
	}
	else
	{
		list = s->private;
	}
	
	if(list_empty(list))
	{
		kfree(list);
		return NULL;
	}
	return list->next;
}

static void* IP_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct list_head *list = s->private;
	struct capture_list* tmp;
	
	if(list != list->next)
	{
		tmp = list_entry(list->next, struct capture_list, list);
		list_del(list->next);
		kfree(tmp);
	}
	
	if(list_empty(list))
	{
		return NULL;
	}
	else
	{
		return list->next;
	}
}

static void IP_seq_stop(struct seq_file *s, void *v)
{
	/* Nothing to do */
	return;
}

static u64 delta_to_ns(const delta_period_t* delta)
{
	return (delta->end.tv_sec - delta->begin.tv_sec)*1000000000 
	       + delta->end.tv_nsec - delta->begin.tv_nsec;
}

static int IP_seq_show(struct seq_file *s, void *v)
{
	/* 
	 * Output formatting function
	 * Called once per entry
	 */
	
	struct list_head *list = s->private;
	struct capture_list *entry = list_entry(list->next, struct capture_list, list);
	u64 highRes_delta;
	struct timespec timestamp_end = entry->entry.timestamp;
	
	highRes_delta = delta_to_ns(&entry->entry.highRes);
	timespec_add_ns(&timestamp_end, highRes_delta);
	
	seq_printf(s, "%d, %d, %llu, %lu.%09lu, %lu.%09lu\n", entry->count,
			   entry->entry.cpu, highRes_delta,
			   entry->entry.timestamp.tv_sec, entry->entry.timestamp.tv_nsec,
			   timestamp_end.tv_sec, timestamp_end.tv_nsec);
	return 0;
}

static int IP_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &IP_seq_ops);
};

static int __init init_idleprobe(void)
{
	/* 
	 * Module init function
	 */
	
	printk(KERN_INFO "idleprobe: Starting idleprobe module\n");
	
	/* Initialize data capture */
	init_capture();
	
	/* Register the begin jprobe */
	if(init_jprobe(&jp_begin) < 0)
	{
		cleanup_capture();
		return -1;
	}
	
	/* Register the end jprobe */
	if(init_jprobe(&jp_end) < 0)
	{
		remove_jprobe(&jp_begin);
		cleanup_capture();
		return -1;
	}
	
	/* Create proc entry */
	if(init_procfs() < 0)
	{
		remove_jprobe(&jp_end);
		remove_jprobe(&jp_begin);
		cleanup_capture();
		return -1;
	}
	return 0;
}

static void __exit cleanup_idleprobe(void)
{
	/* 
	 * Module exit function
	 */
	
	/* Unregister the jprobes */
	remove_jprobe(&jp_end);
	remove_jprobe(&jp_begin);
	
	/* Remove proc entry */
	remove_procfs();
	
	/* Cleanup data capture */
	cleanup_capture();
	
	printk(KERN_INFO "idleprobe: Exiting module\n");
}

module_init(init_idleprobe);
module_exit(cleanup_idleprobe);

MODULE_LICENSE("GPL");
MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
