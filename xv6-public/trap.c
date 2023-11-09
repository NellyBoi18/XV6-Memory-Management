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
extern int mappages(pde_t *pgdir, void *va, uint size, uint pa, int perm);
extern pte_t* walkpgdir(pde_t *pgdir, const void *va, int alloc);

/*
int is_cow_fault(uint faulting_address) {
    // Function to check if the fault is due to a write to a read-only page in a MAP_PRIVATE mapping
    // This involves checking the page table entry for the faulting address
}

void handle_cow_fault(uint faulting_address) {
    // Function to handle copy-on-write fault
    // This involves allocating a new page, copying the content of the old page to the new one, and updating the page table
}
*/

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  // Handle page faults (interrupt number 14)
  if (tf->trapno == 14) {
    uint faulting_address = rcr2(); // Get faulting address
    struct proc *curproc = myproc();

    // Check if faulting address is within a region mapped by mmap
    for (int i = 0; i < MAX_MMAPS; i++) {
      if (curproc->mmaps[i].is_used) {
        void *start = curproc->mmaps[i].addr;
        void *end = start + curproc->mmaps[i].length;

        // Check if the faulting address is within this mmap region
        if (faulting_address >= (uint)start && faulting_address < (uint)end) {
          // Allocate a physical page and map it
          char *mem = kalloc(); // Allocate one page of physical memory
          if (mem == 0) {
            cprintf("Out of memory (lazy allocation)\n");
            curproc->killed = 1;
            return;
          }
          memset(mem, 0, PGSIZE);

          // Map the physical page to the faulting address
          if (mappages(curproc->pgdir, (void*)PGROUNDDOWN(faulting_address), PGSIZE, V2P(mem), PTE_W|PTE_U) < 0) {
            cprintf("mappages failed (lazy allocation)\n");
            kfree(mem);
            curproc->killed = 1;
            return;
          }

          return; // Successfully handled lazy allocation
        }
      }
    }

    // Check for write to a read-only page in a MAP_PRIVATE mapping
    pte_t *pte = walkpgdir(myproc()->pgdir, (void *)faulting_address, 0);
    if (pte && (*pte & PTE_P) && !(*pte & PTE_W)) { // CoW fault if page is present and not writable
      
      struct proc *curproc2 = myproc();
      char *mem = kalloc();
      if (mem == 0) {
          cprintf("Out of memory (CoW)\n");
          curproc2->killed = 1;
          return;
      }

      uint a = PGROUNDDOWN(faulting_address);
      memmove(mem, (char*)a, PGSIZE);

      if (mappages(curproc2->pgdir, (void*)a, PGSIZE, V2P(mem), PTE_W|PTE_U) < 0) {
          cprintf("mappages failed (CoW)\n");
          kfree(mem);
          curproc2->killed = 1;
          return;
      }
      
      
      return;
    }

    // If this is reached, the faulting address is not within a lazily allocated region
    cprintf("Segmentation Fault\n");
    curproc->killed = 1;
    return;
  }

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
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
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

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
