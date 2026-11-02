// SPDX-License-Identifier: GPL-2.0
#include <linux/init.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/string.h>

static int __init boot_identity_init(void)
{
    const char *tag, *id;
    u32 deployment;
    if (!of_chosen || of_property_read_string(of_chosen, "fw,tag", &tag) ||
        of_property_read_string(of_chosen, "fw,id", &id) ||
        of_property_read_u32(of_chosen, "fw,deployment", &deployment)) {
        pr_info("[firmware] [INFO] stage=kernel identity=unknown (bootloader supplied no complete identity)\n");
        return 0;
    }
    if (strnlen(tag, 32) >= 32 || tag[0] != 'v' ||
        strspn(tag + 1, "0123456789.") != strlen(tag + 1) ||
        strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64 || deployment > 1) {
        pr_warn("[firmware] [WARN] stage=kernel invalid boot identity\n");
        return 0;
    }
    pr_info("[firmware] [INFO] stage=kernel tag=%s id=%.8s deployment=%u\n", tag, id, deployment);
    return 0;
}
subsys_initcall(boot_identity_init);
