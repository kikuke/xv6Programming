#include "param.h"
#include "types.h"
#include "defs.h"
#include "x86.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "elf.h"

extern char data[];  // defined by kernel.ld
pde_t *kpgdir;  // for use in scheduler()

// Set up CPU's kernel segment descriptors.
// Run once on entry on each CPU.
void
seginit(void)
{
  struct cpu *c;

  // Map "logical" addresses to virtual addresses using identity map.
  // Cannot share a CODE descriptor for both kernel and user
  // because it would have to have DPL_USR, but the CPU forbids
  // an interrupt from CPL=0 to DPL=3.
  c = &cpus[cpuid()];
  c->gdt[SEG_KCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, 0);
  c->gdt[SEG_KDATA] = SEG(STA_W, 0, 0xffffffff, 0);
  c->gdt[SEG_UCODE] = SEG(STA_X|STA_R, 0, 0xffffffff, DPL_USER);
  c->gdt[SEG_UDATA] = SEG(STA_W, 0, 0xffffffff, DPL_USER);
  lgdt(c->gdt, sizeof(c->gdt));
}

// 중요한 함수!
// 상위 10비트는 페이지 디렉토리 인덱스, 그다음 10비트는 페이지 테이블 인덱스인 듯
// 가상 주소 va에 해당하는 페이지 테이블 pgdir의 PTE 주소를 반환. alloc != 0인 경우 필요한 페이지 테이블 페이지를 만듦
// 최종적으로 위 20비트를 이용해 해당 페이지 디렉토리 테이블 -> 페이지 테이블 -> 해당 가상주소의 페이지의 가상주소가 담긴 pte를 리턴함
// Return the address of the PTE in page table pgdir
// that corresponds to virtual address va.  If alloc!=0,
// create any required page table pages.
static pte_t *
walkpgdir(pde_t *pgdir, const void *va, int alloc)
{
  pde_t *pde;
  pte_t *pgtab;

  pde = &pgdir[PDX(va)]; // 가상주소에서 페이지 디렉토리 인덱스를 추출해 해당 가상주소의 페이지 디렉토리 엔트리를 가져옴
  if(*pde & PTE_P){ // 해당 페이지 디렉토리 엔트리가 가리키는 pte가 존재한다면
    pgtab = (pte_t*)P2V(PTE_ADDR(*pde)); // 해당 pte가 가리키는 값의 하위 12비트(offset)을 제외한뒤 가상주소로 변환. 페이지 테이블이 됨. cpu는 가상주소로 넣어줘야하기에?
  } else { // 해당 페이지 디렉토리 엔트리가 가리키는 페이지가 존재하지 않는다면
    if(!alloc || (pgtab = (pte_t*)kalloc()) == 0) // alloc을 설정했을 경우 없을 경우 페이지를 할당. 페이지 테이블이 됨. 이때 들어오는 값은 가상주소임. cpu에서 자동 변환해주기에?
      return 0;
    // Make sure all those PTE_P bits are zero.
    memset(pgtab, 0, PGSIZE);
    // The permissions here are overly generous, but they can
    // be further restricted by the permissions in the page table
    // entries, if necessary.
    *pde = V2P(pgtab) | PTE_P | PTE_W | PTE_U; // 할당된 페이지를 해당 pde에 저장. 맨뒤에 세 비트는 권한 설정?
  }
  return &pgtab[PTX(va)]; // 가상주소에서 페이지 테이블 인덱스를 추출해 페이지 테이블에서 해당 페이지의 가상주소가 담긴 pte를 리턴함
}

// va에서 시작하는 가상주소에 대한 PTE를 생성. pa에서 시작하는 물리 주소를 참고하고 크기가 페이지 정렬되지 않을 수 있음
// pde가 가리키는 페이지 테이블이 없다면 생성.
// 가상주소를 참고해 페이지 단위로 각 페이지 테이블의 pte에 물리주소를 매핑하고 권한 설정 및 해당 pte가 존재함을 쓰기함
// 인자 첫번째 pgdir은 프로세스들이 생성됐을때 setupkvm 같은거로 할당받은거 쓰는거임
// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned.
static int
mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va); // 시작 가상주소?? 페이지 크기에 맞춰반내림
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1); // 가상주소 끝. 페이지 크기에 맞춰 반내림
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0) // 해당 가상주소의 페이지 디렉토리의 페이지 테이블이 없다면 생성하고 가상주소에 맞는 pte를 가져옴
      return -1;
    if(*pte & PTE_P) // 만약 해당 pte에 페이지가 존재한다면 패닉.
      panic("remap");
    *pte = pa | perm | PTE_P; // 해당 pte에 물리주소, 권한, 해당 페이지가 존재함을 기록
    if(a == last) // 끝날때 까지
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}
// 하나의 프로세스당 하나의 페이지 테이블이 존재하며, CPU가 프로세스를 실행하지 않을 때 사용되는 하나의 페이지 테이블(kpgdir)이 있습니다. 
// 커널은 시스템 호출 및 인터럽트 시 현재 프로세스의 페이지 테이블을 사용합니다. 페이지 보호 비트는 사용자 코드가 커널의 매핑을 사용하지 못하도록 합니다.
// 페이지 테이블은 오직 시스템 호출 및 인터럽트에서 사용할수 있다?? exec()나 setupkvm()때 세팅해줌
// There is one page table per process, plus one that's used when
// a CPU is not running any process (kpgdir). The kernel uses the
// current process's page table during system calls and interrupts;
// page protection bits prevent user code from using the kernel's
// mappings.

// setupkvm() and exec() set up every page table like this:

//   0..KERNBASE: user memory (text+data+stack+heap), mapped to
//                phys memory allocated by the kernel 유저영역은 kmap에 존재 x?
//   KERNBASE..KERNBASE+EXTMEM: mapped to 0..EXTMEM (for I/O space)
//   KERNBASE+EXTMEM..data: mapped to EXTMEM..V2P(data)
//                for the kernel's instructions and r/o data
//   data..KERNBASE+PHYSTOP: mapped to V2P(data)..PHYSTOP,
//                                  rw data + free physical memory
//   0xfe000000..0: mapped direct (devices such as ioapic)

// The kernel allocates physical memory for its heap and for user memory
// between V2P(end) and the end of physical memory (PHYSTOP)
// (directly addressable from end..P2V(PHYSTOP)).

// This table defines the kernel's mappings, which are present in
// every process's page table.
static struct kmap {
  void *virt;
  uint phys_start;
  uint phys_end;
  int perm;
} kmap[] = {
 { (void*)KERNBASE, 0,             EXTMEM,    PTE_W}, // I/O space
 { (void*)KERNLINK, V2P(KERNLINK), V2P(data), 0},     // kern text+rodata
 { (void*)data,     V2P(data),     PHYSTOP,   PTE_W}, // kern data+memory
 { (void*)DEVSPACE, DEVSPACE,      0,         PTE_W}, // more devices
};

// 오직 시스템 호출 및 인터럽트에서 사용하는 페이지 테이블.
// 최초로 해당 프로세스만의 페이지 디렉토리를 만들고
// 페이지 디렉토리, 페이지 테이블에 커널 관련 가상주소 - 피지컬 주소를 미리 매핑
// Set up kernel part of a page table.
pde_t*
setupkvm(void)
{
  pde_t *pgdir;
  struct kmap *k;

  if((pgdir = (pde_t*)kalloc()) == 0) // 페이지를 페이지 디렉토리로 쓰겠다
    return 0;
  memset(pgdir, 0, PGSIZE);
  if (P2V(PHYSTOP) > (void*)DEVSPACE)
    panic("PHYSTOP too high");
  for(k = kmap; k < &kmap[NELEM(kmap)]; k++)
    if(mappages(pgdir, k->virt, k->phys_end - k->phys_start,
                (uint)k->phys_start, k->perm) < 0) {
      freevm(pgdir);
      return 0;
    }
  return pgdir;
}

// Allocate one page table for the machine for the kernel address
// space for scheduler processes.
void
kvmalloc(void)
{
  kpgdir = setupkvm();
  switchkvm();
}

// Switch h/w page table register to the kernel-only page table,
// for when no process is running.
void
switchkvm(void)
{
  lcr3(V2P(kpgdir));   // switch to the kernel page table
}

// Switch TSS and h/w page table to correspond to process p.
void
switchuvm(struct proc *p)
{
  if(p == 0)
    panic("switchuvm: no process");
  if(p->kstack == 0)
    panic("switchuvm: no kstack");
  if(p->pgdir == 0)
    panic("switchuvm: no pgdir");

  pushcli();
  mycpu()->gdt[SEG_TSS] = SEG16(STS_T32A, &mycpu()->ts,
                                sizeof(mycpu()->ts)-1, 0);
  mycpu()->gdt[SEG_TSS].s = 0;
  mycpu()->ts.ss0 = SEG_KDATA << 3;
  mycpu()->ts.esp0 = (uint)p->kstack + KSTACKSIZE;
  // setting IOPL=0 in eflags *and* iomb beyond the tss segment limit
  // forbids I/O instructions (e.g., inb and outb) from user space
  mycpu()->ts.iomb = (ushort) 0xFFFF;
  ltr(SEG_TSS << 3);
  lcr3(V2P(p->pgdir));  // switch to process's address space
  popcli();
}

// Load the initcode into address 0 of pgdir.
// sz must be less than a page.
void
inituvm(pde_t *pgdir, char *init, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pgdir, 0, PGSIZE, V2P(mem), PTE_W|PTE_U);
  memmove(mem, init, sz);
}

// Todo: 이거 분석해보기
// Load a program segment into pgdir.  addr must be page-aligned
// and the pages from addr to addr+sz must already be mapped.
int
loaduvm(pde_t *pgdir, char *addr, struct inode *ip, uint offset, uint sz)
{
  uint i, pa, n;
  pte_t *pte;

  if((uint) addr % PGSIZE != 0)
    panic("loaduvm: addr must be page aligned");
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, addr+i, 0)) == 0)
      panic("loaduvm: address should exist");
    pa = PTE_ADDR(*pte);
    if(sz - i < PGSIZE)
      n = sz - i;
    else
      n = PGSIZE;
    if(readi(ip, P2V(pa), offset+i, n) != n)
      return -1;
  }
  return 0;
}

// 해당 크기만큼 메모리 공간을 넓혀줌
// Allocate page tables and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
int
allocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  char *mem;
  uint a;

  if(newsz >= KERNBASE) // 메모리 크기가 커널 가상 메모리 공간을 침범하면 안됨
    return 0;
  if(newsz < oldsz) // 더 작게되면 그냥 쓰라고 리턴
    return oldsz;

  a = PGROUNDUP(oldsz); // 페이지 반올림
  for(; a < newsz; a += PGSIZE){ // 페이지 단위로 newsz보다 커질때까지 반복해서 페이지 테이블 생성, 메모리 공간 생성 및 매핑
    mem = kalloc(); // 자유 메모리에서 메모리 공간 할당. 해당 메모리의 가상 주소임
    if(mem == 0){ // 실패하면 이전꺼로 복구시킴
      cprintf("allocuvm out of memory\n");
      deallocuvm(pgdir, newsz, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pgdir, (char*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0){ // 페이지 디렉토리, 페이지 테이블에 할당받은 메모리를 가상-물리 주소간 매핑시켜버림
      cprintf("allocuvm out of memory (2)\n");
      deallocuvm(pgdir, newsz, oldsz);
      kfree(mem);
      return 0;
    }
  }
  return newsz;
}

// 여분의 크기만큼을 할당해제시키는듯
// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
int
deallocuvm(pde_t *pgdir, uint oldsz, uint newsz)
{
  pte_t *pte;
  uint a, pa;

  if(newsz >= oldsz)
    return oldsz;

  a = PGROUNDUP(newsz);
  for(; a  < oldsz; a += PGSIZE){
    pte = walkpgdir(pgdir, (char*)a, 0);
    if(!pte)
      a = PGADDR(PDX(a) + 1, 0, 0) - PGSIZE;
    else if((*pte & PTE_P) != 0){
      pa = PTE_ADDR(*pte);
      if(pa == 0)
        panic("kfree");
      char *v = P2V(pa);
      kfree(v);
      *pte = 0;
    }
  }
  return newsz;
}

// 페이지 디렉토리의 모든 내용을 정리함
// Free a page table and all the physical memory pages
// in the user part.
void
freevm(pde_t *pgdir)
{
  uint i;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  deallocuvm(pgdir, KERNBASE, 0);
  for(i = 0; i < NPDENTRIES; i++){
    if(pgdir[i] & PTE_P){
      char * v = P2V(PTE_ADDR(pgdir[i]));
      kfree(v);
    }
  }
  kfree((char*)pgdir);
}

// Clear PTE_U on a page. Used to create an inaccessible
// page beneath the user stack.
void
clearpteu(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if(pte == 0)
    panic("clearpteu");
  *pte &= ~PTE_U;
}

// 해당 페이지 테이블의 내용을 그대로 복사한 페이지 디렉토리를 리턴
// 자식 프로세스를 위해 만드는듯
// Given a parent process's page table, create a copy
// of it for a child.
pde_t*
copyuvm(pde_t *pgdir, uint sz)
{
  pde_t *d;
  pte_t *pte;
  uint pa, i, flags;
  char *mem;

  if((d = setupkvm()) == 0)
    return 0;
  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walkpgdir(pgdir, (void *) i, 0)) == 0)
      panic("copyuvm: pte should exist");
    if(!(*pte & PTE_P))
      panic("copyuvm: page not present");
    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto bad;
    memmove(mem, (char*)P2V(pa), PGSIZE);
    if(mappages(d, (void*)i, PGSIZE, V2P(mem), flags) < 0) {
      kfree(mem);
      goto bad;
    }
  }
  return d;

bad:
  freevm(d);
  return 0;
}

//PAGEBREAK!
// Map user virtual address to kernel address.
char*
uva2ka(pde_t *pgdir, char *uva)
{
  pte_t *pte;

  pte = walkpgdir(pgdir, uva, 0);
  if((*pte & PTE_P) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  return (char*)P2V(PTE_ADDR(*pte));
}

// Copy len bytes from p to user address va in page table pgdir.
// Most useful when pgdir is not the current page table.
// uva2ka ensures this only works for PTE_U pages.
int
copyout(pde_t *pgdir, uint va, void *p, uint len)
{
  char *buf, *pa0;
  uint n, va0;

  buf = (char*)p;
  while(len > 0){
    va0 = (uint)PGROUNDDOWN(va);
    pa0 = uva2ka(pgdir, (char*)va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (va - va0);
    if(n > len)
      n = len;
    memmove(pa0 + (va - va0), buf, n);
    len -= n;
    buf += n;
    va = va0 + PGSIZE;
  }
  return 0;
}

int
vm_getvp(pde_t *pgdir)
{
  uint i, j;
  int cnt = 0;
  pte_t *pgtab;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  for(i = 0; i < PDX(KERNBASE); i++){ // 전체 페이지 디렉토리를 탐색
    if(pgdir[i] & PTE_P){ // 만약 해당 페이지 디렉토리 엔트리가 존재할 경우
      pgtab = (pte_t*)P2V(PTE_ADDR(pgdir[i])); // 페이지 디렉토리 엔트리가 가리키는 페이지 테이블을 꺼냄
      for(j = 0; j < NPTENTRIES; j++){
        if (pgtab[j]) // 할당한 가상 페이지가 있다면 카운트 증가
          cnt++;
      }
    }
  }

  return cnt;
}

int
vm_getpp(pde_t *pgdir)
{
  uint i, j;
  int cnt = 0;
  pte_t *pgtab;

  if(pgdir == 0)
    panic("freevm: no pgdir");
  for(i = 0; i < PDX(KERNBASE); i++){ // 커널 베이스 이전까지의 전체 페이지 디렉토리를 탐색
    if(pgdir[i] & PTE_P){ // 만약 해당 페이지 디렉토리 엔트리가 존재할 경우
      pgtab = (pte_t*)P2V(PTE_ADDR(pgdir[i])); // 페이지 디렉토리 엔트리가 가리키는 페이지 테이블을 꺼냄
      for(j = 0; j < NPTENTRIES; j++){
        if (pgtab[j] & PTE_P) // 만약 페이지가 매핑이 되어있다면 카운트 증가
          cnt++;
      }
    }
  }

  return cnt;
}

// 가상주소만 할당함
// va에서 시작하는 가상주소에 대한 PTE를 생성. pa에서 시작하는 물리 주소를 참고하고 크기가 페이지 정렬되지 않을 수 있음
// pde가 가리키는 페이지 테이블이 없다면 생성.
// 가상주소를 참고해 페이지 단위로 각 페이지 테이블의 pte에 권한 설정만 진행. 물리 매핑이나 존재비트 설정x
static int
ssu_valloc(pde_t *pgdir, void *va, uint size, int perm)
{
  char *a, *last;
  pte_t *pte;

  a = (char*)PGROUNDDOWN((uint)va); // 시작 가상주소?? 페이지 크기에 맞춰반내림
  last = (char*)PGROUNDDOWN(((uint)va) + size - 1); // 가상주소 끝. 페이지 크기에 맞춰 반내림
  for(;;){
    if((pte = walkpgdir(pgdir, a, 1)) == 0) // 해당 가상주소의 페이지 디렉토리의 페이지 테이블이 없다면 생성하고 가상주소에 맞는 pte를 가져옴
      return -1;
    if(*pte) // 만약 해당 pte에 어떠한 값이라도 존재한다면 패닉.
      panic("remap");
    *pte = perm; // 해당 pte에 권한만 기록
    if(a == last) // 끝날때 까지
      break;
    a += PGSIZE;
  }
  return 0;
}

int
vm_ssualloc(pde_t *pgdir, uint oldsz, uint newsz)
{
  uint a;

  if(newsz >= KERNBASE || newsz < oldsz) // 메모리 크기가 커널 가상 메모리 공간을 침범하면 안됨
    return -1;

  a = PGROUNDUP(oldsz); // 페이지 반올림
  for(; a < newsz; a += PGSIZE){ // 페이지 단위로 newsz보다 커질때까지 반복해서 페이지 테이블 생성, 메모리 공간 생성 및 매핑
    if(ssu_valloc(pgdir, (char*)a, PGSIZE, PTE_W|PTE_U) < 0){ // 페이지 디렉토리, 페이지 테이블에 할당받은 메모리를 가상메모리만 매핑시켜버림
      cprintf("allocuvm out of memory (2)\n");
      return -1;
    }
  }
  return newsz;
}
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.
//PAGEBREAK!
// Blank page.

