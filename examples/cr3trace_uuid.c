#include "minivmi/minivmi.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static volatile sig_atomic_t g_stop = 0;

static void on_sig(int signo)
{
    (void)signo;
    g_stop = 1;
}

static void on_cr3(const struct minivmi_cr3_event *ev, void *user)
{
    (void)user;
    /* 第3步（观测结果）：这里只做最简单的打印，你后续可以换成写文件/统计/过滤。 */
    printf("domid=%u uuid=%s vcpu=%u old=0x%lx new=0x%lx rip=0x%lx\n",
           ev->domid,
           ev->uuid[0] ? ev->uuid : "",
           (unsigned)ev->vcpu,
           (unsigned long)ev->old_cr3,
           (unsigned long)ev->new_cr3,
           (unsigned long)ev->rip);
    fflush(stdout);
}

static void usage(const char *argv0)
{
    fprintf(stderr, "usage: %s --uuid <uuid>\n", argv0);
}

int main(int argc, char **argv)
{
    /*
     * 第2步（选择目标）：这里用 uuid 找 domid。
     * - 为什么推荐 uuid：domid 可能会变化（重启/迁移等），uuid 更稳定
     */
    const char *uuid = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--uuid") == 0 && i + 1 < argc) {
            uuid = argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!uuid || uuid[0] == '\0') {
        usage(argv[0]);
        return 2;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    char err[MINIVMI_ERR_MAX] = {0};

    uint32_t domid = 0;
    if (minivmi_find_domid_by_uuid(&domid, uuid, err, sizeof(err)) != 0) {
        fprintf(stderr, "find domid by uuid failed: %s\n", err);
        return 1;
    }

    printf("attach uuid=%s domid=%u\n", uuid, domid);

    /* 第2步（attach）：建立 vm_event 共享 ring + evtchn 通道。 */
    struct minivmi_cr3_monitor *m = minivmi_cr3_monitor_open(domid, uuid, err, sizeof(err));
    if (!m) {
        fprintf(stderr, "monitor_open failed: %s\n", err);
        return 1;
    }

    /* 第3步（开启拦截点）：让 Xen 在写 CR3 时给我们发事件。 */
    if (minivmi_cr3_monitor_enable(m, err, sizeof(err)) != 0) {
        fprintf(stderr, "monitor_enable failed: %s\n", err);
        minivmi_cr3_monitor_close(m);
        return 1;
    }

    printf("monitor started (Ctrl+C to stop)\n");
    /* 第3步（事件循环）：poll -> 读 ring -> 回调 -> 写回 response -> 放行 guest。 */
    int rc = minivmi_cr3_monitor_loop(m, on_cr3, NULL, &g_stop, err, sizeof(err));
    if (rc != 0) {
        fprintf(stderr, "monitor_loop failed: %s\n", err);
    }

    minivmi_cr3_monitor_close(m);
    printf("done\n");
    return rc == 0 ? 0 : 1;
}
