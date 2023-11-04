# Context switch
#
#   void swtch(struct context **old, struct context *new);
# 
# Save the current registers on the stack, creating
# a struct context, and save its address in *old.
# Switch stacks to new and pop previously-saved registers.
# 분석 다시하기
.globl swtch
swtch:
  # old
  movl 4(%esp), %eax
  # new
  movl 8(%esp), %edx

  # 이전 값들을 푸시
  # Save old callee-saved registers
  pushl %ebp
  pushl %ebx
  pushl %esi
  pushl %edi

  # new의 위치를 여기서 찍는거임. eax의 메모리상 위치를 갱신하는 것.
  # Switch stacks
  movl %esp, (%eax)
  # new의 위치를 불러옴
  movl %edx, %esp

  # 새로운 esp로 전환 됐으니
  # Load new callee-saved registers
  popl %edi
  popl %esi
  popl %ebx
  popl %ebp
  ret
