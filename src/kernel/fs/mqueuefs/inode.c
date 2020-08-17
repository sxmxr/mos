#include <include/errno.h>
#include <kernel/fs/vfs.h>
#include <kernel/ipc/message_queue.h>
#include <kernel/memory/vmm.h>
#include <kernel/proc/task.h>
#include <kernel/system/time.h>

#include "mqueuefs.h"

struct vfs_inode *mqueuefs_create_inode(struct vfs_inode *dir, char *filename, mode_t mode)
{
	return mqueuefs_get_inode(dir->i_sb, mode);
}

struct vfs_inode_operations mqueuefs_file_inode_operations = {};

struct vfs_inode_operations mqueuefs_dir_inode_operations = {
	.create = mqueuefs_create_inode,
};
