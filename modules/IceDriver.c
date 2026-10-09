#include <linux/module.h>
#include <linux/kernel.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ice");
MODULE_DESCRIPTION("IceDriver minimal test");

static int __init icedriver_init(void)
{
    pr_info("[IceDriver] minimal test: loaded\n");
    return 0;
}

static void __exit icedriver_exit(void)
{
    pr_info("[IceDriver] minimal test: unloaded\n");
}

module_init(icedriver_init);
module_exit(icedriver_exit);
