/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Provided boilerplate:
 *   - device registration and teardown
 *   - timer setup
 *   - RSS helper
 *   - soft-limit and hard-limit event helpers
 *   - ioctl dispatch shell
 *
 * TODO implemented: All five TODO sections filled in —
 *   - linked-list node struct with PID, container ID, limits, soft_warned flag
 *   - global LIST_HEAD + mutex protecting all list access paths
 *   - timer callback: per-entry RSS check, soft-limit one-shot warn, hard-limit SIGKILL + removal
 *   - MONITOR_REGISTER ioctl: kmalloc node, validate limits, insert under lock
 *   - MONITOR_UNREGISTER ioctl: find by PID or container_id, remove + kfree under lock
 *   - module_exit: drain entire list safely before device teardown
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1 implemented: Linked-list node struct for a tracked container.
 * Stores the host PID, null-terminated container ID string, soft and hard
 * memory limits in bytes, a soft_warned flag (set to 1 after first warning
 * to suppress duplicates), and a list_head for kernel list linkage.
 * ============================================================== */
struct monitored_container {
    pid_t pid;
    char container_id[32];

    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;

    int soft_warned;  // 0 = not warned, 1 = already warned

    struct list_head list;
};

/* ==============================================================
 * TODO 2 implemented: Global container list and mutex.
 * LIST_HEAD(container_list) is the shared doubly-linked list of all
 * monitored entries. DEFINE_MUTEX(container_lock) protects all insert,
 * remove, and iteration operations across the ioctl and timer code paths.
 * Mutex chosen over spinlock because get_rss_bytes() may sleep (mmput/put_task_struct).
 * ============================================================== */
static LIST_HEAD(container_list);
static DEFINE_MUTEX(container_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
 *
 * Returns the Resident Set Size in bytes for the given PID,
 * or -1 if the task no longer exists.
 * --------------------------------------------------------------- */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
      rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ---------------------------------------------------------------
 * Provided: soft-limit helper
 *
 * Log a warning when a process exceeds the soft limit.
 * --------------------------------------------------------------- */
static void log_soft_limit_event(const char *container_id,
                                 pid_t pid,
                                 unsigned long limit_bytes,
                                 long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Provided: hard-limit helper
 *
 * Kill a process when it exceeds the hard limit.
 * --------------------------------------------------------------- */
static void kill_process(const char *container_id,
                         pid_t pid,
                         unsigned long limit_bytes,
                         long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ---------------------------------------------------------------
 * Timer Callback - fires every CHECK_INTERVAL_SEC seconds.
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3 implemented: Periodic RSS monitoring callback (fires every 1s).
     * Acquires container_lock and iterates with list_for_each_entry_safe so
     * nodes can be deleted mid-loop. For each entry: calls get_rss_bytes();
     * if the process has exited (rss <= 0) the node is removed and freed.
     * If RSS exceeds soft_limit and soft_warned is 0, logs a KERN_WARNING and
     * sets soft_warned = 1 (one-shot). If RSS exceeds hard_limit, sends SIGKILL,
     * removes and frees the node. Restarts the timer via mod_timer at the end.
     * ============================================================== */

struct monitored_container *node, *tmp;

    mutex_lock(&container_lock);

    list_for_each_entry_safe(node, tmp, &container_list, list) {

        long rss = get_rss_bytes(node->pid);

        // If process exited → remove
        if (rss <= 0) {
            list_del(&node->list);
            kfree(node);
            continue;
        }

       
        printk(KERN_INFO "[monitor] PID=%d RSS=%ld bytes\n", node->pid, rss);

        // Soft limit check
        if (rss > node->soft_limit_bytes && !node->soft_warned) {
            printk(KERN_WARNING
                   "SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
                   node->container_id,
                   node->pid,
                   rss,
                   node->soft_limit_bytes);

            node->soft_warned = 1;
        }

        // Hard limit check
        if (rss > node->hard_limit_bytes) {
            printk(KERN_ERR
                   "HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
                   node->container_id,
                   node->pid,
                   rss,
                   node->hard_limit_bytes);

            // Kill process
            struct task_struct *task = pid_task(find_vpid(node->pid), PIDTYPE_PID);
            if (task)
                send_sig(SIGKILL, task, 0);

            list_del(&node->list);
            kfree(node);
        }
    }

    mutex_unlock(&container_lock);

    // Restart timer
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);



}

/* ---------------------------------------------------------------
 * IOCTL Handler
 *
 * Supported operations:
 *   - register a PID with soft + hard limits
 *   - unregister a PID when the runtime no longer needs tracking
 * --------------------------------------------------------------- */
static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    if (cmd == MONITOR_REGISTER) {
        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid, req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4 implemented: MONITOR_REGISTER — add a new monitored entry.
         * Validates that soft and hard limits are non-zero and soft <= hard.
         * kmalloc's a zeroed node, copies pid/container_id/limits from req,
         * sets soft_warned = 0, then inserts at list head under container_lock.
         * Returns -EINVAL for bad limits, -ENOMEM on alloc failure, 0 on success.
         * ============================================================== */

	struct monitored_container *node;

	if (req.soft_limit_bytes == 0 || req.hard_limit_bytes == 0 ||
    		req.soft_limit_bytes > req.hard_limit_bytes) {
   	 return -EINVAL;
	}

	node = kmalloc(sizeof(*node), GFP_KERNEL);
	if (!node)
   	 return -ENOMEM;

	memset(node, 0, sizeof(*node));

	node->pid = req.pid;
	strncpy(node->container_id, req.container_id, sizeof(node->container_id) - 1);

	node->soft_limit_bytes = req.soft_limit_bytes;
	node->hard_limit_bytes = req.hard_limit_bytes;
	node->soft_warned = 0;

	// Add to list
	mutex_lock(&container_lock);
	list_add(&node->list, &container_list);
	mutex_unlock(&container_lock);

        return 0;
    }

    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5 implemented: MONITOR_UNREGISTER — remove a tracked entry.
     * Acquires container_lock and iterates with list_for_each_entry_safe.
     * Matches on PID (if non-zero) or container_id (if non-empty string).
     * On first match: list_del + kfree, sets found = 1 and breaks.
     * Returns 0 if an entry was removed, -ENOENT if no match was found.
     * ============================================================== */

struct monitored_container *node, *tmp;
int found = 0;

mutex_lock(&container_lock);

list_for_each_entry_safe(node, tmp, &container_list, list) {
    if ((req.pid != 0 && node->pid == req.pid) ||
        (strlen(req.container_id) > 0 &&
         strcmp(node->container_id, req.container_id) == 0)) {

        list_del(&node->list);
        kfree(node);
        found = 1;
        break;
    }
}

mutex_unlock(&container_lock);

return found ? 0 : -ENOENT;


}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* --- Provided: Module Init --- */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n", DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
    del_timer_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6 implemented: Cleanup all remaining list entries on module unload.
     * After del_timer_sync ensures no timer callback is running, acquires
     * container_lock and uses list_for_each_entry_safe to walk the full list,
     * calling list_del + kfree on every remaining node. Leaves no leaked memory.
     * ============================================================== */

	struct monitored_container *node, *tmp;

	mutex_lock(&container_lock);

	list_for_each_entry_safe(node, tmp, &container_list, list) {
    	list_del(&node->list);
    	kfree(node);
	}

	mutex_unlock(&container_lock);


    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
