// Per-CPU state
struct cpu {
  uchar apicid;                // Local APIC ID
  struct context *scheduler;   // swtch() here to enter scheduler
  struct taskstate ts;         // Used by x86 to find stack for interrupt
  struct segdesc gdt[NSEGS];   // x86 global descriptor table
  volatile uint started;       // Has the CPU started?
  int ncli;                    // Depth of pushcli nesting.
  int intena;                  // Were interrupts enabled before pushcli?
  struct proc *proc;           // The process running on this cpu or null
};

extern struct cpu cpus[NCPU];
extern int ncpu;

//PAGEBREAK: 17
// Saved registers for kernel context switches.
// Don't need to save all the segment registers (%cs, etc),
// because they are constant across kernel contexts.
// Don't need to save %eax, %ecx, %edx, because the
// x86 convention is that the caller has saved them.
// Contexts are stored at the bottom of the stack they
// describe; the stack pointer is the address of the context.
// The layout of the context matches the layout of the stack in swtch.S
// at the "Switch stacks" comment. Switch doesn't save eip explicitly,
// but it is on the stack and allocproc() manipulates it.
struct context {
  uint edi;
  uint esi;
  uint ebx;
  uint ebp;
  uint eip;
};

enum procstate { UNUSED, EMBRYO, SLEEPING, RUNNABLE, RUNNING, ZOMBIE };

// Per-process state
struct proc {
  uint sz;                     // Size of process memory (bytes)
  pde_t* pgdir;                // Page table // 페이징된 공간. 아마 코드들이나 데이터 쓰는거 있는곳? 이게 찐 메모리공간. 페이징 할당 및 복사는 exec시 initkvm, inituvm을 통해 붙여준다. 유저 메모리공간은 증가할수있는 공간. 요상한 메타데이터는 페이지 테이블에만 붙여주고 스택에는 붙이지않음
  char *kstack;                // Bottom of kernel stack for this process // 할당받은 스택 공간. 함수같은 것들 쓰려고 모아놓는 곳. 스택 영역과 코드 읽으려고 쓰는 페이지(데이터, 코드) 영역은 분리되어있다. 스택은 고정크기의 공간.
  enum procstate state;        // Process state
  int pid;                     // Process ID
  struct proc *parent;         // Parent process
  struct trapframe *tf;        // Trap frame for current syscall // 트랩 발생시 trap()에 제공할 정보들 모아두는 곳
  struct context *context;     // swtch() here to run process // 스케쥴링함수일때는 해당 프로세스의 이전 작업으로, 작업중 프로세스일 때는 스케줄링 함수로점프해서 myproc = 0이됨
  void *chan;                  // If non-zero, sleeping on chan
  int killed;                  // If non-zero, have been killed
  struct file *ofile[NOFILE];  // Open files
  struct inode *cwd;           // Current directory
  char name[16];               // Process name (debugging)

  // 추가 된 변수는 매 프로세스 종료때마다 초기화 시켜줘야함
  uint alarm_ticks;
  uint alarm_timer;
};

// Process memory is laid out contiguously, low addresses first:
//   text
//   original data and bss
//   fixed-size stack
//   expandable heap
