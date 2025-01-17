/*
 * Copyright (C) 2015 Versity Software, Inc.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/xattr.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/sched.h>
#include <linux/list_sort.h>

#include "format.h"
#include "super.h"
#include "key.h"
#include "inode.h"
#include "dir.h"
#include "data.h"
#include "scoutfs_trace.h"
#include "xattr.h"
#include "trans.h"
#include "msg.h"
#include "item.h"
#include "client.h"
#include "cmp.h"
#include "omap.h"
#include "btree.h"

/*
 * XXX
 *  - worry about i_ino trunctation, not sure if we do anything
 *  - use inode item value lengths for forward/back compat
 */

/*
 * XXX before committing:
 *  - describe all this better
 *  - describe data locking size problems
 */

struct inode_allocator {
	spinlock_t lock;
	u64 ino;
	u64 nr;
};

struct inode_sb_info {
	struct super_block *sb;
	bool stopped;

	spinlock_t writeback_lock;
	struct list_head writeback_list;
	struct inode_allocator dir_ino_alloc;
	struct inode_allocator ino_alloc;

	struct delayed_work orphan_scan_dwork;

	/* serialize multiple inode ->evict trying to delete same ino's items */
	spinlock_t deleting_items_lock;
	struct list_head deleting_items_list;

	struct work_struct iput_work;
	struct llist_head iput_llist;
};

#define DECLARE_INODE_SB_INFO(sb, name) \
	struct inode_sb_info *name = SCOUTFS_SB(sb)->inode_sb_info

static struct kmem_cache *scoutfs_inode_cachep;

/*
 * This is called once before all the allocations and frees of a inode
 * object within a slab.  It's for inode fields that don't need to be
 * initialized for a given instance of an inode.
 */
static void scoutfs_inode_ctor(void *obj)
{
	struct scoutfs_inode_info *si = obj;

	init_rwsem(&si->extent_sem);
	mutex_init(&si->item_mutex);
	seqcount_init(&si->seqcount);
	si->staging = false;
	scoutfs_per_task_init(&si->pt_data_lock);
	atomic64_set(&si->data_waitq.changed, 0);
	init_waitqueue_head(&si->data_waitq.waitq);
	init_rwsem(&si->xattr_rwsem);
	INIT_LIST_HEAD(&si->writeback_entry);
	scoutfs_lock_init_coverage(&si->ino_lock_cov);
	atomic_set(&si->iput_count, 0);

	inode_init_once(&si->inode);
}

struct inode *scoutfs_alloc_inode(struct super_block *sb)
{
	struct scoutfs_inode_info *si;

	si = kmem_cache_alloc(scoutfs_inode_cachep, GFP_NOFS);
	if (!si)
		return NULL;

	return &si->inode;
}

static void scoutfs_i_callback(struct rcu_head *head)
{
	struct inode *inode = container_of(head, struct inode, i_rcu);

	trace_scoutfs_i_callback(inode);
	kmem_cache_free(scoutfs_inode_cachep, SCOUTFS_I(inode));
}

void scoutfs_destroy_inode(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	DECLARE_INODE_SB_INFO(inode->i_sb, inf);

	spin_lock(&inf->writeback_lock);
	if (!list_empty(&si->writeback_entry))
		list_del_init(&si->writeback_entry);
	spin_unlock(&inf->writeback_lock);

	scoutfs_lock_del_coverage(inode->i_sb, &si->ino_lock_cov);

	call_rcu(&inode->i_rcu, scoutfs_i_callback);
}

static const struct inode_operations scoutfs_file_iops = {
	.getattr	= scoutfs_getattr,
	.setattr	= scoutfs_setattr,
	.setxattr	= scoutfs_setxattr,
	.getxattr	= scoutfs_getxattr,
	.listxattr	= scoutfs_listxattr,
	.removexattr	= scoutfs_removexattr,
	.fiemap		= scoutfs_data_fiemap,
};

static const struct inode_operations scoutfs_special_iops = {
	.getattr	= scoutfs_getattr,
	.setattr	= scoutfs_setattr,
	.setxattr	= scoutfs_setxattr,
	.getxattr	= scoutfs_getxattr,
	.listxattr	= scoutfs_listxattr,
	.removexattr	= scoutfs_removexattr,
};

/*
 * Called once new inode allocation or inode reading has initialized
 * enough of the inode for us to set the ops based on the mode.
 */
static void set_inode_ops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_mapping->a_ops = &scoutfs_file_aops;
		inode->i_op = &scoutfs_file_iops;
		inode->i_fop = &scoutfs_file_fops;
		break;
	case S_IFDIR:
		inode->i_op = &scoutfs_dir_iops.ops;
		inode->i_flags |= S_IOPS_WRAPPER;
		inode->i_fop = &scoutfs_dir_fops;
		break;
	case S_IFLNK:
		inode->i_op = &scoutfs_symlink_iops;
		break;
	default:
		inode->i_op = &scoutfs_special_iops;
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	}

	/* ephemeral data items avoid kmap for pointers to page contents */
	mapping_set_gfp_mask(inode->i_mapping, GFP_USER);
}

/*
 * The caller has ensured that the fields in the incoming scoutfs inode
 * reflect both the inode item and the inode index items.  This happens
 * when reading, refreshing, or updating the inodes.  We set the inode
 * info fields to match so that next time we try to update the inode we
 * can tell which fields have changed.
 */
static void set_item_info(struct scoutfs_inode_info *si,
			  struct scoutfs_inode *sinode)
{
	BUG_ON(!mutex_is_locked(&si->item_mutex));

	memset(si->item_majors, 0, sizeof(si->item_majors));
	memset(si->item_minors, 0, sizeof(si->item_minors));

	si->have_item = true;
	si->item_majors[SCOUTFS_INODE_INDEX_META_SEQ_TYPE] =
		le64_to_cpu(sinode->meta_seq);
	si->item_majors[SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE] =
		le64_to_cpu(sinode->data_seq);
}

static void load_inode(struct inode *inode, struct scoutfs_inode *cinode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	i_size_write(inode, le64_to_cpu(cinode->size));
	inode->i_version = le64_to_cpu(cinode->version);
	set_nlink(inode, le32_to_cpu(cinode->nlink));
	i_uid_write(inode, le32_to_cpu(cinode->uid));
	i_gid_write(inode, le32_to_cpu(cinode->gid));
	inode->i_mode = le32_to_cpu(cinode->mode);
	inode->i_rdev = le32_to_cpu(cinode->rdev);
	inode->i_atime.tv_sec = le64_to_cpu(cinode->atime.sec);
	inode->i_atime.tv_nsec = le32_to_cpu(cinode->atime.nsec);
	inode->i_mtime.tv_sec = le64_to_cpu(cinode->mtime.sec);
	inode->i_mtime.tv_nsec = le32_to_cpu(cinode->mtime.nsec);
	inode->i_ctime.tv_sec = le64_to_cpu(cinode->ctime.sec);
	inode->i_ctime.tv_nsec = le32_to_cpu(cinode->ctime.nsec);

	si->meta_seq = le64_to_cpu(cinode->meta_seq);
	si->data_seq = le64_to_cpu(cinode->data_seq);
	si->data_version = le64_to_cpu(cinode->data_version);
	si->online_blocks = le64_to_cpu(cinode->online_blocks);
	si->offline_blocks = le64_to_cpu(cinode->offline_blocks);
	si->next_readdir_pos = le64_to_cpu(cinode->next_readdir_pos);
	si->next_xattr_id = le64_to_cpu(cinode->next_xattr_id);
	si->flags = le32_to_cpu(cinode->flags);
	si->crtime.tv_sec = le64_to_cpu(cinode->crtime.sec);
	si->crtime.tv_nsec = le32_to_cpu(cinode->crtime.nsec);

	/*
	 * i_blocks is initialized from online and offline and is then
	 * maintained as blocks come and go.
	 */
	inode->i_blocks = (si->online_blocks + si->offline_blocks)
				<< SCOUTFS_BLOCK_SM_SECTOR_SHIFT;

	set_item_info(si, cinode);
}

static void init_inode_key(struct scoutfs_key *key, u64 ino)
{
	*key = (struct scoutfs_key) {
		.sk_zone = SCOUTFS_FS_ZONE,
		.ski_ino = cpu_to_le64(ino),
		.sk_type = SCOUTFS_INODE_TYPE,
	};
}

/*
 * Refresh the vfs inode fields if the lock indicates that the current
 * contents could be stale.
 *
 * This can be racing with many lock holders of an inode.  A bunch of
 * readers can be checking to refresh while one of them is refreshing.
 *
 * The vfs inode field updates can't be racing with valid readers of the
 * fields because they should have already had a locked refreshed inode
 * to be dereferencing its contents.
 */
int scoutfs_inode_refresh(struct inode *inode, struct scoutfs_lock *lock,
			  int flags)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_key key;
	struct scoutfs_inode sinode;
	const u64 refresh_gen = lock->refresh_gen;
	int ret;

	/*
	 * Lock refresh gens are supposed to strictly increase.  Inodes
	 * having a greater gen means memory corruption or
	 * lifetime/logic bugs that could stop the inode from refreshing
	 * and expose stale data.
	 */
	BUG_ON(atomic64_read(&si->last_refreshed) > refresh_gen);

	if (atomic64_read(&si->last_refreshed) == refresh_gen)
		return 0;

	init_inode_key(&key, scoutfs_ino(inode));

	mutex_lock(&si->item_mutex);
	if (atomic64_read(&si->last_refreshed) < refresh_gen) {
		ret = scoutfs_item_lookup_exact(sb, &key, &sinode,
						sizeof(sinode), lock);
		if (ret == 0) {
			load_inode(inode, &sinode);
			atomic64_set(&si->last_refreshed, refresh_gen);
			scoutfs_lock_add_coverage(sb, lock, &si->ino_lock_cov);
			si->drop_invalidated = false;
		}
	} else {
		ret = 0;
	}
	mutex_unlock(&si->item_mutex);

	return ret;
}

int scoutfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		    struct kstat *stat)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct scoutfs_lock *lock = NULL;
	int ret;

	ret = scoutfs_lock_inode(sb, SCOUTFS_LOCK_READ,
				 SCOUTFS_LKF_REFRESH_INODE, inode, &lock);
	if (ret == 0) {
		generic_fillattr(inode, stat);
		scoutfs_unlock(sb, lock, SCOUTFS_LOCK_READ);
	}
	return ret;
}

static int set_inode_size(struct inode *inode, struct scoutfs_lock *lock,
			  u64 new_size, bool truncate)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	LIST_HEAD(ind_locks);
	int ret;

	if (!S_ISREG(inode->i_mode))
		return 0;

	ret = scoutfs_inode_index_lock_hold(inode, &ind_locks, true, false);
	if (ret)
		return ret;

	if (new_size != i_size_read(inode))
		scoutfs_inode_inc_data_version(inode);

	truncate_setsize(inode, new_size);
	inode->i_ctime = inode->i_mtime = CURRENT_TIME;
	if (truncate)
		si->flags |= SCOUTFS_INO_FLAG_TRUNCATE;
	scoutfs_inode_set_data_seq(inode);
	inode_inc_iversion(inode);
	scoutfs_update_inode_item(inode, lock, &ind_locks);

	scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);

	return ret;
}

static int clear_truncate_flag(struct inode *inode, struct scoutfs_lock *lock)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	LIST_HEAD(ind_locks);
	int ret;

	ret = scoutfs_inode_index_lock_hold(inode, &ind_locks, false, false);
	if (ret)
		return ret;

	si->flags &= ~SCOUTFS_INO_FLAG_TRUNCATE;
	scoutfs_update_inode_item(inode, lock, &ind_locks);

	scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);

	return ret;
}

int scoutfs_complete_truncate(struct inode *inode, struct scoutfs_lock *lock)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	u64 start;
	int ret, err;

	trace_scoutfs_complete_truncate(inode, si->flags);

	if (!(si->flags & SCOUTFS_INO_FLAG_TRUNCATE))
		return 0;

	start = (i_size_read(inode) + SCOUTFS_BLOCK_SM_SIZE - 1) >>
		SCOUTFS_BLOCK_SM_SHIFT;
	ret = scoutfs_data_truncate_items(inode->i_sb, inode,
					  scoutfs_ino(inode), start, ~0ULL,
					  false, lock);
	err = clear_truncate_flag(inode, lock);

	return ret ? ret : err;
}

/*
 * If we're changing the file size than the contents of the file are
 * changing and we increment the data_version.  This would prevent
 * staging because the data_version is per-inode today, not per-extent.
 * So if there are any offline extents within the new size then we need
 * to stage them before we truncate.  And this is called with the
 * i_mutex held which would prevent staging so we release it and
 * re-acquire it.  Ideally we'd fix this so that we can acquire the lock
 * instead of the caller.
 */
int scoutfs_setattr(struct dentry *dentry, struct iattr *attr)
{
	struct inode *inode = dentry->d_inode;
	struct super_block *sb = inode->i_sb;
	struct scoutfs_lock *lock = NULL;
	DECLARE_DATA_WAIT(dw);
	LIST_HEAD(ind_locks);
	bool truncate = false;
	u64 attr_size;
	int ret;

	trace_scoutfs_setattr(dentry, attr);

retry:
	ret = scoutfs_lock_inode(sb, SCOUTFS_LOCK_WRITE,
				 SCOUTFS_LKF_REFRESH_INODE, inode, &lock);
	if (ret)
		return ret;

	ret = inode_change_ok(inode, attr);
	if (ret)
		goto out;

	attr_size = (attr->ia_valid & ATTR_SIZE) ? attr->ia_size :
		i_size_read(inode);

	if (S_ISREG(inode->i_mode) && attr->ia_valid & ATTR_SIZE) {
		/*
		 * Complete any truncates that may have failed while
		 * in progress
		 */
		ret = scoutfs_complete_truncate(inode, lock);
		if (ret)
			goto out;

		/* data_version is per inode, all must be online */
		if (attr_size > 0 && attr_size != i_size_read(inode)) {
			ret = scoutfs_data_wait_check(inode, 0, attr_size,
						SEF_OFFLINE,
						SCOUTFS_IOC_DWO_CHANGE_SIZE,
						&dw, lock);
			if (ret < 0)
				goto out;
			if (scoutfs_data_wait_found(&dw)) {
				scoutfs_unlock(sb, lock, SCOUTFS_LOCK_WRITE);

				/* XXX callee locks instead? */
				mutex_unlock(&inode->i_mutex);
				ret = scoutfs_data_wait(inode, &dw);
				mutex_lock(&inode->i_mutex);

				if (ret == 0)
					goto retry;
				goto out;
			}
		}

		/* truncating to current size truncates extents past size */
		truncate = i_size_read(inode) >= attr_size;

		ret = set_inode_size(inode, lock, attr_size, truncate);
		if (ret)
			goto out;

		if (truncate) {
			ret = scoutfs_complete_truncate(inode, lock);
			if (ret)
				goto out;
		}
	}

	ret = scoutfs_inode_index_lock_hold(inode, &ind_locks, false, false);
	if (ret)
		goto out;

	setattr_copy(inode, attr);
	inode_inc_iversion(inode);
	scoutfs_update_inode_item(inode, lock, &ind_locks);

	scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);
out:
	scoutfs_unlock(sb, lock, SCOUTFS_LOCK_WRITE);
	return ret;
}

/*
 * Set a given seq to the current trans seq if it differs.  The caller
 * holds locks and a transaction which prevents the transaction from
 * committing and refreshing the seq.
 */
static void set_trans_seq(struct inode *inode, u64 *seq)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);

	if (*seq != sbi->trans_seq) {
		preempt_disable();
		write_seqcount_begin(&si->seqcount);
		*seq = sbi->trans_seq;
		write_seqcount_end(&si->seqcount);
		preempt_enable();
	}
}

void scoutfs_inode_set_meta_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	set_trans_seq(inode, &si->meta_seq);
}

void scoutfs_inode_set_data_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	set_trans_seq(inode, &si->data_seq);
}

void scoutfs_inode_inc_data_version(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	preempt_disable();
	write_seqcount_begin(&si->seqcount);
	si->data_version++;
	write_seqcount_end(&si->seqcount);
	preempt_enable();
}

void scoutfs_inode_set_data_version(struct inode *inode, u64 data_version)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	preempt_disable();
	write_seqcount_begin(&si->seqcount);
	si->data_version = data_version;
	write_seqcount_end(&si->seqcount);
	preempt_enable();
}

void scoutfs_inode_add_onoff(struct inode *inode, s64 on, s64 off)
{
	struct scoutfs_inode_info *si;

	if (inode && (on || off)) {
		si = SCOUTFS_I(inode);
		preempt_disable();
		write_seqcount_begin(&si->seqcount);

		/* inode and extents out of sync, bad callers */
		if (((s64)si->online_blocks + on < 0) ||
		    ((s64)si->offline_blocks + off < 0)) {
			scoutfs_corruption(inode->i_sb, SC_INODE_BLOCK_COUNTS,
				corrupt_inode_block_counts,
				"ino %llu size %llu online %llu + %lld offline %llu + %lld",
				scoutfs_ino(inode), i_size_read(inode),
				si->online_blocks, on, si->offline_blocks, off);
		}

		si->online_blocks += on;
		si->offline_blocks += off;
		/* XXX not sure if this is right */
		inode->i_blocks += (on + off) * SCOUTFS_BLOCK_SM_SECTORS;

		trace_scoutfs_online_offline_blocks(inode, on, off,
						    si->online_blocks,
						    si->offline_blocks);

		write_seqcount_end(&si->seqcount);
		preempt_enable();
	}

	/* any time offline extents decreased we try and wake waiters */
	if (inode && off < 0)
		scoutfs_data_wait_changed(inode);
}

static u64 read_seqcount_u64(struct inode *inode, u64 *val)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	unsigned int seq;
	u64 v;

	do {
		seq = read_seqcount_begin(&si->seqcount);
		v = *val;
	} while (read_seqcount_retry(&si->seqcount, seq));

	return v;
}

u64 scoutfs_inode_meta_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->meta_seq);
}

u64 scoutfs_inode_data_seq(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->data_seq);
}

u64 scoutfs_inode_data_version(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return read_seqcount_u64(inode, &si->data_version);
}

void scoutfs_inode_get_onoff(struct inode *inode, s64 *on, s64 *off)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	unsigned int seq;

	do {
		seq = read_seqcount_begin(&si->seqcount);
		*on = SCOUTFS_I(inode)->online_blocks;
		*off = SCOUTFS_I(inode)->offline_blocks;
	} while (read_seqcount_retry(&si->seqcount, seq));
}

/*
 * We have inversions between getting cluster locks while performing
 * final deletion on a freeing inode and waiting on a freeing inode
 * while holding a cluster lock.
 *
 * We can avoid these deadlocks by hiding freeing inodes in our hash
 * lookup function.  We're fine with either returning null or populating
 * a new inode overlapping with eviction freeing a previous instance of
 * the inode.
 */
static int scoutfs_iget_test(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	u64 *ino = arg;

	return (si->ino == *ino) && !(inode->i_state & I_FREEING);
}

static int scoutfs_iget_set(struct inode *inode, void *arg)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	u64 *ino = arg;

	inode->i_ino = *ino;
	si->ino = *ino;

	return 0;
}

struct inode *scoutfs_ilookup(struct super_block *sb, u64 ino)
{
	return ilookup5(sb, ino, scoutfs_iget_test, &ino);
}

struct inode *scoutfs_iget(struct super_block *sb, u64 ino, int lkf)
{
	struct scoutfs_lock *lock = NULL;
	struct scoutfs_inode_info *si;
	struct inode *inode;
	int ret;

	ret = scoutfs_lock_ino(sb, SCOUTFS_LOCK_READ, lkf, ino, &lock);
	if (ret)
		return ERR_PTR(ret);

	inode = iget5_locked(sb, ino, scoutfs_iget_test, scoutfs_iget_set,
			     &ino);
	if (!inode) {
		inode = ERR_PTR(-ENOMEM);
		goto out;
	}

	if (inode->i_state & I_NEW) {
		/* XXX ensure refresh, instead clear in drop_inode? */
		si = SCOUTFS_I(inode);
		atomic64_set(&si->last_refreshed, 0);
		inode->i_version = 0;

		ret = scoutfs_inode_refresh(inode, lock, 0);
		if (ret == 0)
			ret = scoutfs_omap_inc(sb, ino);
		if (ret) {
			iget_failed(inode);
			inode = ERR_PTR(ret);
		} else {
			set_inode_ops(inode);
			unlock_new_inode(inode);
		}
	}

out:
	scoutfs_unlock(sb, lock, SCOUTFS_LOCK_READ);
	return inode;
}

static void store_inode(struct scoutfs_inode *cinode, struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	u64 online_blocks;
	u64 offline_blocks;

	scoutfs_inode_get_onoff(inode, &online_blocks, &offline_blocks);

	cinode->size = cpu_to_le64(i_size_read(inode));
	cinode->version = cpu_to_le64(inode->i_version);
	cinode->nlink = cpu_to_le32(inode->i_nlink);
	cinode->uid = cpu_to_le32(i_uid_read(inode));
	cinode->gid = cpu_to_le32(i_gid_read(inode));
	cinode->mode = cpu_to_le32(inode->i_mode);
	cinode->rdev = cpu_to_le32(inode->i_rdev);
	cinode->atime.sec = cpu_to_le64(inode->i_atime.tv_sec);
	cinode->atime.nsec = cpu_to_le32(inode->i_atime.tv_nsec);
	memset(cinode->atime.__pad, 0, sizeof(cinode->atime.__pad));
	cinode->ctime.sec = cpu_to_le64(inode->i_ctime.tv_sec);
	cinode->ctime.nsec = cpu_to_le32(inode->i_ctime.tv_nsec);
	memset(cinode->ctime.__pad, 0, sizeof(cinode->ctime.__pad));
	cinode->mtime.sec = cpu_to_le64(inode->i_mtime.tv_sec);
	cinode->mtime.nsec = cpu_to_le32(inode->i_mtime.tv_nsec);
	memset(cinode->mtime.__pad, 0, sizeof(cinode->mtime.__pad));

	cinode->meta_seq = cpu_to_le64(scoutfs_inode_meta_seq(inode));
	cinode->data_seq = cpu_to_le64(scoutfs_inode_data_seq(inode));
	cinode->data_version = cpu_to_le64(scoutfs_inode_data_version(inode));
	cinode->online_blocks = cpu_to_le64(online_blocks);
	cinode->offline_blocks = cpu_to_le64(offline_blocks);
	cinode->next_readdir_pos = cpu_to_le64(si->next_readdir_pos);
	cinode->next_xattr_id = cpu_to_le64(si->next_xattr_id);
	cinode->flags = cpu_to_le32(si->flags);
	cinode->crtime.sec = cpu_to_le64(si->crtime.tv_sec);
	cinode->crtime.nsec = cpu_to_le32(si->crtime.tv_nsec);
	memset(cinode->crtime.__pad, 0, sizeof(cinode->crtime.__pad));
}

/*
 * Create a pinned dirty inode item so that we can later update the
 * inode item without risking failure.  We often wouldn't want to have
 * to unwind inode modifcations (perhaps by shared vfs code!) if our
 * item update failed.  This is our chance to return errors for enospc
 * for lack of space for new logged dirty inode items.
 *
 * This dirty inode item will be found by lookups in the interim so we
 * have to update it now with the current inode contents.
 *
 * Callers don't delete these dirty items on errors.  They're still
 * valid and will be merged with the current item eventually.
 *
 * The caller has to prevent sync between dirtying and updating the
 * inodes.
 *
 * XXX this will have to do something about variable length inodes
 */
int scoutfs_dirty_inode_item(struct inode *inode, struct scoutfs_lock *lock)
{
	struct super_block *sb = inode->i_sb;
	struct scoutfs_inode sinode;
	struct scoutfs_key key;
	int ret;

	store_inode(&sinode, inode);

	init_inode_key(&key, scoutfs_ino(inode));

	ret = scoutfs_item_update(sb, &key, &sinode, sizeof(sinode), lock);
	if (!ret)
		trace_scoutfs_dirty_inode(inode);
	return ret;
}

struct index_lock {
	struct list_head head;
	struct scoutfs_lock *lock;
	u8 type;
	u64 major;
	u32 minor;
	u64 ino;
};

static bool will_del_index(struct scoutfs_inode_info *si,
			   u8 type, u64 major, u32 minor)
{
	return si && si->have_item &&
	       (si->item_majors[type] != major ||
		si->item_minors[type] != minor);
}

static bool will_ins_index(struct scoutfs_inode_info *si,
			   u8 type, u64 major, u32 minor)
{
	return !si || !si->have_item ||
	       (si->item_majors[type] != major ||
		si->item_minors[type] != minor);
}

static bool inode_has_index(umode_t mode, u8 type)
{
	switch(type) {
		case SCOUTFS_INODE_INDEX_META_SEQ_TYPE:
			return true;
		case SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE:
			return S_ISREG(mode);
		default:
			return WARN_ON_ONCE(false);
	}
}

static int cmp_index_lock(void *priv, struct list_head *A, struct list_head *B)
{
	struct index_lock *a = list_entry(A, struct index_lock, head);
	struct index_lock *b = list_entry(B, struct index_lock, head);

	return ((int)a->type - (int)b->type) ?:
	       scoutfs_cmp_u64s(a->major, b->major) ?:
	       scoutfs_cmp_u64s(a->minor, b->minor) ?:
	       scoutfs_cmp_u64s(a->ino, b->ino);
}

static void clamp_inode_index(u8 type, u64 *major, u32 *minor, u64 *ino)
{
	struct scoutfs_key start;

	scoutfs_lock_get_index_item_range(type, *major, *ino, &start, NULL);

	*major = le64_to_cpu(start.skii_major);
	*minor = 0;
	*ino = le64_to_cpu(start.skii_ino);
}

/*
 * Find the lock that covers the given index item.  Returns NULL if
 * there isn't a lock that covers the item.  We know that the list is
 * sorted at this point so we can stop once our search value is less
 * than a list entry.
 */
static struct scoutfs_lock *find_index_lock(struct list_head *lock_list,
					    u8 type, u64 major, u32 minor,
					    u64 ino)
{
	struct index_lock *ind_lock;
	struct index_lock needle;
	int cmp;

	clamp_inode_index(type, &major, &minor, &ino);
	needle.type = type;
	needle.major = major;
	needle.minor = minor;
	needle.ino = ino;

	list_for_each_entry(ind_lock, lock_list, head) {
		cmp = cmp_index_lock(NULL, &needle.head, &ind_lock->head);
		if (cmp == 0)
			return ind_lock->lock;
		if (cmp < 0)
			break;
	}

	return NULL;
}

void scoutfs_inode_init_index_key(struct scoutfs_key *key, u8 type, u64 major,
				  u32 minor, u64 ino)
{
	*key = (struct scoutfs_key) {
		.sk_zone = SCOUTFS_INODE_INDEX_ZONE,
		.sk_type = type,
		.skii_major = cpu_to_le64(major),
		.skii_ino = cpu_to_le64(ino),
	};
}

/*
 * The inode info reflects the current inode index items.  Create or delete
 * index items to bring the index in line with the caller's item.  The list
 * should contain locks that cover any item modifications that are made.
 */
static int update_index_items(struct super_block *sb,
			      struct scoutfs_inode_info *si, u64 ino, u8 type,
			      u64 major, u32 minor,
			      struct list_head *lock_list)
{
	struct scoutfs_lock *ins_lock;
	struct scoutfs_lock *del_lock;
	struct scoutfs_key ins;
	struct scoutfs_key del;
	int ret;
	int err;

	if (!will_ins_index(si, type, major, minor))
		return 0;

	trace_scoutfs_create_index_item(sb, type, major, minor, ino);

	scoutfs_inode_init_index_key(&ins, type, major, minor, ino);

	ins_lock = find_index_lock(lock_list, type, major, minor, ino);
	ret = scoutfs_item_create_force(sb, &ins, NULL, 0, ins_lock);
	if (ret || !will_del_index(si, type, major, minor))
		return ret;

	trace_scoutfs_delete_index_item(sb, type, si->item_majors[type],
					si->item_minors[type], ino);

	scoutfs_inode_init_index_key(&del, type, si->item_majors[type],
				     si->item_minors[type], ino);

	del_lock = find_index_lock(lock_list, type, si->item_majors[type],
				   si->item_minors[type], ino);
	ret = scoutfs_item_delete_force(sb, &del, del_lock);
	if (ret) {
		err = scoutfs_item_delete(sb, &ins, ins_lock);
		BUG_ON(err);
	}

	return ret;
}

static int update_indices(struct super_block *sb,
			  struct scoutfs_inode_info *si, u64 ino, umode_t mode,
			  struct scoutfs_inode *sinode,
			  struct list_head *lock_list)
{
	struct index_update {
		u8 type;
		u64 major;
		u32 minor;
	} *upd, upds[] = {
		{ SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			le64_to_cpu(sinode->meta_seq), 0 },
		{ SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
			le64_to_cpu(sinode->data_seq), 0 },
	};
	int ret;
	int i;

	for (i = 0, upd = upds; i < ARRAY_SIZE(upds); i++, upd++) {
		if (!inode_has_index(mode, upd->type))
			continue;

		ret = update_index_items(sb, si, ino, upd->type, upd->major,
					 upd->minor, lock_list);
		if (ret)
			break;
	}

	return ret;
}

/*
 * Every time we modify the inode in memory we copy it to its inode
 * item.  This lets us write out items without having to track down
 * dirty vfs inodes.
 *
 * The caller makes sure that the item is dirty and pinned so they don't
 * have to deal with errors and unwinding after they've modified the vfs
 * inode and get here.
 *
 * Index items that track inode fields are updated here as we update the
 * inode item.  The caller must have acquired locks on all the index
 * items that might change.
 */
void scoutfs_update_inode_item(struct inode *inode, struct scoutfs_lock *lock,
			       struct list_head *lock_list)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	const u64 ino = scoutfs_ino(inode);
	struct scoutfs_key key;
	struct scoutfs_inode sinode;
	int ret;
	int err;

	mutex_lock(&si->item_mutex);

	/* set the meta version once per trans for any inode updates */
	scoutfs_inode_set_meta_seq(inode);

	/* only race with other inode field stores once */
	store_inode(&sinode, inode);

	ret = update_indices(sb, si, ino, inode->i_mode, &sinode, lock_list);
	BUG_ON(ret);

	init_inode_key(&key, ino);

	err = scoutfs_item_update(sb, &key, &sinode, sizeof(sinode), lock);
	if (err) {
		scoutfs_err(sb, "inode %llu update err %d", ino, err);
		BUG_ON(err);
	}

	set_item_info(si, &sinode);
	trace_scoutfs_update_inode(inode);

	mutex_unlock(&si->item_mutex);
}

/*
 * We map the item to coarse locks here.  This reduces the number of
 * locks we track and means that when we later try to find the lock that
 * covers an item we can deal with the item update changing a little
 * while still being covered.  It does mean we have to share some logic
 * with lock naming.
 */
static int add_index_lock(struct list_head *list, u64 ino, u8 type, u64 major,
			  u32 minor)
{
	struct index_lock *ind_lock;

	clamp_inode_index(type, &major, &minor, &ino);

	list_for_each_entry(ind_lock, list, head) {
		if (ind_lock->type == type && ind_lock->major == major &&
		    ind_lock->minor == minor && ind_lock->ino == ino) {
			return 0;
		}
	}

	ind_lock = kzalloc(sizeof(struct index_lock), GFP_NOFS);
	if (!ind_lock)
		return -ENOMEM;

	ind_lock->type = type;
	ind_lock->major = major;
	ind_lock->minor = minor;
	ind_lock->ino = ino;
	list_add(&ind_lock->head, list);

	return 0;
}

static int prepare_index_items(struct scoutfs_inode_info *si,
			       struct list_head *list, u64 ino, umode_t mode,
			       u8 type, u64 major, u32 minor)
{
	int ret;

	if (will_ins_index(si, type, major, minor)) {
		ret = add_index_lock(list, ino, type, major, minor);
		if (ret)
			return ret;
	}

	if (will_del_index(si, type, major, minor)) {
		ret = add_index_lock(list, ino, type, si->item_majors[type],
				     si->item_minors[type]);
		if (ret)
			return ret;
	}

	return 0;
}

/*
 * Return the data seq that we expect to see in the updated inode.  The
 * caller tells us if they know they're going to update it.  If the
 * inode doesn't exist it'll also get the current data_seq.
 */
static u64 upd_data_seq(struct scoutfs_sb_info *sbi,
			struct scoutfs_inode_info *si, bool set_data_seq)
{
	if (!si || !si->have_item || set_data_seq)
		return sbi->trans_seq;

	return si->item_majors[SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE];
}

/*
 * Prepare locks that will cover the inode index items that will be
 * modified when this inode's item is updated during the upcoming
 * transaction.
 *
 * To lock the index items that will be created we need to predict the
 * new indexed values.  We assume that the meta seq will always be set
 * to the current seq.  This will usually be a nop in a running
 * transaction.  The caller tells us what the size will be and whether
 * data_seq will also be set to the current transaction.
 */
static int prepare_indices(struct super_block *sb, struct list_head *list,
			   struct scoutfs_inode_info *si, u64 ino,
			   umode_t mode, bool set_data_seq)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct index_update {
		u8 type;
		u64 major;
		u32 minor;
	} *upd, upds[] = {
		{ SCOUTFS_INODE_INDEX_META_SEQ_TYPE, sbi->trans_seq, 0},
		{ SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
			upd_data_seq(sbi, si, set_data_seq), 0},
	};
	int ret;
	int i;

	for (i = 0, upd = upds; i < ARRAY_SIZE(upds); i++, upd++) {
		if (!inode_has_index(mode, upd->type))
			continue;

		ret = prepare_index_items(si, list, ino, mode,
					  upd->type, upd->major, upd->minor);
		if (ret)
			break;
	}

	return ret;
}

int scoutfs_inode_index_prepare(struct super_block *sb, struct list_head *list,
			        struct inode *inode, bool set_data_seq)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	return prepare_indices(sb, list, si, scoutfs_ino(inode),
			       inode->i_mode, set_data_seq);
}

/*
 * This is used to initially create the index items for a newly created
 * inode.  We don't have a populated vfs inode yet.  The existing
 * indexed values don't matter because it's 'have_item' is false.  It
 * will try to create all the appropriate index items.
 */
int scoutfs_inode_index_prepare_ino(struct super_block *sb,
				    struct list_head *list, u64 ino,
				    umode_t mode)
{
	return prepare_indices(sb, list, NULL, ino, mode, true);
}

/*
 * Prepare the locks needed to delete all the index items associated
 * with the inode.  We know the items have to exist and can skip straight
 * to adding locks for each of them.
 */
static int prepare_index_deletion(struct super_block *sb,
				  struct list_head *list, u64 ino,
				  umode_t mode, struct scoutfs_inode *sinode)
{
	struct index_item {
		u8 type;
		u64 major;
		u32 minor;
	} *ind, inds[] = {
		{ SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			le64_to_cpu(sinode->meta_seq), 0 },
		{ SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
			le64_to_cpu(sinode->data_seq), 0 },
	};
	int ret;
	int i;

	for (i = 0, ind = inds; i < ARRAY_SIZE(inds); i++, ind++) {
		if (!inode_has_index(mode, ind->type))
			continue;

		ret = add_index_lock(list, ino, ind->type,  ind->major,
				     ind->minor);
		if (ret)
			break;
	}

	return ret;
}

/*
 * Sample the transaction sequence before we start checking it to see if
 * indexed meta seq and data seq items will change.
 */
int scoutfs_inode_index_start(struct super_block *sb, u64 *seq)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);

	/* XXX this feels racey in a bad way :) */
	*seq = sbi->trans_seq;
	return 0;
}

/*
 * Acquire the prepared index locks and hold the transaction.  If the
 * sequence number changes as we enter the transaction then we need to
 * retry so that we can use the new seq to prepare locks.
 *
 * Returns > 0 if the seq changed and the locks should be retried.
 */
int scoutfs_inode_index_try_lock_hold(struct super_block *sb,
				      struct list_head *list, u64 seq, bool allocing)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct index_lock *ind_lock;
	int ret = 0;

	list_sort(NULL, list, cmp_index_lock);

	list_for_each_entry(ind_lock, list, head) {
		ret = scoutfs_lock_inode_index(sb, SCOUTFS_LOCK_WRITE_ONLY,
					       ind_lock->type, ind_lock->major,
					       ind_lock->ino, &ind_lock->lock);
		if (ret)
			goto out;
	}

	ret = scoutfs_hold_trans(sb, allocing);
	if (ret == 0 && seq != sbi->trans_seq) {
		scoutfs_release_trans(sb);
		ret = 1;
	}

out:
	if (ret)
		scoutfs_inode_index_unlock(sb, list);

	return ret;
}

int scoutfs_inode_index_lock_hold(struct inode *inode, struct list_head *list,
				  bool set_data_seq, bool allocing)
{
	struct super_block *sb = inode->i_sb;
	int ret;
	u64 seq;

	do {
		ret = scoutfs_inode_index_start(sb, &seq) ?:
		      scoutfs_inode_index_prepare(sb, list, inode,
						  set_data_seq) ?:
		      scoutfs_inode_index_try_lock_hold(sb, list, seq, allocing);
	} while (ret > 0);

	return ret;
}

/*
 * Unlocks and frees all the locks on the list.
 */
void scoutfs_inode_index_unlock(struct super_block *sb, struct list_head *list)
{
	struct index_lock *ind_lock;
	struct index_lock *tmp;

	list_for_each_entry_safe(ind_lock, tmp, list, head) {
		scoutfs_unlock(sb, ind_lock->lock, SCOUTFS_LOCK_WRITE_ONLY);
		list_del_init(&ind_lock->head);
		kfree(ind_lock);
	}
}

/* this is called on final inode cleanup so enoent is fine */
static int remove_index(struct super_block *sb, u64 ino, u8 type, u64 major,
			u32 minor, struct list_head *ind_locks)
{
	struct scoutfs_key key;
	struct scoutfs_lock *lock;
	int ret;

	scoutfs_inode_init_index_key(&key, type, major, minor, ino);

	lock = find_index_lock(ind_locks, type, major, minor, ino);
	ret = scoutfs_item_delete_force(sb, &key, lock);
	if (ret == -ENOENT)
		ret = 0;
	return ret;
}

/*
 * Remove all the inode's index items.  The caller has ensured that
 * there are no more active users of the inode.  This can be racing with
 * users of the inode index items.  Once we can use them we'll get CW
 * locks around the index items to invalidate remote caches.  Racing
 * users of the index items already have to deal with the possibility
 * that the inodes returned by the index queries can go out of sync by
 * the time they get to it, including being deleted.
 */
static int remove_index_items(struct super_block *sb, u64 ino,
			      struct scoutfs_inode *sinode,
			      struct list_head *ind_locks)
{
	umode_t mode = le32_to_cpu(sinode->mode);
	int ret;

	ret = remove_index(sb, ino, SCOUTFS_INODE_INDEX_META_SEQ_TYPE,
			   le64_to_cpu(sinode->meta_seq), 0, ind_locks);
	if (ret == 0 && S_ISREG(mode))
		ret = remove_index(sb, ino, SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE,
				   le64_to_cpu(sinode->data_seq), 0, ind_locks);
	return ret;
}

/*
 * A quick atomic sample of the last inode number that's been allocated.
 */
u64 scoutfs_last_ino(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super = &sbi->super;
	u64 last;

	spin_lock(&sbi->next_ino_lock);
	last = le64_to_cpu(super->next_ino);
	spin_unlock(&sbi->next_ino_lock);

	return last;
}

/*
 * Return an allocated and unused inode number.  Returns -ENOSPC if
 * we're out of inode.
 *
 * Each parent directory has its own pool of free inode numbers.  Items
 * are sorted by their inode numbers as they're stored in segments.
 * This will tend to group together files that are created in a
 * directory at the same time in segments.  Concurrent creation across
 * different directories will be stored in their own regions.
 *
 * Inode numbers are never reclaimed.  If the inode is evicted or we're
 * unmounted the pending inode numbers will be lost.  Asking for a
 * relatively small number from the server each time will tend to
 * minimize that loss while still being large enough for typical
 * directory file counts.
 */
int scoutfs_alloc_ino(struct super_block *sb, bool is_dir, u64 *ino_ret)
{
	DECLARE_INODE_SB_INFO(sb, inf);
	struct inode_allocator *ia;
	u64 ino;
	u64 nr;
	int ret;

	ia = is_dir ? &inf->dir_ino_alloc : &inf->ino_alloc;

	spin_lock(&ia->lock);

	if (ia->nr == 0) {
		spin_unlock(&ia->lock);
		ret = scoutfs_client_alloc_inodes(sb,
					SCOUTFS_LOCK_INODE_GROUP_NR * 10,
					&ino, &nr);
		if (ret < 0)
			goto out;
		spin_lock(&ia->lock);
		if (ia->nr == 0) {
			ia->ino = ino;
			ia->nr = nr;
		}
	}

	*ino_ret = ia->ino++;
	ia->nr--;

	spin_unlock(&ia->lock);
	ret = 0;
out:
	trace_scoutfs_alloc_ino(sb, ret, *ino_ret, ia->ino, ia->nr);
	return ret;
}

/*
 * Allocate and initialize a new inode.  The caller is responsible for
 * creating links to it and updating it.  @dir can be null.
 */
struct inode *scoutfs_new_inode(struct super_block *sb, struct inode *dir,
				umode_t mode, dev_t rdev, u64 ino,
				struct scoutfs_lock *lock)
{
	struct scoutfs_inode_info *si;
	struct scoutfs_key key;
	struct scoutfs_inode sinode;
	struct inode *inode;
	int ret;

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	si = SCOUTFS_I(inode);
	si->ino = ino;
	si->data_version = 0;
	si->online_blocks = 0;
	si->offline_blocks = 0;
	si->next_readdir_pos = SCOUTFS_DIRENT_FIRST_POS;
	si->next_xattr_id = 0;
	si->have_item = false;
	atomic64_set(&si->last_refreshed, lock->refresh_gen);
	scoutfs_lock_add_coverage(sb, lock, &si->ino_lock_cov);
	si->drop_invalidated = false;
	si->flags = 0;

	scoutfs_inode_set_meta_seq(inode);
	scoutfs_inode_set_data_seq(inode);

	inode->i_ino = ino; /* XXX overflow */
	inode_init_owner(inode, dir, mode);
	inode_set_bytes(inode, 0);
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	inode->i_rdev = rdev;
	set_inode_ops(inode);

	store_inode(&sinode, inode);
	init_inode_key(&key, scoutfs_ino(inode));

	ret = scoutfs_omap_inc(sb, ino);
	if (ret < 0)
		goto out;

	ret = scoutfs_item_create(sb, &key, &sinode, sizeof(sinode), lock);
	if (ret < 0)
		scoutfs_omap_dec(sb, ino);
out:
	if (ret) {
		iput(inode);
		inode = ERR_PTR(ret);
	}

	return inode;
}

static void init_orphan_key(struct scoutfs_key *key, u64 ino)
{
	*key = (struct scoutfs_key) {
		.sk_zone = SCOUTFS_ORPHAN_ZONE,
		.sko_ino = cpu_to_le64(ino),
		.sk_type = SCOUTFS_ORPHAN_TYPE,
	};
}

/*
 * Create an orphan item.  The orphan items are maintained in their own
 * zone under a write only lock while the caller has the inode protected
 * by a write lock.
 */
int scoutfs_inode_orphan_create(struct super_block *sb, u64 ino, struct scoutfs_lock *lock)
{
	struct scoutfs_key key;

	init_orphan_key(&key, ino);

	return scoutfs_item_create_force(sb, &key, NULL, 0, lock);
}

int scoutfs_inode_orphan_delete(struct super_block *sb, u64 ino, struct scoutfs_lock *lock)
{
	struct scoutfs_key key;

	init_orphan_key(&key, ino);

	return scoutfs_item_delete_force(sb, &key, lock);
}

struct deleting_ino_entry {
	struct list_head head;
	u64 ino;
};

static bool added_deleting_ino(struct inode_sb_info *inf, struct deleting_ino_entry *del, u64 ino)
{
	struct deleting_ino_entry *tmp;
	bool added = true;

	spin_lock(&inf->deleting_items_lock);

	list_for_each_entry(tmp, &inf->deleting_items_list, head) {
		if (tmp->ino == ino) {
			added = false;
			break;
		}
	}

	if (added) {
		del->ino = ino;
		list_add_tail(&del->head, &inf->deleting_items_list);
	}

	spin_unlock(&inf->deleting_items_lock);

	return added;
}

static void del_deleting_ino(struct inode_sb_info *inf, struct deleting_ino_entry *del)
{
	if (del->ino) {
		spin_lock(&inf->deleting_items_lock);
		list_del_init(&del->head);
		spin_unlock(&inf->deleting_items_lock);
	}
}

/*
 * Remove all the items associated with a given inode.  This is only
 * called once nlink has dropped to zero and nothing has the inode open
 * so we don't have to worry about dirents referencing the inode or link
 * backrefs.  Dropping nlink to 0 also created an orphan item.  That
 * orphan item will continue triggering attempts to finish previous
 * partial deletion until all deletion is complete and the orphan item
 * is removed.
 *
 * Currently this can be called multiple times for multiple cached
 * inodes for a given ino number (ilookup avoids freeing inodes to avoid
 * cluster lock<->inode flag waiting inversions).  Some items are not
 * safe to delete concurrently, for example concurrent data truncation
 * could free extents multiple times.  We use a very silly list of inos
 * being deleted.  Duplicates just return success.  If the first
 * deletion ends up failing orphan deletion will come back around later
 * and retry.
 */
static int delete_inode_items(struct super_block *sb, u64 ino, struct scoutfs_lock *lock,
			      struct scoutfs_lock *orph_lock)
{
	DECLARE_INODE_SB_INFO(sb, inf);
	struct deleting_ino_entry del = {{NULL, }};
	struct scoutfs_inode sinode;
	struct scoutfs_key key;
	LIST_HEAD(ind_locks);
	bool release = false;
	umode_t mode;
	u64 ind_seq;
	u64 size;
	int ret;

	if (!added_deleting_ino(inf, &del, ino)) {
		ret = 0;
		goto out;
	}

	init_inode_key(&key, ino);

	ret = scoutfs_item_lookup_exact(sb, &key, &sinode, sizeof(sinode),
					lock);
	if (ret < 0) {
		if (ret == -ENOENT)
			ret = 0;
		goto out;
	}

	/* XXX corruption, inode probably won't be freed without repair */
	if (le32_to_cpu(sinode.nlink)) {
		scoutfs_warn(sb, "Dangling orphan item for inode %llu.", ino);
		ret = -EIO;
		goto out;
	}

	mode = le32_to_cpu(sinode.mode);
	size = le64_to_cpu(sinode.size);
	trace_scoutfs_delete_inode(sb, ino, mode, size);

	/* remove data items in their own transactions */
	if (S_ISREG(mode)) {
		ret = scoutfs_data_truncate_items(sb, NULL, ino, 0, ~0ULL,
						  false, lock);
		if (ret)
			goto out;
	}

	ret = scoutfs_xattr_drop(sb, ino, lock);
	if (ret)
		goto out;

	/* then delete the small known number of remaining inode items */
retry:
	ret = scoutfs_inode_index_start(sb, &ind_seq) ?:
	      prepare_index_deletion(sb, &ind_locks, ino, mode, &sinode) ?:
	      scoutfs_inode_index_try_lock_hold(sb, &ind_locks, ind_seq, false);
	if (ret > 0)
		goto retry;
	if (ret)
		goto out;

	release = true;

	ret = remove_index_items(sb, ino, &sinode, &ind_locks);
	if (ret)
		goto out;

	if (S_ISLNK(mode)) {
		ret = scoutfs_symlink_drop(sb, ino, lock, size);
		if (ret)
			goto out;
	}

	ret = scoutfs_item_delete(sb, &key, lock);
	if (ret)
		goto out;

	ret = scoutfs_inode_orphan_delete(sb, ino, orph_lock);
out:
	del_deleting_ino(inf, &del);
	if (release)
		scoutfs_release_trans(sb);
	scoutfs_inode_index_unlock(sb, &ind_locks);

	return ret;
}

/*
 * iput_final has already written out the dirty pages to the inode
 * before we get here.  We're left with a clean inode that we have to
 * tear down.  We use locking and open inode number bitmaps to decide if
 * we should finally destroy an inode that is no longer open nor
 * reachable through directory entries.
 *
 * Because lookup ignores freeing inodes we can get here from multiple
 * instances of an inode that is being deleted.  Orphan scanning in
 * particular can race with deletion.   delete_inode_items() resolves
 * concurrent attempts.
 */
void scoutfs_evict_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	const u64 ino = scoutfs_ino(inode);
	struct scoutfs_lock *orph_lock;
	struct scoutfs_lock *lock;
	int ret;

	trace_scoutfs_evict_inode(inode->i_sb, scoutfs_ino(inode),
				  inode->i_nlink, is_bad_inode(inode));

	if (is_bad_inode(inode))
		goto clear;

	truncate_inode_pages_final(&inode->i_data);

	ret = scoutfs_omap_should_delete(sb, inode, &lock, &orph_lock);
	if (ret > 0) {
		ret = delete_inode_items(inode->i_sb, scoutfs_ino(inode), lock, orph_lock);
		scoutfs_unlock(sb, lock, SCOUTFS_LOCK_WRITE);
		scoutfs_unlock(sb, orph_lock, SCOUTFS_LOCK_WRITE_ONLY);
	}
	if (ret < 0) {
		scoutfs_err(sb, "error %d while checking to delete inode nr %llu, it might linger.",
			    ret, ino);
	}

	scoutfs_omap_dec(sb, ino);

clear:
	clear_inode(inode);
}

/*
 * We want to remove inodes from the cache as their count goes to 0 if
 * they're no longer covered by a cluster lock or if while locked they
 * were unlinked.
 *
 * We don't want unused cached inodes to linger outside of cluster
 * locking so that they don't prevent final inode deletion on other
 * nodes.  We don't have specific per-inode or per-dentry locks which
 * would otherwise remove the stale caches as they're invalidated.
 * Stale cached inodes provide little value because they're going to be
 * refreshed the next time they're locked.  Populating the item cache
 * and loading the inode item is a lot more expensive than initializing
 * and inserting a newly allocated vfs inode.
 */
int scoutfs_drop_inode(struct inode *inode)
{
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;

	trace_scoutfs_drop_inode(sb, scoutfs_ino(inode), inode->i_nlink, inode_unhashed(inode),
				 si->drop_invalidated);

	return si->drop_invalidated || !scoutfs_lock_is_covered(sb, &si->ino_lock_cov) ||
	       generic_drop_inode(inode);
}

static void iput_worker(struct work_struct *work)
{
	struct inode_sb_info *inf = container_of(work, struct inode_sb_info, iput_work);
	struct scoutfs_inode_info *si;
	struct scoutfs_inode_info *tmp;
	struct llist_node *inodes;
	bool more;

	inodes = llist_del_all(&inf->iput_llist);

	llist_for_each_entry_safe(si, tmp, inodes, iput_llnode) {
		do {
			more = atomic_dec_return(&si->iput_count) > 0;
			iput(&si->inode);
		} while (more);
	}
}

/*
 * Final iput can get into evict and perform final inode deletion which
 * can delete a lot of items spanning multiple cluster locks and
 * transactions.  It should be understood as a heavy high level
 * operation, more like file writing and less like dropping a refcount.
 *
 * Unfortunately we also have incentives to use igrab/iput from internal
 * contexts that have no business doing that work, like lock
 * invalidation or dirty inode writeback during transaction commit.
 *
 * In those cases we can kick iput off to background work context.
 * Nothing stops multiple puts of an inode before the work runs so we
 * can track multiple puts in flight.
 */
void scoutfs_inode_queue_iput(struct inode *inode)
{
	DECLARE_INODE_SB_INFO(inode->i_sb, inf);
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	if (atomic_inc_return(&si->iput_count) == 1)
		llist_add(&si->iput_llnode, &inf->iput_llist);
	smp_wmb(); /* count and list visible before work executes */
	schedule_work(&inf->iput_work);
}

/*
 * All mounts are performing this work concurrently.  We introduce
 * significant jitter between them to try and keep them from all
 * bunching up and working on the same inodes.
 */
static void schedule_orphan_dwork(struct inode_sb_info *inf)
{
#define ORPHAN_SCAN_MIN_MS (10 * MSEC_PER_SEC)
#define ORPHAN_SCAN_JITTER_MS (40 * MSEC_PER_SEC)
	unsigned long delay = msecs_to_jiffies(ORPHAN_SCAN_MIN_MS +
					       prandom_u32_max(ORPHAN_SCAN_JITTER_MS));
	if (!inf->stopped) {
		delay = msecs_to_jiffies(ORPHAN_SCAN_MIN_MS +
					 prandom_u32_max(ORPHAN_SCAN_JITTER_MS));
		schedule_delayed_work(&inf->orphan_scan_dwork, delay);
	}
}

/*
 * Find and delete inodes whose only remaining reference is the
 * persistent orphan item that was created as they were unlinked.
 *
 * Orphan items are created as the final directory entry referring to an
 * inode is deleted.  They're deleted as the final cached inode is
 * evicted and the inode items are destroyed.  They can linger if all
 * the cached inodes pinning the inode fail to delete as they are
 * evicted from the cache -- either through crashing or errors.
 *
 * This work runs in all mounts in the background looking for those
 * orphaned inodes that weren't fully deleted.
 *
 * First, we search for items in the current persistent fs root.  We'll
 * only find orphan items that made it to the fs root after being merged
 * from a mount's log btree.  This naturally avoids orphan items that
 * exist while inodes have been unlinked but are still cached, including
 * O_TMPFILE inodes that are actively used during normal operations.
 * Scanning the read-only persistent fs root uses cached blocks and
 * avoids the lock contention we'd cause if we tried to use the
 * consistent item cache.  The downside is that it adds a bit of
 * latency.  If an orphan was created in error it'll take until the
 * mount's log btree is finalized and merged.  A crash will have the log
 * btree merged after it is fenced.
 *
 * Once we find candidate orphan items we can first check our local
 * inode cache for inodes that are already on their way to eviction and
 * can be skipped.  Then we ask the server for the open map containing
 * the inode.  Only if we don't have it cached, and no one else does, do
 * we try and read it into our cache and evict it to trigger the final
 * inode deletion process.
 */
static void inode_orphan_scan_worker(struct work_struct *work)
{
	struct inode_sb_info *inf = container_of(work, struct inode_sb_info,
						 orphan_scan_dwork.work);
	struct super_block *sb = inf->sb;
	struct scoutfs_open_ino_map omap;
	struct scoutfs_net_roots roots;
	SCOUTFS_BTREE_ITEM_REF(iref);
	struct scoutfs_key last;
	struct scoutfs_key key;
	struct inode *inode;
	u64 group_nr;
	int bit_nr;
	u64 ino;
	int ret;

	scoutfs_inc_counter(sb, orphan_scan);

	init_orphan_key(&last, U64_MAX);
	omap.args.group_nr = cpu_to_le64(U64_MAX);

	ret = scoutfs_client_get_roots(sb, &roots);
	if (ret)
		goto out;

	for (ino = SCOUTFS_ROOT_INO + 1; ino != 0; ino++) {
		if (inf->stopped) {
			ret = 0;
			goto out;
		}

		/* find the next orphan item */
		init_orphan_key(&key, ino);
		ret = scoutfs_btree_next(sb, &roots.fs_root, &key, &iref);
		if (ret < 0) {
			if (ret == -ENOENT)
				break;
			goto out;
		}

		key = *iref.key;
		scoutfs_btree_put_iref(&iref);

		if (scoutfs_key_compare(&key, &last) > 0)
			break;

		scoutfs_inc_counter(sb, orphan_scan_item);
		ino = le64_to_cpu(key.sko_ino);

		/* locally cached inodes will already be deleted */
		inode = scoutfs_ilookup(sb, ino);
		if (inode) {
			scoutfs_inc_counter(sb, orphan_scan_cached);
			iput(inode);
			continue;
		}

		/* get an omap that covers the orphaned ino */
		group_nr = ino >> SCOUTFS_OPEN_INO_MAP_SHIFT;
		bit_nr = ino & SCOUTFS_OPEN_INO_MAP_MASK;

		if (le64_to_cpu(omap.args.group_nr) != group_nr) {
			ret = scoutfs_client_open_ino_map(sb, group_nr, &omap);
			if (ret < 0)
				goto out;
		}

		/* don't need to evict if someone else has it open (cached) */
		if (test_bit_le(bit_nr, omap.bits)) {
			scoutfs_inc_counter(sb, orphan_scan_omap_set);
			continue;
		}

		/* try to cached and evict unused inode to delete, can be racing */
		inode = scoutfs_iget(sb, ino, 0);
		if (IS_ERR(inode)) {
			ret = PTR_ERR(inode);
			if (ret == -ENOENT)
				continue;
			else
				goto out;
		}

		scoutfs_inc_counter(sb, orphan_scan_read);
		SCOUTFS_I(inode)->drop_invalidated = true;
		iput(inode);
	}

	ret = 0;

out:
	if (ret < 0)
		scoutfs_inc_counter(sb, orphan_scan_error);

	schedule_orphan_dwork(inf);
}

/*
 * Track an inode that could have dirty pages.  Used to kick off
 * writeback on all dirty pages during transaction commit without tying
 * ourselves in knots trying to call through the high level vfs sync
 * methods.
 *
 * File data block allocations tend to advance through free space so we
 * add the inode to the end of the list to roughly encourage sequential
 * IO.
 *
 * This is called by writers who hold the inode and transaction.  The
 * inode is removed from the list by evict->destroy if it's unlinked
 * during the transaction or by committing the transaction.  Pruning the
 * icache won't try to evict the inode as long as it has dirty buffers.
 */
void scoutfs_inode_queue_writeback(struct inode *inode)
{
	DECLARE_INODE_SB_INFO(inode->i_sb, inf);
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);

	if (list_empty(&si->writeback_entry)) {
		spin_lock(&inf->writeback_lock);
		if (list_empty(&si->writeback_entry))
			list_add_tail(&si->writeback_entry, &inf->writeback_list);
		spin_unlock(&inf->writeback_lock);
	}
}

/*
 * Walk our dirty inodes and either start dirty page writeback or wait
 * for writeback to complete.
 *
 * This is called by transaction committing so other writers are
 * excluded.  We're still very careful to iterate over the tree while it
 * and the inodes could be changing.
 *
 * Because writes are excluded we know that there's no remaining dirty
 * pages once waiting returns successfully.
 *
 * XXX not sure what to do about retrying io errors.
 */
int scoutfs_inode_walk_writeback(struct super_block *sb, bool write)
{
	DECLARE_INODE_SB_INFO(sb, inf);
	struct scoutfs_inode_info *si;
	struct scoutfs_inode_info *tmp;
	struct inode *inode;
	int ret;

	spin_lock(&inf->writeback_lock);

	list_for_each_entry_safe(si, tmp, &inf->writeback_list, writeback_entry) {
		inode = igrab(&si->inode);
		if (!inode)
			continue;

		spin_unlock(&inf->writeback_lock);

		if (write)
			ret = filemap_fdatawrite(inode->i_mapping);
		else
			ret = filemap_fdatawait(inode->i_mapping);
		trace_scoutfs_inode_walk_writeback(sb, scoutfs_ino(inode),
						   write, ret);
		if (ret) {
			scoutfs_inode_queue_iput(inode);
			goto out;
		}

		spin_lock(&inf->writeback_lock);

		/* restore tmp after reacquiring lock */
		if (WARN_ON_ONCE(list_empty(&si->writeback_entry)))
			tmp = list_first_entry(&inf->writeback_list, struct scoutfs_inode_info,
					       writeback_entry);
		else
			tmp = list_next_entry(si, writeback_entry);

		if (!write)
			list_del_init(&si->writeback_entry);

		scoutfs_inode_queue_iput(inode);
	}

	spin_unlock(&inf->writeback_lock);
out:

	return ret;
}

int scoutfs_inode_setup(struct super_block *sb)
{
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct inode_sb_info *inf;

	inf = kzalloc(sizeof(struct inode_sb_info), GFP_KERNEL);
	if (!inf)
		return -ENOMEM;

	inf->sb = sb;
	spin_lock_init(&inf->writeback_lock);
	INIT_LIST_HEAD(&inf->writeback_list);
	spin_lock_init(&inf->dir_ino_alloc.lock);
	spin_lock_init(&inf->ino_alloc.lock);
	INIT_DELAYED_WORK(&inf->orphan_scan_dwork, inode_orphan_scan_worker);
	spin_lock_init(&inf->deleting_items_lock);
	INIT_LIST_HEAD(&inf->deleting_items_list);
	INIT_WORK(&inf->iput_work, iput_worker);
	init_llist_head(&inf->iput_llist);

	sbi->inode_sb_info = inf;

	return 0;
}

/*
 * Our inode subsystem is setup pretty early but orphan scanning uses
 * many other subsystems like networking and the server.  We only kick
 * it off once everything is ready.
 */
void scoutfs_inode_start(struct super_block *sb)
{
	DECLARE_INODE_SB_INFO(sb, inf);

	schedule_orphan_dwork(inf);
}

/*
 * Orphan scanning can instantiate inodes.  We shut it down before
 * calling into the vfs to tear down dentries and inodes during unmount.
 */
void scoutfs_inode_orphan_stop(struct super_block *sb)
{
	DECLARE_INODE_SB_INFO(sb, inf);

	if (inf) {
		inf->stopped = true;
		cancel_delayed_work_sync(&inf->orphan_scan_dwork);
	}
}

void scoutfs_inode_flush_iput(struct super_block *sb)
{
	DECLARE_INODE_SB_INFO(sb, inf);

	if (inf)
		flush_work(&inf->iput_work);
}

void scoutfs_inode_destroy(struct super_block *sb)
{
	struct inode_sb_info *inf = SCOUTFS_SB(sb)->inode_sb_info;

	kfree(inf);
}

void scoutfs_inode_exit(void)
{
	if (scoutfs_inode_cachep) {
		rcu_barrier();
		kmem_cache_destroy(scoutfs_inode_cachep);
		scoutfs_inode_cachep = NULL;
	}
}

int scoutfs_inode_init(void)
{
	scoutfs_inode_cachep = kmem_cache_create("scoutfs_inode_info",
					sizeof(struct scoutfs_inode_info), 0,
					SLAB_RECLAIM_ACCOUNT,
					scoutfs_inode_ctor);
	if (!scoutfs_inode_cachep)
		return -ENOMEM;

	return 0;
}
