// SPDX-License-Identifier: GPL-2.0
/*
 * Common Code for DAMON Modules
 *
 * Author: SeongJae Park <sj@kernel.org>
 *
 * Hysteris 4.19 integration: independent reclaim and LRU-sort policies share
 * one PADDR monitoring context.  Each policy retains its own schemes,
 * parameters, callbacks, and statistics; access-bit sampling happens once.
 */

#include <linux/damon.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/smp.h>

#include "modules-common.h"

static DEFINE_MUTEX(damon_modules_paddr_lock);
static struct damon_ctx *damon_modules_paddr_ctx;
static struct damon_target *damon_modules_paddr_target;
static int damon_modules_paddr_users;
static struct damon_callback damon_modules_paddr_callbacks[2];
static int damon_modules_nr_paddr_callbacks;

static int damon_modules_after_wmarks_check(struct damon_ctx *ctx)
{
	int i, nr, err;

	nr = READ_ONCE(damon_modules_nr_paddr_callbacks);
	smp_rmb();
	for (i = 0; i < nr; i++) {
		if (!damon_modules_paddr_callbacks[i].after_wmarks_check)
			continue;
		err = damon_modules_paddr_callbacks[i].after_wmarks_check(ctx);
		if (err)
			pr_warn("PADDR policy watermark callback failed: %d\n",
					err);
	}
	return 0;
}

static int damon_modules_after_aggregation(struct damon_ctx *ctx)
{
	int i, nr, err;

	nr = READ_ONCE(damon_modules_nr_paddr_callbacks);
	smp_rmb();
	for (i = 0; i < nr; i++) {
		if (!damon_modules_paddr_callbacks[i].after_aggregation)
			continue;
		err = damon_modules_paddr_callbacks[i].after_aggregation(ctx);
		if (err)
			pr_warn("PADDR policy aggregation callback failed: %d\n",
					err);
	}
	return 0;
}

/* Allocate an independent PADDR context, used for staging policy updates. */
int damon_modules_new_paddr_ctx_target(struct damon_ctx **ctxp,
		struct damon_target **targetp)
{
	struct damon_ctx *ctx;
	struct damon_target *target;

	ctx = damon_new_ctx();
	if (!ctx)
		return -ENOMEM;
	if (damon_select_ops(ctx, DAMON_OPS_PADDR)) {
		damon_destroy_ctx(ctx);
		return -EINVAL;
	}
	target = damon_new_target();
	if (!target) {
		damon_destroy_ctx(ctx);
		return -ENOMEM;
	}
	damon_add_target(ctx, target);
	*ctxp = ctx;
	*targetp = target;
	return 0;
}

/* Return the one PADDR context shared by all enabled policy modules. */
int damon_modules_get_shared_paddr_ctx_target(struct damon_ctx **ctxp,
		struct damon_target **targetp)
{
	int err = 0;

	mutex_lock(&damon_modules_paddr_lock);
	if (!damon_modules_paddr_ctx) {
		err = damon_modules_new_paddr_ctx_target(
				&damon_modules_paddr_ctx,
				&damon_modules_paddr_target);
		if (err)
			goto out;
		damon_modules_paddr_ctx->callback.after_wmarks_check =
			damon_modules_after_wmarks_check;
		damon_modules_paddr_ctx->callback.after_aggregation =
			damon_modules_after_aggregation;
	}
	*ctxp = damon_modules_paddr_ctx;
	*targetp = damon_modules_paddr_target;
out:
	mutex_unlock(&damon_modules_paddr_lock);
	return err;
}

int damon_modules_register_paddr_callbacks(struct damon_ctx *ctx,
		struct damon_callback *callback)
{
	int err = 0;
	int nr;

	mutex_lock(&damon_modules_paddr_lock);
	nr = damon_modules_nr_paddr_callbacks;
	if (ctx != damon_modules_paddr_ctx)
		err = -EINVAL;
	else if (nr >= ARRAY_SIZE(damon_modules_paddr_callbacks))
		err = -ENOSPC;
	else {
		damon_modules_paddr_callbacks[nr] = *callback;
		/* Publish the complete callback before increasing the visible count. */
		smp_wmb();
		WRITE_ONCE(damon_modules_nr_paddr_callbacks, nr + 1);
	}
	mutex_unlock(&damon_modules_paddr_lock);
	return err;
}

static bool damon_modules_ctx_running(struct damon_ctx *ctx);

int damon_modules_start_paddr(struct damon_ctx *ctx)
{
	int err = 0;

	mutex_lock(&damon_modules_paddr_lock);
	if (ctx != damon_modules_paddr_ctx) {
		err = -EINVAL;
		goto out;
	}
	if (!damon_modules_ctx_running(ctx)) {
		struct damon_ctx *ctxs[] = { ctx };

		/*
		 * A dead worker destroys its adaptive regions.  Require all
		 * policy users to stop first, so the next first-user parameter
		 * commit can restore the shared target before restarting.
		 */
		if (damon_modules_paddr_users) {
			err = -EIO;
			goto out;
		}
		err = damon_start(ctxs, 1, true);
		if (err)
			goto out;
	}
	damon_modules_paddr_users++;
out:
	mutex_unlock(&damon_modules_paddr_lock);
	return err;
}

int damon_modules_stop_paddr(struct damon_ctx *ctx)
{
	int err = 0;

	mutex_lock(&damon_modules_paddr_lock);
	if (ctx != damon_modules_paddr_ctx || !damon_modules_paddr_users) {
		err = -EINVAL;
		goto out;
	}
	damon_modules_paddr_users--;
	if (!damon_modules_paddr_users &&
			damon_modules_ctx_running(ctx)) {
		struct damon_ctx *ctxs[] = { ctx };

		err = damon_stop(ctxs, 1);
		if (err)
			damon_modules_paddr_users++;
	}
out:
	mutex_unlock(&damon_modules_paddr_lock);
	return err;
}

struct damon_modules_policy_commit {
	struct damon_ctx *ctx;
	struct damon_ctx *param_ctx;
	enum damos_action *actions;
	int nr_actions;
	bool update_ctx;
	bool validate_shared;
};

static bool damon_modules_same_attrs(struct damon_attrs *a,
		struct damon_attrs *b);
static bool damon_modules_same_regions(struct damon_ctx *a,
		struct damon_ctx *b);

/*
 * Keep cross-policy action order independent of built-in initcall order:
 * first protect hot pages, then demote cold pages, and finally reclaim the
 * coldest eligible pages.  All three still consume the same DAMON samples.
 */
static void damon_modules_order_paddr_schemes(struct damon_ctx *ctx)
{
	static const enum damos_action order[] = {
		DAMOS_LRU_PRIO,
		DAMOS_LRU_DEPRIO,
		DAMOS_PAGEOUT,
	};
	struct damos *scheme, *next;
	LIST_HEAD(ordered);
	int i;

	for (i = 0; i < ARRAY_SIZE(order); i++) {
		damon_for_each_scheme_safe(scheme, next, ctx) {
			if (scheme->action == order[i])
				list_move_tail(&scheme->list, &ordered);
		}
	}
	list_splice_tail(&ordered, &ctx->schemes);
}

static int damon_modules_commit_policy_in_kdamond(void *data)
{
	struct damon_modules_policy_commit *commit = data;
	int err;

	if (commit->validate_shared &&
			(!damon_modules_same_attrs(&commit->ctx->attrs,
					&commit->param_ctx->attrs) ||
			 !damon_modules_same_regions(commit->ctx,
					commit->param_ctx)))
		return -EINVAL;

	err = damon_commit_schemes_for_actions(commit->ctx,
			commit->param_ctx, commit->actions,
			commit->nr_actions);
	if (err)
		return err;
	damon_modules_order_paddr_schemes(commit->ctx);
	if (!commit->update_ctx)
		return 0;
	return damon_commit_ctx_without_schemes(commit->ctx,
			commit->param_ctx);
}

static bool damon_modules_ctx_running(struct damon_ctx *ctx)
{
	bool running;

	mutex_lock(&ctx->kdamond_lock);
	running = ctx->kdamond != NULL;
	mutex_unlock(&ctx->kdamond_lock);
	return running;
}

static bool damon_modules_same_attrs(struct damon_attrs *a,
		struct damon_attrs *b)
{
	return a->sample_interval == b->sample_interval &&
		a->aggr_interval == b->aggr_interval &&
		a->ops_update_interval == b->ops_update_interval &&
		a->min_nr_regions == b->min_nr_regions &&
		a->max_nr_regions == b->max_nr_regions;
}

static bool damon_modules_same_regions(struct damon_ctx *a,
		struct damon_ctx *b)
{
	struct damon_target *ta, *tb;
	struct damon_region *a_first, *a_last, *b_first, *b_last;

	if (list_empty(&a->adaptive_targets) ||
			list_empty(&b->adaptive_targets))
		return false;
	ta = list_first_entry(&a->adaptive_targets, struct damon_target, list);
	tb = list_first_entry(&b->adaptive_targets, struct damon_target, list);
	if (!ta->nr_regions || !tb->nr_regions)
		return false;
	a_first = damon_first_region(ta);
	a_last = damon_last_region(ta);
	b_first = damon_first_region(tb);
	b_last = damon_last_region(tb);
	return a_first->ar.start == b_first->ar.start &&
		a_last->ar.end == b_last->ar.end;
}

static int damon_modules_commit_policy(struct damon_modules_policy_commit *commit,
		bool in_kdamond)
{
	struct damon_call_control control = {
		.fn = damon_modules_commit_policy_in_kdamond,
		.data = commit,
	};
	int err;

	if (damon_modules_ctx_running(commit->ctx) && !in_kdamond) {
		err = damon_call(commit->ctx, &control);
		if (err)
			return err;
		return control.return_code;
	}
	return damon_modules_commit_policy_in_kdamond(commit);
}

int damon_modules_commit_paddr_policy(struct damon_ctx *ctx,
		struct damon_ctx *param_ctx, enum damos_action *actions,
		int nr_actions, bool in_kdamond)
{
	bool running;
	int err;
	struct damon_modules_policy_commit commit = {
		.ctx = ctx,
		.param_ctx = param_ctx,
		.actions = actions,
		.nr_actions = nr_actions,
	};

	if (ctx != damon_modules_paddr_ctx)
		return -EINVAL;
	/*
	 * Serialize external updates with context start/stop.  A callback already
	 * running on kdamond must not take this lock, because stop waits for that
	 * thread to finish.
	 */
	if (!in_kdamond)
		mutex_lock(&damon_modules_paddr_lock);
	running = damon_modules_ctx_running(ctx);
	if (!running && READ_ONCE(damon_modules_paddr_users)) {
		err = -EIO;
		goto out;
	}
	/*
	 * Validate live geometry in kdamond, not from the sysfs writer, because
	 * PADDR's adaptive region list can change concurrently with sampling.
	 */
	commit.validate_shared = running;
	/* A live shared monitor already has the common target and attributes. */
	commit.update_ctx = !running;
	err = damon_modules_commit_policy(&commit, in_kdamond);
out:
	if (!in_kdamond)
		mutex_unlock(&damon_modules_paddr_lock);
	return err;
}

int damon_modules_remove_paddr_policy(struct damon_ctx *ctx,
		enum damos_action *actions, int nr_actions)
{
	struct damon_ctx *empty_ctx;
	struct damon_modules_policy_commit commit = {
		.ctx = ctx,
		.actions = actions,
		.nr_actions = nr_actions,
		.update_ctx = false,
	};
	int err;

	if (ctx != damon_modules_paddr_ctx)
		return -EINVAL;
	mutex_lock(&damon_modules_paddr_lock);
	empty_ctx = damon_new_ctx();
	if (!empty_ctx) {
		err = -ENOMEM;
		goto out;
	}
	commit.param_ctx = empty_ctx;
	err = damon_modules_commit_policy(&commit, false);
	damon_destroy_ctx(empty_ctx);
out:
	mutex_unlock(&damon_modules_paddr_lock);
	return err;
}
