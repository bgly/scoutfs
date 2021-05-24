#include <unistd.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include <uuid/uuid.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <inttypes.h>
#include <argp.h>

#include "sparse.h"
#include "cmd.h"
#include "util.h"
#include "format.h"
#include "parse.h"
#include "crc.h"
#include "rand.h"
#include "dev.h"
#include "key.h"
#include "bitops.h"
#include "btree.h"
#include "leaf_item_hash.h"
#include "blkid.h"

/*
 * Update the block header fields and write out the block.
 */
static int write_block(int fd, u32 magic, __le64 fsid, u64 seq, u64 blkno,
		       int shift, struct scoutfs_block_header *hdr)
{
	size_t size = 1ULL << shift;
	ssize_t ret;

	hdr->magic = cpu_to_le32(magic);
	hdr->fsid = fsid;
	hdr->blkno = cpu_to_le64(blkno);
	hdr->seq = cpu_to_le64(seq);
	hdr->crc = cpu_to_le32(crc_block(hdr, size));

	ret = pwrite(fd, hdr, size, blkno << shift);
	if (ret != size) {
		fprintf(stderr, "write to blkno %llu returned %zd: %s (%d)\n",
			blkno, ret, strerror(errno), errno);
		return -errno;
	}

	return 0;
}

/*
 * Write the single btree block that contains the blkno and len indexed
 * items to store the given extent, and update the root to point to it.
 */
static int write_alloc_root(int fd, __le64 fsid,
			    struct scoutfs_alloc_root *root,
			    struct scoutfs_btree_block *bt,
			    u64 seq, u64 blkno, u64 start, u64 len)
{
	struct scoutfs_key key;

	btree_init_root_single(&root->root, bt, seq, blkno);
	root->total_len = cpu_to_le64(len);

	memset(&key, 0, sizeof(key));
	key.sk_zone = SCOUTFS_FREE_EXTENT_ZONE;
	key.sk_type = SCOUTFS_FREE_EXTENT_BLKNO_TYPE;
	key.skii_ino = cpu_to_le64(SCOUTFS_ROOT_INO);
	key.skfb_end = cpu_to_le64(start + len - 1);
	key.skfb_len = cpu_to_le64(len);
	btree_append_item(bt, &key, NULL, 0);

	memset(&key, 0, sizeof(key));
	key.sk_zone = SCOUTFS_FREE_EXTENT_ZONE;
	key.sk_type = SCOUTFS_FREE_EXTENT_LEN_TYPE;
	key.skii_ino = cpu_to_le64(SCOUTFS_ROOT_INO);
	key.skfl_neglen = cpu_to_le64(-len);
	key.skfl_blkno = cpu_to_le64(start);
	btree_append_item(bt, &key, NULL, 0);

	return write_block(fd, SCOUTFS_BLOCK_MAGIC_BTREE, fsid, seq, blkno,
			   SCOUTFS_BLOCK_LG_SHIFT, &bt->hdr);
}

struct mkfs_args {
	char *meta_device;
	char *data_device;
	unsigned long long max_meta_size;
	unsigned long long max_data_size;
	bool force;
	int nr_slots;
	struct scoutfs_quorum_slot slots[SCOUTFS_QUORUM_MAX_SLOTS];
};

/*
 * Make a new file system by writing:
 *  - super blocks
 *  - btree ring blocks with manifest and allocator btree blocks
 *  - segment with root inode items
 *
 * Superblock is written to both metadata and data devices, everything else is
 * written only to the metadata device.
 */
static int do_mkfs(struct mkfs_args *args)
{
	struct scoutfs_super_block *super = NULL;
	struct scoutfs_inode inode;
	struct scoutfs_alloc_list_block *lblk;
	struct scoutfs_btree_block *bt = NULL;
	struct scoutfs_block_header *hdr;
	struct scoutfs_key key;
	struct timeval tv;
	int meta_fd = -1;
	int data_fd = -1;
	char uuid_str[37];
	void *zeros = NULL;
	char *indent;
	u64 blkno;
	u64 meta_size;
	u64 data_size;
	u64 next_meta;
	u64 last_meta;
	u64 first_data;
	u64 last_data;
	u64 meta_start;
	u64 meta_len;
	__le64 fsid;
	int ret;
	int i;

	gettimeofday(&tv, NULL);
	pseudo_random_bytes(&fsid, sizeof(fsid));

	meta_fd = open(args->meta_device, O_RDWR | O_EXCL);
	if (meta_fd < 0) {
		ret = -errno;
		fprintf(stderr, "failed to open '%s': %s (%d)\n",
			args->meta_device, strerror(errno), errno);
		goto out;
	}
	if (!args->force) {
		ret = check_bdev(meta_fd, args->meta_device, "meta");
		if (ret)
			return ret;
	}

	data_fd = open(args->data_device, O_RDWR | O_EXCL);
	if (data_fd < 0) {
		ret = -errno;
		fprintf(stderr, "failed to open '%s': %s (%d)\n",
			args->data_device, strerror(errno), errno);
		goto out;
	}
	if (!args->force) {
		ret = check_bdev(data_fd, args->data_device, "data");
		if (ret)
			return ret;
	}


	super = calloc(1, SCOUTFS_BLOCK_SM_SIZE);
	bt = calloc(1, SCOUTFS_BLOCK_LG_SIZE);
	zeros = calloc(1, SCOUTFS_BLOCK_SM_SIZE);
	if (!super || !bt || !zeros) {
		ret = -errno;
		fprintf(stderr, "failed to allocate block mem: %s (%d)\n",
			strerror(errno), errno);
		goto out;
	}

	ret = device_size(args->meta_device, meta_fd, 2ULL * (1024 * 1024 * 1024),
			  args->max_meta_size, "meta", &meta_size);
	if (ret)
		goto out;

	ret = device_size(args->data_device, data_fd, 8ULL * (1024 * 1024 * 1024),
			  args->max_data_size, "data", &data_size);
	if (ret)
		goto out;

	next_meta = SCOUTFS_META_DEV_START_BLKNO;
	last_meta = (meta_size >> SCOUTFS_BLOCK_LG_SHIFT) - 1;
	/* Data blocks go on the data dev */
	first_data = SCOUTFS_DATA_DEV_START_BLKNO;
	last_data = (data_size >> SCOUTFS_BLOCK_SM_SHIFT) - 1;

	/* partially initialize the super so we can use it to init others */
	memset(super, 0, SCOUTFS_BLOCK_SM_SIZE);
	super->version = cpu_to_le64(SCOUTFS_INTEROP_VERSION);
	uuid_generate(super->uuid);
	super->next_ino = cpu_to_le64(round_up(SCOUTFS_ROOT_INO + 1, SCOUTFS_LOCK_INODE_GROUP_NR));
	super->next_trans_seq = cpu_to_le64(1);
	super->total_meta_blocks = cpu_to_le64(last_meta + 1);
	super->first_meta_blkno = cpu_to_le64(next_meta);
	super->last_meta_blkno = cpu_to_le64(last_meta);
	super->total_data_blocks = cpu_to_le64(last_data - first_data + 1);
	super->first_data_blkno = cpu_to_le64(first_data);
	super->last_data_blkno = cpu_to_le64(last_data);

	assert(sizeof(args->slots) ==
		     member_sizeof(struct scoutfs_super_block, qconf.slots));
	memcpy(super->qconf.slots, args->slots, sizeof(args->slots));

	/* fs root starts with root inode and its index items */
	blkno = next_meta++;
	btree_init_root_single(&super->fs_root, bt, 1, blkno);

	memset(&key, 0, sizeof(key));
	key.sk_zone = SCOUTFS_INODE_INDEX_ZONE;
	key.sk_type = SCOUTFS_INODE_INDEX_META_SEQ_TYPE;
	key.skii_ino = cpu_to_le64(SCOUTFS_ROOT_INO);
	btree_append_item(bt, &key, NULL, 0);

	memset(&key, 0, sizeof(key));
	key.sk_zone = SCOUTFS_FS_ZONE;
	key.ski_ino = cpu_to_le64(SCOUTFS_ROOT_INO);
	key.sk_type = SCOUTFS_INODE_TYPE;

	memset(&inode, 0, sizeof(inode));
	inode.next_readdir_pos = cpu_to_le64(2);
	inode.nlink = cpu_to_le32(SCOUTFS_DIRENT_FIRST_POS);
	inode.mode = cpu_to_le32(0755 | 0040000);
	inode.atime.sec = cpu_to_le64(tv.tv_sec);
	inode.atime.nsec = cpu_to_le32(tv.tv_usec * 1000);
	inode.ctime.sec = inode.atime.sec;
	inode.ctime.nsec = inode.atime.nsec;
	inode.mtime.sec = inode.atime.sec;
	inode.mtime.nsec = inode.atime.nsec;
	btree_append_item(bt, &key, &inode, sizeof(inode));

	ret = write_block(meta_fd, SCOUTFS_BLOCK_MAGIC_BTREE, fsid, 1, blkno,
			  SCOUTFS_BLOCK_LG_SHIFT, &bt->hdr);
	if (ret)
		goto out;

	/* fill an avail list block for the first server transaction */
	blkno = next_meta++;
	lblk = (void *)bt;
	memset(lblk, 0, SCOUTFS_BLOCK_LG_SIZE);

	meta_len = (64 * 1024 * 1024) >> SCOUTFS_BLOCK_LG_SHIFT;
	for (i = 0; i < meta_len; i++) {
		lblk->blknos[i] = cpu_to_le64(next_meta);
		next_meta++;
	}
	lblk->nr = cpu_to_le32(i);

	super->server_meta_avail[0].ref.blkno = cpu_to_le64(blkno);
	super->server_meta_avail[0].ref.seq = cpu_to_le64(1);
	super->server_meta_avail[0].total_nr = le32_to_le64(lblk->nr);
	super->server_meta_avail[0].first_nr = lblk->nr;

	ret = write_block(meta_fd, SCOUTFS_BLOCK_MAGIC_ALLOC_LIST, fsid, 1,
			  blkno, SCOUTFS_BLOCK_LG_SHIFT, &lblk->hdr);
	if (ret)
		goto out;

	/* the data allocator has a single extent */
	blkno = next_meta++;
	ret = write_alloc_root(meta_fd, fsid, &super->data_alloc, bt,
			       1, blkno, first_data,
			       le64_to_cpu(super->total_data_blocks));
	if (ret < 0)
		goto out;

	/*
	 * Initialize all the meta_alloc roots with an equal portion of
	 * the free metadata extents, excluding the blocks we're going
	 * to use for the allocators.
	 */
	meta_start = next_meta + array_size(super->meta_alloc);
	meta_len = DIV_ROUND_UP(last_meta - meta_start + 1,
			        array_size(super->meta_alloc));

	/* each meta alloc root contains a portion of free metadata extents */
	for (i = 0; i < array_size(super->meta_alloc); i++) {
		blkno = next_meta++;
		ret = write_alloc_root(meta_fd, fsid, &super->meta_alloc[i], bt,
				       1, blkno, meta_start,
				       min(meta_len,
					   last_meta - meta_start + 1));
		if (ret < 0)
			goto out;

		meta_start += meta_len;
	}

	/* zero out quorum blocks */
	hdr = zeros;
	for (i = 0; i < SCOUTFS_QUORUM_BLOCKS; i++) {
		ret = write_block(meta_fd, SCOUTFS_BLOCK_MAGIC_QUORUM, fsid,
				  1, SCOUTFS_QUORUM_BLKNO + i,
				  SCOUTFS_BLOCK_SM_SHIFT, hdr);
		if (ret < 0) {
			fprintf(stderr, "error zeroing quorum block: %s (%d)\n",
				strerror(-errno), -errno);
			goto out;
		}
	}

	/* write the super block to data dev and meta dev*/
	ret = write_block(data_fd, SCOUTFS_BLOCK_MAGIC_SUPER, fsid, 1,
			  SCOUTFS_SUPER_BLKNO, SCOUTFS_BLOCK_SM_SHIFT,
			  &super->hdr);
	if (ret)
		goto out;

	if (fsync(data_fd)) {
		ret = -errno;
		fprintf(stderr, "failed to fsync '%s': %s (%d)\n",
			args->data_device, strerror(errno), errno);
		goto out;
	}

	super->flags |= cpu_to_le64(SCOUTFS_FLAG_IS_META_BDEV);
	ret = write_block(meta_fd, SCOUTFS_BLOCK_MAGIC_SUPER, fsid,
			  1, SCOUTFS_SUPER_BLKNO, SCOUTFS_BLOCK_SM_SHIFT,
			  &super->hdr);
	if (ret)
		goto out;

	if (fsync(meta_fd)) {
		ret = -errno;
		fprintf(stderr, "failed to fsync '%s': %s (%d)\n",
			args->meta_device, strerror(errno), errno);
		goto out;
	}

	uuid_unparse(super->uuid, uuid_str);

	printf("Created scoutfs filesystem:\n"
	       "  meta device path:     %s\n"
	       "  data device path:     %s\n"
	       "  fsid:                 %llx\n"
	       "  version:              %llx\n"
	       "  uuid:                 %s\n"
	       "  64KB metadata blocks: "SIZE_FMT"\n"
	       "  4KB data blocks:      "SIZE_FMT"\n"
	       "  quorum slots:         ",
		args->meta_device,
	        args->data_device,
		le64_to_cpu(super->hdr.fsid),
		le64_to_cpu(super->version),
		uuid_str,
		SIZE_ARGS(le64_to_cpu(super->total_meta_blocks),
			  SCOUTFS_BLOCK_LG_SIZE),
		SIZE_ARGS(le64_to_cpu(super->total_data_blocks),
			  SCOUTFS_BLOCK_SM_SIZE));

	indent = "";
	for (i = 0; i < SCOUTFS_QUORUM_MAX_SLOTS; i++) {
		struct scoutfs_quorum_slot *sl = &super->qconf.slots[i];
		struct in_addr in;

		if (sl->addr.v4.family != cpu_to_le16(SCOUTFS_AF_IPV4))
			continue;

		in.s_addr = htonl(le32_to_cpu(sl->addr.v4.addr));
		printf("%s%u: %s:%u", indent,
		       i, inet_ntoa(in), le16_to_cpu(sl->addr.v4.port));
		indent = "\n                        ";
	}
	printf("\n");

	ret = 0;
out:
	if (super)
		free(super);
	if (bt)
		free(bt);
	if (zeros)
		free(zeros);
	if (meta_fd != -1)
		close(meta_fd);
	if (data_fd != -1)
		close(data_fd);
	return ret;
}

static bool valid_quorum_slots(struct scoutfs_quorum_slot *slots)
{
	struct in_addr in;
	bool valid = true;
	char *addr;
	int i;
	int j;

	for (i = 0; i < SCOUTFS_QUORUM_MAX_SLOTS; i++) {
		if (slots[i].addr.v4.family == cpu_to_le16(SCOUTFS_AF_NONE))
			continue;

		if (slots[i].addr.v4.family != cpu_to_le16(SCOUTFS_AF_IPV4)) {
			fprintf(stderr, "quorum slot nr %u has invalid family %u\n",
				i, le16_to_cpu(slots[i].addr.v4.family));
			valid = false;
		}

		for (j = i + 1; j < SCOUTFS_QUORUM_MAX_SLOTS; j++) {
			if (slots[i].addr.v4.family != cpu_to_le16(SCOUTFS_AF_IPV4))
				continue;

			if (slots[i].addr.v4.addr == slots[j].addr.v4.addr &&
			    slots[i].addr.v4.port == slots[j].addr.v4.port) {

				in.s_addr =
					htonl(le32_to_cpu(slots[i].addr.v4.addr));
				addr = inet_ntoa(in);
				fprintf(stderr, "quorum slot nr %u and %u have the same address %s:%u\n",
					i, j, addr,
					le16_to_cpu(slots[i].addr.v4.port));
				valid = false;
			}
		}
	}

	return valid;
}

static int parse_opt(int key, char *arg, struct argp_state *state)
{
	struct mkfs_args *args = state->input;
	struct scoutfs_quorum_slot slot;
	int ret;

	switch (key) {
	case 'Q':
		ret = parse_quorum_slot(&slot, arg);
		if (ret < 0)
			return ret;
		if (args->slots[ret].addr.v4.family != cpu_to_le16(SCOUTFS_AF_NONE))
			argp_error(state, "Quorum slot %u already specified before slot '%s'\n",
				   ret, arg);
		args->slots[ret] = slot;
		args->nr_slots++;
		break;
	case 'f':
		args->force = true;
		break;
	case 'm': /* max-meta-size */
	{
		u64 prev_val;
		ret = parse_human(arg, &args->max_meta_size);
		if (ret)
			return ret;
		prev_val = args->max_meta_size;
		args->max_meta_size = round_down(args->max_meta_size, SCOUTFS_BLOCK_LG_SIZE);
		if (args->max_meta_size != prev_val)
			fprintf(stderr, "Meta dev size %llu rounded down to %llu bytes\n",
				prev_val, args->max_meta_size);
		break;
	}
	case 'd': /* max-data-size */
	{
		u64 prev_val;
		ret = parse_human(arg, &args->max_data_size);
		if (ret)
			return ret;
		prev_val = args->max_data_size;
		args->max_data_size = round_down(args->max_data_size, SCOUTFS_BLOCK_SM_SIZE);
		if (args->max_data_size != prev_val)
			fprintf(stderr, "Data dev size %llu rounded down to %llu bytes\n",
				prev_val, args->max_data_size);
		break;
	}
	case ARGP_KEY_ARG:
		if (!args->meta_device)
			args->meta_device = strdup_or_error(state, arg);
		else if (!args->data_device)
			args->data_device = strdup_or_error(state, arg);
		else
			argp_error(state, "more than two arguments given");
		break;
	case ARGP_KEY_FINI:
		if (!args->nr_slots)
			argp_error(state, "must specify at least one quorum slot with --quorum-count|-Q");
		if (!args->meta_device)
			argp_error(state, "no metadata device argument given");
		if (!args->data_device)
			argp_error(state, "no data device argument given");
		if (!valid_quorum_slots(args->slots))
			argp_error(state, "invalid quorum slot configuration");
		break;
	default:
		break;
	}

	return 0;
}

static struct argp_option options[] = {
	{ "quorum-slot", 'Q', "NR,ADDR,PORT", 0, "Specify quorum slot addresses [Required]"},
	{ "force", 'f', NULL, 0, "Overwrite existing data on block devices"},
	{ "max-meta-size", 'm', "SIZE", 0, "Use a size less than the base metadata device size (bytes or KMGTP units)"},
	{ "max-data-size", 'd', "SIZE", 0, "Use a size less than the base data device size (bytes or KMGTP units)"},
	{ NULL }
};

static struct argp argp = {
	options,
	parse_opt,
	"META-DEVICE DATA-DEVICE",
	"Initialize a new ScoutFS filesystem"
};

static int mkfs_cmd(int argc, char *argv[])
{
	struct mkfs_args mkfs_args = {NULL,};
	int ret;

	ret = argp_parse(&argp, argc, argv, 0, NULL, &mkfs_args);
	if (ret)
		return ret;

	return do_mkfs(&mkfs_args);
}

static void __attribute__((constructor)) mkfs_ctor(void)
{
	cmd_register_argp("mkfs", &argp, GROUP_CORE, mkfs_cmd);

	/* for lack of some other place to put these.. */
	build_assert(sizeof(uuid_t) == SCOUTFS_UUID_BYTES);
}
