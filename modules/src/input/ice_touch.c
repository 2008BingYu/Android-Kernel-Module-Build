#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/device.h>
#include <linux/bitops.h>
#include <linux/mutex.h>

#include "ice_touch.h"

/*
 * lsdriver virtual_input.h 方案的 IceDriver 移植版。
 *
 * 思路: 不注册任何新输入设备, 直接劫持真实触摸屏设备的 input_dev,
 * 把其 slot 池划分为「物理槽位 + 虚拟槽位」, 虚拟手指混入真实设备
 * 事件流上报。系统设备列表零变化, getevent -pl / InputDevice 枚举
 * 均不可见。
 *
 * 与 lsdriver 原版差异:
 *  1. 设备发现: lsdriver 用 kallsyms 找 input_class; IceDriver 无
 *     kallsyms, 改为注册临时 input_handler, 借 connect 回调遍历所有
 *     已注册设备偷取指针, 零符号解析。
 *  2. 总 slot 池取设备真实 num_slots(<=10) 而非硬编码 10, 防越界。
 *  3. ioctl 层 mutex 串行化, 修并发踩踏。
 *
 * 代价(与 lsdriver 相同): 真实手指可用槽位被压缩, 10 指 TP 上
 * 虚拟占 2 槽时真机最多 8 指; 虚拟手指按住期间物理 BTN_TOUCH 被锁。
 */

#define ICE_TOUCH_MAX_VIRTUAL_SLOTS     5
#define ICE_TOUCH_DEFAULT_VIRTUAL_SLOTS 2
#define ICE_TOUCH_TRACKING_ID_BASE      40000
#define ICE_TOUCH_MAX_POOL              10

static DEFINE_MUTEX(ice_touch_lock);

static struct input_dev *g_dev;

static int g_pool_slots;          /* 真实设备总 slot 数 */
static int g_physical_slots;      /* 留给真实驱动的槽位 */
static int g_virtual_slots;       /* 虚拟槽位数 */
static int g_virtual_slot_base;   /* 虚拟槽位起始索引 */

static int  g_tracking_ids[ICE_TOUCH_MAX_VIRTUAL_SLOTS];
static bool g_has_touch_major;
static bool g_has_width_major;
static bool g_has_pressure;
static bool g_keys_locked;
static bool g_initialized;

static int g_max_x, g_max_y;

/* ==================== 设备发现: handler connect 顺路偷指针 ==================== */

static struct input_dev *g_found;

static const struct input_device_id ice_finder_ids[] = {
    { .driver_info = 1 },   /* catch-all: 匹配所有设备 */
    { }
};

static int ice_finder_connect(struct input_handler *handler,
                              struct input_dev *dev,
                              const struct input_device_id *id)
{
    /* 只拿指针不建 handle, 返回错误表示不接受, 不干扰 evdev 等已有 handler */
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

    /* register 期间 input core 已同步遍历全部已注册设备触发 connect */
    input_unregister_handler(&ice_finder_handler);

    dev = g_found;
    g_found = NULL;
    return dev;
}

/* ==================== slot 划分 ==================== */

static inline int ice_vt_active_count(void)
{
    int i, count = 0;

    for (i = 0; i < g_virtual_slots; i++)
        if (g_tracking_ids[i] != -1)
            count++;
    return count;
}

/*
 * 截断内核侧 slot 数: 真实驱动只能写到 0~(physical-1),
 * 但通过 absinfo 让 Android 看到完整池。
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

    g_physical_slots   = g_pool_slots - g_virtual_slots;
    g_virtual_slot_base = g_physical_slots;

    mt->num_slots = g_physical_slots;

    mt->flags &= ~INPUT_MT_DROP_UNUSED;  /* 未更新槽位不丢弃 */
    mt->flags |= INPUT_MT_DIRECT;
    mt->flags &= ~INPUT_MT_POINTER;      /* 禁内核自动按键计算, 防 key flapping */

    /* Android 侧仍看到完整 slot 范围, 否则虚拟手指无处安放 */
    input_set_abs_params(dev, ABS_MT_SLOT, 0, g_pool_slots - 1, 0, 0);

    return 0;
}

/* 暂时剥夺(或恢复)设备上报全局按键的能力, 拦截物理驱动的 UP 信号 */
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
    int i, count = 0;

    /* 统计物理槽位上的真实手指 */
    for (i = 0; i < g_physical_slots; i++) {
        if (input_mt_get_value(&mt->slots[i], ABS_MT_TRACKING_ID) != -1)
            count++;
    }

    count += ice_vt_active_count();

    /*
     * 锁定期间 keybit 被清空, input_event 会静默丢弃我们的按键,
     * 所以先临时恢复, 发完再清回: 只拦物理驱动, 不拦虚拟注入。
     */
    if (relock)
        ice_touch_set_key_bits(dev, true);

    /* 必须直用 input_event, 不能走 input_report_key */
    input_event(dev, EV_KEY, BTN_TOUCH, count > 0);
    input_event(dev, EV_KEY, BTN_TOOL_FINGER, count == 1);
    input_event(dev, EV_KEY, BTN_TOOL_DOUBLETAP, count >= 2);

    if (relock)
        ice_touch_set_key_bits(dev, false);
}

/* 调用者传 0 ~ virtual_slots-1 的虚拟槽位号, 内部映射到硬件坑位 */
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

    /* 记住真实驱动正在操作的坑位 */
    old_slot = mt->slot;

    /* 瞬间放开全部 slot */
    mt->num_slots = g_pool_slots;

    /* 选中虚拟坑位 */
    input_event(dev, EV_ABS, ABS_MT_SLOT, hw_slot);

    /* 上报存活/死亡状态。宣告死亡后严禁再上报任何 ABS 属性 */
    input_event(dev, EV_ABS, ABS_MT_TRACKING_ID, tracking_id);

    if (touching) {
        input_event(dev, EV_ABS, ABS_MT_POSITION_X, x);
        input_event(dev, EV_ABS, ABS_MT_POSITION_Y, y);

        /* 按设备真实能力伪造面积/压力 */
        if (g_has_touch_major)
            input_event(dev, EV_ABS, ABS_MT_TOUCH_MAJOR, 10);
        if (g_has_width_major)
            input_event(dev, EV_ABS, ABS_MT_WIDTH_MAJOR, 10);
        if (g_has_pressure)
            input_event(dev, EV_ABS, ABS_MT_PRESSURE, 60);
    }

    /*
     * 绝不调 input_mt_sync_frame: Type B 下手动精准控制每个属性,
     * sync_frame 会强制刷新真实手指的残缺帧导致抖动。
     */

    /* 把写入权还给真实驱动被打断的坑位, 解决"坐标跳跃" */
    input_event(dev, EV_ABS, ABS_MT_SLOT, old_slot);

    /* 收回 slot, 真实驱动只看到 physical 个 */
    mt->num_slots = g_physical_slots;

    /* 手动维护全局按键 */
    ice_touch_update_global_keys();

    /* 提交总帧 */
    input_event(dev, EV_SYN, SYN_REPORT, 0);

    return 0;
}

/* ==================== 初始化 / 销毁 ==================== */

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

    g_pool_slots = min(dev->mt->num_slots, ICE_TOUCH_MAX_POOL);
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

    /* 抬起所有仍按下的虚拟手指 */
    for (i = 0; i < g_virtual_slots; i++) {
        if (g_tracking_ids[i] != -1) {
            g_tracking_ids[i] = -1;
            ice_touch_send_report(i, 0, 0, false);
        }
    }

    /* 还控制权给物理驱动 */
    if (g_dev) {
        ice_touch_set_key_bits(g_dev, true);
        g_keys_locked = false;
    }

    if (g_dev && g_dev->mt) {
        g_dev->mt->num_slots = g_pool_slots;
        input_set_abs_params(g_dev, ABS_MT_SLOT, 0, g_pool_slots - 1, 0, 0);
        g_dev->mt->flags |= INPUT_MT_DROP_UNUSED;
    }

    if (g_dev) {
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

    /* 零参数 insmod 即自动劫持默认 2 个虚拟槽位 */
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
        /* 仅查询/确保初始化 */
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
        /* 写回: x=屏最大X, y=屏最大Y, y_max=实际虚拟槽位数 */
        t->x     = g_max_x;
        t->y     = g_max_y;
        t->y_max = g_virtual_slots;
    }

    mutex_unlock(&ice_touch_lock);
    return ret;
}

/* ==================== 事件入口 ==================== */

static int ice_touch_down(const TOUCH_IO *t)
{
    int vslot = t->slot;
    int ret;

    if ((unsigned)vslot >= (unsigned)g_virtual_slots)
        return -EINVAL;
    if (t->x < 0 || t->y < 0 || t->x >= g_max_x || t->y >= g_max_y)
        return -EINVAL;

    if (g_tracking_ids[vslot] != -1) {
        /* 已按下, 视作 MOVE 更新坐标 */
        return ice_touch_send_report(vslot, t->x, t->y, true);
    }

    g_tracking_ids[vslot] = ICE_TOUCH_TRACKING_ID_BASE + vslot;

    if (ice_vt_active_count() == 1) {
        /* 首个虚拟手指: 上报成功后上锁, 物理 UP 无法打断虚拟滑动 */
        ret = ice_touch_send_report(vslot, t->x, t->y, true);
        if (ret) {
            g_tracking_ids[vslot] = -1;
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
        return 0;   /* 已抬起, 重复 UP 忽略 */

    g_tracking_ids[vslot] = -1;

    ice_touch_send_report(vslot, 0, 0, false);

    /* 最后一个虚拟手指抬起, 把全局按键能力还给物理驱动 */
    if (ice_vt_active_count() == 0) {
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