# PMEM-Crash-Engine

`PMEM-Crash-Engine` 是一个面向 Intel 持久化内存编程范式的 C++20 实验库骨架：
直接在 `mmap` 映射区域上组织数据结构，模拟 NVM/PMEM 崩溃一致性路径，避免传统“先写文件系统再 fsync”的磁盘式流程。

> 当前仓库按要求只提供接口与测试框架，**核心持久化算法故意留空**（TODO），用于研究与教学起点。

---

## 1. PMEM vs DRAM：本质差异

1. **掉电语义不同**
   - DRAM：掉电即失。
   - PMEM：掉电后介质保留数据，但 CPU Cache 里的脏数据可能丢失。

2. **一致性责任上移到软件**
   - DRAM 程序通常只管并发可见性（memory ordering）。
   - PMEM 程序还必须管**持久化顺序（persistence ordering）**，即：
     - 先把数据写好并持久化；
     - 再发布指针/元数据；
     - 再持久化发布动作。

3. **崩溃模型更苛刻**
   - PMEM 程序需承受“任意指令间断电”。
   - 若写入跨 Cache Line 或持久化顺序不当，可能出现 torn/partial persistence，导致恢复后结构损坏。

4. **性能画像介于 DRAM 与 SSD 之间**
   - 延迟高于 DRAM，远低于 SSD。
   - 适合“内存式访问 + 持久化语义”场景。

---

## 2. 代码结构（当前实现）

```text
.
├── CMakeLists.txt
├── include
│   ├── CacheFlush.h
│   ├── UndoLog.h
│   └── PersistentLinkedList.h
├── src
│   └── PersistentLinkedList.cpp
└── tests
    └── CrashSimulation.cpp
```

### 2.1 工程基础

- CMake + C++20
- 链接 `pthread`
- 使用 `mmap(MAP_SHARED)` 映射文件，模拟 PMEM 设备区

### 2.2 核心原语与接口（核心逻辑未实现）

- `include/CacheFlush.h`
  - 声明 `clflush()` / `sfence()` 包装器
  - 保留空实现（TODO）

- `include/UndoLog.h`
  - `UndoLogEntry` 强制 `alignas(64)`
  - `static_assert(sizeof == 64)`，避免跨 Cache Line 撕裂风险

- `include/PersistentLinkedList.h`
  - 定义持久化单向链表布局（Header + Node）
  - Node/Header 均为 64B 对齐
  - 给出插入协议契约（先持久化数据，再原子发布指针）

---

## 3. 恶劣掉电模拟测试框架

`tests/CrashSimulation.cpp` 提供了一个“故障注入 + 进程重启恢复”测试：

1. 父进程创建共享内存控制块（`shm_open + mmap`）。
2. 子进程启动多线程并发调用 `PersistentLinkedList::insert()`。
3. 父进程在随机微秒窗口注入扰动：
   - `SIGSTOP` / `SIGCONT`（挂起/恢复）
   - 共享标志位暂停工作线程
4. 父进程用 `SIGKILL` 强制杀死子进程（模拟内核级突然断电）。
5. 父进程再拉起“恢复子进程”（模拟重启）：
   - 重新映射同一 `mmap` 文件
   - 调用 `recover()`
   - 断言无环、无越界/悬空指针
   - 验证“可见 durable 视图”不超过可达节点数

> 注：由于核心事务算法按要求留空，当前恢复断言聚焦于**结构安全性与恢复流程完整性**。

---

## 4. Build & Run

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

也可直接运行：

```bash
./build/crash_recovery_test
```

---

## 5. 研究任务（下一步）

1. **实现内存屏障原语**
   - 用内联汇编完成 `clflush` / `sfence`
   - 明确平台能力检测（例如 eADR 场景）

2. **实现微型 Undo Log**
   - 事务状态机：prepare -> commit
   - 恢复期支持回滚/重放策略

3. **实现防撕裂持久化链表**
   - 节点写入 + 校验 + 发布顺序严格化
   - 指针更新原子发布并持久化
   - 恢复时裁剪坏链、拒绝半提交节点

---

## 6. 必读清单（含中文摘要）

### 6.1 Mnemosyne: Lightweight Persistent Memory (ASPLOS '11)
- 论文提出语言/编译器/运行时协同的持久化编程框架，目标是在接近普通内存编程模型的前提下获得崩溃恢复能力。
- 启发：将“持久化更新”抽象为更高层语义，避免开发者手写大量易错 flush/fence 序列。

### 6.2 System Programming with Persistent Memory
- 面向工程实践的系统化教材，覆盖 cache line flush、fence、事务、日志、恢复、PMDK 设计思路。
- 启发：把 PMEM 程序拆成“失败原子更新单元”，并始终用可证明的恢复协议约束数据结构演化。

### 6.3 AGAMOTTO: How Persistent is your Persistent Memory Application? (OSDI '20)
- 该工作通过符号执行和故障注入系统性发现 PM 应用中的持久化错误，尤其强调“崩溃前后跨阶段语义”验证。
- 与 Torn/部分持久化窗口相关的关键启发：即使并发逻辑正确，也可能因 flush/fence 顺序或持久化边界错误，导致恢复后出现结构不一致。

---

## 7. 当前状态说明

- 已完成：工程骨架、接口定义、64B 对齐约束、恶劣崩溃模拟测试框架。
- 未完成（故意留空）：`clflush/sfence` 实现、Undo Log 核心算法、持久化链表事务插入与恢复核心协议。
