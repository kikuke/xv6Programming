#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct run_queue run_queue_list[NPROC]; // 최대 프로세스 개수만큼 할당
struct run_queue *run_queues[MAXRUNQ]; // 25개의 run queue

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

void
put_runqueue(struct proc *proc)
{
  struct run_queue *rq = 0; // 사용할 공간
  struct run_queue *it; // 순회용

  for (int i = 0; i < NPROC; i++) { // 사용중이지 않은 run_queue 탐색
    if (!run_queue_list[i].is_used) {
      rq = &run_queue_list[i];
      break;
    }
  }
  if (!rq)
    panic("put_runqueue");
  
  rq->is_used = 1;
  rq->rproc = proc;
  rq->next = 0;
  rq->tail = rq;

  if (run_queues[proc->priority / 4] == 0) {
    run_queues[proc->priority / 4] = rq;
    return;
  }

  for (it = run_queues[proc->priority / 4]; it->next != 0; it = it->next) { // 순회하며 run_queues 값 업데이트
    it->tail = rq;
  }
  it->next = rq;
  it->tail = rq;

  //Test
  // if(proc->pid > 2) {
  //   for(it = run_queues[proc->priority / 4]; it!= 0; it = it->next)
  //     cprintf("putq - q: %d, pid: %d", proc->priority / 4, it->rproc->pid);
  //   cprintf("\n");
  // }
}

void
pull_runqueue(struct proc *proc)
{
  struct run_queue *bef_it = 0;
  struct run_queue *it; // 순회용
  struct run_queue *t_it;
  // proc에 해당되는 run_queue 탐색
  for (it = run_queues[proc->priority / 4]; it->rproc != proc; it = it->next)
    bef_it = it;
  
  // 큐 조정
  if (it == run_queues[proc->priority / 4]) { // 첫 번째 run_queue일 경우
    run_queues[proc->priority / 4] = it->next;
  } else {
    bef_it->next = it->next;
    if (bef_it->next == 0) { // tail이었을 경우 업데이트
      for(t_it = run_queues[proc->priority / 4]; t_it->next != 0; t_it = t_it->next)
        t_it->tail = bef_it;
    }
  }

  it->is_used = 0;
  // 오류 예방
  it->next = 0;
  it->rproc = 0;
  it->tail = 0;
}

int
get_best_priority()
{
  struct run_queue *q;
  int best_pri = MAXPRIOR; // 가장 높은 우선순위. MAXPRIOR도 스케줄 되야 하므로

  for (int i=0; i < MAXRUNQ - 1; i++) { // 가장 마지막 큐를 제외한 전체 RUNQ 탐색.
    for (q = run_queues[i]; q != 0; q = q->next) { // 연결 리스트 탐색
      if (q->rproc->priority < best_pri) // 이전 프로세스보다 우선순위가 높을경우
        best_pri = q->rproc->priority;
    }
    if (best_pri != MAXPRIOR) // 해당 큐에서 발견됐다면 리턴
      break;
  }
  if (best_pri == MAXPRIOR)
    best_pri = 0;
  
  return best_pri;
}

void
update_priority(struct proc *proc, int priority)
{
  pull_runqueue(proc);
  proc->priority = priority;
  put_runqueue(proc);
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->priority = MAXPRIOR; // idle process(init process)는 priority 99(최대 값)
  p->state = RUNNABLE;
  put_runqueue(p);

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;
  np->priority = get_best_priority(); // 현재 run_queue에서 관리하는 프로세스중 가장 작은 priority 값 부여
  if (np->pid == 1 || np->pid == 2) // 만약 idle 프로세스라면 최대 prior 값 부여
    np->priority = MAXPRIOR;
  put_runqueue(np); // run_queue에 등록

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  pull_runqueue(curproc); // 종료시 runqueue에서 빼냄
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;

        p->priority = MAXPRIOR; // 종료된 프로세스는 혹시 모르니 최대값
        p->proc_tick = 0;
        p->cpu_used = 0;

        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// 스케줄 될 프로세스를 얻는 함수
struct proc*
ssu_schedule()
{
  struct run_queue *q;
  int best_pri = MAXPRIOR + 1; // 가장 높은 우선순위. MAXPRIOR도 스케줄 되야 하므로
  struct proc *best_proc = 0; // 가장 높은 우선순위 proc

  for (int i=0; i < MAXRUNQ; i++) { // 전체 RUNQ 탐색
    for (q = run_queues[i]; q != 0; q = q->next) { // 연결 리스트 탐색
      if (q->rproc->state == RUNNABLE && q->rproc->priority < best_pri) { // RUNNABLE이고 이전 프로세스보다 우선순위가 높을경우
        best_proc = q->rproc;
        best_pri = best_proc->priority;
      }
    }
    if (best_proc) // 해당 큐에서 발견됐다면 리턴
      break;
  }

  return best_proc;
}

// 우선순위 증가 함수
void
ssu_update_priority()
{
  struct run_queue *q;
  struct run_queue *nq;
  struct run_queue *eq;
  int up_prior;

  //Test
  // cprintf("\nssu_update_start\n");

  for (int i=0; i < MAXRUNQ; i++) { // 전체 RUNQ 탐색
    eq = q = run_queues[i];
    do {
      if (q == 0)
        break;
      nq = q->next;

      up_prior = q->rproc->priority + (q->rproc->proc_tick / 10);
      if (q->rproc->pid == 1 || q->rproc->pid == 2 || up_prior > 99) // IDLEPROC은 고정
        up_prior = 99;

      //Test
      // if(q->rproc->pid > 2)
      //   cprintf("update - qidx: %d pid: %d\n", i, q->rproc->pid);

      update_priority(q->rproc, up_prior);
      //Todo: 위치 이거 이상하다고 생각함
      q->rproc->proc_tick = 0; // 다시 사용시간 초기화

      q = nq;
    } while (q != eq);
  }

  //Test
  // cprintf("\nssu_update_end\n");
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    // ssu_scheduling으로 변경
    if((p = ssu_schedule())) {

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

#ifdef DEBUG
      cprintf("scheduler pid: %d, priority: %d, proc_tick: %d, cpu_used: %d\n", p->pid, p->priority, p->proc_tick, p->cpu_used);
#endif

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }

    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      if(p->pid != 1 && p->pid != 2) // IDLEPROC이 아니라면
        update_priority(p, get_best_priority()); // 현재 run_queue에서 관리하는 프로세스중 가장 작은 priority 값 부여
      p->state = RUNNABLE;
    }
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
