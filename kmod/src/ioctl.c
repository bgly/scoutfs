/*
 * Copyright (C) 2016 Versity Software, Inc.  All rights reserved.
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
#include <linux/file.h>
#include <linux/uaccess.h>
#include <linux/compiler.h>
#include <linux/uio.h>
#include <linux/slab.h>
#include <linux/mount.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/aio.h>
#include <linux/list_sort.h>

#include "format.h"
#include "key.h"
#include "dir.h"
#include "ioctl.h"
#include "super.h"
#include "inode.h"
#include "item.h"
#include "forest.h"
#include "data.h"
#include "client.h"
#include "lock.h"
#include "trans.h"
#include "xattr.h"
#include "hash.h"
#include "srch.h"
#include "alloc.h"
#include "server.h"
#include "counters.h"
#include "scoutfs_trace.h"

/*
 * We make inode index items coherent by locking fixed size regions of
 * the key space.  But the inode index item key space is vast and can
 * have huge sparse regions.  To avoid trying every possible lock in the
 * sparse regions we use the manifest to find the next stable key in the
 * key space after we find no items in a given lock region.  This is
 * relatively cheap because reading is going to check the segments
 * anyway.
 *
 * This is copying to userspace while holding a read lock.  This is safe
 * because faulting can send a request for a write lock while the read
 * lock is being used.  The cluster locks don't block tasks in a node,
 * they match and the tasks fall back to local locking.  In this case
 * the spin locks around the item cache.
 */
static long scoutfs_ioc_walk_inodes(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_walk_inodes __user *uwalk = (void __user *)arg;
	struct scoutfs_ioctl_walk_inodes walk;
	struct scoutfs_ioctl_walk_inodes_entry ent;
	struct scoutfs_key next_key;
	struct scoutfs_key last_key;
	struct scoutfs_key key;
	struct scoutfs_lock *lock;
	u64 last_seq;
	int ret = 0;
	u32 nr = 0;
	u8 type;

	if (copy_from_user(&walk, uwalk, sizeof(walk)))
		return -EFAULT;

	trace_scoutfs_ioc_walk_inodes(sb, &walk);

	if (walk.index == SCOUTFS_IOC_WALK_INODES_META_SEQ)
		type = SCOUTFS_INODE_INDEX_META_SEQ_TYPE;
	else if (walk.index == SCOUTFS_IOC_WALK_INODES_DATA_SEQ)
		type = SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE;
	else
		return -EINVAL;

	/* clamp results to the inodes in the farthest stable seq */
	if (type == SCOUTFS_INODE_INDEX_META_SEQ_TYPE ||
	    type == SCOUTFS_INODE_INDEX_DATA_SEQ_TYPE) {

		ret = scoutfs_client_get_last_seq(sb, &last_seq);
		if (ret)
			return ret;

		if (last_seq < walk.last.major) {
			walk.last.major = last_seq;
			walk.last.minor = ~0;
			walk.last.ino = ~0ULL;
		}
	}

	scoutfs_inode_init_index_key(&key, type, walk.first.major,
				     walk.first.minor, walk.first.ino);
	scoutfs_inode_init_index_key(&last_key, type, walk.last.major,
				     walk.last.minor, walk.last.ino);

	/* cap nr to the max the ioctl can return to a compat task */
	walk.nr_entries = min_t(u64, walk.nr_entries, INT_MAX);

	ret = scoutfs_lock_inode_index(sb, SCOUTFS_LOCK_READ, type,
				       walk.first.major, walk.first.ino,
				       &lock);
	if (ret < 0)
		goto out;

	for (nr = 0; nr < walk.nr_entries; ) {

		ret = scoutfs_item_next(sb, &key, &last_key, NULL, 0, lock);
		if (ret < 0 && ret != -ENOENT)
			break;

		if (ret == -ENOENT) {

			/* done if lock covers last iteration key */
			if (scoutfs_key_compare(&last_key, &lock->end) <= 0) {
				ret = 0;
				break;
			}

			/* continue iterating after locked empty region */
			key = lock->end;
			scoutfs_key_inc(&key);

			scoutfs_unlock(sb, lock, SCOUTFS_LOCK_READ);

			ret = scoutfs_forest_next_hint(sb, &key, &next_key);
			if (ret < 0 && ret != -ENOENT)
				goto out;

			if (ret == -ENOENT ||
			    scoutfs_key_compare(&next_key, &last_key) > 0) {
				ret = 0;
				goto out;
			}

			key = next_key;

			ret = scoutfs_lock_inode_index(sb, SCOUTFS_LOCK_READ,
						key.sk_type,
						le64_to_cpu(key.skii_major),
						le64_to_cpu(key.skii_ino),
						&lock);
			if (ret < 0)
				goto out;

			continue;
		}

		ent.major = le64_to_cpu(key.skii_major);
		ent.minor = 0;
		ent.ino = le64_to_cpu(key.skii_ino);

		if (copy_to_user((void __user *)walk.entries_ptr, &ent,
				 sizeof(ent))) {
			ret = -EFAULT;
			break;
		}

		nr++;
		walk.entries_ptr += sizeof(ent);

		scoutfs_key_inc(&key);
	}

	scoutfs_unlock(sb, lock, SCOUTFS_LOCK_READ);

out:
	if (nr > 0)
		ret = nr;

	return ret;
}

/*
 * See the comment above the definition of struct scoutfs_ioctl_ino_path
 * for ioctl semantics.
 */
static long scoutfs_ioc_ino_path(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_ino_path_result __user *ures;
	struct scoutfs_link_backref_entry *last_ent;
	struct scoutfs_link_backref_entry *ent;
	struct scoutfs_ioctl_ino_path args;
	LIST_HEAD(list);
	u16 copied;
	char term;
	int ret;

	if (!capable(CAP_DAC_READ_SEARCH))
		return -EPERM;

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	ures = (void __user *)(unsigned long)args.result_ptr;

	ret = scoutfs_dir_get_backref_path(sb, args.ino, args.dir_ino,
					   args.dir_pos, &list);
	if (ret < 0)
		goto out;

	last_ent = list_last_entry(&list, struct scoutfs_link_backref_entry,
				   head);
	copied = 0;
	list_for_each_entry(ent, &list, head) {

		if (offsetof(struct scoutfs_ioctl_ino_path_result,
			     path[copied + ent->name_len + 1])
				> args.result_bytes) {
			ret = -ENAMETOOLONG;
			goto out;
		}

		if (copy_to_user(&ures->path[copied],
				 ent->dent.name, ent->name_len)) {
			ret = -EFAULT;
			goto out;
		}

		copied += ent->name_len;

		if (ent == last_ent)
			term = '\0';
		else
			term = '/';

		if (put_user(term, &ures->path[copied])) {
			ret = -EFAULT;
			break;
		}

		copied++;
	}

	/* fill the result header now that we know the copied path length */
	if (put_user(last_ent->dir_ino, &ures->dir_ino) ||
	    put_user(last_ent->dir_pos, &ures->dir_pos) ||
	    put_user(copied, &ures->path_bytes)) {
		ret = -EFAULT;
	} else {
		ret = 0;
	}

out:
	scoutfs_dir_free_backref_path(sb, &list);
	return ret;
}

/*
 * The caller has a version of the data available in the given byte
 * range in an external archive.  As long as the data version still
 * matches we free the blocks fully contained in the range and mark them
 * offline.  Attempts to use the blocks in the future will trigger
 * recall from the archive.
 *
 * If the file's online blocks drop to 0 then we also truncate any
 * blocks beyond i_size.  This honors the intent of fully releasing a file
 * without the user needing to know to release past i_size or truncate.
 *
 * XXX permissions?
 * XXX a lot of this could be generic file write prep
 */
static long scoutfs_ioc_release(struct file *file, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_ioctl_release args;
	struct scoutfs_lock *lock = NULL;
	u64 sblock;
	u64 eblock;
	u64 online;
	u64 offline;
	u64 isize;
	int ret;

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	trace_scoutfs_ioc_release(sb, scoutfs_ino(inode), &args);

	if (args.length == 0)
		return 0;
	if (((args.offset + args.length) < args.offset) ||
	    (args.offset & SCOUTFS_BLOCK_SM_MASK) ||
	    (args.length & SCOUTFS_BLOCK_SM_MASK))
		return -EINVAL;


	ret = mnt_want_write_file(file);
	if (ret)
		return ret;

	mutex_lock(&inode->i_mutex);

	ret = scoutfs_lock_inode(sb, SCOUTFS_LOCK_WRITE,
				 SCOUTFS_LKF_REFRESH_INODE, inode, &lock);
	if (ret)
		goto out;

	if (!S_ISREG(inode->i_mode)) {
		ret = -EINVAL;
		goto out;
	}

	if (!(file->f_mode & FMODE_WRITE)) {
		ret = -EINVAL;
		goto out;
	}

	if (scoutfs_inode_data_version(inode) != args.data_version) {
		ret = -ESTALE;
		goto out;
	}

	inode_dio_wait(inode);

	/* drop all clean and dirty cached blocks in the range */
	truncate_inode_pages_range(&inode->i_data, args.offset,
				   args.offset + args.length - 1);

	sblock = args.offset >> SCOUTFS_BLOCK_SM_SHIFT;
	eblock = (args.offset + args.length - 1) >> SCOUTFS_BLOCK_SM_SHIFT;
	ret = scoutfs_data_truncate_items(sb, inode, scoutfs_ino(inode),
					  sblock,
					  eblock, true,
					  lock);
	if (ret == 0) {
		scoutfs_inode_get_onoff(inode, &online, &offline);
		isize = i_size_read(inode);
		if (online == 0 && isize) {
			sblock = (isize + SCOUTFS_BLOCK_SM_SIZE - 1)
					>> SCOUTFS_BLOCK_SM_SHIFT;
			ret = scoutfs_data_truncate_items(sb, inode,
							  scoutfs_ino(inode),
							  sblock, U64_MAX,
							  false, lock);
		}
	}

out:
	scoutfs_unlock(sb, lock, SCOUTFS_LOCK_WRITE);
	mutex_unlock(&inode->i_mutex);
	mnt_drop_write_file(file);

	trace_scoutfs_ioc_release_ret(sb, scoutfs_ino(inode), ret);
	return ret;
}

static long scoutfs_ioc_data_wait_err(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_data_wait_err args;
	struct scoutfs_lock *lock = NULL;
	struct inode *inode = NULL;
	u64 sblock;
	u64 eblock;
	long ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;
	if (args.count == 0)
		return 0;
	if ((args.op & SCOUTFS_IOC_DWO_UNKNOWN) || !IS_ERR_VALUE(args.err))
		return -EINVAL;
	if ((args.op & SCOUTFS_IOC_DWO_UNKNOWN) || !IS_ERR_VALUE(args.err))
		return -EINVAL;

	trace_scoutfs_ioc_data_wait_err(sb, &args);

	sblock = args.offset >> SCOUTFS_BLOCK_SM_SHIFT;
	eblock = (args.offset + args.count - 1) >> SCOUTFS_BLOCK_SM_SHIFT;

	if (sblock > eblock)
		return -EINVAL;

	inode = scoutfs_ilookup(sb, args.ino);
	if (!inode) {
		ret = -ESTALE;
		goto out;
	}

	mutex_lock(&inode->i_mutex);

	ret = scoutfs_lock_inode(sb, SCOUTFS_LOCK_READ,
				 SCOUTFS_LKF_REFRESH_INODE, inode, &lock);
	if (ret)
		goto unlock;

	if (!S_ISREG(inode->i_mode)) {
		ret = -EINVAL;
	} else if (scoutfs_inode_data_version(inode) != args.data_version) {
		ret = -ESTALE;
	} else {
		ret = scoutfs_data_wait_err(inode, sblock, eblock, args.op,
					    args.err);
	}

	scoutfs_unlock(sb, lock, SCOUTFS_LOCK_READ);
unlock:
	mutex_unlock(&inode->i_mutex);
	iput(inode);
out:
	return ret;
}

/*
 * Write the archived contents of the file back if the data_version
 * still matches.
 *
 * This is a data plane operation only.  We don't want the write to
 * change any fields in the inode.  It only changes the file contents.
 *
 * Keep in mind that the staging writes can easily span transactions and
 * can crash partway through.  If we called the normal write path and
 * restored the inode afterwards the modified inode could be commited
 * partway through by a transaction and then left that way by a crash
 * before the write finishes and we restore the fields.  It also
 * wouldn't be great if the temporarily updated inode was visible to
 * paths that don't serialize with write.
 *
 * We're implementing the buffered write path down to the start of
 * generic_file_buffered_writes() without all the stuff that would
 * change the inode: file_remove_suid(), file_update_time().  The
 * easiest way to do that is to call generic_file_buffered_write().
 * We're careful to only allow staging writes inside i_size.
 *
 * We set a  bool on the inode which tells our code to update the
 * offline extents and to not update the data_version counter.
 *
 * This doesn't support any fancy write modes or side-effects: aio,
 * direct, append, sync, breaking suid, sending rlimit signals.
 */
static long scoutfs_ioc_stage(struct file *file, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct address_space *mapping = inode->i_mapping;
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	SCOUTFS_DECLARE_PER_TASK_ENTRY(pt_ent);
	struct scoutfs_ioctl_stage args;
	struct scoutfs_lock *lock = NULL;
	struct kiocb kiocb;
	struct iovec iov;
	size_t written;
	loff_t end_size;
	loff_t isize;
	loff_t pos;
	int ret;

	if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
		return -EFAULT;

	trace_scoutfs_ioc_stage(sb, scoutfs_ino(inode), &args);

	end_size = args.offset + args.length;

	/* verify arg constraints that aren't dependent on file */
	if (args.length < 0 || (end_size < args.offset) ||
	    args.offset & SCOUTFS_BLOCK_SM_MASK) {
		return -EINVAL;
	}

	if (args.length == 0)
		return 0;

	/* the iocb is really only used for the file pointer :P */
	init_sync_kiocb(&kiocb, file);
	kiocb.ki_pos = args.offset;
	kiocb.ki_left = args.length;
	kiocb.ki_nbytes = args.length;
	iov.iov_base = (void __user *)(unsigned long)args.buf_ptr;
	iov.iov_len = args.length;

	ret = mnt_want_write_file(file);
	if (ret)
		return ret;

	mutex_lock(&inode->i_mutex);

	ret = scoutfs_lock_inode(sb, SCOUTFS_LOCK_WRITE,
				 SCOUTFS_LKF_REFRESH_INODE, inode, &lock);
	if (ret)
		goto out;

	scoutfs_per_task_add(&si->pt_data_lock, &pt_ent, lock);

	isize = i_size_read(inode);

	if (!S_ISREG(inode->i_mode) ||
	    !(file->f_mode & FMODE_WRITE) ||
	    (file->f_flags & (O_APPEND | O_DIRECT | O_DSYNC)) ||
	    IS_SYNC(file->f_mapping->host) ||
	    (end_size > isize) ||
	    ((end_size & SCOUTFS_BLOCK_SM_MASK) && (end_size != isize))) {
		ret = -EINVAL;
		goto out;
	}

	if (scoutfs_inode_data_version(inode) != args.data_version) {
		ret = -ESTALE;
		goto out;
	}

	si->staging = true;
	current->backing_dev_info = mapping->backing_dev_info;

	pos = args.offset;
	written = 0;
	do {
		ret = generic_file_buffered_write(&kiocb, &iov, 1, pos, &pos,
						  args.length, written);
		BUG_ON(ret == -EIOCBQUEUED);
		if (ret > 0)
			written += ret;
	} while (ret > 0 && written < args.length);

	si->staging = false;
	current->backing_dev_info = NULL;
out:
	scoutfs_per_task_del(&si->pt_data_lock, &pt_ent);
	scoutfs_unlock(sb, lock, SCOUTFS_LOCK_WRITE);
	mutex_unlock(&inode->i_mutex);
	mnt_drop_write_file(file);

	trace_scoutfs_ioc_stage_ret(sb, scoutfs_ino(inode), ret);
	return ret;
}

static long scoutfs_ioc_stat_more(struct file *file, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct scoutfs_ioctl_stat_more stm;

	if (get_user(stm.valid_bytes, (__u64 __user *)arg))
		return -EFAULT;

	stm.valid_bytes = min_t(u64, stm.valid_bytes,
				sizeof(struct scoutfs_ioctl_stat_more));
	stm.meta_seq = scoutfs_inode_meta_seq(inode);
	stm.data_seq = scoutfs_inode_data_seq(inode);
	stm.data_version = scoutfs_inode_data_version(inode);
	scoutfs_inode_get_onoff(inode, &stm.online_blocks, &stm.offline_blocks);
	stm.crtime_sec = si->crtime.tv_sec;
	stm.crtime_nsec = si->crtime.tv_nsec;

	if (copy_to_user((void __user *)arg, &stm, stm.valid_bytes))
		return -EFAULT;

	return 0;
}

static bool inc_wrapped(u64 *ino, u64 *iblock)
{
	return (++(*iblock) == 0) && (++(*ino) == 0);
}

static long scoutfs_ioc_data_waiting(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_data_waiting idw;
	struct scoutfs_ioctl_data_waiting_entry __user *udwe;
	struct scoutfs_ioctl_data_waiting_entry dwe[16];
	unsigned int nr;
	int total;
	int ret;

	if (copy_from_user(&idw, (void __user *)arg, sizeof(idw)))
		return -EFAULT;

	if (idw.flags & SCOUTFS_IOC_DATA_WAITING_FLAGS_UNKNOWN)
		return -EINVAL;

	udwe = (void __user *)(long)idw.ents_ptr;
	total = 0;
	ret = 0;
	while (idw.ents_nr && !inc_wrapped(&idw.after_ino, &idw.after_iblock)) {
		nr = min_t(size_t, idw.ents_nr, ARRAY_SIZE(dwe));

		ret = scoutfs_data_waiting(sb, idw.after_ino, idw.after_iblock,
					   dwe, nr);
		BUG_ON(ret > nr); /* stack overflow \o/ */
		if (ret <= 0)
			break;

		if (copy_to_user(udwe, dwe, ret * sizeof(dwe[0]))) {
			ret = -EFAULT;
			break;
		}

		idw.after_ino = dwe[ret - 1].ino;
		idw.after_iblock = dwe[ret - 1].iblock;

		udwe += ret;
		idw.ents_nr -= ret;
		total += ret;
		ret = 0;
	}

	return ret ?: total;
}

/*
 * This is used when restoring files, it lets the caller set all the
 * inode attributes which are otherwise unreachable.  Changing the file
 * size can only be done for regular files with a data_version of 0.
 */
static long scoutfs_ioc_setattr_more(struct file *file, unsigned long arg)
{
	struct inode *inode = file->f_inode;
	struct scoutfs_inode_info *si = SCOUTFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct scoutfs_ioctl_setattr_more __user *usm = (void __user *)arg;
	struct scoutfs_ioctl_setattr_more sm;
	struct scoutfs_lock *lock = NULL;
	LIST_HEAD(ind_locks);
	bool set_data_seq;
	int ret;

	if (!capable(CAP_SYS_ADMIN)) {
		ret = -EPERM;
		goto out;
	}

	if (!(file->f_mode & FMODE_WRITE)) {
		ret = -EBADF;
		goto out;
	}

	if (copy_from_user(&sm, usm, sizeof(sm))) {
		ret = -EFAULT;
		goto out;
	}

	if ((sm.i_size > 0 && sm.data_version == 0) ||
	    ((sm.flags & SCOUTFS_IOC_SETATTR_MORE_OFFLINE) && !sm.i_size) ||
	    (sm.flags & SCOUTFS_IOC_SETATTR_MORE_UNKNOWN)) {
		ret = -EINVAL;
		goto out;
	}

	ret = mnt_want_write_file(file);
	if (ret)
		goto out;

	mutex_lock(&inode->i_mutex);

	ret = scoutfs_lock_inode(sb, SCOUTFS_LOCK_WRITE,
				 SCOUTFS_LKF_REFRESH_INODE, inode, &lock);
	if (ret)
		goto unlock;

	/* can only change size/dv on untouched regular files */
	if ((sm.i_size != 0 || sm.data_version != 0) &&
	    ((!S_ISREG(inode->i_mode) ||
	      scoutfs_inode_data_version(inode) != 0))) {
		ret = -EINVAL;
		goto unlock;
	}

	/* create offline extents in potentially many transactions */
	if (sm.flags & SCOUTFS_IOC_SETATTR_MORE_OFFLINE) {
		ret = scoutfs_data_init_offline_extent(inode, sm.i_size, lock);
		if (ret)
			goto unlock;
	}

	/* setting only so we don't see 0 data seq with nonzero data_version */
	set_data_seq = sm.data_version != 0 ? true : false;
	ret = scoutfs_inode_index_lock_hold(inode, &ind_locks, set_data_seq, false);
	if (ret)
		goto unlock;

	if (sm.data_version)
		scoutfs_inode_set_data_version(inode, sm.data_version);
	if (sm.i_size)
		i_size_write(inode, sm.i_size);
	inode->i_ctime.tv_sec = sm.ctime_sec;
	inode->i_ctime.tv_nsec = sm.ctime_nsec;
	si->crtime.tv_sec = sm.crtime_sec;
	si->crtime.tv_nsec = sm.crtime_nsec;

	scoutfs_update_inode_item(inode, lock, &ind_locks);
	ret = 0;

	scoutfs_release_trans(sb);
unlock:
	scoutfs_inode_index_unlock(sb, &ind_locks);
	scoutfs_unlock(sb, lock, SCOUTFS_LOCK_WRITE);
	mutex_unlock(&inode->i_mutex);
	mnt_drop_write_file(file);
out:

	return ret;
}

/*
 * This lists .hide. attributes on the inode.  It doesn't include normal
 * xattrs that are visible to listxattr because we don't perform as
 * rigorous security access checks as normal vfs listxattr does.
 */
static long scoutfs_ioc_listxattr_hidden(struct file *file, unsigned long arg)
{
	struct inode *inode = file->f_inode;
	struct scoutfs_ioctl_listxattr_hidden __user *ulxr = (void __user *)arg;
	struct scoutfs_ioctl_listxattr_hidden lxh;
	struct page *page = NULL;
	unsigned int bytes;
	int total = 0;
	int ret;

	ret = inode_permission(inode, MAY_READ);
	if (ret < 0)
		goto out;

	if (copy_from_user(&lxh, ulxr, sizeof(lxh))) {
		ret = -EFAULT;
		goto out;
	}

	page = alloc_page(GFP_KERNEL);
	if (!page) {
		ret = -ENOMEM;
		goto out;
	}

	while (lxh.buf_bytes) {
		bytes = min_t(int, lxh.buf_bytes, PAGE_SIZE);
		ret = scoutfs_list_xattrs(inode, page_address(page), bytes,
					  &lxh.hash_pos, &lxh.id_pos,
					  false, true);
		if (ret <= 0)
			break;

		if (copy_to_user((void __user *)lxh.buf_ptr,
				 page_address(page), ret)) {
			ret = -EFAULT;
			break;
		}

		lxh.buf_ptr += ret;
		lxh.buf_bytes -= ret;
		total += ret;
		ret = 0;
	}

out:
	if (page)
		__free_page(page);

	if (ret == 0 && (__put_user(lxh.hash_pos, &ulxr->hash_pos) ||
			 __put_user(lxh.id_pos, &ulxr->id_pos)))
		ret = -EFAULT;

	return ret ?: total;
}

/*
 * Return the inode numbers of inodes which might contain the given
 * named xattr.  This will only find scoutfs xattrs with the index tag
 * but we don't check that the callers xattr name contains the tag and
 * search for it regardless.
 */
static long scoutfs_ioc_search_xattrs(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_search_xattrs __user *usx = (void __user *)arg;
	struct scoutfs_ioctl_search_xattrs sx;
	struct scoutfs_xattr_prefix_tags tgs;
	struct scoutfs_srch_rb_root sroot;
	struct scoutfs_srch_rb_node *snode;
	u64 __user *uinos;
	struct rb_node *node;
	char *name = NULL;
	bool done = false;
	u64 prev_ino = 0;
	u64 total = 0;
	int ret;

	if (!(file->f_mode & FMODE_READ)) {
		ret = -EBADF;
		goto out;
	}

	if (!capable(CAP_SYS_ADMIN)) {
		ret = -EPERM;
		goto out;
	}

	if (copy_from_user(&sx, usx, sizeof(sx))) {
		ret = -EFAULT;
		goto out;
	}
	uinos = (u64 __user *)sx.inodes_ptr;

	if (sx.name_bytes > SCOUTFS_XATTR_MAX_NAME_LEN) {
		ret = -EINVAL;
		goto out;
	}

	if (sx.nr_inodes == 0 || sx.last_ino < sx.next_ino) {
		ret = 0;
		goto out;
	}

	name = kmalloc(sx.name_bytes, GFP_KERNEL);
	if (!name) {
		ret = -ENOMEM;
		goto out;
	}

	if (copy_from_user(name, (void __user *)sx.name_ptr, sx.name_bytes)) {
		ret = -EFAULT;
		goto out;
	}

	if (scoutfs_xattr_parse_tags(name, sx.name_bytes, &tgs) < 0 ||
	    !tgs.srch) {
		ret = -EINVAL;
		goto out;
	}

	ret = scoutfs_srch_search_xattrs(sb, &sroot,
					 scoutfs_hash64(name, sx.name_bytes),
					 sx.next_ino, sx.last_ino, &done);
	if (ret < 0)
		goto out;

	prev_ino = 0;
	scoutfs_srch_foreach_rb_node(snode, node, &sroot) {
		if (prev_ino == snode->ino)
			continue;

		if (put_user(snode->ino, uinos + total)) {
			ret = -EFAULT;
			break;
		}
		prev_ino = snode->ino;

		if (++total == sx.nr_inodes)
			break;
	}

	sx.output_flags = 0;
	if (done && total == sroot.nr)
		sx.output_flags |= SCOUTFS_SEARCH_XATTRS_OFLAG_END;

	if (put_user(sx.output_flags, &usx->output_flags))
		ret = -EFAULT;
	else
		ret = 0;

	scoutfs_srch_destroy_rb_root(&sroot);

out:
	kfree(name);
	return ret ?: total;
}

static long scoutfs_ioc_statfs_more(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_sb_info *sbi = SCOUTFS_SB(sb);
	struct scoutfs_super_block *super;
	struct scoutfs_ioctl_statfs_more sfm;
	int ret;

	if (get_user(sfm.valid_bytes, (__u64 __user *)arg))
		return -EFAULT;

	super = kzalloc(sizeof(struct scoutfs_super_block), GFP_NOFS);
	if (!super)
		return -ENOMEM;

	ret = scoutfs_read_super(sb, super);
	if (ret)
		goto out;

	sfm.valid_bytes = min_t(u64, sfm.valid_bytes,
				sizeof(struct scoutfs_ioctl_statfs_more));
	sfm.fsid = le64_to_cpu(super->hdr.fsid);
	sfm.rid = sbi->rid;
	sfm.total_meta_blocks = le64_to_cpu(super->total_meta_blocks);
	sfm.total_data_blocks = le64_to_cpu(super->total_data_blocks);
	sfm.reserved_meta_blocks = scoutfs_server_reserved_meta_blocks(sb);

	ret = scoutfs_client_get_last_seq(sb, &sfm.committed_seq);
	if (ret)
		goto out;

	if (copy_to_user((void __user *)arg, &sfm, sfm.valid_bytes))
		ret = -EFAULT;
	else
		ret = 0;
out:
	kfree(super);
	return ret;
}

struct copy_alloc_detail_args {
	struct scoutfs_ioctl_alloc_detail_entry __user *uade;
	u64 nr;
	u64 copied;
};

static int copy_alloc_detail_to_user(struct super_block *sb, void *arg,
				     int owner, u64 id, bool meta, bool avail,
				     u64 blocks)
{
	struct copy_alloc_detail_args *args = arg;
	struct scoutfs_ioctl_alloc_detail_entry ade;

	if (args->copied == args->nr)
		return -EOVERFLOW;

	ade.blocks = blocks;
	ade.id = id;
	ade.meta = !!meta;
	ade.avail = !!avail;

	if (copy_to_user(&args->uade[args->copied], &ade, sizeof(ade)))
		return -EFAULT;

	args->copied++;
	return 0;
}

static long scoutfs_ioc_alloc_detail(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_alloc_detail __user *uad = (void __user *)arg;
	struct scoutfs_ioctl_alloc_detail ad;
	struct copy_alloc_detail_args args;

	if (copy_from_user(&ad, uad, sizeof(ad)))
		return -EFAULT;

	args.uade = (struct scoutfs_ioctl_alloc_detail_entry __user *)
			(uintptr_t)ad.entries_ptr;
	args.nr = ad.entries_nr;
	args.copied = 0;

	return scoutfs_alloc_foreach(sb, copy_alloc_detail_to_user, &args) ?:
	       args.copied;
}

static long scoutfs_ioc_move_blocks(struct file *file, unsigned long arg)
{
	struct inode *to = file_inode(file);
	struct super_block *sb = to->i_sb;
	struct scoutfs_ioctl_move_blocks __user *umb = (void __user *)arg;
	struct scoutfs_ioctl_move_blocks mb;
	struct file *from_file;
	struct inode *from;
	int ret;

	if (copy_from_user(&mb, umb, sizeof(mb)))
		return -EFAULT;

	if (mb.len == 0)
		return 0;

	if (mb.from_off + mb.len < mb.from_off ||
	    mb.to_off + mb.len < mb.to_off)
		return -EOVERFLOW;

	from_file = fget(mb.from_fd);
	if (!from_file)
		return -EBADF;
	from = file_inode(from_file);

	if (from == to) {
		ret = -EINVAL;
		goto out;
	}

	if (from->i_sb != sb) {
		ret = -EXDEV;
		goto out;
	}

	if (mb.flags & SCOUTFS_IOC_MB_UNKNOWN) {
		ret = -EINVAL;
		goto out;
	}

	ret = mnt_want_write_file(file);
	if (ret < 0)
		goto out;

	ret = scoutfs_data_move_blocks(from, mb.from_off, mb.len,
				       to, mb.to_off, !!(mb.flags & SCOUTFS_IOC_MB_STAGE),
				       mb.data_version);
	mnt_drop_write_file(file);
out:
	fput(from_file);

	return ret;
}

static long scoutfs_ioc_resize_devices(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_resize_devices __user *urd = (void __user *)arg;
	struct scoutfs_ioctl_resize_devices rd;
	struct scoutfs_net_resize_devices nrd;
	int ret;

	if (!(file->f_mode & FMODE_READ)) {
		ret = -EBADF;
		goto out;
	}

	if (!capable(CAP_SYS_ADMIN)) {
		ret = -EPERM;
		goto out;
	}

	if (copy_from_user(&rd, urd, sizeof(rd))) {
		ret = -EFAULT;
		goto out;
	}

	nrd.new_total_meta_blocks = cpu_to_le64(rd.new_total_meta_blocks);
	nrd.new_total_data_blocks = cpu_to_le64(rd.new_total_data_blocks);

	ret = scoutfs_client_resize_devices(sb, &nrd);
out:
	return ret;
}

struct xattr_total_entry {
	struct rb_node node;
	struct scoutfs_ioctl_xattr_total xt;
	u64 fs_seq;
	u64 fs_total;
	u64 fs_count;
	u64 fin_seq;
	u64 fin_total;
	s64 fin_count;
	u64 log_seq;
	u64 log_total;
	s64 log_count;
};

static int cmp_xt_entry_name(const struct xattr_total_entry *a,
			     const struct xattr_total_entry *b)

{
	return scoutfs_cmp_u64s(a->xt.name[0], b->xt.name[0]) ?:
	       scoutfs_cmp_u64s(a->xt.name[1], b->xt.name[1]) ?:
	       scoutfs_cmp_u64s(a->xt.name[2], b->xt.name[2]);
}

/*
 * Record the contribution of the three classes of logged items we can
 * see: the item in the fs_root, items from finalized log btrees, and
 * items from active log btrees.  Once we have the full set the caller
 * can decide which of the items contribute to the total it sends to the
 * user.
 */
static int read_xattr_total_item(struct super_block *sb, struct scoutfs_key *key,
				 u64 seq, u8 flags, void *val, int val_len, int fic, void *arg)
{
	struct scoutfs_xattr_totl_val *tval = val;
	struct xattr_total_entry *ent;
	struct xattr_total_entry rd;
	struct rb_root *root = arg;
	struct rb_node *parent;
	struct rb_node **node;
	int cmp;

	rd.xt.name[0] = le64_to_cpu(key->skxt_a);
	rd.xt.name[1] = le64_to_cpu(key->skxt_b);
	rd.xt.name[2] = le64_to_cpu(key->skxt_c);

	/* find entry matching name */
	node = &root->rb_node;
	parent = NULL;
	cmp = -1;
	while (*node) {
		parent = *node;
		ent = container_of(*node, struct xattr_total_entry, node);

		/* sort merge items by key then newest to oldest */
		cmp = cmp_xt_entry_name(&rd, ent);
		if (cmp < 0)
			node = &(*node)->rb_left;
		else if (cmp > 0)
			node = &(*node)->rb_right;
		else
			break;
	}

	/* allocate and insert new node if we need to */
	if (cmp != 0) {
		ent = kzalloc(sizeof(*ent), GFP_KERNEL);
		if (!ent)
			return -ENOMEM;

		memcpy(&ent->xt.name, &rd.xt.name, sizeof(ent->xt.name));

		rb_link_node(&ent->node, parent, node);
		rb_insert_color(&ent->node, root);
	}

	if (fic & FIC_FS_ROOT) {
		ent->fs_seq = seq;
		ent->fs_total = le64_to_cpu(tval->total);
		ent->fs_count = le64_to_cpu(tval->count);
	} else if (fic & FIC_FINALIZED) {
		ent->fin_seq = seq;
		ent->fin_total += le64_to_cpu(tval->total);
		ent->fin_count += le64_to_cpu(tval->count);
	} else {
		ent->log_seq = seq;
		ent->log_total += le64_to_cpu(tval->total);
		ent->log_count += le64_to_cpu(tval->count);
	}

	scoutfs_inc_counter(sb, totl_read_item);

	return 0;
}

/* these are always _safe, node stores next */
#define for_each_xt_ent(ent, node, root)					\
	for (node = rb_first(root);						\
	     node && (ent = rb_entry(node, struct xattr_total_entry, node),	\
		      node = rb_next(node), 1); )

#define for_each_xt_ent_reverse(ent, node, root)				\
	for (node = rb_last(root);						\
	     node && (ent = rb_entry(node, struct xattr_total_entry, node),	\
		      node = rb_prev(node), 1); )

static void free_xt_ent(struct rb_root *root, struct xattr_total_entry *ent)
{
	rb_erase(&ent->node, root);
	kfree(ent);
}

static void free_all_xt_ents(struct rb_root *root)
{
	struct xattr_total_entry *ent;
	struct rb_node *node;

	for_each_xt_ent(ent, node, root)
		free_xt_ent(root, ent);
}

/*
 * Starting from the caller's pos_name, copy the names, totals, and
 * counts for the .totl. tagged xattrs in the system sorted by their
 * name until the user's buffer is full.  This only sees xattrs that
 * have been committed.  It doesn't use locking to force commits and
 * block writers so it can be a little bit out of date with respect to
 * dirty xattrs in memory across the system.
 *
 * Our reader has to be careful because the log btree merging code can
 * write partial results to the fs_root.  This means that a reader can
 * see both cases where new finalized logs should be applied to the old
 * fs items and where old finalized logs have already been applied to
 * the partially merged fs items.  Currently active logged items are
 * always applied on top of all cases.
 *
 * These cases are differentiated with a combination of sequence numbers
 * in items, the count of contributing xattrs, and a flag
 * differentiating finalized and active logged items.  This lets us
 * recognize all cases, including when finalized logs were merged and
 * deleted the fs item.
 *
 * We're allocating a tracking struct for each totl name we see while
 * traversing the item btrees.  The forest reader is providing the items
 * it finds in leaf blocks that contain the search key.  In the worst
 * case all of these blocks are full and none of the items overlap.  At
 * most, figure order a thousand names per mount.  But in practice many
 * of these factors fall away: leaf blocks aren't fill, leaf items
 * overlap, there aren't finalized log btrees, and not all mounts are
 * actively changing totals.   We're much more likely to only read a
 * leaf block's worth of totals that have been long since merged into
 * the fs_root.
 */
static long scoutfs_ioc_read_xattr_totals(struct file *file, unsigned long arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct scoutfs_ioctl_read_xattr_totals __user *urxt = (void __user *)arg;
	struct scoutfs_ioctl_read_xattr_totals rxt;
	struct scoutfs_ioctl_xattr_total __user *uxt;
	struct xattr_total_entry *ent;
	struct scoutfs_key key;
	struct scoutfs_key bloom_key;
	struct scoutfs_key start;
	struct scoutfs_key end;
	struct rb_root root = RB_ROOT;
	struct rb_node *node;
	int count = 0;
	int ret;

	if (!(file->f_mode & FMODE_READ)) {
		ret = -EBADF;
		goto out;
	}

	if (!capable(CAP_SYS_ADMIN)) {
		ret = -EPERM;
		goto out;
	}

	if (copy_from_user(&rxt, urxt, sizeof(rxt))) {
		ret = -EFAULT;
		goto out;
	}
	uxt = (void __user *)rxt.totals_ptr;

	if ((rxt.totals_ptr & (sizeof(__u64) - 1)) ||
	    (rxt.totals_bytes < sizeof(struct scoutfs_ioctl_xattr_total))) {
		ret = -EINVAL;
		goto out;
	}

	scoutfs_key_set_zeros(&bloom_key);
	bloom_key.sk_zone = SCOUTFS_XATTR_TOTL_ZONE;
	scoutfs_xattr_init_totl_key(&start, rxt.pos_name);

	while (rxt.totals_bytes >= sizeof(struct scoutfs_ioctl_xattr_total)) {

		scoutfs_key_set_ones(&end);
		end.sk_zone = SCOUTFS_XATTR_TOTL_ZONE;
		if (scoutfs_key_compare(&start, &end) > 0)
			break;

		key = start;
		ret = scoutfs_forest_read_items(sb, &key, &bloom_key, &start, &end,
						read_xattr_total_item, &root);
		if (ret < 0) {
			if (ret == -ESTALE) {
				free_all_xt_ents(&root);
				continue;
			}
			goto out;
		}

		if (RB_EMPTY_ROOT(&root))
			break;

		/* trim totals that fall outside of the consistent range */
		for_each_xt_ent(ent, node, &root) {
			scoutfs_xattr_init_totl_key(&key, ent->xt.name);
			if (scoutfs_key_compare(&key, &start) < 0) {
				free_xt_ent(&root, ent);
			} else {
				break;
			}
		}
		for_each_xt_ent_reverse(ent, node, &root) {
			scoutfs_xattr_init_totl_key(&key, ent->xt.name);
			if (scoutfs_key_compare(&key, &end) > 0) {
				free_xt_ent(&root, ent);
			} else {
				break;
			}
		}

		/* copy resulting unique non-zero totals to userspace */
		for_each_xt_ent(ent, node, &root) {
			if (rxt.totals_bytes < sizeof(ent->xt))
				break;

			/* start with the fs item if we have it */
			if (ent->fs_seq != 0) {
				ent->xt.total = ent->fs_total;
				ent->xt.count = ent->fs_count;
				scoutfs_inc_counter(sb, totl_read_fs);
			}

			/* apply finalized logs if they're newer or creating */
			if (((ent->fs_seq != 0) && (ent->fin_seq > ent->fs_seq)) ||
			    ((ent->fs_seq == 0) && (ent->fin_count > 0))) {
				ent->xt.total += ent->fin_total;
				ent->xt.count += ent->fin_count;
				scoutfs_inc_counter(sb, totl_read_finalized);
			}

			/* always apply active logs which must be newer than fs and finalized */
			if (ent->log_seq > 0) {
				ent->xt.total += ent->log_total;
				ent->xt.count += ent->log_count;
				scoutfs_inc_counter(sb, totl_read_logged);
			}

			if (ent->xt.total != 0 || ent->xt.count != 0) {
				if (copy_to_user(uxt, &ent->xt, sizeof(ent->xt))) {
					ret = -EFAULT;
					goto out;
				}

				uxt++;
				rxt.totals_bytes -= sizeof(ent->xt);
				count++;
				scoutfs_inc_counter(sb, totl_read_copied);
			}

			free_xt_ent(&root, ent);
		}

		/* continue after the last possible key read */
		start = end;
		scoutfs_key_inc(&start);
	}

	ret = 0;
out:
	free_all_xt_ents(&root);

	return ret ?: count;
}

long scoutfs_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case SCOUTFS_IOC_WALK_INODES:
		return scoutfs_ioc_walk_inodes(file, arg);
	case SCOUTFS_IOC_INO_PATH:
		return scoutfs_ioc_ino_path(file, arg);
	case SCOUTFS_IOC_RELEASE:
		return scoutfs_ioc_release(file, arg);
	case SCOUTFS_IOC_STAGE:
		return scoutfs_ioc_stage(file, arg);
	case SCOUTFS_IOC_STAT_MORE:
		return scoutfs_ioc_stat_more(file, arg);
	case SCOUTFS_IOC_DATA_WAITING:
		return scoutfs_ioc_data_waiting(file, arg);
	case SCOUTFS_IOC_SETATTR_MORE:
		return scoutfs_ioc_setattr_more(file, arg);
	case SCOUTFS_IOC_LISTXATTR_HIDDEN:
		return scoutfs_ioc_listxattr_hidden(file, arg);
	case SCOUTFS_IOC_SEARCH_XATTRS:
		return scoutfs_ioc_search_xattrs(file, arg);
	case SCOUTFS_IOC_STATFS_MORE:
		return scoutfs_ioc_statfs_more(file, arg);
	case SCOUTFS_IOC_DATA_WAIT_ERR:
		return scoutfs_ioc_data_wait_err(file, arg);
	case SCOUTFS_IOC_ALLOC_DETAIL:
		return scoutfs_ioc_alloc_detail(file, arg);
	case SCOUTFS_IOC_MOVE_BLOCKS:
		return scoutfs_ioc_move_blocks(file, arg);
	case SCOUTFS_IOC_RESIZE_DEVICES:
		return scoutfs_ioc_resize_devices(file, arg);
	case SCOUTFS_IOC_READ_XATTR_TOTALS:
		return scoutfs_ioc_read_xattr_totals(file, arg);
	}

	return -ENOTTY;
}
