#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct spinlock tickslock;
uint ticks;

extern char trampoline[], uservec[], userret[];

// in kernelvec.S, calls kerneltrap().
void kernelvec();

extern int devintr();

void
trapinit(void)
{
  initlock(&tickslock, "time");
}

// set up to take exceptions and traps while in the kernel.
void
trapinithart(void)
{
  w_stvec((uint64)kernelvec);
}

//
// handle an interrupt, exception, or system call from user space.
// called from trampoline.S
//
void
usertrap(void)
{
  int which_dev = 0;
  uint64 scause = r_scause();
  if((r_sstatus() & SSTATUS_SPP) != 0)
    panic("usertrap: not from user mode");

  // send interrupts and exceptions to kerneltrap(),
  // since we're now in the kernel.
  w_stvec((uint64)kernelvec);

  struct proc *p = myproc();
  
  // save user program counter.
  p->trapframe->epc = r_sepc();



  if(scause == 8){
    // system call

    if(p->killed)
      exit(-1);

    // sepc points to the ecall instruction,
    // but we want to return to the next instruction.
    p->trapframe->epc += 4;

    // an interrupt will change sstatus &c registers,
    // so don't enable until done with those registers.
    intr_on();

    syscall();
  } else if((which_dev = devintr()) != 0){
    // ok
  } else {
    #ifndef NONE
      //page fault
      if(((scause == 12))|| ((scause == 13)) || ((scause == 15))){
        if(handle_pgfault(myproc()) != -1)
          usertrapret();
      }
    #endif
    printf("usertrap(): unexpected scause %p pid=%d\n", r_scause(), p->pid);
    printf("            sepc=%p stval=%p\n", r_sepc(), r_stval());
    p->killed = 1;
  }

  if(p->killed)
    exit(-1);

  // give up the CPU if this is a timer interrupt.
  if(which_dev == 2)
    yield();

  usertrapret();
}

//
// return to user space
//
void
usertrapret(void)
{
  struct proc *p = myproc();

  // we're about to switch the destination of traps from
  // kerneltrap() to usertrap(), so turn off interrupts until
  // we're back in user space, where usertrap() is correct.
  intr_off();

  // send syscalls, interrupts, and exceptions to trampoline.S
  w_stvec(TRAMPOLINE + (uservec - trampoline));

  // set up trapframe values that uservec will need when
  // the process next re-enters the kernel.
  p->trapframe->kernel_satp = r_satp();         // kernel page table
  p->trapframe->kernel_sp = p->kstack + PGSIZE; // process's kernel stack
  p->trapframe->kernel_trap = (uint64)usertrap;
  p->trapframe->kernel_hartid = r_tp();         // hartid for cpuid()

  // set up the registers that trampoline.S's sret will use
  // to get to user space.
  
  // set S Previous Privilege mode to User.
  unsigned long x = r_sstatus();
  x &= ~SSTATUS_SPP; // clear SPP to 0 for user mode
  x |= SSTATUS_SPIE; // enable interrupts in user mode
  w_sstatus(x);

  // set S Exception Program Counter to the saved user pc.
  w_sepc(p->trapframe->epc);

  // tell trampoline.S the user page table to switch to.
  uint64 satp = MAKE_SATP(p->pagetable);

  // jump to trampoline.S at the top of memory, which 
  // switches to the user page table, restores user registers,
  // and switches to user mode with sret.
  uint64 fn = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64,uint64))fn)(TRAPFRAME, satp);
}

// interrupts and exceptions from kernel code go here via kernelvec,
// on whatever the current kernel stack is.
void 
kerneltrap()
{
  int which_dev = 0;
  uint64 sepc = r_sepc();
  uint64 sstatus = r_sstatus();
  uint64 scause = r_scause();
  

  if((sstatus & SSTATUS_SPP) == 0)
    panic("kerneltrap: not from supervisor mode");
  if(intr_get() != 0)
    panic("kerneltrap: interrupts enabled");

  
  

  if((which_dev = devintr()) == 0){
    #ifndef NONE
      //page fault
      if(((scause == 12))|| ((scause == 13)) || ((scause == 15))){
        if(handle_pgfault(myproc()) != -1)
          usertrapret();
      }
    #endif
    printf("scause %p\n", scause);
    printf("sepc=%p stval=%p\n", r_sepc(), r_stval());
    panic("kerneltrap");
  }

  

  // the yield() may have caused some traps to occur,
  // so restore trap registers for use by kernelvec.S's sepc instruction.
  w_sepc(sepc);
  w_sstatus(sstatus);
}

void
clockintr()
{
  acquire(&tickslock);
  ticks++;
  wakeup(&ticks);
  release(&tickslock);
}

// check if it's an external interrupt or software interrupt,
// and handle it.
// returns 2 if timer interrupt,
// 1 if other device,
// 0 if not recognized.
int
devintr()
{
  uint64 scause = r_scause();

  if((scause & 0x8000000000000000L) &&
     (scause & 0xff) == 9){
    // this is a supervisor external interrupt, via PLIC.

    // irq indicates which device interrupted.
    int irq = plic_claim();

    if(irq == UART0_IRQ){
      uartintr();
    } else if(irq == VIRTIO0_IRQ){
      virtio_disk_intr();
    } else if(irq){
      printf("unexpected interrupt irq=%d\n", irq);
    }

    // the PLIC allows each device to raise at most one
    // interrupt at a time; tell the PLIC the device is
    // now allowed to interrupt again.
    if(irq)
      plic_complete(irq);

    return 1;
  } else if(scause == 0x8000000000000001L){
    // software interrupt from a machine-mode timer interrupt,
    // forwarded by timervec in kernelvec.S.

    if(cpuid() == 0){
      clockintr();
    }
    
    // acknowledge the software interrupt by clearing
    // the SSIP bit in sip.
    w_sip(r_sip() & ~2);

    return 2;
  } else {
    return 0;
  }
}

#ifndef NONE
int
import_pg(struct proc *p,uint64 s_va,pte_t *pte){
  uint64 pa;
  int offset;
  
  if((pa = (uint64)kalloc()) == 0){
    panic("kalloc");
  }
  if(mappages(p->pagetable,s_va,PGSIZE,pa,PTE_W|PTE_X|PTE_R|PTE_U) != 0){
      panic("import_pg: mappages");
  }
  
  if((offset = find_page_index(pte,'s')) < 0)
    return -1;
  if(readFromSwapFile(p,(char *)pa,PGSIZE*offset,PGSIZE) < 0)
    return -1;
  free_swap(offset);
  if(occupied_ram_slots(p) == MAX_PSYC_PAGES)
    ram_to_disk(p);
  (*pte) |= PTE_V;
  (*pte) &= ~PTE_PG; // pte = pte & ~PTE_PG
  printf("page %p imported from disk\n",pte);
  if (update_ram(s_va,pte) < 0)
    return -1;
  // flash to tlb after change in page table.
  sfence_vma();
  return 0;
} 

int
handle_pgfault(struct proc *p){
  uint64 f_va = r_stval();
  uint64 s_va = PGROUNDDOWN(f_va);
  pte_t *pte = walk(p->pagetable,s_va,0);
  
  
  if(pte == 0)
    panic("pte is null");
  if((*pte & PTE_V))
    return -1;
  if(!(*pte & PTE_PG)){
    printf("pid: %d s_val: %p pte: %p\n",p->pid,s_va,pte);
    for (int i = 0; i < MAX_PSYC_PAGES; i++)
    {
      printf("index: %d s_val: %p pte: %p\n",i,p->ram[i].addr,p->ram[i].pte);
    }
     for (int i = 0; i < MAX_PSYC_PAGES; i++)
    {
      if(p->swap[i].state == PUSED)
      printf("index: %d s_val: %p pte: %p\n",i,p->swap[i].addr,p->swap[i].pte);
    }
    return -1;
  }
  return import_pg(p,s_va,pte);
}
#endif
