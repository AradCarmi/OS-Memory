#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S
// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.



// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  
  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  #ifndef NONE
    if(p->pid > 2){
      release(&p->lock);
    if(createSwapFile(p) != 0){
      freeproc(p);
      return 0;
    }
      acquire(&p->lock);
    for(int i = 0; i < MAX_PSYC_PAGES; i++){
      p->ram[i].state = PUNUSED;
      p->swap[i].state = PUNUSED;
    }
    }
  #endif

  #ifdef SCFIFO
    p->next = 0;
  #endif

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  #ifndef NONE    
    for (int i = 0; i < MAX_PSYC_PAGES; i++){
      p->ram[i].state = PUNUSED;
      p->swap[i].state = PUNUSED;
    }
    p->swapFile = 0;
  #endif
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

#ifndef NONE
  void
  move_forward_byproc(int start,struct proc *p){
     
    for (int i = start+1; i < MAX_PSYC_PAGES; i++)
    {
      if(p->ram[i].state == PUSED){
        p->ram[i-1].state = p->ram[i].state;
        p->ram[i-1].addr = p->ram[i].addr;
        p->ram[i-1].pte = p->ram[i].pte;
      }
    }
    p->ram[MAX_PSYC_PAGES-1].state = PUNUSED;
    p->ram[MAX_PSYC_PAGES-1].addr = 0;
    p->ram[MAX_PSYC_PAGES-1].pte = 0;
  }

  void
  free_ram_byproc(int offset,struct proc *p){
    uint64 pa;
    
    pa = PTE2PA(*p->ram[offset].pte);
    if(pa == 0)
      panic("free_ram: pa is Null");
    kfree((void *)pa);
    p->ram[offset].state = PUNUSED;
    p->ram[offset].addr = 0;
    p->ram[offset].pte = 0;
    #ifdef SCFIFO
      p->next = 0;
      move_forward_byproc(offset,p);
    #endif
  }

  int
  find_free_slot_ram_byproc(struct proc *p){
    for (int i = 0; i < MAX_PSYC_PAGES; i++)
      {
      if(p->ram[i].state == PUNUSED)
          return i;
      }
      return -1;
  }


  int
  update_ram_byproc(uint64 va,pte_t *pte,struct proc *p){
    int i = find_free_slot_ram_byproc(p);
    // printf("i: %d %p\n",i,pte);
    if((i >= 0) && (p->ram[i].state == PUNUSED)){
      p->ram[i].state = PUSED;
      p->ram[i].addr = va;
      p->ram[i].pte = pte;
      return i;
    }
    return -1;
  }

  void
  add_page_byproc(pagetable_t pagetable,uint64 va,struct proc *p){
    
    // printf("proc name: %s pid: %d\n",p->name,p->pid);
    if(strncmp(p->name,"sh",3) != 0){
      if(p->pagetable == pagetable){
        pte_t *pte = walk(pagetable,va,0);
        if(pte == 0)
          panic("add_page_byproc: walk");
        if(occupied_ram_slots(p) == MAX_PSYC_PAGES)
          ram_to_disk(p);
        if(update_ram_byproc(va,pte,p) < 0)
          panic("add_page_byproc: update_ram_byproc");
      }
    }
  }

#endif

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

 
 

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  #ifdef SCFIFO
    np->next = p->next;
  #endif
  release(&np->lock);
 
 #ifndef NONE   
    for (int i = 0; i < MAX_PSYC_PAGES; i++)
  {
    // np->ram[i] = p->ram[i];
    // np->swap[i] = p->swap[i];
    if(p->ram[i].state == PUSED){
      // printf("i: %d\n",i);
      np->ram[i].addr = p->ram[i].addr;
      np->ram[i].state = p->ram[i].state;
      // np->ram[i].pte = p->ram[i].pte;
      np->ram[i].pte = walk(np->pagetable,np->ram[i].addr,0);
      printf("pid: %d  np pte: %p , p pte: %p\n",np->pid,np->ram[i].pte,p->ram[i].pte);
      #if defined(NFUA) || defined(LAPA)
        np->ram[i].counter = p->ram[i].counter;
      #endif
    }
    if(p->swap[i].state == PUSED){
      np->swap[i].addr = p->swap[i].addr;
      np->swap[i].state = p->swap[i].state;
      // np->swap[i].pte = p->swap[i].pte;
      np->swap[i].pte = walk(np->pagetable,np->swap[i].addr,0);
      
      printf("pid: %d swap np pte: %p , p pte: %p\n",np->pid,np->swap[i].pte,p->swap[i].pte);
    }
  }
  #endif
  
  #ifndef NONE
    if((p->swapFile) && (np->swapFile)){
        printf("copy swapfile\n");
        copy_swapfile(p,np);
        printf("end copy swapfile\n");
    }
  #endif

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  #ifndef NONE
     if(p->pid > 2){
    if (removeSwapFile(p) != 0)
      panic("remove swapfile");
  }
  #endif

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }
  

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  
  release(&wait_lock);
  
  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

#if defined(NFUA) || defined(LAPA)
  void
  update_page_counter(struct proc *p){
    printf("Update counters\n");
    for (int i = 0; i < MAX_PSYC_PAGES; i++)
    {
      if(p->ram[i].state == PUSED){
        p->ram[i].counter >>= 1; // shift one to the right
        if((*p->ram[i].pte & PTE_A) != 0){
          p->ram[i].counter |= 0x80000000; // turn the MSB
				  *p->ram[i].pte &= ~PTE_A;
				  sfence_vma();
        }
      }
    }
  }
#endif
// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  printf("yield\n");
  #if defined(NFUA) || defined(LAPA)
    update_page_counter(p);
  #endif
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

#ifndef NONE
  int
  occupied_ram_slots(struct proc *p){
    int counter = 0;
    for (int i = 0; i < MAX_PSYC_PAGES; i++)
    {
      if(p->ram[i].state == PUSED)
        counter++;
    }
    return counter;
  }
  int
  find_page_index(pte_t *pte,char c){
    struct proc *p = myproc();
    if(c == 's'){
      for (int i = 0; i < MAX_PSYC_PAGES; i++)
      {
        if(p->swap[i].state == PUSED){
          if(p->swap[i].pte == pte)
            return i;
        }
      }
      return -1;
    }else{
      for (int i = 0; i < MAX_PSYC_PAGES; i++)
      {
        
        if(p->ram[i].state == PUSED){
          if(p->ram[i].pte == pte)
            return i;
        }
      }
      return -1;
    }
  }

  int
  find_free_slot_ram(){
  struct proc *p = myproc();
  for (int i = 0; i < MAX_PSYC_PAGES; i++)
    {
    if(p->ram[i].state == PUNUSED)
        return i;
    }
    return -1;
  }

  #ifdef LAPA
    uint
    count_ones(uint num){
          uint counter = 0;
          while(num) {
            if (num % 2 != 0) 
              counter++;
            num /= 2;
          }
          return counter;
    }
  #endif

  int
  choose_page_by_algo(struct proc *p){
    #ifdef SCFIFO
      for (int i = p->next; i < MAX_PSYC_PAGES+1; i++)
      {
        if(p->ram[i%MAX_PSYC_PAGES].state == PUSED){
          if((*p->ram[i%MAX_PSYC_PAGES].pte & PTE_A) == 0){
            p->next = (i+1)%MAX_PSYC_PAGES;
            // printf("page: %d\n",i%MAX_PSYC_PAGES);
            return (i%MAX_PSYC_PAGES);
          }
          else{
            // printf("acc page: %d\n",i%MAX_PSYC_PAGES);
            *p->ram[i%MAX_PSYC_PAGES].pte &= ~PTE_A;
          }
        }
      }
    #endif
    #ifdef NFUA
      int min_index = 0;
      uint min_int = 0;
      for (int i = 0; i < MAX_PSYC_PAGES; i++)
      {
        printf("NFUA choose page pte: %p count: %d\n",p->ram[i].pte,p->ram[i].counter);
        if(i == 0){
          min_int = p->ram[i].counter;
        }else{
          if(p->ram[i].counter < min_int){
            min_int = p->ram[i].counter;
            min_index = i;
          }
        }
      }
      return min_index;
    #endif
    #ifdef LAPA
      int min_index = 0;
      uint min_int = 0;
      for (int i = 0; i < MAX_PSYC_PAGES; i++)
      {
        printf("LAPA choose page pte: %p count: %d\n",p->ram[i].pte,count_ones(p->ram[i].counter));
        if(i == 0){
          min_int = count_ones(p->ram[i].counter);
        }else{
          if(count_ones(p->ram[i].counter) < min_int){
            min_int = count_ones(p->ram[i].counter);
            min_index = i;
          }
        }
      }
      return min_index;
    #endif
    return -1;
  }
  
  void
  move_forward(int start){
    struct proc *p = myproc();
    for (int i = start+1; i < MAX_PSYC_PAGES; i++)
    {
      if(p->ram[i].state == PUSED){
        p->ram[i-1].state = p->ram[i].state;
        p->ram[i-1].addr = p->ram[i].addr;
        p->ram[i-1].pte = p->ram[i].pte;
      }
    }
    p->ram[MAX_PSYC_PAGES-1].state = PUNUSED;
    p->ram[MAX_PSYC_PAGES-1].addr = 0;
    p->ram[MAX_PSYC_PAGES-1].pte = 0;
  }

  void
  free_ram(int offset){
    struct proc *p = myproc();
    uint64 pa;
    
    pa = PTE2PA(*p->ram[offset].pte);
    if(pa == 0)
      panic("free_ram: pa is Null");
    kfree((void *)pa);
    p->ram[offset].state = PUNUSED;
    p->ram[offset].addr = 0;
    p->ram[offset].pte = 0;
    #ifdef SCFIFO
      p->next = 0;
      move_forward(offset);
    #endif
  }

  void
  free_swap(int offset){
    struct proc *p = myproc();
    p->swap[offset].state = PUNUSED;
    p->swap[offset].addr = 0;
    p->swap[offset].pte = 0;
    
  }
  // get an empty cell in ram array, return -1 if cannot find any free cell
  int
  update_ram(uint64 va,pte_t *pte){
    struct proc *p = myproc();
    int i = find_free_slot_ram();
    // printf("i: %d %p\n",i,pte);
    if((i >= 0) && (p->ram[i].state == PUNUSED)){
      p->ram[i].state = PUSED;
      p->ram[i].addr = va;
      p->ram[i].pte = pte;
      #ifdef NFUA
        p->ram[i].counter = 0;
      #endif
      #ifdef LAPA
        p->ram[i].counter = -1; // 0xFFFFFFFF
      #endif

      return i;
    }
    return -1;
  }

  int
  update_swap(struct proc *p,uint64 va, pte_t *pte){
  for (int i = 0; i < MAX_PSYC_PAGES; i++)
    {
      if(p->swap[i].state == PUNUSED){
        p->swap[i].state = PUSED;
        p->swap[i].addr = va;
        p->swap[i].pte = pte;
        return i;
      }
    }
    return -1;
  }

  void
  ram_to_disk(struct proc *p){
    
    int ram_i =  choose_page_by_algo(p);
    if(ram_i == -1)
      panic("choose page");
    int swap_i = update_swap(p,p->ram[ram_i].addr,p->ram[ram_i].pte);
    if(swap_i == -1)
      panic("update_swap");
    pte_t *pte = p->ram[ram_i].pte;
    printf("move page %p to disk\n",pte);
    (*pte) |= PTE_PG;
    (*pte) &= ~PTE_V; // pte = pte & ~PTE_PG
    writeToSwapFile(p,(char *)PTE2PA(*pte),PGSIZE*swap_i,PGSIZE);
    free_ram_byproc(ram_i,p);
    
    // flash to tlb after change in page table.
    sfence_vma();
  }

  void 
  init_pages(){
    struct proc *p = myproc();
    for (int i = 0; i < MAX_PSYC_PAGES; i++)
    {
      p->ram[i].state = PUNUSED;
      p->ram[i].addr = 0;
      p->ram[i].pte = 0;
      p->swap[i].state = PUNUSED;
      p->swap[i].addr = 0;
      p->swap[i].pte = 0;
    }
  }

  void
  add_page(pagetable_t pagetable,uint64 va){
    struct proc *p = myproc();
    // printf("proc name: %s pid: %d\n",p->name,p->pid);
    if((p->pid > 2) && (strncmp(p->name,"sh",3)) != 0){
      if(p->pagetable == pagetable){
        pte_t *pte = walk(pagetable,va,0);
        printf("pid: %d  va: %p  pte: %p\n",p->pid,va,pte);
        if(pte == 0)
          panic("add_page: walk");
        if(occupied_ram_slots(p) == MAX_PSYC_PAGES)
          ram_to_disk(p);
        if(update_ram(va,pte) < 0)
          panic("add_page: update_ram");
        
      }
    }
  }

  void
  update_ram_exe(){
    struct proc *p = myproc();
    for(int i = 0; i < p->sz; i += PGSIZE)
    {
      add_page(p->pagetable,i);
    }
  }
  int
  no_first_proc(){
    struct proc *p = myproc();
    if((p->pid > 2) && (strncmp(p->name,"sh",3)) != 0)
      return 1;
    return 0;

  }
#endif