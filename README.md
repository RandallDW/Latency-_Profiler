# Latency Profiler

## Introduction
In this project, we developed a latency profiler, which measures and accumulates time that a task starts sleep and wakes up. A task would sleep due to various reasons including blocking IO operations (e.g., disk, network) or contention on synchronization primitives (e.g., mutex, semaphore). In many cases, such long sleeping causes high latency in application behavior. Our latency profiler helps to find such latency bottleneck by measuring sleeping time of all tasks in a system.

## Running Environment
    * Linux Kernel v4.12

## Approach
    * Measuring latency -- The unit of measurement is CPU clock cycles
    * Call stack (kernel/user-space) -- Maintain sleeping time infomation with each pid and call stack combination
    * Print out result -- Print out 1000 longest time results with proc file system

## Design
### Related Libraries
	* kprobe
	* proc file system
	* stack trace
### Design Structure
#### Monitor Task Sleep And Weak Up Time
    Related functions ----- dequeue_entity() < remove a task to run queue >
                        |
                        --- enqueue_entity() < add a task to run queue >
Ideally, we should use kprobe to monitor these two functions to get task sleeping and weaking up time.
Howerver, since these two functions are static funcitons which are unable monitor, and the functions caller order looks like,

        deactivate_task() --> dequeue_task_fair() --> dequeuer_entity()
        (none static)         (static)                (static)

        activate_task()   --> enqueue_task_fair() --> enqueuer_entity()
        (none static)         (static)                (static)

So, we used kprobe to monitor deactivate_task() and activate_task() functions to get task sleep and weak up time.

#### Record Task Info
    ----------------------------------------------------
    |  Task Sleep  | Create/Update Timer/PID Hash Node |
    ----------------------------------------------------
    |              |      Delete Timer Hash Node       |
    |  Task Wakeup | Create/Update Task Info Hash Node |
    |              |    Insert/Update RB_Tree Node     |
    ----------------------------------------------------

### Data Structure
    * Hash Table     ------- Timer hash table
                       |
                       ----- Task info hash table
    * Red Black Tree ------- Task info rb_tree, sorted by accumulated time 
#### Hash Key
    /* MAX_STACK 32 */
    struct hash_key {
        pid_t pid;                              /* task pid */
        struct stack_trace stack;               /* task stack trace */
        unsigned long stack_entries[MAX_STACK]; /* stack entries */
    };

![alt text](https://github.com/RandallDW/Latency_Profiler/blob/master/blank_diagram.png "hash_key blank diagram")
#### Timer/PID Hash Table
    struct pid_hash_node {
        struct hlist_node hash; 
        uint64_t start_time;    /* task start sleeping time */
        pid_t pid;              /* task pid */
        struct hash_key *key;   /* task hash key */
    };
#### Task Info Hash Table
    struct hash_node {
        struct hlist_node hash;
        struct hash_key *key;   /* hash key */
        char comm[20];          /* task function name */
        uint64_t total_time;    /* accumulated sleeping time */
        int in_tree;            /* if this node is added into rb_tree */
    };
#### Task Info RB_Tree
    struct tree_node {
        struct rb_node node;
        uint64_t total_time;            /* accumulated sleeping time */
        struct hash_node *inter_node;   /* cooresponding hash node in task info hash table */
    };

### Execute Program
    - $ make
    - $ sudo insmod xcfs
    - $ cat /proc/lattop (print out results)
