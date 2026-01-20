#define _GNU_SOURCE

#include "minivmi/minivmi.h"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/*
 * 开发记录（核心思路）：
 * - minivmi 的目标不是用最少的代码跑通 VMI 的闭环。
 * - 我们直接调用 Xen 的公开 C API（libxc/xenstore/xenevtchn），不引入额外封装层：
 *   这样更利于学习“每一步到底发生了什么”。
 */
#include <xenctrl.h>
#include <xenstore.h>
#include <xenevtchn.h>

#include <xen/domctl.h>
#include <xen/io/ring.h>
#include <xen/vm_event.h>

/*
 * 内部会话状态（只做 CR3 监控所需的最小集合）：
 * - 一个 domain（domid/uuid）
 * - 一套 vm_event 共享 ring（来自 xc_monitor_enable）
 * - 一条 event channel（evtchn），用于 Xen 通知“ring 里有新事件”
 *
 * 注意（踩坑提示）：
 * - 同一个 domain 通常只能被一个 monitor 连接；否则 xc_monitor_enable() 可能 EBUSY。
 */
struct minivmi_cr3_monitor {
    uint32_t domid;
    char     uuid[MINIVMI_UUID_MAX];

    xc_interface     *xch;
    xenevtchn_handle *xce;
    int               evtchn_fd;

    void   *ring_page;
    unsigned long ring_page_len; /* vm_event ring 固定是一页；这里用 unsigned long 贴合 ring 宏 */

    evtchn_port_t remote_port; /* returned by xc_monitor_enable */
    evtchn_port_t local_port;  /* returned by xenevtchn_bind_interdomain */

    vm_event_back_ring_t back_ring;

    bool monitor_enabled;
    bool cr3_enabled;
};

static void set_err(char *err, size_t err_len, const char *fmt, ...)
{
    if (!err || err_len == 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_len, fmt, ap);
    va_end(ap);
}

static void safe_copy(char *dst, size_t dst_sz, const char *src, size_t src_len)
{
    if (!dst || dst_sz == 0) return;
    size_t n = src_len;
    if (!src) n = 0;
    if (n >= dst_sz) n = dst_sz - 1;
    if (n) memcpy(dst, src, n);
    dst[n] = '\0';
}

/*
 * 第1步（域信息）：从 xenstore 读取一个 key，返回 malloc 出来的 NUL 结尾字符串。
 * - xenstore 的 xs_read 返回的是一段 raw buffer（需要 free）
 * - 这里把它复制成 C 字符串，便于后续处理与打印
 */
static char *xs_read_strdup(struct xs_handle *xs, const char *path)
{
    unsigned int len = 0;
    void *raw = xs_read(xs, XBT_NULL, path, &len);
    if (!raw) return NULL;

    char *s = (char *)malloc((size_t)len + 1);
    if (!s) {
        free(raw);
        return NULL;
    }
    memcpy(s, raw, (size_t)len);
    s[len] = '\0';
    free(raw);
    return s;
}

void minivmi_domains_free(struct minivmi_domain *domains)
{
    free(domains);
}

int minivmi_domains_snapshot(struct minivmi_domain **out_domains,
                             size_t *out_count,
                             char *err, size_t err_len)
{
    if (!out_domains || !out_count) {
        set_err(err, err_len, "bad args");
        return -1;
    }
    *out_domains = NULL;
    *out_count = 0;

    xc_interface *xch = xc_interface_open(NULL, NULL, 0);
    if (!xch) {
        set_err(err, err_len, "xc_interface_open failed: %s", strerror(errno));
        return -1;
    }

    struct xs_handle *xs = xs_open(XS_OPEN_READONLY);
    if (!xs) {
        set_err(err, err_len, "xs_open failed: %s", strerror(errno));
        xc_interface_close(xch);
        return -1;
    }

    /*
     * 第1步（域枚举）：通过一次 libxc hypercall 获取域列表。
     * - 这里不做复杂分页/动态扩容，先设一个上限 cap，够学习与 demo 使用
     */
    const unsigned int cap = 1024;
    xc_domaininfo_t *infos = (xc_domaininfo_t *)calloc(cap, sizeof(*infos));
    if (!infos) {
        set_err(err, err_len, "oom");
        xs_close(xs);
        xc_interface_close(xch);
        return -1;
    }

    const int n = xc_domain_getinfolist(xch, 0, cap, infos);
    if (n < 0) {
        set_err(err, err_len, "xc_domain_getinfolist failed: %s", strerror(errno));
        free(infos);
        xs_close(xs);
        xc_interface_close(xch);
        return -1;
    }

    struct minivmi_domain *domains = (struct minivmi_domain *)calloc((size_t)n, sizeof(*domains));
    if (!domains) {
        set_err(err, err_len, "oom");
        free(infos);
        xs_close(xs);
        xc_interface_close(xch);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        const uint32_t domid = (uint32_t)infos[i].domain;
        domains[i].domid = domid;
        domains[i].xen_flags = infos[i].flags;

        /*
         * 第1步（补齐 name/uuid）：xenstore 的常见路径约定：
         * - /local/domain/<domid>/name  -> 可读名字（如 "ubuntu-guest"）
         * - /local/domain/<domid>/vm    -> "/vm/<uuid>"（这里的 uuid 是我们更想要的）
         */
        char path[256];

        snprintf(path, sizeof(path), "/local/domain/%u/name", domid);
        char *name = xs_read_strdup(xs, path);
        if (name) {
            safe_copy(domains[i].name, sizeof(domains[i].name), name, strlen(name));
            free(name);
        }

        snprintf(path, sizeof(path), "/local/domain/%u/vm", domid);
        char *vm = xs_read_strdup(xs, path);
        if (vm) {
            const char *u = vm;
            if (strncmp(vm, "/vm/", 4) == 0) u = vm + 4;
            safe_copy(domains[i].uuid, sizeof(domains[i].uuid), u, strlen(u));
            free(vm);
        }
    }

    free(infos);
    xs_close(xs);
    xc_interface_close(xch);

    *out_domains = domains;
    *out_count = (size_t)n;
    return 0;
}

int minivmi_find_domid_by_uuid(uint32_t *out_domid,
                               const char *uuid,
                               char *err, size_t err_len)
{
    if (!out_domid || !uuid || uuid[0] == '\0') {
        set_err(err, err_len, "bad args");
        return -1;
    }

    struct minivmi_domain *domains = NULL;
    size_t count = 0;
    if (minivmi_domains_snapshot(&domains, &count, err, err_len) != 0) return -1;

    for (size_t i = 0; i < count; i++) {
        if (domains[i].uuid[0] && strcmp(domains[i].uuid, uuid) == 0) {
            *out_domid = domains[i].domid;
            minivmi_domains_free(domains);
            return 0;
        }
    }

    minivmi_domains_free(domains);
    set_err(err, err_len, "uuid not found in xenstore: %s", uuid);
    return -1;
}

static int ensure_hvm_domain(xc_interface *xch, uint32_t domid, char *err, size_t err_len)
{
    /*
     * 第2步（attach 前检查）：这里用 xc_dominfo_t（xenctrl.h 的工具侧结构），
     * 它把常用状态（hvm/dying/shutdown）做成 bit 字段，读取更直观。
     */
    xc_dominfo_t info;
    memset(&info, 0, sizeof(info));

    const int n = xc_domain_getinfo(xch, domid, 1, &info);
    if (n != 1 || info.domid != domid) {
        set_err(err, err_len, "xc_domain_getinfo failed for domid=%u: %s", domid, strerror(errno));
        return -1;
    }

    if (!info.hvm) {
        set_err(err, err_len, "domid=%u is not HVM", domid);
        return -1;
    }
    if (info.dying) {
        set_err(err, err_len, "domid=%u is dying", domid);
        return -1;
    }
    if (info.shutdown) {
        set_err(err, err_len, "domid=%u is shutdown", domid);
        return -1;
    }

    return 0;
}

static void ring_init_back(struct minivmi_cr3_monitor *m)
{
    /*
     * 第2步（打通 vm_event 基础设施）：初始化共享 ring。
     * - vm_event 的事件数据通过“一页共享内存 + ring 协议”在 Xen 与 dom0 用户态之间传递
     * - ring 的通用宏来自 xen/io/ring.h
     * - vm_event 的 ring 类型来自 xen/vm_event.h（DEFINE_RING_TYPES）
     *
     * 这里做两件事：
     *  1) 清零并初始化 shared ring（SHARED_RING_INIT）
     *  2) 初始化我们在 dom0 侧的 back_ring 视图（BACK_RING_INIT）
     */
    vm_event_sring_t *sring = (vm_event_sring_t *)m->ring_page;
    memset(sring, 0, m->ring_page_len);
    SHARED_RING_INIT(sring);
    BACK_RING_INIT(&m->back_ring, sring, m->ring_page_len);
}

struct minivmi_cr3_monitor *minivmi_cr3_monitor_open(uint32_t domid,
                                                     const char *uuid_hint,
                                                     char *err, size_t err_len)
{
    /*
     * 第2步（attach）：建立一条“监控会话”。
     * - 验证目标域是 HVM 且存活
     * - 开启 vm_event（xc_monitor_enable）：拿到共享 ring 页 + 一个 remote evtchn port
     * - 建立 event channel（bind interdomain）：拿到本地 port + fd，用于 poll 等待事件
     */
    struct minivmi_cr3_monitor *m = (struct minivmi_cr3_monitor *)calloc(1, sizeof(*m));
    if (!m) {
        set_err(err, err_len, "oom");
        return NULL;
    }

    m->domid = domid;
    if (uuid_hint && uuid_hint[0]) {
        safe_copy(m->uuid, sizeof(m->uuid), uuid_hint, strlen(uuid_hint));
    }

    m->ring_page_len = (unsigned long)getpagesize();

    m->xch = xc_interface_open(NULL, NULL, 0);
    if (!m->xch) {
        set_err(err, err_len, "xc_interface_open failed: %s", strerror(errno));
        minivmi_cr3_monitor_close(m);
        return NULL;
    }

    if (ensure_hvm_domain(m->xch, domid, err, err_len) != 0) {
        minivmi_cr3_monitor_close(m);
        return NULL;
    }

    /*
     * 第2步（关键 hypercall）：开启 vm_event。
     * - 返回值：一页 mmap 到本进程的共享内存（ring）
     * - out 参数：remote_port（给 xenevtchn_bind_interdomain 用）
     */
    m->ring_page = xc_monitor_enable(m->xch, domid, &m->remote_port);
    if (!m->ring_page) {
        set_err(err, err_len, "xc_monitor_enable failed for domid=%u: %s", domid, strerror(errno));
        minivmi_cr3_monitor_close(m);
        return NULL;
    }
    m->monitor_enabled = true;

    /*
     * 第2步（事件通道）：绑定 interdomain evtchn。
     * - vm_event ring 里有事件时，Xen 会通过 evtchn 唤醒 dom0 用户态
     * - 我们用 poll(fd) 等待它变为可读
     */
    m->xce = xenevtchn_open(NULL, 0);
    if (!m->xce) {
        set_err(err, err_len, "xenevtchn_open failed: %s", strerror(errno));
        minivmi_cr3_monitor_close(m);
        return NULL;
    }

    xenevtchn_port_or_error_t p = xenevtchn_bind_interdomain(m->xce, domid, m->remote_port);
    if (p < 0) {
        set_err(err, err_len, "xenevtchn_bind_interdomain failed: %s", strerror(errno));
        minivmi_cr3_monitor_close(m);
        return NULL;
    }
    m->local_port = (evtchn_port_t)p;

    m->evtchn_fd = xenevtchn_fd(m->xce);
    if (m->evtchn_fd < 0) {
        set_err(err, err_len, "xenevtchn_fd failed: %s", strerror(errno));
        minivmi_cr3_monitor_close(m);
        return NULL;
    }

    ring_init_back(m);
    return m;
}

int minivmi_cr3_monitor_enable(struct minivmi_cr3_monitor *m,
                               char *err, size_t err_len)
{
    if (!m) {
        set_err(err, err_len, "bad args");
        return -1;
    }

    /*
     * 第3步（配置拦截点）：让 Xen 在“写 CR3”时产生 vm_event 事件。
     * - sync=true：同步拦截（guest 在事件点暂停，直到我们写回 response）
     * - onchangeonly=true：只在 CR3 真变化时触发，减少噪声
     */
    const int rc = xc_monitor_write_ctrlreg(m->xch, m->domid,
                                           VM_EVENT_X86_CR3,
                                           true,  /* enable */
                                           true,  /* sync */
                                           0,     /* bitmask */
                                           true); /* onchangeonly */
    if (rc != 0) {
        set_err(err, err_len, "xc_monitor_write_ctrlreg(CR3) failed: %s", strerror(errno));
        return -1;
    }

    m->cr3_enabled = true;
    return 0;
}

/*
 * 第3步（读 ring）：从共享 ring 取出一个 request。
 * 返回：
 * - 1：读到了一个 request
 * - 0：ring 为空
 */
static int ring_pop_req(vm_event_back_ring_t *br, vm_event_request_t *out)
{
    if (!RING_HAS_UNCONSUMED_REQUESTS(br)) return 0;

    const RING_IDX cons = br->req_cons;
    memcpy(out, RING_GET_REQUEST(br, cons), sizeof(*out));

    br->req_cons = cons + 1;
    br->sring->req_event = br->req_cons + 1;
    return 1;
}

static void ring_put_rsp(vm_event_back_ring_t *br, const vm_event_response_t *rsp)
{
    const RING_IDX prod = br->rsp_prod_pvt;
    memcpy(RING_GET_RESPONSE(br, prod), rsp, sizeof(*rsp));
    br->rsp_prod_pvt = prod + 1;
}

int minivmi_cr3_monitor_loop(struct minivmi_cr3_monitor *m,
                             minivmi_cr3_cb cb,
                             void *user,
                             volatile sig_atomic_t *stop_flag,
                             char *err, size_t err_len)
{
    if (!m || !cb || !stop_flag) {
        set_err(err, err_len, "bad args");
        return -1;
    }

    struct pollfd pfd;
    pfd.fd = m->evtchn_fd;
    pfd.events = POLLIN | POLLERR;

    while (!(*stop_flag)) {
        pfd.revents = 0;
        const int prc = poll(&pfd, 1, 200);
        if (prc < 0) {
            if (errno == EINTR) continue;
            set_err(err, err_len, "poll(evtchn) failed: %s", strerror(errno));
            return -1;
        }
        if (prc == 0) continue;

        /*
         * 第3步（等事件 + 消费通知）：
         * - poll(fd) 告诉我们“有 evtchn 通知到了”
         * - xenevtchn_pending() 取出哪个 port 触发，并进入 masked 状态
         * - 我们处理完 ring 后，必须 xenevtchn_unmask() 才能继续收下一次通知
         *
         * 小坑：按 xenevtchn.h 的建议，先 poll 再 pending。
         */
        const xenevtchn_port_or_error_t pend = xenevtchn_pending(m->xce);
        if (pend < 0) {
            set_err(err, err_len, "xenevtchn_pending failed: %s", strerror(errno));
            return -1;
        }

        int handled = 0;
        vm_event_request_t req;

        while (ring_pop_req(&m->back_ring, &req)) {
            /*
             * 第3步（写回 response）：默认做法是“原样回显”。
             * - 对 minivmi 这个最小 demo 来说：不改寄存器/不注入动作
             * - 只要写回 response 并 notify，Xen 就会放行 guest 继续执行
             */
            vm_event_response_t rsp = req;

            if (req.reason == VM_EVENT_REASON_WRITE_CTRLREG &&
                req.u.write_ctrlreg.index == VM_EVENT_X86_CR3) {

                struct minivmi_cr3_event ev;
                memset(&ev, 0, sizeof(ev));
                ev.domid = m->domid;
                safe_copy(ev.uuid, sizeof(ev.uuid), m->uuid, strlen(m->uuid));
                ev.vcpu = (uint16_t)req.vcpu_id;
                ev.old_cr3 = req.u.write_ctrlreg.old_value;
                ev.new_cr3 = req.u.write_ctrlreg.new_value;
                ev.rip = req.data.regs.x86.rip;

                /* 第3步（对外暴露）：把 CR3 事件交给用户回调（示例里会打印出来）。 */
                cb(&ev, user);
            }

            ring_put_rsp(&m->back_ring, &rsp);
            handled++;
        }

        if (handled) {
            /*
             * 第3步（闭环完成）：push responses + notify Xen。
             * - RING_PUSH_RESPONSES：把 rsp_prod_pvt 刷到共享 ring
             * - xenevtchn_notify：告诉 Xen “response 已准备好，可以放行 guest”
             */
            RING_PUSH_RESPONSES(&m->back_ring);
            if (xenevtchn_notify(m->xce, m->local_port) < 0) {
                set_err(err, err_len, "xenevtchn_notify failed: %s", strerror(errno));
                return -1;
            }
        }

        if (xenevtchn_unmask(m->xce, (evtchn_port_t)pend) < 0) {
            set_err(err, err_len, "xenevtchn_unmask failed: %s", strerror(errno));
            return -1;
        }
    }

    return 0;
}

void minivmi_cr3_monitor_close(struct minivmi_cr3_monitor *m)
{
    if (!m) return;

    /*
     * 第4步（收尾）：尽力清理（best-effort）。
     * - 真实环境里经常遇到“中途失败/被 Ctrl+C 打断”，所以 close 不能假设状态完美。
     */
    if (m->cr3_enabled) {
        (void)xc_monitor_write_ctrlreg(m->xch, m->domid, VM_EVENT_X86_CR3, false, true, 0, true);
        m->cr3_enabled = false;
    }

    if (m->xce && m->local_port) {
        (void)xenevtchn_unbind(m->xce, m->local_port);
        m->local_port = 0;
    }

    if (m->xce) {
        (void)xenevtchn_close(m->xce);
        m->xce = NULL;
    }

    if (m->monitor_enabled && m->xch) {
        (void)xc_monitor_disable(m->xch, m->domid);
        m->monitor_enabled = false;
    }

    if (m->ring_page) {
        (void)munmap(m->ring_page, (size_t)m->ring_page_len);
        m->ring_page = NULL;
    }

    if (m->xch) {
        (void)xc_interface_close(m->xch);
        m->xch = NULL;
    }

    free(m);
}
