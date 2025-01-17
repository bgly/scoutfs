#ifndef _SCOUTFS_IOCTL_H_
#define _SCOUTFS_IOCTL_H_

/*
 * We naturally align explicit width fields in the ioctl structs so that
 * userspace doesn't need to deal with padding or unaligned packing and
 * we don't have to deal with 32/64 compat.  It makes it a little
 * awkward to communicate persistent packed structs through the ioctls
 * but that happens very rarely.  An interesting special case are
 * 0length arrays that follow the structs.  We make those start at the
 * next aligned offset of the struct to be safe.
 *
 * This is enforced by pahole scripting in external build environments.
 */

/* XXX I have no idea how these are chosen. */
#define SCOUTFS_IOCTL_MAGIC 's'

/*
 * Packed scoutfs keys rarely cross the ioctl boundary so we have a
 * translation struct.
 */
struct scoutfs_ioctl_key {
	__le64	_sk_first;
	__le64	_sk_second;
	__le64	_sk_third;
	__u8	_sk_fourth;
	__u8	sk_type;
	__u8	sk_zone;
	__u8	_pad[5];
};

struct scoutfs_ioctl_walk_inodes_entry {
	__u64 major;
	__u64 ino;
	__u32 minor;
	__u8  _pad[4];
};

/*
 * Walk inodes in an index that is sorted by one of their fields.
 *
 * Each index is built from generic index items that have major and
 * minor values that are set to the field being indexed.  In time
 * indices, for example, major is seconds and minor is nanoseconds.
 *
 * @first       The first index entry that can be returned.
 * @last        The last index entry that can be returned.
 * @entries_ptr Pointer to emory containing buffer for entry results.
 * @nr_entries  The number of entries that can fit in the buffer.
 * @index       Which index to walk, enumerated in _WALK_INODES_ constants.
 *
 * To start iterating first can be memset to 0 and last to 0xff.  Then
 * after each set of results first can be set to the last entry returned
 * and then the fields can be incremented in reverse sort order (ino <
 * minor < major) as each increasingly significant value wraps around to
 * 0.
 *
 * These indexes are not strictly consistent.  The items that back these
 * index entries aren't updated with cluster locks so they're not
 * guaranteed to be visible the moment you read after writing.  They're
 * only visible when the transaction that updated them is synced.
 *
 * In addition, the seq indexes will only allow walking through sequence
 * space that has been consistent.  This prevents old dirty entries from
 * becoming visible after newer stable entries are displayed.
 *
 * If first is greater than last then the walk will return 0 entries.
 *
 * XXX invalidate before reading.
 */
struct scoutfs_ioctl_walk_inodes {
	struct scoutfs_ioctl_walk_inodes_entry first;
	struct scoutfs_ioctl_walk_inodes_entry last;
	__u64 entries_ptr;
	__u32 nr_entries;
	__u8 index;
	__u8 _pad[11]; /* padded to align walk_inodes_entry total size */
};

enum scoutfs_ino_walk_seq_type {
	SCOUTFS_IOC_WALK_INODES_META_SEQ = 0,
	SCOUTFS_IOC_WALK_INODES_DATA_SEQ,
	SCOUTFS_IOC_WALK_INODES_UNKNOWN,
};

/*
 * Adds entries to the user's buffer for each inode that is found in the
 * given index between the first and last positions.
 */
#define SCOUTFS_IOC_WALK_INODES _IOR(SCOUTFS_IOCTL_MAGIC, 1, \
				     struct scoutfs_ioctl_walk_inodes)

/*
 * Fill the result buffer with the next absolute path to the target
 * inode searching from a given position in a parent directory.
 *
 * @ino: The target ino that we're finding paths to.  Constant across
 * all the calls that make up an iteration over all the inode's paths.
 *
 * @dir_ino: The inode number of the directory containing the entry to
 * our inode to search from.  If this parent directory contains no more
 * entries to our inode then we'll search through other parent directory
 * inodes in inode order.
 *
 * @dir_pos: The position in the dir_ino parent directory of the entry
 * to our inode to search from.  If there is no entry at this position
 * then we'll search through other entry positions in increasing order.
 * If we exhaust the parent directory then we'll search through
 * additional parent directories in inode order.
 *
 * @result_ptr: A pointer to the buffer where the result struct and
 * absolute path will be stored.
 *
 * @result_bytes: The size of the buffer that will contain the result
 * struct and the null terminated absolute path name.
 *
 * To start iterating set the desired target inode, dir_ino to 0,
 * dir_pos to 0, and set result_ptr and _bytes to a sufficiently large
 * buffeer (sizeof(result) + PATH_MAX is a solid choice).
 *
 * After each returned result set the next search dir_ino and dir_pos to
 * the returned dir_ino and dir_pos.  Then increment the search dir_pos,
 * and if it wrapped to 0, increment dir_ino.
 *
 * This only walks back through full hard links.  None of the returned
 * paths will reflect symlinks to components in the path.
 *
 * This doesn't ensure that the caller has permissions to traverse the
 * returned paths to the inode.  It requires CAP_DAC_READ_SEARCH which
 * bypasses permissions checking.
 *
 * This call is not serialized with any modification (create, rename,
 * unlink) of the path components.  It will return all the paths that
 * were stable both before and after the call.  It may or may not return
 * paths which are created or unlinked during the call.
 *
 * On success 0 is returned and result struct is filled with the next
 * absolute path.  The path_bytes length of the path includes a null
 * terminating byte.  dir_ino and dir_pos refer to the position of the
 * final component in its parent directory and can be advanced to search
 * for the next terminal entry whose path is then built by walking up
 * parent directories.
 *
 * ENOENT is returned when no paths are found.
 *
 * ENAMETOOLONG is returned when the result struct and path found
 * doesn't fit in the result buffer.
 *
 * Many other errnos indicate hard failure to find the next path.
 */
struct scoutfs_ioctl_ino_path {
	__u64 ino;
	__u64 dir_ino;
	__u64 dir_pos;
	__u64 result_ptr;
	__u16 result_bytes;
	__u8 _pad[6];
};

struct scoutfs_ioctl_ino_path_result {
	__u64 dir_ino;
	__u64 dir_pos;
	__u16 path_bytes;
	__u8  _pad[6];
	__u8  path[];
};

/* Get a single path from the root to the given inode number */
#define SCOUTFS_IOC_INO_PATH _IOR(SCOUTFS_IOCTL_MAGIC, 2, \
				  struct scoutfs_ioctl_ino_path)

/*
 * "Release" a contiguous range of logical blocks of file data.
 * Released blocks are removed from the file system like truncation, but
 * an offline record is left behind to trigger demand staging if the
 * file is read.
 *
 * The starting file offset and number of bytes to release must be in
 * multiples of 4KB.
 *
 * The specified range can extend past i_size and can straddle sparse
 * regions or blocks that are already offline.  The only change it makes
 * is to free and mark offline any existing blocks that intersect with
 * the region.
 *
 * Returns 0 if the operation succeeds.  If an error is returned then
 * some partial region of the blocks in the region may have been marked
 * offline.
 *
 * If the operation succeeds then inode metadata that reflects file data
 * contents are not updated.  This is intended to be transparent to the
 * presentation of the data in the file.
 */
struct scoutfs_ioctl_release {
	__u64 offset;
	__u64 length;
	__u64 data_version;
};

#define SCOUTFS_IOC_RELEASE _IOW(SCOUTFS_IOCTL_MAGIC, 3, \
				 struct scoutfs_ioctl_release)

struct scoutfs_ioctl_stage {
	__u64 data_version;
	__u64 buf_ptr;
	__u64 offset;
	__s32 length;
	__u32 _pad;
};

#define SCOUTFS_IOC_STAGE _IOW(SCOUTFS_IOCTL_MAGIC, 4, \
			       struct scoutfs_ioctl_stage)

/*
 * Give the user inode fields that are not otherwise visible.  statx()
 * isn't always available and xattrs are relatively expensive.
 *
 * @valid_bytes stores the number of bytes that are valid in the
 * structure.  The caller sets this to the size of the struct that they
 * understand.  The kernel then fills and copies back the min of the
 * size they and the user caller understand.  The user can tell if a
 * field is set if all of its bytes are within the valid_bytes that the
 * kernel set on return.
 *
 * New fields are only added to the end of the struct.
 */
struct scoutfs_ioctl_stat_more {
	__u64 valid_bytes;
	__u64 meta_seq;
	__u64 data_seq;
	__u64 data_version;
	__u64 online_blocks;
	__u64 offline_blocks;
	__u64 crtime_sec;
	__u32 crtime_nsec;
	__u8  _pad[4];
};

#define SCOUTFS_IOC_STAT_MORE _IOR(SCOUTFS_IOCTL_MAGIC, 5, \
				   struct scoutfs_ioctl_stat_more)


struct scoutfs_ioctl_data_waiting_entry {
	__u64 ino;
	__u64 iblock;
	__u8 op;
	__u8 _pad[7];
};

#define SCOUTFS_IOC_DWO_READ		(1 << 0)
#define SCOUTFS_IOC_DWO_WRITE		(1 << 1)
#define SCOUTFS_IOC_DWO_CHANGE_SIZE	(1 << 2)
#define SCOUTFS_IOC_DWO_UNKNOWN		(U8_MAX << 3)

struct scoutfs_ioctl_data_waiting {
	__u64 flags;
	__u64 after_ino;
	__u64 after_iblock;
	__u64 ents_ptr;
	__u16 ents_nr;
	__u8 _pad[6];
};

#define SCOUTFS_IOC_DATA_WAITING_FLAGS_UNKNOWN		(U64_MAX << 0)

#define SCOUTFS_IOC_DATA_WAITING _IOR(SCOUTFS_IOCTL_MAGIC, 6, \
				      struct scoutfs_ioctl_data_waiting)

/*
 * If i_size is set then data_version must be non-zero.  If the offline
 * flag is set then i_size must be set and a offline extent will be
 * created from offset 0 to i_size.  The time fields are always applied
 * to the inode.
 */
struct scoutfs_ioctl_setattr_more {
	__u64 data_version;
	__u64 i_size;
	__u64 flags;
	__u64 ctime_sec;
	__u32 ctime_nsec;
	__u32 crtime_nsec;
	__u64 crtime_sec;
};

#define SCOUTFS_IOC_SETATTR_MORE_OFFLINE		(1 << 0)
#define SCOUTFS_IOC_SETATTR_MORE_UNKNOWN		(U64_MAX << 1)

#define SCOUTFS_IOC_SETATTR_MORE _IOW(SCOUTFS_IOCTL_MAGIC, 7, \
				      struct scoutfs_ioctl_setattr_more)

struct scoutfs_ioctl_listxattr_hidden {
	__u64 id_pos;
	__u64 buf_ptr;
	__u32 buf_bytes;
	__u32 hash_pos;
};

#define SCOUTFS_IOC_LISTXATTR_HIDDEN _IOR(SCOUTFS_IOCTL_MAGIC, 8, \
					  struct scoutfs_ioctl_listxattr_hidden)

/*
 * Return the inode numbers of inodes which might contain the given
 * xattr.  The inode may not have a set xattr with that name, the caller
 * must check the returned inodes to see if they match.
 *
 * @next_ino: The next inode number that could be returned.  Initialized
 * to 0 when first searching and set to one past the last inode number
 * returned to continue searching.
 * @last_ino: The last inode number that could be returned.  U64_MAX to
 * find all inodes.
 * @name_ptr: The address of the name of the xattr to search for.  It is
 * not null terminated.
 * @inodes_ptr: The address of the array of uint64_t inode numbers in
 * which to store inode numbers that may contain the xattr.  EFAULT may
 * be returned if this address is not naturally aligned.
 * @output_flags: Set as success is returned.  If an error is returned
 * then this field is undefined and should not be read.
 * @nr_inodes: The number of elements in the array found at inodes_ptr.
 * @name_bytes: The number of non-null bytes found in the name at
 * name_ptr.
 *
 * This requires the CAP_SYS_ADMIN capability and will return -EPERM if
 * it's not granted.
 *
 * The number of inode numbers stored in the inodes_ptr array is
 * returned.  If nr_inodes is 0 or last_ino is less than next_ino then 0
 * will be immediately returned.
 *
 * Partial progress can be returned if an error is hit or if nr_inodes
 * was larger than the internal limit on the number of inodes returned
 * in a search pass.  The _END output flag is set if all the results
 * including last_ino were searched in this pass.
 *
 * It's valuable to provide a large inodes array so that all the results
 * can be found in one search pass and _END can be set.  There are
 * significant constant costs for performing each search pass.
 */
struct scoutfs_ioctl_search_xattrs {
	__u64 next_ino;
	__u64 last_ino;
	__u64 name_ptr;
	__u64 inodes_ptr;
	__u64 output_flags;
	__u64 nr_inodes;
	__u16 name_bytes;
	__u8 _pad[6];
};

/* set in output_flags if returned inodes reached last_ino */
#define SCOUTFS_SEARCH_XATTRS_OFLAG_END (1ULL << 0)

#define SCOUTFS_IOC_SEARCH_XATTRS _IOR(SCOUTFS_IOCTL_MAGIC, 9, \
				     struct scoutfs_ioctl_search_xattrs)

/*
 * Give the user information about the filesystem.
 *
 * @valid_bytes stores the number of bytes that are valid in the
 * structure.  The caller sets this to the size of the struct that they
 * understand.  The kernel then fills and copies back the min of the
 * size they and the user caller understand.  The user can tell if a
 * field is set if all of its bytes are within the valid_bytes that the
 * kernel set on return.
 *
 * @committed_seq: All seqs up to and including this seq have been
 * committed.  Can be compared with meta_seq and data_seq from inodes in
 * stat_more to discover if changes have been committed to disk.
 *
 * New fields are only added to the end of the struct.
 */
struct scoutfs_ioctl_statfs_more {
	__u64 valid_bytes;
	__u64 fsid;
	__u64 rid;
	__u64 committed_seq;
	__u64 total_meta_blocks;
	__u64 total_data_blocks;
	__u64 reserved_meta_blocks;
};

#define SCOUTFS_IOC_STATFS_MORE _IOR(SCOUTFS_IOCTL_MAGIC, 10, \
				     struct scoutfs_ioctl_statfs_more)

/*
 * Cause matching waiters to return an error.
 *
 * Find current waiters that match the inode, op, and block range to wake
 * up and return an error.
 */
struct scoutfs_ioctl_data_wait_err {
	__u64 ino;
	__u64 data_version;
	__u64 offset;
	__u64 count;
	__u64 op;
	__s64 err;
};

#define SCOUTFS_IOC_DATA_WAIT_ERR _IOR(SCOUTFS_IOCTL_MAGIC, 11, \
				       struct scoutfs_ioctl_data_wait_err)


struct scoutfs_ioctl_alloc_detail {
	__u64 entries_ptr;
	__u64 entries_nr;
};

struct scoutfs_ioctl_alloc_detail_entry {
	__u64 id;
	__u64 blocks;
	__u8 type;
	__u8 meta:1,
	     avail:1;
	__u8 __bit_pad:6;
	__u8 __pad[6];
};

#define SCOUTFS_IOC_ALLOC_DETAIL _IOR(SCOUTFS_IOCTL_MAGIC, 12, \
				      struct scoutfs_ioctl_alloc_detail)

/*
 * Move extents from one regular file to another at a different offset,
 * on the same file system.
 *
 * from_fd specifies the source file and the ioctl is called on the
 * destination file.  Both files must have write access.  from_off specifies
 * the byte offset in the source, to_off is the byte offset in the
 * destination, and len is the number of bytes in the region to move.  All of
 * the offsets and lengths must be in multiples of 4KB, except in the case
 * where the from_off + len ends at the i_size of the source
 * file. data_version is only used when STAGE flag is set (see below).  flags
 * field is currently only used to optionally specify STAGE behavior.
 *
 * This interface only moves extents which are block granular, it does
 * not perform RMW of sub-block byte extents and it does not overwrite
 * existing extents in the destination.  It will split extents in the
 * source.
 *
 * Only extents within i_size on the source are moved.  The destination
 * i_size will be updated if extents are moved beyond its current
 * i_size.  The i_size update will maintain final partial blocks in the
 * source.
 *
 * If STAGE flag is not set, it will return an error if either of the files
 * have offline extents.  It will return 0 when all of the extents in the
 * source region have been moved to the destination.  Moving extents updates
 * the ctime, mtime, meta_seq, data_seq, and data_version fields of both the
 * source and destination inodes.  If an error is returned then partial
 * progress may have been made and inode fields may have been updated.
 *
 * If STAGE flag is set, as above except destination range must be in an
 * offline extent. Fields are updated only for source inode.
 *
 * Errors specific to this interface include:
 *
 * EINVAL: from_off, len, or to_off aren't a multiple of 4KB; the source
 *	   and destination files are the same inode; either the source or
 *	   destination is not a regular file; the destination file has
 *	   an existing overlapping extent (if STAGE flag not set); the
 *	   destination range is not in an offline extent (if STAGE set).
 * EOVERFLOW: either from_off + len or to_off + len exceeded 64bits.
 * EBADF: from_fd isn't a valid open file descriptor.
 * EXDEV: the source and destination files are in different filesystems.
 * EISDIR: either the source or destination is a directory.
 * ENODATA: either the source or destination file have offline extents and
 *	    STAGE flag is not set.
 * ESTALE: data_version does not match destination data_version.
 */
#define SCOUTFS_IOC_MB_STAGE		(1 << 0)
#define SCOUTFS_IOC_MB_UNKNOWN		(U64_MAX << 1)

struct scoutfs_ioctl_move_blocks {
	__u64 from_fd;
	__u64 from_off;
	__u64 len;
	__u64 to_off;
	__u64 data_version;
	__u64 flags;
};

#define SCOUTFS_IOC_MOVE_BLOCKS _IOR(SCOUTFS_IOCTL_MAGIC, 13, \
				     struct scoutfs_ioctl_move_blocks)

struct scoutfs_ioctl_resize_devices {
	__u64 new_total_meta_blocks;
	__u64 new_total_data_blocks;
};

#define SCOUTFS_IOC_RESIZE_DEVICES \
	_IOR(SCOUTFS_IOCTL_MAGIC, 14, struct scoutfs_ioctl_resize_devices)

#define SCOUTFs_IOCTL_XATTR_TOTAL_NAME_NR 3

/*
 * Copy global totals of .totl. xattr value payloads to the user.   This
 * only sees xattrs which have been committed and this doesn't force
 * commits of dirty data throughout the system.  This can be out of sync
 * by the amount of xattrs that can be dirty in open transactions that
 * are being built throughout the system.
 *
 * pos_name: The array name of the first total that can be returned.
 * The name is derived from the key of the xattrs that contribute to the
 * total.  For xattrs with a .totl.1.2.3 key, the pos_name[] should be
 * {1, 2, 3}.
 *
 * totals_ptr: An aligned pointer to a buffer that will be filled with
 * an array of scoutfs_ioctl_xattr_total structs for each total copied.
 *
 * totals_bytes: The size of the buffer in bytes.  There must be room
 * for at least one struct element so that returning 0 can promise that
 * there were no more totals to copy after the pos_name.
 *
 * The number of copied elements is returned and 0 is returned if there
 * were no more totals to copy after the pos_name.
 *
 * In addition to the usual errnos (EIO, EINVAL, EPERM, EFAULT) this
 * adds:
 *
 * EINVAL: The totals_ buffer was not aligned or was not large enough
 * for a single struct entry.
 */
struct scoutfs_ioctl_read_xattr_totals {
	__u64 pos_name[SCOUTFs_IOCTL_XATTR_TOTAL_NAME_NR];
	__u64 totals_ptr;
	__u64 totals_bytes;
};

/*
 * An individual total that is given to userspace.   The total is the
 * sum of all the values in the xattr payloads matching the name.  The
 * count is the number of xattrs, not number of files, contributing to
 * the total.
 */
struct scoutfs_ioctl_xattr_total {
	__u64 name[SCOUTFs_IOCTL_XATTR_TOTAL_NAME_NR];
	__u64 total;
	__u64 count;
};

#define SCOUTFS_IOC_READ_XATTR_TOTALS \
	_IOR(SCOUTFS_IOCTL_MAGIC, 15, struct scoutfs_ioctl_read_xattr_totals)

#endif
