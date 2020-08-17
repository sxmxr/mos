#include "mouse.h"

#include <include/errno.h>
#include <kernel/cpu/hal.h>
#include <kernel/cpu/idt.h>
#include <kernel/cpu/pic.h>
#include <kernel/fs/char_dev.h>
#include <kernel/fs/poll.h>
#include <kernel/fs/vfs.h>
#include <kernel/memory/vmm.h>
#include <kernel/proc/task.h>
#include <kernel/utils/printf.h>
#include <kernel/utils/string.h>

#define MOUSE_PORT 0x60
#define MOUSE_STATUS 0x64
#define MOUSE_ABIT 0x02
#define MOUSE_BBIT 0x01
#define MOUSE_WRITE 0xD4
#define MOUSE_F_BIT 0x20
#define MOUSE_V_BIT 0x08

static uint8_t mouse_cycle = 0;
static uint8_t mouse_byte[4];
static struct mouse_event current_mouse_motion;
struct list_head nodelist;
struct wait_queue_head hwait;

void mouse_notify_readers(struct mouse_event *mm)
{
	struct mouse_inode *iter;
	list_for_each_entry(iter, &nodelist, sibling)
	{
		iter->tail = (iter->tail + 1) / MOUSE_PACKET_QUEUE_LEN;
		iter->packets[iter->tail] = *mm;
		if (iter->tail == iter->head)
			iter->head = (iter->head + 1) / MOUSE_PACKET_QUEUE_LEN;
		iter->ready = true;
	}
	wake_up(&hwait);
}

static int mouse_open(struct vfs_inode *inode, struct vfs_file *file)
{
	struct mouse_inode *mi = kcalloc(sizeof(struct mouse_inode), 1);
	mi->file = file;
	file->private_data = mi;
	list_add_tail(&mi->sibling, &nodelist);

	return 0;
}

static ssize_t mouse_read(struct vfs_file *file, char *buf, size_t count, loff_t ppos)
{
	struct mouse_inode *mi = (struct mouse_inode *)file->private_data;
	wait_event(&hwait, mi->ready);

	if (mi->head == mi->tail)
		return -EINVAL;

	memcpy(buf, &mi->packets[mi->head], sizeof(struct mouse_event));
	mi->head = (mi->head + 1) % MOUSE_PACKET_QUEUE_LEN;

	if (mi->head == mi->tail)
		mi->ready = false;

	return 0;
}

static unsigned int mouse_poll(struct vfs_file *file, struct poll_table *pt)
{
	struct mouse_inode *mi = (struct mouse_inode *)file->private_data;
	poll_wait(file, &hwait, pt);

	return mi->ready ? POLLIN : 0;
}

static int mouse_release(struct vfs_inode *inode, struct vfs_file *file)
{
	struct mouse_inode *mi = (struct mouse_inode *)file->private_data;
	list_del(&mi->sibling);
	kfree(mi);

	return 0;
}

static struct vfs_file_operations mouse_fops = {
	.open = mouse_open,
	.read = mouse_read,
	.poll = mouse_poll,
	.release = mouse_release,
};

static struct char_device cdev_mouse = {
	.name = "mouse",
	.dev = MKDEV(MOUSE_MAJOR, 1),
	.f_ops = &mouse_fops};

static void mouse_calculate_position()
{
	mouse_cycle = 0;
	uint8_t state = mouse_byte[0];
	int move_x = mouse_byte[1];
	int move_y = mouse_byte[2];

	if (move_x && state & (1 << 4))
	{
		/* Sign bit */
		move_x = move_x - 0x100;
	}
	if (move_y && state & (1 << 5))
	{
		/* Sign bit */
		move_y = move_y - 0x100;
	}
	if (state & (1 << 6) || state & (1 << 7))
	{
		/* Overflow */
		move_x = 0;
		move_y = 0;
	}

	current_mouse_motion.x = move_x;
	current_mouse_motion.y = move_y;

	current_mouse_motion.buttons = 0;
	// FIXME: MQ 2020-03-22 on Mac 10.15.3, qemu 4.2.0
	// after left/right clicking, next mouse events, first mouse packet state still has left/right state
	// workaround via using left/right command to simuate left/right click
	// if (state & MOUSE_LEFT_CLICK)
	// {
	//   current_mouse_motion.buttons |= MOUSE_LEFT_CLICK;
	// }
	// if (state & MOUSE_RIGHT_CLICK)
	// {
	//   current_mouse_motion.buttons |= MOUSE_RIGHT_CLICK;
	// }
	// if (state & MOUSE_MIDDLE_CLICK)
	// {
	//   current_mouse_motion.buttons |= MOUSE_MIDDLE_CLICK;
	// }
}

static int32_t irq_mouse_handler(struct interrupt_registers *regs)
{
	uint8_t status = inportb(MOUSE_STATUS);
	if ((status & MOUSE_BBIT) && (status & MOUSE_F_BIT))
	{
		uint8_t mouse_in = inportb(MOUSE_PORT);
		irq_ack(regs->int_no);
		switch (mouse_cycle)
		{
		case 0:
			mouse_byte[0] = mouse_in;
			if (!(mouse_in & MOUSE_V_BIT))
				break;
			mouse_cycle++;
			break;
		case 1:
			mouse_byte[1] = mouse_in;
			mouse_cycle++;
			break;
		case 2:
			mouse_byte[2] = mouse_in;
			mouse_calculate_position();
			if (current_mouse_motion.x != 0 || current_mouse_motion.y != 0 || current_mouse_motion.buttons != 0)
				mouse_notify_readers(&current_mouse_motion);
			break;
		}
	}
	else
		irq_ack(regs->int_no);

	return IRQ_HANDLER_CONTINUE;
}

static void mouse_wait(uint32_t type)
{
	if (type == 0)
	{
		for (uint32_t i = 0; i < 100000; i++)
		{
			if ((inportb(MOUSE_STATUS) & 1) == 1)
			{
				return;
			}
		}
		return;
	}
	else
	{
		for (uint32_t i = 0; i < 100000; i++)
		{
			if ((inportb(MOUSE_STATUS) & 2) == 0)
			{
				return;
			}
		}
		return;
	}
}

static void mouse_output(uint8_t value)
{
	mouse_wait(1);
	outportb(MOUSE_STATUS, 0xD4);
	mouse_wait(1);
	outportb(MOUSE_PORT, value);
}

static uint8_t mouse_input(void)
{
	mouse_wait(0);
	return inportb(MOUSE_PORT);
}

void mouse_init()
{
	DEBUG &&debug_println(DEBUG_INFO, "[mouse] - Initializing");
	INIT_LIST_HEAD(&nodelist);
	INIT_LIST_HEAD(&hwait.list);

	DEBUG &&debug_println(DEBUG_INFO, "[dev] - Mount mouse");
	register_chrdev(&cdev_mouse);
	vfs_mknod("/dev/input/mouse", S_IFCHR, cdev_mouse.dev);

	// empty input buffer
	while ((inportb(MOUSE_STATUS) & 0x01))
	{
		inportb(MOUSE_PORT);
	}

	uint8_t status = 0;

	current_mouse_motion.x = current_mouse_motion.y = 0;
	current_mouse_motion.buttons = 0;

	// activate mouse device
	mouse_wait(1);
	outportb(MOUSE_STATUS, 0xA8);

	// get commando-byte, set bit 1 (enables IRQ12), send back
	mouse_wait(1);
	outportb(MOUSE_STATUS, 0x20);

	mouse_wait(0);
	status = (inportb(MOUSE_PORT) | 3);

	mouse_wait(1);
	outportb(MOUSE_STATUS, 0x60);
	mouse_wait(1);
	outportb(MOUSE_PORT, status);

	// set sample rate
	mouse_output(0xF6);
	mouse_input();

	// start sending packets
	mouse_output(0xF4);
	mouse_input();

	register_interrupt_handler(IRQ12, irq_mouse_handler);
	pic_clear_mask(12);
	DEBUG &&debug_println(DEBUG_INFO, "[mouse] - Done");
}
