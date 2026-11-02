// SPDX-License-Identifier: GPL-2.0-only
/* Import an immutable snapshot; never expose arbitrary physical memory. */
#include <linux/crc32.h>
#include <linux/io.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#define STRIDE 16384U
#define MAGIC 0x31474f4cU
struct record {
	__le32 magic, version, stage, length, dropped, crc, reserved[2];
	u8 data[];
};
struct bootlog {
	struct kobject *kobj;
	struct bin_attribute attr[3];
	u8 *snapshot;
};
static ssize_t log_read(struct file *file, struct kobject *kobj,
		       const struct bin_attribute *attr, char *buf,
		       loff_t off, size_t count)
{
	return memory_read_from_buffer(buf, count, &off, attr->private, attr->size);
}
static int bootlog_probe(struct platform_device *pdev)
{
	static const char * const names[] = { "r5-spl", "a53-spl", "uboot" };
	struct device_node *np;
	struct resource res;
	struct bootlog *log;
	void __iomem *mapped;
	int ret, i;

	np = of_parse_phandle(pdev->dev.of_node, "memory-region", 0);
	if (!np)
		return -EINVAL;
	ret = of_address_to_resource(np, 0, &res);
	of_node_put(np);
	if (ret || resource_size(&res) < 3 * STRIDE)
		return -EINVAL;
	log = devm_kzalloc(&pdev->dev, sizeof(*log), GFP_KERNEL);
	if (!log)
		return -ENOMEM;
	log->snapshot = devm_kmalloc(&pdev->dev, 3 * STRIDE, GFP_KERNEL);
	if (!log->snapshot)
		return -ENOMEM;
	mapped = ioremap(res.start, 3 * STRIDE);
	if (!mapped)
		return -ENOMEM;
	memcpy_fromio(log->snapshot, mapped, 3 * STRIDE);
	iounmap(mapped);
	log->kobj = kobject_create_and_add("bootlog", firmware_kobj);
	if (!log->kobj)
		return -ENOMEM;
	for (i = 0; i < 3; i++) {
		struct record *r = (void *)(log->snapshot + i * STRIDE);
		u32 len = le32_to_cpu(r->length);

		if (le32_to_cpu(r->magic) != MAGIC || le32_to_cpu(r->version) != 1 ||
		    le32_to_cpu(r->stage) != i + 1 || len > STRIDE - sizeof(*r) ||
		    le32_to_cpu(r->crc) != (crc32_le(~0U, r->data, len) ^ ~0U))
			continue;
		sysfs_bin_attr_init(&log->attr[i]);
		log->attr[i].attr.name = names[i];
		log->attr[i].attr.mode = 0400;
		log->attr[i].size = len;
		log->attr[i].read = log_read;
		log->attr[i].private = r->data;
		ret = sysfs_create_bin_file(log->kobj, &log->attr[i]);
		if (ret)
			goto fail;
		dev_info(&pdev->dev, "%s: %u bytes, %u dropped\n", names[i], len,
			 le32_to_cpu(r->dropped));
	}
	platform_set_drvdata(pdev, log);
	return 0;
fail:
	while (--i >= 0)
		if (log->attr[i].attr.name)
			sysfs_remove_bin_file(log->kobj, &log->attr[i]);
	kobject_put(log->kobj);
	return ret;
}
static void bootlog_remove(struct platform_device *pdev)
{
	struct bootlog *log = platform_get_drvdata(pdev);
	int i;

	for (i = 0; i < 3; i++)
		if (log->attr[i].attr.name)
			sysfs_remove_bin_file(log->kobj, &log->attr[i]);
	kobject_put(log->kobj);
}
static const struct of_device_id bootlog_match[] = {
	{ .compatible = "bsp,bootlog-v1" }, {}
};
MODULE_DEVICE_TABLE(of, bootlog_match);
static struct platform_driver bootlog_driver = {
	.probe = bootlog_probe,
	.remove = bootlog_remove,
	.driver = { .name = "bsp-bootlog", .of_match_table = bootlog_match },
};
module_platform_driver(bootlog_driver);
MODULE_LICENSE("GPL");
