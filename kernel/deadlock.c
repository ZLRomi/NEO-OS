#include "deadlock.h"
#include "process.h"
#include "printk.h"

// 等待图邻接矩阵：wait_graph[from][to] = 1 表示 from 在等待 to
static int wait_graph[MAX_PROCESSES][MAX_PROCESSES];

static int visited[MAX_PROCESSES];
static int on_stack[MAX_PROCESSES];

void deadlock_record_wait(int from_pid, int to_pid) {
    if (from_pid < 0 || from_pid >= MAX_PROCESSES) return;
    if (to_pid < 0 || to_pid >= MAX_PROCESSES) return;
    if (from_pid == to_pid) {
        // 自环直接算死锁
        printk("deadlock: self-wait pid=%d\n", from_pid);
        return;
    }
    wait_graph[from_pid][to_pid] = 1;
}

void deadlock_clear_wait(int pid) {
    int i;
    if (pid < 0 || pid >= MAX_PROCESSES) return;
    // 清除 pid 的所有出边
    for (i = 0; i < MAX_PROCESSES; i++) {
        wait_graph[pid][i] = 0;
    }
    // 清除指向 pid 的所有入边
    for (i = 0; i < MAX_PROCESSES; i++) {
        wait_graph[i][pid] = 0;
    }
}

// DFS 检测环，返回环中一个 pid，无环返回 -1
static int dfs(int u) {
    int v;
    visited[u] = 1;
    on_stack[u] = 1;

    for (v = 0; v < MAX_PROCESSES; v++) {
        if (!wait_graph[u][v]) continue;
        if (!visited[v]) {
            int r = dfs(v);
            if (r >= 0) return r;
        } else if (on_stack[v]) {
            // 发现环
            return v;
        }
    }

    on_stack[u] = 0;
    return -1;
}

int deadlock_detect(void) {
    int i;
    for (i = 0; i < MAX_PROCESSES; i++) {
        visited[i] = 0;
        on_stack[i] = 0;
    }

    for (i = 0; i < MAX_PROCESSES; i++) {
        if (visited[i]) continue;
        int r = dfs(i);
        if (r >= 0) {
            printk("deadlock: cycle detected involving pid %d\n", r);
            return r;
        }
    }
    return -1;
}

void deadlock_reset(void) {
    int i, j;
    for (i = 0; i < MAX_PROCESSES; i++) {
        for (j = 0; j < MAX_PROCESSES; j++) {
            wait_graph[i][j] = 0;
        }
    }
}
