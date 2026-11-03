// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/string.h>

static int __init boot_identity_init(void)
{
    const char *tag = "unknown", *id = "unknown", *root_sha = "unknown";
    struct device_node *verity;
    u32 deployment, kernel_slot, rootfs_slot, kernel_version, rootfs_version;

    if (!of_chosen ||
        of_property_read_u32(of_chosen, "fw,deployment", &deployment) ||
        of_property_read_u32(of_chosen, "fw,kernel-slot", &kernel_slot) ||
        of_property_read_u32(of_chosen, "fw,rootfs-slot", &rootfs_slot) ||
        of_property_read_u32(of_chosen, "fw,kernel-version", &kernel_version) ||
        of_property_read_u32(of_chosen, "fw,rootfs-version", &rootfs_version) ||
        deployment > 1 || kernel_slot > 1 || rootfs_slot > 1) {
        pr_info("[fwctl] [INFO] stage=kernel firmware slots unavailable from U-Boot\n");
        return 0;
    }
    if (of_property_read_string(of_chosen, "fw,tag", &tag) ||
        of_property_read_string(of_chosen, "fw,id", &id)) {
        tag = "unknown";
        id = "unknown";
    } else if (strnlen(tag, 32) >= 32 || tag[0] != 'v' ||
               strspn(tag + 1, "0123456789.") != strlen(tag + 1) ||
               strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64) {
        pr_warn("[fwctl] [WARN] stage=kernel invalid firmware identity from U-Boot\n");
        tag = "unknown";
        id = "unknown";
    }
    verity = of_find_node_by_path("/chosen/fw-verity");
    if (verity && (!of_property_read_string(verity, "payload-sha256", &root_sha)) &&
        (strlen(root_sha) != 64 || strspn(root_sha, "0123456789abcdef") != 64))
        root_sha = "unknown";
    pr_info("[fwctl] [INFO] stage=kernel kernel FIT verified by U-Boot: "
            "slot=%c version=v%u.%u.%u release_id=%.8s deployment=%u\n",
            'A' + kernel_slot, kernel_version >> 16,
            (kernel_version >> 8) & 255, kernel_version & 255,
            id, deployment);
    pr_info("[fwctl] [INFO] stage=kernel rootfs selected from FIT: "
            "slot=%c version=v%u.%u.%u release_id=%.8s "
            "expected_sha256=%.8s payload_verification=pending-initramfs\n",
            'A' + rootfs_slot, rootfs_version >> 16,
            (rootfs_version >> 8) & 255, rootfs_version & 255,
            id, root_sha);
    of_node_put(verity);
    return 0;
}
subsys_initcall(boot_identity_init);
