#include "minivmi/minivmi.h"

#include <stdio.h>
#include <xenctrl.h> /* 提供 XEN_DOMINF_* flags（内部会包含 xen/domctl.h） */

static const char *yesno(int v) { return v ? "yes" : "no"; }

int main(void)
{
    /*
     * 第1步（先把链路跑通）：列出当前 dom0 看到的所有 domain。
     * - 这一步的意义：确认 libxc + xenstore 能正常工作
     * - 输出：domid / 是否 HVM / 状态 flags / name / uuid（如果 xenstore 里有）
     */
    char err[MINIVMI_ERR_MAX] = {0};

    struct minivmi_domain *domains = NULL;
    size_t count = 0;

    if (minivmi_domains_snapshot(&domains, &count, err, sizeof(err)) != 0) {
        fprintf(stderr, "minivmi_domains_snapshot failed: %s\n", err);
        return 1;
    }

    printf("count=%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        const int is_hvm = (domains[i].xen_flags & XEN_DOMINF_hvm_guest) != 0;
        const int dying  = (domains[i].xen_flags & XEN_DOMINF_dying) != 0;
        const int shut   = (domains[i].xen_flags & XEN_DOMINF_shutdown) != 0;

        printf("domid=%u hvm=%s dying=%s shutdown=%s name='%s' uuid='%s'\n",
               domains[i].domid,
               yesno(is_hvm), yesno(dying), yesno(shut),
               domains[i].name[0] ? domains[i].name : "",
               domains[i].uuid[0] ? domains[i].uuid : "");
    }

    minivmi_domains_free(domains);
    return 0;
}
