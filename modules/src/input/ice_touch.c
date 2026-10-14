#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>
#include <linux/bitops.h>
#include <linux/mutex.h>

#include "ice_touch.h"


#define ICE_TOUCH_MAX_VIRTUAL_SLOTS     5
#define ICE_TOUCH_DEFAULT_VIRTUAL_SLOTS 2
#define ICE_TOUCH_TRACKING_ID_BASE      40000

static DEFINE_MUTEX(ice_touch_lock);

static struct input_dev *g_dev;

static int g_pool_slots;          /* 真实设备总 slot 数(保持原样) */
static int g_physical_slots;      /* 注入期间留给真实驱动的槽位 */
static int g_virtual_slots;       /* 虚拟槽位数 */
static int g_virtual_slot_base;   /* 虚拟槽位起始索引(= physical) */

static unsigned int g_orig_flags;   /* hijack 前 mt->flags, destroy 还原 */
static bool g_slots_truncated;      /* 当前是否处于截断态(仅注入期间) */

static int  g_tracking_ids[ICE_TOUCH_MAX_VIRTUAL_SLOTS];
static bool g_has_touch_major;
static bool g_has_width_major;
static bool g_has_pressure;
static bool g_keys_locked;
static bool g_initialized;

static int g_max_x, g_max_y;

static struct input_dev *g_found;

static const struct input_device_id ice_finder_ids[] = {
    { .driver_info = 1 },
    { }
};

static int ice_finder_connect(struct input_handler *handler,
                              struct input_dev *dev,
                              const struct input_device_id *id)
{
    if (!g_found && dev->mt &&
        test_bit(EV_ABS, dev->evbit) &&
        test_bit(ABS_MT_POSITION_X, dev->absbit) &&
        test_bit(BTN_TOUCH, dev->keybit)) {
        g_found = dev;
        get_device(&dev->dev);
    }
    return -ENODEV;
}

static struct input_handler ice_finder_handler = {
    .name     = "icedriver-touchfinder",
    .id_table = ice_finder_ids,
    .connect  = ice_finder_connect,
};

static struct input_dev *ice_touch_find_dev(void)
{
    struct input_dev *dev;

    g_found = NULL;

    if (input_register_handler(&ice_finder_handler))
        return NULL;

    input_unregister_handler(&ice_finder_handler);

    dev = g_found;
    g_found = NULL;
    return dev;
}


static inline int ice_vt_active_count(void)
{
    int i, count = 0;

    for (i = 0; i < g_virtual_slots; i++)
        if (g_tracking_ids[i] != -1)
            count++;
    return count;
}

/*
 * 懒截断方案。
 *
 * 平时: num_slots 保持真实池大小, absinfo 原样, 真实驱动满血,
 *       设备能力与未安装模块时完全一致, 设备从不注销/重注册,
 *       无热插拔, InputReader 无需重读 —— 隐蔽性与真实触摸影响
 *       双双为零。
 *
 * 注入期间: 首个虚拟手指按下时把尾部槽位收归虚拟(截断 num_slots),
 *       虚拟槽位落在 InputReader 已缓存的真实池范围内, 直接生效;
 *       全部虚拟手指抬起后立即恢复, 真实多指满血回归。
 *
 * 唯一代价窗口: 虚拟手指按住期间真实手指可用槽位 = physical。
 * 不变式: g_slots_truncated == true 当且仅当存在活跃虚拟手指
 *         (DOWN 失败路径已回滚, 不会残留截断态)。
 */
static int ice_touch_hijack_slots(struct input_dev *dev)
{
    struct input_mt *mt = dev->mt;

    if (!mt || g_pool_slots < 2)
        return -EINVAL;
    if (g_virtual_slots < 1 ||
        g_virtual_slots >= g_pool_slots ||
        g_virtual_slots > ICE_TOUCH_MAX_VIRTUAL_SLOTS)
        return -EINVAL;

    g_physical_slots    = g_pool_slots - g_virtual_slots;
    g_virtual_slot_base = g_physical_slots;
    g_slots_truncated   = false;

    /*
     * flags 是内核内部状态, 用户态不可见, 不影响隐蔽性:
     * DROP_UNUSED 清掉避免未更新槽位被释放;
     * DIRECT 置位; POINTER 清掉禁用内核自动按键计算,
     * 全局按键由我们手动维护。
     */
    g_orig_flags = mt->flags;
    mt->flags &= ~INPUT_MT_DROP_UNUSED;
    mt->flags |= INPUT_MT_DIRECT;
    mt->flags &= ~INPUT_MT_POINTER;

    return 0;
}

/* 截断/恢复槽位: 只在注入进行时截断, 平时保持真实池满血 */
static inline void ice_touch_truncate_slots(void)
{
    if (!g_slots_truncated && g_dev && g_dev->mt) {
        g_dev->mt->num_slots = g_physical_slots;
        g_slots_truncated = true;
    }
}

static inline void ice_touch_restore_slots(void)
{
    if (g_slots_truncated && g_dev && g_dev->mt) {
        g_dev->mt->num_slots = g_pool_slots;
        g_slots_truncated = false;
    }
}

static void ice_touch_set_key_bits(struct input_dev *dev, bool enable)
{
    if (enable) {
        set_bit(BTN_TOUCH, dev->keybit);
        set_bit(BTN_TOOL_FINGER, dev->keybit);
        set_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
    } else {
        clear_bit(BTN_TOUCH, dev->keybit);
        clear_bit(BTN_TOOL_FINGER, dev->keybit);
        clear_bit(BTN_TOOL_DOUBLETAP, dev->keybit);
    }
}

static void ice_touch_update_global_keys(void)
{
    struct input_dev *dev = g_dev;
    struct input_mt *mt = dev->mt;
    bool relock = g_keys_locked;
    int phys, i, count = 0;

    /* 截断态只统计真实槽位; 未截断(首次DOWN瞬间)统计完整池 */
    phys = g_slots_truncated ? g_physical_slots : g_pool_slots;

    for (i = 0; i < phys; i++) {
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
            count++;
    }

    count += ice_vt_active_count();

    if (relock)
        ice_touch_set_key_bits(dev, true);

    input_event(dev, EV_KEY, BTN_TOUCH, count > 0);
    input_event(dev, EV_KEY, BTN_TOOL_FINGER, count == 1);
    input_event(dev, EV_KEY, BTN_TOOL_DOUBLETAP, count >= 2);

    if (relock)
        ice_touch_set_key_bits(dev, false);
}

static int ice_touch_send_report(int vslot, int x, int y, bool touching)
{
    struct input_dev *dev = g_dev;
    struct input_mt *mt;
    int hw_slot, tracking_id, old_slot;

    if (!dev)
        return -ENODEV;

    mt = dev->mt;
    if (!mt)
        return -ENODEV;

    if ((unsigned)vslot >= (unsigned)g_virtual_slots)
        return -EINVAL;

    if (touching && g_tracking_ids[vslot] == -1)
        return -EINVAL;

    hw_slot     = g_virtual_slot_base + vslot;
    tracking_id = touching ? g_tracking_ids[vslot] : -1;

    old_slot = mt->slot;

    /* 虚拟槽在池内, 但截断态会被 input core 拒收;
     * 发送前临时放开 num_slots, 发完按截断态收回 */
    mt->num_slots = g_pool_slots;

    input_event(dev, EV_ABS, ABS_MT_SLOT, hw_slot);

    input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, tracking_id);

    if (touching) {
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, x);
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, y);

        if (g_has_touch_major)
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
        if (g_has_width_major)
            input_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, 10);
        if (g_has_pressure)
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, 60);
    }

    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    /* 归还真实驱动坑位后, 按截断态收回尾部槽位 */
    if (g_slots_truncated)
        mt->num_slots = g_physical_slots;

    ice_touch_update_global_keys();

    input_event(dev, EV_SYN, SYN_REPORT, 0);

    return 0;
}

static int ice_touch_hijack_locked(int virtual_slots)
{
    struct input_dev *dev;
    int ret;

    g_dev = NULL;

    dev = ice_touch_find_dev();
    if (!dev) {
        pr_err("[IceDriver] touch: no touchscreen found\n");
        return -ENODEV;
    }

    g_pool_slots = dev->mt->num_slots;
    if (g_pool_slots < 2) {
        put_device(&dev->dev);
        return -EINVAL;
    }

    g_virtual_slots = virtual_slots;

    ret = ice_touch_hijack_slots(dev);
    if (ret) {
        put_device(&dev->dev);
        return ret;
    }

    g_dev = dev;
    g_keys_locked = false;

    g_has_touch_major = test_bit(ABS_MT_TOUCH_MAJOR, dev->absbit);
    g_has_width_major = test_bit(ABS_MT_WIDTH_MAJOR, dev->absbit);
    g_has_pressure    = test_bit(ABS_MT_PRESSURE, dev->absbit);

    g_max_x = dev->absinfo[ABS_MT_POSITION_X].maximum;
    g_max_y = dev->absinfo[ABS_MT_POSITION_Y].maximum;

    g_initialized = true;

    pr_info("[IceDriver] touch hijack ready: pool=%d physical=%d virtual=%d range=%dx%d\n",
            g_pool_slots, g_physical_slots, g_virtual_slots, g_max_x, g_max_y);
    return 0;
}

static void ice_touch_destroy_locked(void)
{
    int i;

    if (!g_initialized)
        return;

    for (i = 0; i < g_virtual_slots; i++) {
        if (g_tracking_ids[i] != -1) {
            g_tracking_ids[i] = -1;
            ice_touch_send_report(i, 0, 0, false);
        }
    }

    /* 截断态还原: num_slots 回到真实池 */
    ice_touch_restore_slots();

    if (g_dev) {
        /* 还原 hijack 时改过的 mt->flags */
        if (g_dev->mt)
            g_dev->mt->flags = g_orig_flags;
        ice_touch_set_key_bits(g_dev, true);
        g_keys_locked = false;
        /* 懒截断版从不注销设备, 只归还发现时持的引用 */
        put_device(&g_dev->dev);
        g_dev = NULL;
    }

    g_initialized = false;

    for (i = 0; i < ICE_TOUCH_MAX_VIRTUAL_SLOTS; i++)
        g_tracking_ids[i] = -1;
}

int ice_touch_init(void)
{
    int ret;

    mutex_lock(&ice_touch_lock);
    ret = ice_touch_hijack_locked(ICE_TOUCH_DEFAULT_VIRTUAL_SLOTS);
    mutex_unlock(&ice_touch_lock);

    if (ret)
        pr_warn("[IceDriver] touch init failed: %d\n", ret);
    return ret;
}

void ice_touch_exit(void)
{
    mutex_lock(&ice_touch_lock);
    ice_touch_destroy_locked();
    mutex_unlock(&ice_touch_lock);
}

int ice_touch_config(TOUCH_IO *t)
{
    int ret = 0;

    if (!t)
        return -EINVAL;

    mutex_lock(&ice_touch_lock);

    if (t->x_max == 0) {
        if (!g_initialized)
            ret = ice_touch_hijack_locked(ICE_TOUCH_DEFAULT_VIRTUAL_SLOTS);
    } else {
        if (t->x_max < 1 || t->x_max > ICE_TOUCH_MAX_VIRTUAL_SLOTS) {
            ret = -EINVAL;
            goto out;
        }

        ice_touch_destroy_locked();
        ret = ice_touch_hijack_locked(t->x_max);
    }

out:
    if (ret == 0) {
        t->x     = g_max_x;
        t->y     = g_max_y;
        t->y_max = g_virtual_slots;
    }

    mutex_unlock(&ice_touch_lock);
    return ret;
}


static int ice_touch_down(const TOUCH_IO *t)
{
    int vslot = t->slot;
    int ret;

    if ((unsigned)vslot >= (unsigned)g_virtual_slots)
        return -EINVAL;
    if (t->x < 0 || t->y < 0 || t->x >= g_max_x || t->y >= g_max_y)
        return -EINVAL;

    if (g_tracking_ids[vslot] != -1) {
        return ice_touch_send_report(vslot, t->x, t->y, true);
    }

    g_tracking_ids[vslot] = ICE_TOUCH_TRACKING_ID_BASE + vslot;

    if (ice_vt_active_count() == 1) {
        /* 首个虚拟手指: 先截断尾部槽位, 虚拟坑位生效 */
        ice_touch_truncate_slots();

        ret = ice_touch_send_report(vslot, t->x, t->y, true);
        if (ret) {
            g_tracking_ids[vslot] = -1;
            ice_touch_restore_slots();
            return ret;
        }
        ice_touch_set_key_bits(g_dev, false);
        g_keys_locked = true;
    } else {
        ret = ice_touch_send_report(vslot, t->x, t->y, true);
        if (ret)
            g_tracking_ids[vslot] = -1;
    }

    return ret;
}

static int ice_touch_move(const TOUCH_IO *t)
{
    int vslot = t->slot;

    if ((unsigned)vslot >= (unsigned)g_virtual_slots)
        return -EINVAL;
    if (g_tracking_ids[vslot] == -1)
        return -EINVAL;
    if (t->x < 0 || t->y < 0 || t->x >= g_max_x || t->y >= g_max_y)
        return -EINVAL;

    return ice_touch_send_report(vslot, t->x, t->y, true);
}

static int ice_touch_up(const TOUCH_IO *t)
{
    int vslot = t->slot;

    if ((unsigned)vslot >= (unsigned)g_virtual_slots)
        return -EINVAL;
    if (g_tracking_ids[vslot] == -1)
        return 0;

    g_tracking_ids[vslot] = -1;

    ice_touch_send_report(vslot, 0, 0, false);

    if (ice_vt_active_count() == 0) {
        /* 全部虚拟手指抬起: 槽位满血回归 */
        ice_touch_restore_slots();
        ice_touch_set_key_bits(g_dev, true);
        g_keys_locked = false;
    }

    return 0;
}

static int ice_touch_cancel(void)
{
    int i;

    for (i = 0; i < g_virtual_slots; i++) {
        if (g_tracking_ids[i] == -1)
            continue;
        g_tracking_ids[i] = -1;
        ice_touch_send_report(i, 0, 0, false);
    }

    if (g_keys_locked) {
        ice_touch_restore_slots();
        ice_touch_set_key_bits(g_dev, true);
        g_keys_locked = false;
    }

    return 0;
}

int ice_touch_event(const TOUCH_IO *t)
{
    int ret;

    if (!t)
        return -EINVAL;

    mutex_lock(&ice_touch_lock);

    if (!g_initialized || !g_dev) {
        mutex_unlock(&ice_touch_lock);
        return -ENODEV;
    }

    switch (t->action) {
    case TOUCH_ACTION_DOWN:   ret = ice_touch_down(t);   break;
    case TOUCH_ACTION_MOVE:   ret = ice_touch_move(t);   break;
    case TOUCH_ACTION_UP:     ret = ice_touch_up(t);     break;
    case TOUCH_ACTION_CANCEL: ret = ice_touch_cancel();  break;
    default:                  ret = -EINVAL;             break;
    }

    mutex_unlock(&ice_touch_lock);
    return ret;
}
