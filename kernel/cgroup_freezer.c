/*
 * cgroup_freezer.c -  control group freezer subsystem
 *
 * Copyright IBM Corporation, 2007
 *
 * Author : Cedric Le Goater <clg@fr.ibm.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2.1 of the GNU Lesser General Public License
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 */

#include <linux/module.h>
#include <linux/cgroup.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/freezer.h>
#include <linux/seq_file.h>

enum freezer_state {
	STATE_RUNNING = 0,
	STATE_FREEZING,
	STATE_FROZEN,
};

struct freezer {
	struct cgroup_subsys_state css;
	enum freezer_state state;
	spinlock_t lock; /* protects _writes_ to state */
};

static inline struct freezer *cgroup_freezer(
		struct cgroup *cgroup)
{
	return container_of(
		cgroup_subsys_state(cgroup, freezer_subsys_id),
		struct freezer, css);
}

static inline struct freezer *task_freezer(struct task_struct *task)
{
	return container_of(task_subsys_state(task, freezer_subsys_id),
			    struct freezer, css);
}

int cgroup_frozen(struct task_struct *task)
{
	struct freezer *freezer;
	enum freezer_state state;

	task_lock(task);
	freezer = task_freezer(task);
	state = freezer->state;
	task_unlock(task);

	return state == STATE_FROZEN;
}

/*
 * cgroups_write_string() limits the size of freezer state strings to
 * CGROUP_LOCAL_BUFFER_SIZE
 */
static const char *freezer_state_strs[] = {
	"RUNNING",
	"FREEZING",
	"FROZEN",
};

/*
 * State diagram
 * Transitions are caused by userspace writes to the freezer.state file.
 * The values in parenthesis are state labels. The rest are edge labels.
 *
 * (RUNNING) --FROZEN--> (FREEZING) --FROZEN--> (FROZEN)
 *    ^ ^                     |                       |
 *    | \_______RUNNING_______/                       |
 *    \_____________________________RUNNING___________/
 */

struct cgroup_subsys freezer_subsys;

/* Locks taken and their ordering
 * ------------------------------
 * css_set_lock
 * cgroup_mutex (AKA cgroup_lock)
 * task->alloc_lock (AKA task_lock)
 * freezer->lock
 * task->sighand->siglock
 *
 * cgroup code forces css_set_lock to be taken before task->alloc_lock
 *
 * freezer_create(), freezer_destroy():
 * cgroup_mutex [ by cgroup core ]
 *
 * can_attach():
 * cgroup_mutex
 *
 * cgroup_frozen():
 * task->alloc_lock (to get task's cgroup)
 *
 * freezer_fork() (preserving fork() performance means can't take cgroup_mutex):
 * task->alloc_lock (to get task's cgroup)
 * freezer->lock
 *  sighand->siglock (if the cgroup is freezing)
 *
 * freezer_read():
 * cgroup_mutex
 *  freezer->lock
 *   read_lock css_set_lock (cgroup iterator start)
 *
 * freezer_write() (freeze):
 * cgroup_mutex
 *  freezer->lock
 *   read_lock css_set_lock (cgroup iterator start)
 *    sighand->siglock
 *
 * freezer_write() (unfreeze):
 * cgroup_mutex
 *  freezer->lock
 *   read_lock css_set_lock (cgroup iterator start)
 *    task->alloc_lock (to prevent races with freeze_task())
 *     sighand->siglock
 */
static struct cgroup_subsys_state *freezer_create(struct cgroup_subsys *ss,
						  struct cgroup *cgroup)
{
	struct freezer *freezer;

	freezer = kzalloc(sizeof(struct freezer), GFP_KERNEL);
	if (!freezer)
		return ERR_PTR(-ENOMEM);

	spin_lock_init(&freezer->lock);
	freezer->state = STATE_RUNNING;
	return &freezer->css;
}

static void freezer_destroy(struct cgroup_subsys *ss,
			    struct cgroup *cgroup)
{
	kfree(cgroup_freezer(cgroup));
}


static int freezer_can_attach(struct cgroup_subsys *ss,
			      struct cgroup *new_cgroup,
			      struct task_struct *task)
{
	struct freezer *freezer;
	int retval = 0;

	/*
	 * The call to cgroup_lock() in the freezer.state write method prevents
	 * a write to that file racing against an attach, and hence the
	 * can_attach() result will remain valid until the attach completes.
	 */
	freezer = cgroup_freezer(new_cgroup);
	if (freezer->state == STATE_FROZEN)
		retval = -EBUSY;
	return retval;
}

static void freezer_fork(struct cgroup_subsys *ss, struct task_struct *task)
{
	struct freezer *freezer;

	task_lock(task);
	freezer = task_freezer(task);
	task_unlock(task);

	BUG_ON(freezer->state == STATE_FROZEN);
	spin_lock_irq(&freezer->lock);
	/* Locking avoids race with FREEZING -> RUNNING transitions. */
	if (freezer->state == STATE_FREEZING)
		freeze_task(task, true);
	spin_unlock_irq(&freezer->lock);
}

/*
 * caller must hold freezer->lock
 */
static void check_if_frozen(struct cgroup *cgroup,
			     struct freezer *freezer)
{
	struct cgroup_iter it;
	struct task_struct *task;
	unsigned int nfrozen = 0, ntotal = 0;

	cgroup_iter_start(cgroup, &it);
	while ((task = cgroup_iter_next(cgroup, &it))) {
		ntotal++;
		/*
		 * Task is frozen or will freeze immediately when next it gets
		 * woken
		 */
		if (frozen(task) ||
		    (task_is_stopped_or_traced(task) && freezing(task)))
			nfrozen++;
	}

	/*
	 * Transition to FROZEN when no new tasks can be added ensures
	 * that we never exist in the FROZEN state while there are unfrozen
	 * tasks.
	 */
	if (nfrozen == ntotal)
		freezer->state = STATE_FROZEN;
	cgroup_iter_end(cgroup, &it);
}

static int freezer_read(struct cgroup *cgroup, struct cftype *cft,
			struct seq_file *m)
{
	struct freezer *freezer;
	enum freezer_state state;

	if (!cgroup_lock_live_group(cgroup))
		return -ENODEV;

	freezer = cgroup_freezer(cgroup);
	spin_lock_irq(&freezer->lock);
	state = freezer->state;
	if (state == STATE_FREEZING) {
		/* We change from FREEZING to FROZEN lazily if the cgroup was
		 * only partially frozen when we exitted write. */
		check_if_frozen(cgroup, freezer);
		state = freezer->state;
	}
	spin_unlock_irq(&freezer->lock);
	cgroup_unlock();

	seq_puts(m, freezer_state_strs[state]);
	seq_putc(m, '\n');
	return 0;
}

static int try_to_freeze_cgroup(struct cgroup *cgroup, struct freezer *freezer)
{
	struct cgroup_iter it;
	struct task_struct *task;
	unsigned int num_cant_freeze_now = 0;

	freezer->state = STATE_FREEZING;
	cgroup_iter_start(cgroup, &it);
	while ((task = cgroup_iter_next(cgroup, &it))) {
		if (!freeze_task(task, true))
			continue;
		if (task_is_stopped_or_traced(task) && freezing(task))
			/*
			 * The freeze flag is set so these tasks will
			 * immediately go into the fridge upon waking.
			 */
			continue;
		if (!freezing(task) && !freezer_should_skip(task))
			num_cant_freeze_now++;
	}
	cgroup_iter_end(cgroup, &it);

	return num_cant_freeze_now ? -EBUSY : 0;
}

static int unfreeze_cgroup(struct cgroup *cgroup, struct freezer *freezer)
{
	struct cgroup_iter it;
	struct task_struct *task;

	cgroup_iter_start(cgroup, &it);
	while ((task = cgroup_iter_next(cgroup, &it))) {
		int do_wake;

		task_lock(task);
		do_wake = __thaw_process(task);
		task_unlock(task);
		if (do_wake)
			wake_up_process(task);
	}
	cgroup_iter_end(cgroup, &it);
	freezer->state = STATE_RUNNING;

	return 0;
}

static int freezer_change_state(struct cgroup *cgroup,
				enum freezer_state goal_state)
{
	struct freezer *freezer;
	int retval = 0;

	freezer = cgroup_freezer(cgroup);
	spin_lock_irq(&freezer->lock);
	check_if_frozen(cgroup, freezer); /* may update freezer->state */
	if (goal_state == freezer->state)
		goto out;
	switch (freezer->state) {
	case STATE_RUNNING:
		retval = try_to_freeze_cgroup(cgroup, freezer);
		break;
	case STATE_FREEZING:
		if (goal_state == STATE_FROZEN) {
			/* Userspace is retrying after
			 * "/bin/echo FROZEN > freezer.state" returned -EBUSY */
			retval = try_to_freeze_cgroup(cgroup, freezer);
			break;
		}
		/* state == FREEZING and goal_state == RUNNING, so unfreeze */
	case STATE_FROZEN:
		retval = unfreeze_cgroup(cgroup, freezer);
		break;
	default:
		break;
	}
out:
	spin_unlock_irq(&freezer->lock);

	return retval;
}

static int freezer_write(struct cgroup *cgroup,
			 struct cftype *cft,
			 const char *buffer)
{
	int retval;
	enum freezer_state goal_state;

	if (strcmp(buffer, freezer_state_strs[STATE_RUNNING]) == 0)
		goal_state = STATE_RUNNING;
	else if (strcmp(buffer, freezer_state_strs[STATE_FROZEN]) == 0)
		goal_state = STATE_FROZEN;
	else
		return -EIO;

	if (!cgroup_lock_live_group(cgroup))
		return -ENODEV;
	retval = freezer_change_state(cgroup, goal_state);
	cgroup_unlock();
	return retval;
}

static struct cftype files[] = {
	{
		.name = "state",
		.read_seq_string = freezer_read,
		.write_string = freezer_write,
	},
};

static int freezer_populate(struct cgroup_subsys *ss, struct cgroup *cgroup)
{
	return cgroup_add_files(cgroup, ss, files, ARRAY_SIZE(files));
}

struct cgroup_subsys freezer_subsys = {
	.name		= "freezer",
	.create		= freezer_create,
	.destroy	= freezer_destroy,
	.populate	= freezer_populate,
	.subsys_id	= freezer_subsys_id,
	.can_attach	= freezer_can_attach,
	.attach		= NULL,
	.fork		= freezer_fork,
	.exit		= NULL,
};