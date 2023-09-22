# Context switch
#
#   void swtch(struct context **old, struct context *new);
# 
# Save the current registers on the stack, creating
# a struct context, and save its address in *old.
# Switch stacks to new and pop previously-saved registers.

.globl swtch
swtch:
  # new
  # esp + 4의 값을 eax에 담음. 두번째 인자를 eax에 담은 것 p->context. new
  # eax에는 new 주소값이 담김
  movl 4(%esp), %eax
  # esp + 8의 값을 edx에 담음. 아마 첫번째 인자를 edx에 담은 것 &(c->scheduler). old
  # edx 에는 old(컨텍스트가 있었던 위치; cpu의 스케줄러 부분임; (c->scheduler)) 주소값이 담김
  # old
  movl 8(%esp), %edx

  # 이전 값들을 푸시
  # Save old callee-saved registers
  pushl %ebp
  pushl %ebx
  pushl %esi
  pushl %edi

  # 현재의 esp를 eax에 담음
  # eax의 위치(p->context)에 현재 esp가 담김.
  # Switch stacks
  movl %esp, (%eax)
  # edx를 현재의 esp로 삼음
  # 이전의 context 위치(c->scheduler)를 esp로 삼음
  movl %edx, %esp

  # 새로운 esp로 전환 됐으니
  # Load new callee-saved registers
  popl %edi
  popl %esi
  popl %ebx
  popl %ebp
  ret
