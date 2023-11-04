#include "mmu.h"

  # vectors.S sends all traps here.
.globl alltraps
alltraps:
  # 이거 호출하기전에 설정된 레지스터 값들을 스택에 때려박아 trapframe 데이터를 만들어 인자를 넣는 효과
  # x86h trapframe의 아래 데이터 부터 쌓아 올려나감. ds부터. trapno 아래는 이전에 쌓아 올려져있었음
  # Build trap frame.
  pushl %ds
  pushl %es
  pushl %fs
  pushl %gs
  pushal
  
  # 데이터 세그먼트가 뭔지 모르지만 그걸 ds와 es 레지스터에 넣고 있음
  # Set up data segments.
  movw $(SEG_KDATA<<3), %ax
  movw %ax, %ds
  movw %ax, %es

  # 마지막으로 trapframe데이터에 esp값 넣어줌(푸쉬).
  # esp, ebp는 범용과 스택이 따로 있음
  # Call trap(tf), where tf=%esp
  pushl %esp
  call trap
  # 여긴 trapret 점프가 담겨있을듯. 초기 설정에 있었음
  addl $4, %esp

  # Return falls through to trapret...
.globl trapret
trapret:
  popal
  popl %gs
  popl %fs
  popl %es
  popl %ds
  addl $0x8, %esp  # trapno and errcode
  iret
