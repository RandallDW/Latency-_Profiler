#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/jhash.h>
#include <linux/module.h>
#include <linux/hashtable.h>
#include <linux/kprobes.h>
#include <asm/processor.h>
#include <linux/spinlock.h>
#include <linux/stacktrace.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>

#include <linux/sched.h>
#include <linux/export.h>
#include <linux/uaccess.h>
#include <asm/stacktrace.h>
#include <asm/unwind.h>


#define MAX_STACK 32
#define BUFSIZE 2048



/* hash table functions and structs */
/* hash key */
struct hash_key {
	pid_t pid;
	struct stack_trace stack;
	unsigned long stack_entries[MAX_STACK];
};

/* hash node functions */
struct hash_node {
	struct hlist_node hash;
	struct hash_key *key;
	char comm[20];
	uint64_t total_time;
	int in_tree;
};

void print_hash_node(struct hash_node *);
struct hash_node* search_node(struct hash_key *);
void hash_table_remove_all_node(void);

/* pid hash table */
struct pid_hash_node {
	struct hlist_node hash;
	uint64_t start_time;
	pid_t pid;
	struct hash_key *key;
};

void delete_pid_node(pid_t pid);
void remove_all_pid_node(void);
struct pid_hash_node* search_pid_node(pid_t pid);


/* rb tree functions and structs */
/* rb tree node */
struct tree_node {
	struct rb_node node;
	uint64_t total_time;
	struct hash_node *inter_node;
};

int tree_node_insert(struct tree_node *, struct rb_root *);
void print_tree(struct rb_root *, int);
struct tree_node* tree_node_search(struct rb_root *, uint64_t,
		struct hash_node *);
void tree_node_delete(struct rb_root *, struct tree_node *);
void rb_tree_remove_all_node(struct rb_root *);

struct stack_frame_user {
	const void __user	*next_fp;
	unsigned long		ret_addr;
};

static int
copy_stack_frame(const void __user *fp, struct stack_frame_user *frame)
{
	int ret;

	if (!access_ok(VERIFY_READ, fp, sizeof(*frame)))
		return 0;

	ret = 1;
	pagefault_disable();
	if (__copy_from_user_inatomic(frame, fp, sizeof(*frame)))
		ret = 0;
	pagefault_enable();

	return ret;
}

static inline void __save_stack_trace_user(struct stack_trace *trace)
{
	const struct pt_regs *regs = task_pt_regs(current);
	const void __user *fp = (const void __user *)regs->bp;

	if (trace->nr_entries < trace->max_entries)
		trace->entries[trace->nr_entries++] = regs->ip;

	while (trace->nr_entries < trace->max_entries) {
		struct stack_frame_user frame;

		frame.next_fp = NULL;
		frame.ret_addr = 0;
		if (!copy_stack_frame(fp, &frame))
			break;
		if ((unsigned long)fp < regs->sp)
			break;
		if (frame.ret_addr) {
			trace->entries[trace->nr_entries++] =
				frame.ret_addr;
		}
		if (fp == frame.next_fp)
			break;
		fp = frame.next_fp;
	}
}

void save_stack_trace_user(struct stack_trace *trace)
{
	/*
	 * Trace user stack if we are not a kernel thread
	 */
	if (current->mm) {
		__save_stack_trace_user(trace);
	}
	if (trace->nr_entries < trace->max_entries)
		trace->entries[trace->nr_entries++] = ULONG_MAX;
}
