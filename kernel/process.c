#include "process.h"
#include "heap.h"
#include "memory.h"
#include "auth.h"
#include "printk.h"
#include "klog.h"
#include "panic.h"
#include <stdint.h>

#define PAGE_SIZE 4096
#define KERNEL_STACK_BASE 0xC1000000
#define TEMP_PD_VADDR 0xB0000000

__asm__(
    ".global fork_child_trampoline\n"
    "fork_child_trampoline:\n\t"
    "xor %eax, %eax\n\t"
    "ret\n\t"
);
extern void fork_child_trampoline(void);

static process_t* process_table[MAX_PROCESSES];
static int next_pid = 0;
static process_t* current_process = NULL;
static process_t* ready_queue = NULL;
static uint32_t next_stack_vaddr = KERNEL_STACK_BASE;

static void set_name(process_t* p, const char* name) {
    int i;
    for (i = 0; i < 15 && name[i]; i++) p->name[i] = name[i];
    p->name[i] = '\0';
}

// 空闲进程入口：HLT 循环，等待中断唤醒
void cpu_idle(void) {
    while (1) {
        __asm__ volatile("hlt");
    }
}

// 进程退出包装：entry 返回时跳到这里
void process_exit_wrapper(void) {
    process_t* cur = process_get_current();
    if (cur) {
        cur->state = PROCESS_TERMINATED;
        process_ready_remove(cur);
    }
    // 调度启用后由 schedule 切走；禁用时 hlt
    while (1) __asm__ volatile("hlt");
}

static uint32_t read_cr3(void) {
    uint32_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}

static uint32_t alloc_kernel_stack(void) {
    uint32_t phys = pmm_alloc_page();
    kernel_assert(phys != 0);
    uint32_t virt = next_stack_vaddr;
    kernel_assert(map_page(virt, phys, PAGE_PRESENT | PAGE_WRITE) != 0);
    next_stack_vaddr += PAGE_SIZE;
    return virt + PAGE_SIZE;
}

static uint32_t process_create_page_dir(void) {
    uint32_t new_pd_phys = pmm_alloc_page();
    kernel_assert(new_pd_phys != 0);

    kernel_assert(map_page(TEMP_PD_VADDR, new_pd_phys, PAGE_PRESENT | PAGE_WRITE) != 0);

    uint32_t* new_pd = (uint32_t*)TEMP_PD_VADDR;
    uint32_t* kern_pd = (uint32_t*)0xFFFFF000;

    // 复制内核映射（identity map 低地址 + 高地址内核空间）
    for (int i = 0; i < 1023; i++) {
        new_pd[i] = kern_pd[i];
    }
    // 递归映射：pd[1023] 指向新 pd 自身
    new_pd[1023] = new_pd_phys | PAGE_PRESENT | PAGE_WRITE;

    return new_pd_phys;
}

// 深拷贝用户空间：为新 pd 重新分配页表与数据页，复制父进程用户页内容
static void deep_copy_user_space(uint32_t* new_pd) {
    uint32_t* kern_pd = (uint32_t*)0xFFFFF000;
    for (int pd_idx = 0; pd_idx < 768; pd_idx++) {
        uint32_t pde = kern_pd[pd_idx];
        if (!(pde & PAGE_PRESENT)) continue;
        if (!(pde & PAGE_USER)) continue;

        uint32_t new_pt_phys = pmm_alloc_page();
        if (!new_pt_phys) {
            printk("fork: pmm new_pt failed pd_idx=%d\n", pd_idx);
            new_pd[pd_idx] = 0;
            continue;
        }
        if (!map_page(0xB0010000, new_pt_phys, PAGE_PRESENT | PAGE_WRITE)) {
            pmm_free_page(new_pt_phys);
            new_pd[pd_idx] = 0;
            continue;
        }
        uint32_t* new_pt = (uint32_t*)0xB0010000;
        uint32_t* orig_pt = (uint32_t*)(0xFFC00000 + (pd_idx << 12));

        for (int pt_idx = 0; pt_idx < 1024; pt_idx++) {
            uint32_t pte = orig_pt[pt_idx];
            if (!(pte & PAGE_PRESENT)) { new_pt[pt_idx] = 0; continue; }
            if (!(pte & PAGE_USER))    { new_pt[pt_idx] = 0; continue; }

            uint32_t new_data_phys = pmm_alloc_page();
            if (!new_data_phys) { new_pt[pt_idx] = 0; continue; }
            if (!map_page(0xB0020000, new_data_phys, PAGE_PRESENT | PAGE_WRITE)) {
                pmm_free_page(new_data_phys);
                new_pt[pt_idx] = 0;
                continue;
            }
            uint32_t src_phys = pte & 0xFFFFF000;
            if (!map_page(0xB0030000, src_phys, PAGE_PRESENT | PAGE_WRITE)) {
                pmm_free_page(new_data_phys);
                new_pt[pt_idx] = 0;
                continue;
            }
            uint8_t* src = (uint8_t*)0xB0030000;
            uint8_t* dst = (uint8_t*)0xB0020000;
            for (int i = 0; i < 4096; i++) dst[i] = src[i];
            new_pt[pt_idx] = (new_data_phys & 0xFFFFF000) | (pte & 0xFFF);
        }
        new_pd[pd_idx] = (new_pt_phys & 0xFFFFF000) | (pde & 0xFFF);
    }
}

static void ready_enqueue(process_t* p) {
    p->next = NULL;
    if (!ready_queue) {
        ready_queue = p;
        return;
    }
    process_t* cur = ready_queue;
    while (cur->next) cur = cur->next;
    cur->next = p;
}

static void ready_remove(process_t* p) {
    process_t* prev = NULL;
    process_t* cur = ready_queue;
    while (cur) {
        if (cur == p) {
            if (prev) prev->next = cur->next;
            else ready_queue = cur->next;
            cur->next = NULL;
            return;
        }
        prev = cur;
        cur = cur->next;
    }
}

void process_init(void) {
    int i;
    for (i = 0; i < MAX_PROCESSES; i++) process_table[i] = NULL;
    next_pid = 0;
    ready_queue = NULL;

    process_t* p = (process_t*)kmalloc(sizeof(process_t));
    kernel_assert(p != NULL);
    for (i = 0; i < 8; i++) p->regs[i] = 0;
    p->pid = next_pid++;
    p->state = PROCESS_RUNNING;
    p->priority = 0;
    p->time_slice = 10;
    p->page_dir = read_cr3();
    p->eip = (uint32_t)cpu_idle;
    p->eflags = 0x202;
    p->kernel_stack = 0;  // 运行时由 switch_to 保存真实 esp
    p->uid = 0;
    for (i = 0; i < NSIG; i++) p->signal_handlers[i] = 0;
    p->pending_signals = 0;
    p->signal_mask = 0;
    p->parent_pid = -1;
    set_name(p, "idle");
    p->next = NULL;
    process_table[p->pid] = p;
    current_process = p;

    printk("process: idle process pid=0 created\n");
}

int process_create(uint32_t entry, int priority, const char* name) {
    int slot = -1;
    int i;
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) { slot = i; break; }
    }
    if (slot < 0) {
        printk("process_create: process table full\n");
        return -1;
    }

    process_t* p = (process_t*)kmalloc(sizeof(process_t));
    kernel_assert(p != NULL);

    uint32_t stack_top = alloc_kernel_stack();
    kernel_assert(stack_top != 0);

    // 预设栈帧：栈顶放 process_exit_wrapper 地址，作为 entry 返回地址
    uint32_t* sp = (uint32_t*)(stack_top - 4);
    *sp = (uint32_t)process_exit_wrapper;
    uint32_t init_esp = stack_top - 4;

    for (i = 0; i < 8; i++) p->regs[i] = 0;
    p->regs[7] = init_esp;  // esp
    p->pid = next_pid++;
    p->state = PROCESS_READY;
    p->priority = priority;
    p->time_slice = 10;
    p->page_dir = process_create_page_dir();
    kernel_assert(p->page_dir != 0);
    p->eip = entry;
    p->eflags = 0x202;
    p->kernel_stack = init_esp;
    p->uid = auth_get_current_uid();
    for (i = 0; i < NSIG; i++) p->signal_handlers[i] = 0;
    p->pending_signals = 0;
    p->signal_mask = 0;
    p->parent_pid = current_process ? current_process->pid : -1;
    set_name(p, name);
    p->next = NULL;

    process_table[slot] = p;
    ready_enqueue(p);

    printk("process: created pid=%d name=%s entry=0x%x\n", p->pid, p->name, entry);
    klog_write_level(KLOG_INFO, "process: created pid=%d name=%s\n", p->pid, p->name);
    return p->pid;
}

int process_fork_current(void) {
    process_t* parent = current_process;
    if (!parent) return -1;

    int slot = -1;
    for (int i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == NULL) { slot = i; break; }
    }
    if (slot < 0) {
        printk("fork: process table full\n");
        return -1;
    }

    process_t* child = (process_t*)kmalloc(sizeof(process_t));
    kernel_assert(child != NULL);

    uint32_t child_stack_top = alloc_kernel_stack();
    kernel_assert(child_stack_top != 0);

    uint32_t child_pd_phys = process_create_page_dir();
    kernel_assert(child_pd_phys != 0);
    uint32_t* child_pd = (uint32_t*)TEMP_PD_VADDR;
    deep_copy_user_space(child_pd);

    // 子进程栈顶预设父进程 eip，配合 fork_child_trampoline 的 ret 跳回父 eip
    uint32_t* sp = (uint32_t*)(child_stack_top - 4);
    *sp = parent->eip;
    uint32_t child_esp = child_stack_top - 4;

    for (int i = 0; i < 8; i++) child->regs[i] = parent->regs[i];
    child->regs[0] = 0;          // eax（trampoline 会真正设置）
    child->regs[7] = child_esp;  // esp
    child->pid = next_pid++;
    child->state = PROCESS_READY;
    child->priority = parent->priority;
    child->time_slice = 10;
    child->page_dir = child_pd_phys;
    child->eip = (uint32_t)fork_child_trampoline;
    child->eflags = parent->eflags;
    child->kernel_stack = child_esp;
    child->uid = parent->uid;
    for (int i = 0; i < NSIG; i++) child->signal_handlers[i] = parent->signal_handlers[i];
    child->pending_signals = 0;
    child->signal_mask = parent->signal_mask;
    child->parent_pid = parent->pid;
    set_name(child, parent->name);
    child->next = NULL;

    process_table[slot] = child;
    ready_enqueue(child);

    printk("fork: parent=%d child=%d\n", parent->pid, child->pid);
    return child->pid;
}

void process_exit(int pid) {
    process_t* p = process_get_by_pid(pid);
    int i;
    if (!p) {
        printk("process_exit: pid %d not found\n", pid);
        return;
    }
    ready_remove(p);
    p->state = PROCESS_TERMINATED;
    if (p->parent_pid > 0) {
        signal_send(p->parent_pid, SIGCHLD);
    }
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == p) { process_table[i] = NULL; break; }
    }
    kfree(p);
    printk("process: pid %d exited\n", pid);
    klog_write_level(KLOG_INFO, "process: exit pid=%d\n", pid);
}

void process_block(int state) {
    if (!current_process) return;
    current_process->state = state;
    ready_remove(current_process);
}

void process_wake(int pid) {
    process_t* p = process_get_by_pid(pid);
    if (!p) return;
    p->state = PROCESS_READY;
    ready_enqueue(p);
}

process_t* process_get_current(void) {
    return current_process;
}

process_t* process_get_by_pid(int pid) {
    int i;
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] && process_table[i]->pid == pid) return process_table[i];
    }
    return NULL;
}

int process_get_all(process_t** out, int max) {
    int n = 0;
    for (int i = 0; i < MAX_PROCESSES && n < max; i++) {
        if (process_table[i]) out[n++] = process_table[i];
    }
    return n;
}

void __attribute__((noinline)) switch_to(process_t* prev, process_t* next) {
    if (!prev || !next || prev == next) return;
    __asm__ volatile(
        "movl %%ebx, %0\n\t"
        "movl %%esi, %1\n\t"
        "movl %%edi, %2\n\t"
        "movl %%ebp, %3\n\t"
        "movl %%esp, %4\n\t"
        "pushfl\n\t"
        "popl %5\n\t"
        "movl $1f, %6\n\t"
        "movl %7, %%eax\n\t"
        "cmpl %8, %%eax\n\t"
        "je 3f\n\t"
        "movl %%eax, %%cr3\n\t"
        "3:\n\t"
        "movl %9, %%esp\n\t"
        "movl %10, %%ebp\n\t"
        "movl %11, %%edi\n\t"
        "movl %12, %%esi\n\t"
        "movl %13, %%ebx\n\t"
        "pushl %14\n\t"
        "popfl\n\t"
        "movl %15, %%eax\n\t"
        "jmp *%%eax\n\t"
        "1:\n\t"
        :
        : "m"(prev->regs[1]), "m"(prev->regs[4]), "m"(prev->regs[5]),
          "m"(prev->regs[6]), "m"(prev->regs[7]), "m"(prev->eflags),
          "m"(prev->eip),
          "m"(next->page_dir), "m"(prev->page_dir),
          "m"(next->regs[7]), "m"(next->regs[6]), "m"(next->regs[5]),
          "m"(next->regs[4]), "m"(next->regs[1]), "m"(next->eflags),
          "m"(next->eip)
        : "eax", "ebx", "esi", "edi", "ebp", "memory"
    );
}

void process_set_current(process_t* p) { current_process = p; }
process_t* process_ready_head(void) { return ready_queue; }
void process_ready_enqueue(process_t* p) { ready_enqueue(p); }
void process_ready_remove(process_t* p) { ready_remove(p); }

// 从 cur 之后遍历进程表，找下一个 READY 进程（round-robin，回绕）
process_t* process_get_next_ready_after(process_t* cur) {
    int i, cur_idx = -1;
    // 找到 cur 在表中的位置
    for (i = 0; i < MAX_PROCESSES; i++) {
        if (process_table[i] == cur) { cur_idx = i; break; }
    }
    if (cur_idx < 0) return NULL;
    // 从 cur 之后到末尾
    for (i = cur_idx + 1; i < MAX_PROCESSES; i++) {
        if (process_table[i] && process_table[i]->state == PROCESS_READY)
            return process_table[i];
    }
    // 回绕：从头到 cur
    for (i = 0; i < cur_idx; i++) {
        if (process_table[i] && process_table[i]->state == PROCESS_READY)
            return process_table[i];
    }
    return NULL;
}
