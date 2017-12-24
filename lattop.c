#include "lattop.h"

/* parameters */
static struct kprobe enqueue_kp;
static struct kprobe dequeue_kp;
static DEFINE_HASHTABLE(my_hash, 14);
static DEFINE_HASHTABLE(pid_hash, 14);
static DEFINE_SPINLOCK(mr_lock);


unsigned long flags;
struct rb_root root;

uint64_t periodic_time;
uint64_t periodic_cycles;

int enqueue_kprobe;
int dequeue_kprobe;

struct proc_dir_entry *my_proc_entry;



/********************************************************************
 * proc file part
 ********************************************************************/
static int my_proc_show(struct seq_file *m, void *v) {
	int i;
	int printed_num = 0;
	struct tree_node *cur_tree_node;
	struct rb_node *cur_rb_node;
	struct hash_node *cur_hash_node;

	spin_lock_irqsave(&mr_lock, flags);
	cur_rb_node = rb_last(&root);
	while (cur_rb_node && printed_num < 1000) {
		printed_num += 1;
		cur_tree_node = rb_entry(cur_rb_node, struct tree_node, node);

		/* get tree node, print hash node */
		cur_hash_node = cur_tree_node->inter_node;
		seq_printf(m, "## pid = %d, comm = %s, cycles = %llu\n", \
				cur_hash_node->key->pid, cur_hash_node->comm, \
				cur_hash_node->total_time);
		for (i = 0; i < cur_hash_node->key->stack.nr_entries; i++) {
			if (cur_hash_node->key->stack.entries[i] == 0xffffffffffffffff) {
				break;
			} 
			seq_printf(m, "\t%pB\n", \
					(void *) cur_hash_node->key->stack.entries[i]);

		}

		cur_rb_node = rb_prev(cur_rb_node);
		seq_printf(m, "\n");
	}
	
	spin_unlock_irqrestore(&mr_lock, flags);
	return 0;
}

static int my_proc_open(struct inode *inode, struct file *file) {
	return single_open(file, my_proc_show, NULL);
}

static const struct file_operations proc_fops = {
	.owner = THIS_MODULE,
	.open = my_proc_open,
	.llseek = seq_lseek,
	.release = single_release,
	.read = seq_read,
};


/********************************************************************
 * hash table part
 ********************************************************************/
/**
 * print out hash node information
 * @node: Be prited node.
 */
void print_hash_node(struct hash_node *node) {
	printk("## pid = %d, comm = %s, cycles = %llu", node->key->pid, \
			node->comm, node->total_time);
	print_stack_trace(&node->key->stack, 0);
}



/**
 * hash table search node function
 * @key: hash key
 * @return: if found, return hash_node, otherwise, return NULL
 */
struct hash_node* search_node(struct hash_key *key) {
	struct hash_node *cur_node;
	unsigned int hash_code;
	int i, bkt;
	int is_same = -1;

	hash_code = jhash(key, sizeof(struct hash_key*), 0);
	hash_for_each(my_hash, bkt, cur_node, hash) {
		if(key->pid == cur_node->key->pid) {
			is_same = 0;
			for (i = 0; i < cur_node->key->stack.nr_entries; i++) {
					if (key->stack.entries[i] != 
							cur_node->key->stack.entries[i]) {
						is_same = -1;
						break;
					}
			}
			if (is_same == 0) {
				return cur_node;
			}
		} 
	}
	return NULL;
}


/* remove all node, and release memory */
void hash_table_remove_all_node(void) {
	struct hash_node *node;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(my_hash, bkt, tmp, node, hash) {
		/* free key memory first, then hash node memory */
		kfree(node->key);
		hash_del(&node->hash);
		kfree(node);
	}
	printk(KERN_INFO "--- remove all node & destruct hash table ---\n");
}
/********************************************************************
 * pid hash table part
 ********************************************************************/
struct pid_hash_node* search_pid_node(pid_t pid) {
	struct pid_hash_node *cur_node;

	hash_for_each_possible(pid_hash, cur_node, hash, pid) {
		if (pid == cur_node->pid) {
			return cur_node;
		} 
	}
	return NULL;
}

void delete_pid_node(pid_t pid) {
	struct pid_hash_node *cur_node;
	struct hlist_node *tmp;
	hash_for_each_possible_safe(pid_hash, cur_node, tmp, hash, pid) {
		if (pid == cur_node->pid) {
			hash_del(&cur_node->hash);
			kfree(cur_node);
		}
	}

}

/**
 * remove all node, and release memory
 */
void remove_all_pid_node(void) {
	struct pid_hash_node *node;
	struct hlist_node *tmp;
	int bkt;

	printk(KERN_INFO "--- remove all node & destruct pid hash table ---\n");
	hash_for_each_safe(pid_hash, bkt, tmp, node, hash) {
		hash_del(&node->hash);
		kfree(node);
	}
}

/********************************************************************
 * red black tree functions
 * *****************************************************************/
/**
 * tree node insert function 
 * @node: insert node
 * @root: rb_tree root
 */
int tree_node_insert(struct tree_node *node, struct rb_root *root) {
	struct rb_node **temp = &(root->rb_node);
	struct rb_node *parent = NULL;
	struct tree_node *cur_tree_node;

	/* find the insert node parent location */
	while (*temp) {
		parent = *temp;
		cur_tree_node = rb_entry(parent, struct tree_node, node);
		if (cur_tree_node->total_time > node->total_time) {
			temp = &(parent->rb_left);	
		}
		else {
			temp = &(parent->rb_right);
		}
	}

	/* set parent and color */
	rb_link_node(&node->node, parent, temp);
	rb_insert_color(&node->node, root);
	return 0;
}


/**
 * print out the tree from large to less
 * @num_to_print: print node number, if total tree node number less than 
 * 				  num_to_print, print out whole tree, and exit
 */
void print_tree(struct rb_root *root, int num_to_print) {
	int printed_num = 0;
	struct tree_node *cur_tree_node;
	struct rb_node *cur_rb_node;
	cur_rb_node = rb_last(root);
	
	while (cur_rb_node && printed_num < num_to_print) {
		printed_num += 1;
		cur_tree_node = rb_entry(cur_rb_node, struct tree_node, node);
		print_hash_node(cur_tree_node->inter_node);
		cur_rb_node = rb_prev(cur_rb_node);
	}
}



/**
 * search if val inside of the rb_tree
 * @root: rb_root node
 * @val: searched key
 * @node: searched value
 * @return: searched result
 */
struct tree_node* tree_node_search(struct rb_root *root, uint64_t val, \
		struct hash_node *node) {
	struct tree_node *res;
	struct rb_node **temp, *parent, *prev, *next;
	unsigned int tree_node_hash_code, searched_hash_node;


	searched_hash_node = jhash(node->key, sizeof(struct hash_key*), 0);
	temp = &(root->rb_node);
	while (*temp) {
		parent = *temp;
		res = rb_entry(parent, struct tree_node, node);
		if (res->total_time > val) {
			temp = &(parent->rb_left);
		}
		else if (res->total_time < val) {
			temp = &(parent->rb_right);
		}
		else {
			tree_node_hash_code = jhash(res->inter_node->key, 
					sizeof(struct hash_key*), 0);
			if (tree_node_hash_code == searched_hash_node) {
				return res;
			}

			prev = rb_prev(parent);
			while (prev) {
				res = rb_entry(prev, struct tree_node, node);
				if (res->total_time != val) {
					break;
				}
				tree_node_hash_code = jhash(res->inter_node->key, 
						sizeof(struct hash_key*), 0);
				if (tree_node_hash_code == searched_hash_node) {
					return res;
				}
				prev = rb_prev(prev);
			}

			next = rb_next(parent);
			while (next) {
				res = rb_entry(next, struct tree_node, node);
				if (res->total_time != val) {
					break;
				}
				tree_node_hash_code = jhash(res->inter_node->key, 
						sizeof(struct hash_key*), 0);
				if (tree_node_hash_code == searched_hash_node) {
					return res;
				}
				next = rb_next(next);
			}

			return NULL;
		}
	}
	return NULL;
}


/**
 * delete tree node, and free memory
 * @root: rb tree root
 * @node: need to delete node
 */
void tree_node_delete(struct rb_root *root, struct tree_node *delete_node) {
	rb_erase(&delete_node->node, root);
	kfree(delete_node);
}


/**
 * delete all tree node, and free memory
 * @root: rb tree root
 */
void rb_tree_remove_all_node(struct rb_root *root) {
	struct rb_node *cur;
	struct tree_node *cur_tree_node;

	/* tree node inter node directly point to hash table node, 
	 * so don't need to free hash_node memory, hash_node memory 
	 * deallocate will be handled by hash_table_remove_all_node function
	 */
	while ((cur = rb_first(root))) {
		cur_tree_node = rb_entry(cur, struct tree_node, node);
		rb_erase(cur, root);	
		kfree(cur_tree_node);
	}
	printk(KERN_INFO "--- remove all node & destruct rb_tree ---\n");
}


/********************************************************************
 * Module.
 * This part uses for detect sleep, wake up task, 
 * and calculate lantency time
 * *****************************************************************/
/* wake up */
static int enqueue_handler_pre(struct kprobe *p, struct pt_regs *regs) {
	struct hash_key *key;
	struct hash_node *cur_hash_node;
	struct pid_hash_node *pid_node;
	struct tree_node *cur_tree_node, *need_delete_tree_node;
			
	uint64_t sleep_time, total_time;
	unsigned int hash_code;

	pid_node = search_pid_node(current->pid);
	if (pid_node) {
		sleep_time = rdtsc() - pid_node->start_time;

		/* delete pid_node, and free memory, 
		 * but key object didn't deallocate
		 */
		key = pid_node->key;
		delete_pid_node(pid_node->pid);

		cur_hash_node = search_node(key);
		if (!cur_hash_node) {
			/* hash node doesn't exist, hash key will be used inside of hash
			 * node, don't need free key variable memory
			 */
			cur_hash_node = kmalloc(sizeof(*cur_hash_node), GFP_ATOMIC);
			if (!cur_hash_node) {
				kfree(key);
				return -ENOMEM;
			}
				
			strncpy(cur_hash_node->comm, current->comm, 20);
			cur_hash_node->total_time = sleep_time;
			cur_hash_node->key = key;
			
			hash_code = jhash(key, sizeof(struct hash_key*), 0);
			hash_add(my_hash, &cur_hash_node->hash, hash_code);

			cur_tree_node = kmalloc(sizeof(*cur_tree_node), GFP_ATOMIC);
			if (!cur_tree_node) {
				/* do not need free hash_node and key node,
				 * since key node and hash node still in hash table
				 */
				cur_hash_node->in_tree = 0;
				return -ENOMEM;
			}
			
			cur_hash_node->in_tree = 1;
			cur_tree_node->total_time = cur_hash_node->total_time;
			cur_tree_node->inter_node = cur_hash_node;	
			tree_node_insert(cur_tree_node, &root);
		}
		else {
			/* hash node exist, hash node already hold one hash key,
			 * so free current key memory
			 */
			kfree(key);
			if (cur_hash_node->in_tree == 0) {
				cur_hash_node->total_time = sleep_time + \
											cur_hash_node->total_time;

				cur_tree_node = kmalloc(sizeof(*cur_tree_node), GFP_ATOMIC);
				if (!cur_tree_node) {
					return -ENOMEM;
				}
				cur_tree_node->total_time = cur_hash_node->total_time;
				cur_tree_node->inter_node = cur_hash_node;	
				tree_node_insert(cur_tree_node, &root);

				cur_hash_node->in_tree = 1;
			}
			else {
				total_time = cur_hash_node->total_time;
				need_delete_tree_node = tree_node_search(&root, total_time, \
					 cur_hash_node);
				if (need_delete_tree_node) {
					tree_node_delete(&root, need_delete_tree_node);
					cur_hash_node->total_time = total_time + sleep_time;

					cur_tree_node = kmalloc(sizeof(*cur_tree_node), GFP_ATOMIC);
					if (!cur_tree_node) {
						/* previous node is deleted, 
						 * but new node didn't insert
						 */
						cur_hash_node->in_tree = 0;
						return -ENOMEM;
					}
					cur_tree_node->total_time = cur_hash_node->total_time;
					cur_tree_node->inter_node = cur_hash_node;	
					tree_node_insert(cur_tree_node, &root);
				}
			}
		}
	}
	return 0;
}

/* sleep task */
static int dequeue_handler_pre(struct kprobe *p, struct pt_regs *regs) {
	struct pid_hash_node *node;
	uint64_t start_time;
	struct hash_key *key;

	key = kmalloc(sizeof(*key), GFP_ATOMIC);
	if (!key) {
		return -ENOMEM;
	}

	key->stack.nr_entries =  0;
	key->stack.max_entries = MAX_STACK;
	key->stack.skip = 0;
	key->stack.entries = key->stack_entries;
	memset(&key->stack_entries, 0, sizeof(key->stack_entries));
	if (current->mm) {
		save_stack_trace_user(&key->stack);
	}
	else {
		save_stack_trace(&key->stack);
	}
	key->pid = current->pid;	

	node = search_pid_node(current->pid);
	if (node) {
		start_time = rdtsc();
		node->start_time = start_time;	
		/* pid_hash_node exist, update pid_hash_node data, 
		 * free previous key object and update to new key object
		 */
		kfree(node->key);
		node->key = key;
	}
	else {
		node = kmalloc(sizeof(*node), GFP_ATOMIC);
		if (!node) {
			/* key is not used by any data structure*/
			kfree(key);
			return -ENOMEM;
		}
		start_time = rdtsc();
		node->start_time = start_time;
		node->pid = current->pid;
		node->key = key;
		hash_add(pid_hash, &node->hash, node->pid);
	}
	return 0;
}

static int __init lattop_init(void) {
	enqueue_kprobe = -1;
	dequeue_kprobe = -1;
	periodic_time = 0;
	// 2700 MHz cpu. 2700 * 10 ^ 6 * 10
	periodic_cycles = 27000000000;

	printk(KERN_INFO "*** Load latency profiler module ***\n");
	root = RB_ROOT;

	dequeue_kp.pre_handler = dequeue_handler_pre;
	dequeue_kp.addr = (kprobe_opcode_t *) 
			kallsyms_lookup_name("deactivate_task");
	dequeue_kprobe = register_kprobe(&dequeue_kp);
	if (dequeue_kprobe < 0) {
		pr_err("register_kprobe failed, returned %d\n", dequeue_kprobe);
		return dequeue_kprobe;
	}

	enqueue_kp.pre_handler = enqueue_handler_pre;
	enqueue_kp.addr = (kprobe_opcode_t *) 
			kallsyms_lookup_name("activate_task");
	enqueue_kprobe = register_kprobe(&enqueue_kp);
	if (enqueue_kprobe < 0) {
		pr_err("register_kprobe failed, returned %d\n", enqueue_kprobe);
		return enqueue_kprobe;
	}
	
	my_proc_entry = proc_create("lattop", 0666, NULL, &proc_fops);
	if (!my_proc_entry) {
		printk("Unable to create /proc/lattop\n");
		return -ENOMEM;
	}
	return 0;
}

static void __exit lattop_exit(void) {
	if (enqueue_kprobe >= 0) {
		unregister_kprobe(&enqueue_kp);
	}
	if (dequeue_kprobe >= 0) {
		unregister_kprobe(&dequeue_kp);
	}
	if (my_proc_entry) {
		remove_proc_entry("lattop", NULL);	
	}
	remove_all_pid_node();
	hash_table_remove_all_node();
	rb_tree_remove_all_node(&root);

	printk(KERN_INFO "*** Unload latency profiler module ***\n");
	return;
}

module_init(lattop_init);
module_exit(lattop_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dong Wang");
MODULE_DESCRIPTION("Latency profiler");
