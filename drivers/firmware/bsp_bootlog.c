// SPDX-License-Identifier: GPL-2.0-only
/* Bounded warm-reset startup journal, independent of the root/data filesystem. */
#include <linux/bsp_bootlog_layout.h>
#include <linux/console.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/random.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>

static const char * const names[] = {"r5-spl", "a53-spl", "uboot", "kernel"};
struct bootlog {
	struct kobject *kobj;
	struct bin_attribute attr[12];
	char attr_names[12][32];
	void __iomem *mapped;
	u8 *snapshot;
	struct console console;
	raw_spinlock_t lock;
	unsigned int active_slot, kernel_length, kernel_crc, kernel_dropped;
	bool sealed, cleared, registered;
	char ack[80];
};
static struct bootlog *instance;
static DEFINE_MUTEX(snapshot_lock);

static struct bl_bank *snap_bank(struct bootlog *log, int slot)
{
	return (void *)(log->snapshot + slot * BL_BANK_SIZE);
}
static struct bl_record *snap_record(struct bootlog *log, int slot, int stage)
{
	return (void *)snap_bank(log, slot) + bl_offset[stage];
}
static void capture_write(struct console *console, const char *s, unsigned int count)
{
	struct bootlog *log = container_of(console, struct bootlog, console);
	unsigned long flags;
	unsigned int n;
	struct bl_record r;
	void __iomem *target = log->mapped + log->active_slot * BL_BANK_SIZE + bl_offset[3];

	if (!raw_spin_trylock_irqsave(&log->lock, flags))
		return;
	if (log->sealed)
		goto out;
	n = min(count, bl_size[3] - (unsigned int)sizeof(r) - log->kernel_length);
	/* Publish the old committed prefix before touching the new tail. */
	memset(&r, 0, sizeof(r));
	r.reserved[0] = log->kernel_length;
	r.reserved[1] = log->kernel_crc;
	memcpy_toio(target + offsetof(struct bl_record, reserved), r.reserved, sizeof(r.reserved));
	wmb();
	memcpy_toio(target + sizeof(r) + log->kernel_length, s, n);
	log->kernel_crc = bl_crc(log->kernel_crc, s, n);
	log->kernel_length += n;
	log->kernel_dropped += min(count - n, U32_MAX - log->kernel_dropped);
	r.magic = BL_RECORD_MAGIC;
	r.version = 2;
	r.stage = 4;
	r.length = log->kernel_length;
	r.crc = log->kernel_crc;
	r.dropped = log->kernel_dropped;
	/* Payload first, header last. A torn final record is rejected, not trusted. */
	wmb();
	memcpy_toio(target, &r, sizeof(r));
	wmb();
out:
	raw_spin_unlock_irqrestore(&log->lock, flags);
}
static ssize_t log_read(struct file *file, struct kobject *kobj,
		       const struct bin_attribute *attr, char *buf,
		       loff_t off, size_t count)
{
	struct bootlog *log = instance;
	unsigned long index = (unsigned long)attr->private;
	unsigned int slot = index / BL_STAGES, stage = index % BL_STAGES;
	struct bl_record *r;
	ssize_t result;

	guard(mutex)(&snapshot_lock);
	if (!log || log->cleared || !bl_valid(snap_bank(log, slot)))
		return 0;
	r = snap_record(log, slot, stage);
	if (!bl_record_valid(r, stage) && !bl_prefix_valid(r, stage))
		return 0;
	result = memory_read_from_buffer(buf, count, &off, r->data,
		bl_record_valid(r, stage) ? r->length : r->reserved[0]);
	return result;
}
static ssize_t banks_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct bootlog *log = instance;
	ssize_t n = 0;
	int slot, stage;

	guard(mutex)(&snapshot_lock);
	for (slot = 0; slot < 2; slot++) {
		struct bl_bank *b = snap_bank(log, slot);

		if (log->cleared || !bl_valid(b))
			continue;
		n += sysfs_emit_at(buf, n, "%u %pUb %llu %u\n", slot, b->id,
			(unsigned long long)b->generation, slot == log->active_slot);
		for (stage = 0; stage < BL_STAGES; stage++) {
			struct bl_record *r = snap_record(log, slot, stage);
			bool valid = bl_record_valid(r, stage);
			bool prefix = !valid && bl_prefix_valid(r, stage);

			n += sysfs_emit_at(buf, n, "%s %u %08x %u %u\n", names[stage],
				valid ? r->length : prefix ? r->reserved[0] : 0,
				valid ? r->crc : prefix ? r->reserved[1] : 0,
				r->magic == BL_RECORD_MAGIC ? r->dropped : 0,
				valid ? 1 : prefix ? 3 : r->magic ? 2 : 0);
		}
	}
	return n;
}
static ssize_t seal_store(struct kobject *kobj, struct kobj_attribute *attr,
			  const char *buf, size_t count)
{
	struct bootlog *log = instance;
	unsigned long flags;

	guard(mutex)(&snapshot_lock);
	if (!sysfs_streq(buf, "freeze"))
		return -EINVAL;
	if (!log->sealed) {
		raw_spin_lock_irqsave(&log->lock, flags);
		log->sealed = true;
		raw_spin_unlock_irqrestore(&log->lock, flags);
		unregister_console(&log->console);
		log->registered = false;
		memcpy_fromio(log->snapshot, log->mapped, BL_TOTAL);
	}
	return count;
}
static ssize_t ack_store(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count)
{
	struct bootlog *log = instance;
	char expected[80];
	char id[2][40] = {"-", "-"};
	int slot;

	guard(mutex)(&snapshot_lock);
	if (!log->sealed)
		return -EBUSY;
	if (log->cleared)
		return sysfs_streq(buf, log->ack) ? count : -ESTALE;
	for (slot = 0; slot < 2; slot++)
		if (bl_valid(snap_bank(log, slot)))
			snprintf(id[slot], sizeof(id[slot]), "%pUb", snap_bank(log, slot)->id);
	snprintf(expected, sizeof(expected), "%s %s", id[0], id[1]);
	if (!sysfs_streq(buf, expected))
		return -ESTALE;
	/* No active writer after seal. Do not erase live printk or pstore history. */
	/* Invalidate both headers before erasing any payload. Reset during erase
	 * must not leave a valid ID attached to a partially erased archive. */
	writel(0, log->mapped);
	writel(0, log->mapped + BL_BANK_SIZE);
	wmb();
	memset_io(log->mapped, 0, BL_TOTAL);
	wmb();
	strscpy(log->ack, expected, sizeof(log->ack));
	WRITE_ONCE(log->cleared, true);
	return count;
}
static ssize_t state_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	struct bootlog *log = instance;

	guard(mutex)(&snapshot_lock);
	return sysfs_emit(buf, "startup-state=%s\nactive-buffer=%u\nnext-buffer=%s\n",
		log->cleared ? "acknowledged" : log->sealed ? "sealed" : "collecting",
		log->active_slot + 1, log->cleared ? "1" : "rotate-on-reset");
}
static struct kobj_attribute state_attr = __ATTR_RO(state);
static struct kobj_attribute banks_attr = __ATTR_RO(banks);
static struct kobj_attribute seal_attr = __ATTR_WO(seal);
static struct kobj_attribute ack_attr = __ATTR_WO(ack);
static struct attribute *control_attrs[] = {&banks_attr.attr, &seal_attr.attr, &ack_attr.attr, &state_attr.attr, NULL};
static const struct attribute_group controls = {.attrs = control_attrs};

static int bootlog_probe(struct platform_device *pdev)
{
	struct device_node *np;
	struct resource res;
	struct bootlog *log;
	int ret, i, slot;

	if (instance)
		return -EBUSY;
	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!np)
		return -EINVAL;
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret || resource_size(&res) < BL_TOTAL)
		return -EINVAL;
	log = devm_kzalloc(&pdev->dev, sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;
	log->snapshot = devm_kmalloc(&pdev->dev, BL_TOTAL, GFP_KERNEL);
	if (!log->snapshot)
		return -ENOMEM;
	log->mapped = devm_ioremap(&pdev->dev, res.start, BL_TOTAL);
	if (!log->mapped)
		return -ENOMEM;
	memcpy_fromio(log->snapshot, log->mapped, BL_TOTAL);
	ret = bl_latest(snap_bank(log, 0), snap_bank(log, 1));
	if (ret < 0)
		return dev_err_probe(&pdev->dev, -ENODATA, "no valid bootlog bank\n");
	log->active_slot = ret;
	for (slot = 0; slot < 2; slot++) {
		struct bl_bank *b = snap_bank(log, slot);

		if (!bl_valid(b))
			continue;
		if (!memchr_inv(b->id, 0, 16)) {
			generate_random_uuid(b->id);
			b->crc = bl_crc(0, b, 32);
			memcpy_toio(log->mapped + slot * BL_BANK_SIZE, b, sizeof(*b));
		}
	}
	raw_spin_lock_init(&log->lock);
	log->kobj = kobject_create_and_add("bootlog", firmware_kobj);
	if (!log->kobj)
		return -ENOMEM;
	instance = log;
	/* Current stage aliases preserve the existing local collector contract. */
	for (i = 0; i < 12; i++) {
		unsigned int stage = i % BL_STAGES;
		unsigned int bank = i < 8 ? i / BL_STAGES : log->active_slot;

		if (i < 8)
			snprintf(log->attr_names[i], 32, "bank%u-%s", bank, names[stage]);
		else
			strscpy(log->attr_names[i], names[stage], 32);
		sysfs_bin_attr_init(&log->attr[i]);
		log->attr[i].attr.name = log->attr_names[i];
		log->attr[i].attr.mode = 0400;
		log->attr[i].size = bl_size[stage] - sizeof(struct bl_record);
		log->attr[i].read = log_read;
		log->attr[i].private = (void *)(unsigned long)(bank * BL_STAGES + stage);
		ret = sysfs_create_bin_file(log->kobj, &log->attr[i]);
		if (ret)
			goto fail;
	}
	ret = sysfs_create_group(log->kobj, &controls);
	if (ret)
		goto fail;
	log->console = (struct console){.name = "bootlog", .write = capture_write,
		.flags = CON_ENABLED | CON_ANYTIME | CON_PRINTBUFFER, .index = -1};
	register_console(&log->console);
	log->registered = true;
	platform_set_drvdata(pdev, log);
	return 0;
fail:
	while (--i >= 0)
		sysfs_remove_bin_file(log->kobj, &log->attr[i]);
	instance = NULL;
	kobject_put(log->kobj);
	return ret;
}
static void bootlog_remove(struct platform_device *pdev)
{
	struct bootlog *log = platform_get_drvdata(pdev);
	int i;

	if (log->registered)
		unregister_console(&log->console);
	sysfs_remove_group(log->kobj, &controls);
	for (i = 0; i < 12; i++)
		sysfs_remove_bin_file(log->kobj, &log->attr[i]);
	instance = NULL;
	kobject_put(log->kobj);
}
static const struct of_device_id bootlog_match[] = {
	{ .compatible = "bsp,bootlog-v2" }, {}
};
MODULE_DEVICE_TABLE(of, bootlog_match);
static struct platform_driver bootlog_driver = {
	.probe = bootlog_probe, .remove = bootlog_remove,
	.driver = { .name = "bsp-bootlog", .of_match_table = bootlog_match },
};
module_platform_driver(bootlog_driver);
MODULE_LICENSE("GPL");
