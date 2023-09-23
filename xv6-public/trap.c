#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

// main.c의 main에서 idtinit보다 앞서 호출됨
// vectors.S -> vectors[] 이용해 idt를 초기화 한다고함
// 인터럽트 발생하면 idt를 이용해 트랩 프레임 만들고 alltraps -> trap() 호출??
void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

// main.c의 main() -> mpmain() 에서 스케줄러 호출 이전 실행됨. lidt는 load interrupt data table
// 이후 스케줄러에서 init프로세스 만들고 init에서 sh 실행해서 sh에서 프로그램을 입력해 실행가능
void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

// 코드 자체는 명령줄일 뿐이라 인터럽트 발생시마다 트랩으로 뛰어들지만 acquire(&tickslock);의 인터럽트 무시 명령때문에 수행되지 않는다
//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER: // 틱마다발생되는 타이머 인터럽트
    if(cpuid() == 0){ // 첫번째 cpu인 경우
      // 락 될때까지 계속 시도. 스핀락
      acquire(&tickslock);
      // 틱은 다른 프로세스가 걸렸을 때에도 올라감. 전역이어서
      ticks++;
      // 알람이 설정됐을 경우 알람 틱 증가
      // 러닝 상태 프로세스가 있을 경우
      // ++처리 하면 안됨. 다른 프로세스 잡혔을 경우 틱 증가 반영이 안됨
      if(myproc() && myproc()->alarm_timer > 0)
        myproc()->alarm_ticks = ticks;
      // 잠시 락을 걸며 해당 채널의 모든 프로세스를 러너블 상태로 만듦
      // 아마 이거 때문에 자던애들이 스케쥴 될수 있음
      wakeup(&ticks);
      // 락을 풀어줌??
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // 러닝 프로세스이고 알람 타이머가 끝났다면 프린트후 종료
  if(myproc() && myproc()->alarm_timer && 
      myproc()->alarm_timer <= myproc()->alarm_ticks) {
    cprintf("SSU_Alarm!\n");
    print_date();
    exit();
  }

  // 이거 때문에 스케줄링이 일어남. 결국 타이머 울릴때마다 스케줄링 됨
  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
