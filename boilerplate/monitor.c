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
 * YOUR WORK: Fill in all sections marked // TODO.
 */
#include <linux/timer.h>
#include <linux/jiffies.h>
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
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ==============================================================
 * TODO 1: Define your linked-list node struct.
 * ============================================================== */
struct monitored_container {
    struct list_head    list;
    pid_t               pid;
    char                container_id[MONITOR_NAME_LEN];
    unsigned long       soft_limit_bytes;
    unsigned long       hard_limit_bytes;
    int                 soft_warned;    /* 1 once soft warning has been emitted */
};

/* ==============================================================
 * TODO 2: Declare the global monitored list and a lock.
 *
 * We use a mutex rather than a spinlock because both code paths
 * that touch the list (timer callback and ioctl handler) may
 * sleep: kmalloc(GFP_KERNEL) in the ioctl path can sleep, and
 * get_task_mm / mmput in the timer path acquire sleeping locks
 * internally.  A spinlock would be illegal in those contexts.
 * ============================================================== */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(monitored_list_lock);

/* --- Provided: internal device / timer state --- */
static struct timer_list monitor_timer;
static dev_t dev_num;
static struct cdev c_dev;
static struct class *cl;

/* ---------------------------------------------------------------
 * Provided: RSS Helper
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
 * Timer Callback
 * --------------------------------------------------------------- */
static void timer_callback(struct timer_list *t)
{
    /* ==============================================================
     * TODO 3: Implement periodic monitoring.
     * ============================================================== */
    struct monitored_container *mc, *tmp;

    /*
     * list_for_each_entry_safe allows us to delete the current node
     * (mc) during iteration because it saves the next pointer (tmp)
     * before we potentially free mc.
     */
    mutex_lock(&monitored_list_lock);
    list_for_each_entry_safe(mc, tmp, &monitored_list, list) {
        long rss = get_rss_bytes(mc->pid);
        printk(KERN_INFO "[monitor] pid=%d rss=%ld\n", mc->pid, rss);

        /* Process has exited — clean up its entry */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] pid=%d (%s) exited, removing entry\n",
                   mc->pid, mc->container_id);
            list_del(&mc->list);
            kfree(mc);
            continue;
        }

        /* Soft limit: warn once */
        if (!mc->soft_warned && (unsigned long)rss >= mc->soft_limit_bytes) {
            log_soft_limit_event(mc->container_id, mc->pid,
                                 mc->soft_limit_bytes, rss);
            mc->soft_warned = 1;
        }

        /* Hard limit: kill and remove */
        if ((unsigned long)rss >= mc->hard_limit_bytes) {
            kill_process(mc->container_id, mc->pid,
                         mc->hard_limit_bytes, rss);
            list_del(&mc->list);
            kfree(mc);
            /* mc is gone — do NOT touch it again; safe because we
             * are using list_for_each_entry_safe and tmp is intact */
        }
    }
    mutex_unlock(&monitored_list_lock);

    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ---------------------------------------------------------------
 * IOCTL Handler
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
        struct monitored_container *mc;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* ==============================================================
         * TODO 4: Add a monitored entry.
         * ============================================================== */

        /* Validate limits: soft must be strictly less than hard */
        if (req.soft_limit_bytes == 0 || req.hard_limit_bytes == 0 ||
            req.soft_limit_bytes >= req.hard_limit_bytes) {
            printk(KERN_ERR
                   "[container_monitor] Invalid limits: soft=%lu hard=%lu\n",
                   req.soft_limit_bytes, req.hard_limit_bytes);
            return -EINVAL;
        }

        mc = kmalloc(sizeof(*mc), GFP_KERNEL);
        if (!mc)
            return -ENOMEM;

        mc->pid              = req.pid;
        mc->soft_limit_bytes = req.soft_limit_bytes;
        mc->hard_limit_bytes = req.hard_limit_bytes;
        mc->soft_warned      = 0;
        strncpy(mc->container_id, req.container_id, MONITOR_NAME_LEN - 1);
        mc->container_id[MONITOR_NAME_LEN - 1] = '\0';
        INIT_LIST_HEAD(&mc->list);

        mutex_lock(&monitored_list_lock);
        list_add_tail(&mc->list, &monitored_list);
        mutex_unlock(&monitored_list_lock);

        return 0;
    }

    /* cmd == MONITOR_UNREGISTER */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    /* ==============================================================
     * TODO 5: Remove a monitored entry on explicit unregister.
     *
     * Match on both PID and container_id for safety: two containers
     * could theoretically share a PID across different namespaces,
     * so requiring both fields avoids false removals.
     * ============================================================== */
    {
        struct monitored_container *mc, *tmp;
        int found = 0;

        mutex_lock(&monitored_list_lock);
        list_for_each_entry_safe(mc, tmp, &monitored_list, list) {
            if (mc->pid == req.pid &&
                strncmp(mc->container_id, req.container_id,
                        MONITOR_NAME_LEN) == 0) {
                list_del(&mc->list);
                kfree(mc);
                found = 1;
                break;      /* PIDs are unique within our list */
            }
        }
        mutex_unlock(&monitored_list_lock);

        if (!found)
            return -ENOENT;
    }

    return 0;
}

/* --- Provided: file operations --- */
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

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* --- Provided: Module Exit --- */
static void __exit monitor_exit(void)
{
   timer_shutdown_sync(&monitor_timer);

    /* ==============================================================
     * TODO 6: Free all remaining monitored entries.
     * ============================================================== */
    {
        struct monitored_container *mc, *tmp;

        mutex_lock(&monitored_list_lock);
        list_for_each_entry_safe(mc, tmp, &monitored_list, list) {
            list_del(&mc->list);
            kfree(mc);
        }
        mutex_unlock(&monitored_list_lock);
    }

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
