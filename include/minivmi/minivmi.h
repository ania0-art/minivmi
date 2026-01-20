#ifndef MINIVMI_MINIVMI_H
#define MINIVMI_MINIVMI_H

/*
 * minivmi：一个极小的 Xen/HVM/CR3-only VMI 小库（学习与实践用）。
 *
 * 开发目标（按“从零到一”的顺序）：
 *  1) 先能列出域（domid/name/uuid）——验证 XenStore + libxc 基础链路
 *  2) 再能 attach 到某个 guest ——验证目标域可用（HVM/未 dying/未 shutdown）
 *  3) 最后跑通 vm_event ——开启 CR3 监控、收事件、回调、写回响应并放行 guest
 *
 * 设计原则：
 *  - 只实现 demo 所需最小能力：域枚举 + CR3 写入事件（vm_event）。
 *
 * 运行提示：大多数操作需要在 dom0 以 root 运行。
 */

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MINIVMI_ERR_MAX   256
#define MINIVMI_UUID_MAX  64
#define MINIVMI_NAME_MAX  128

struct minivmi_domain {
    uint32_t domid;
    uint32_t xen_flags; /* XEN_DOMINF_* bitmask (from xen/domctl.h) */
    char     uuid[MINIVMI_UUID_MAX]; /* "" if unavailable */
    char     name[MINIVMI_NAME_MAX]; /* "" if unavailable */
};

/*
 * 第1步：枚举域列表
 * - 通过 libxc 获取域列表（domid + flags）
 * - 通过 xenstore 读取 name/uuid
 * 返回数组由调用方释放（minivmi_domains_free）。
 */
int  minivmi_domains_snapshot(struct minivmi_domain **out_domains,
                              size_t *out_count,
                              char *err, size_t err_len);
void minivmi_domains_free(struct minivmi_domain *domains);

/* 工具函数：通过 UUID 找 domid（UUID 是 xenstore /local/domain/<id>/vm 里的 "/vm/<uuid>" 的 <uuid> 部分）。 */
int minivmi_find_domid_by_uuid(uint32_t *out_domid,
                               const char *uuid,
                               char *err, size_t err_len);

/* 第3步：vm_event 回调里对外暴露的最小 CR3 事件结构。 */
struct minivmi_cr3_event {
    uint32_t domid;
    char     uuid[MINIVMI_UUID_MAX];
    uint16_t vcpu;

    uint64_t old_cr3;
    uint64_t new_cr3;

    uint64_t rip; /* 事件触发点的 RIP（vm_event 提供的寄存器快照） */
};

typedef void (*minivmi_cr3_cb)(const struct minivmi_cr3_event *ev, void *user);

struct minivmi_cr3_monitor;

/*
 * 第2步：打开一个“监控会话”（attach + 建立 vm_event 通道）
 * - domid：目标域 ID
 * - uuid_hint：可选，仅用于输出更友好的日志
 */
struct minivmi_cr3_monitor *minivmi_cr3_monitor_open(uint32_t domid,
                                                     const char *uuid_hint,
                                                     char *err, size_t err_len);

/*
 * 开启 CR3 写入监控（Xen 的 monitor_write_ctrlreg）。
 * 注意：必须先 open，再 enable，最后才 loop。
 */
int  minivmi_cr3_monitor_enable(struct minivmi_cr3_monitor *m,
                                char *err, size_t err_len);

/*
 * 第3步：事件循环（真正的 VMI 监控闭环）
 * - poll 等待 evtchn fd
 * - 从共享 ring 读 vm_event request
 * - 对 CR3 写入事件调用 cb
 * - 写回 response 并 notify Xen 放行 guest
 */
int  minivmi_cr3_monitor_loop(struct minivmi_cr3_monitor *m,
                              minivmi_cr3_cb cb,
                              void *user,
                              volatile sig_atomic_t *stop_flag,
                              char *err, size_t err_len);

void minivmi_cr3_monitor_close(struct minivmi_cr3_monitor *m);

#ifdef __cplusplus
}
#endif

#endif
