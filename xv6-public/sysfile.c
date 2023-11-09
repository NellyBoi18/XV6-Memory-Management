//
// File-system system calls.
// Mostly argument checking, since we don't trust
// user code, and calls into file.c and fs.c.
//

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "fcntl.h"
#include "mmap.h"
#include "memlayout.h"

// Fetch the nth word-sized system call argument as a file descriptor
// and return both the descriptor and the corresponding struct file.
static int
argfd(int n, int *pfd, struct file **pf)
{
  int fd;
  struct file *f;

  if(argint(n, &fd) < 0)
    return -1;
  if(fd < 0 || fd >= NOFILE || (f=myproc()->ofile[fd]) == 0)
    return -1;
  if(pfd)
    *pfd = fd;
  if(pf)
    *pf = f;
  return 0;
}

// Allocate a file descriptor for the given file.
// Takes over file reference from caller on success.
static int
fdalloc(struct file *f)
{
  int fd;
  struct proc *curproc = myproc();

  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd] == 0){
      curproc->ofile[fd] = f;
      return fd;
    }
  }
  return -1;
}

int
sys_dup(void)
{
  struct file *f;
  int fd;

  if(argfd(0, 0, &f) < 0)
    return -1;
  if((fd=fdalloc(f)) < 0)
    return -1;
  filedup(f);
  return fd;
}

int
sys_read(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return fileread(f, p, n);
}

int
sys_write(void)
{
  struct file *f;
  int n;
  char *p;

  if(argfd(0, 0, &f) < 0 || argint(2, &n) < 0 || argptr(1, &p, n) < 0)
    return -1;
  return filewrite(f, p, n);
}

int
sys_close(void)
{
  int fd;
  struct file *f;

  if(argfd(0, &fd, &f) < 0)
    return -1;
  myproc()->ofile[fd] = 0;
  fileclose(f);
  return 0;
}

int
sys_fstat(void)
{
  struct file *f;
  struct stat *st;

  if(argfd(0, 0, &f) < 0 || argptr(1, (void*)&st, sizeof(*st)) < 0)
    return -1;
  return filestat(f, st);
}

// Create the path new as a link to the same inode as old.
int
sys_link(void)
{
  char name[DIRSIZ], *new, *old;
  struct inode *dp, *ip;

  if(argstr(0, &old) < 0 || argstr(1, &new) < 0)
    return -1;

  begin_op();
  if((ip = namei(old)) == 0){
    end_op();
    return -1;
  }

  ilock(ip);
  if(ip->type == T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }

  ip->nlink++;
  iupdate(ip);
  iunlock(ip);

  if((dp = nameiparent(new, name)) == 0)
    goto bad;
  ilock(dp);
  if(dp->dev != ip->dev || dirlink(dp, name, ip->inum) < 0){
    iunlockput(dp);
    goto bad;
  }
  iunlockput(dp);
  iput(ip);

  end_op();

  return 0;

bad:
  ilock(ip);
  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);
  end_op();
  return -1;
}

// Is the directory dp empty except for "." and ".." ?
static int
isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for(off=2*sizeof(de); off<dp->size; off+=sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if(de.inum != 0)
      return 0;
  }
  return 1;
}

//PAGEBREAK!
int
sys_unlink(void)
{
  struct inode *ip, *dp;
  struct dirent de;
  char name[DIRSIZ], *path;
  uint off;

  if(argstr(0, &path) < 0)
    return -1;

  begin_op();
  if((dp = nameiparent(path, name)) == 0){
    end_op();
    return -1;
  }

  ilock(dp);

  // Cannot unlink "." or "..".
  if(namecmp(name, ".") == 0 || namecmp(name, "..") == 0)
    goto bad;

  if((ip = dirlookup(dp, name, &off)) == 0)
    goto bad;
  ilock(ip);

  if(ip->nlink < 1)
    panic("unlink: nlink < 1");
  if(ip->type == T_DIR && !isdirempty(ip)){
    iunlockput(ip);
    goto bad;
  }

  memset(&de, 0, sizeof(de));
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("unlink: writei");
  if(ip->type == T_DIR){
    dp->nlink--;
    iupdate(dp);
  }
  iunlockput(dp);

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  end_op();

  return 0;

bad:
  iunlockput(dp);
  end_op();
  return -1;
}

static struct inode*
create(char *path, short type, short major, short minor)
{
  struct inode *ip, *dp;
  char name[DIRSIZ];

  if((dp = nameiparent(path, name)) == 0)
    return 0;
  ilock(dp);

  if((ip = dirlookup(dp, name, 0)) != 0){
    iunlockput(dp);
    ilock(ip);
    if(type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if(type == T_DIR){  // Create . and .. entries.
    dp->nlink++;  // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if(dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if(dirlink(dp, name, ip->inum) < 0)
    panic("create: dirlink");

  iunlockput(dp);

  return ip;
}

int
sys_open(void)
{
  char *path;
  int fd, omode;
  struct file *f;
  struct inode *ip;

  if(argstr(0, &path) < 0 || argint(1, &omode) < 0)
    return -1;

  begin_op();

  if(omode & O_CREATE){
    ip = create(path, T_FILE, 0, 0);
    if(ip == 0){
      end_op();
      return -1;
    }
  } else {
    if((ip = namei(path)) == 0){
      end_op();
      return -1;
    }
    ilock(ip);
    if(ip->type == T_DIR && omode != O_RDONLY){
      iunlockput(ip);
      end_op();
      return -1;
    }
  }

  if((f = filealloc()) == 0 || (fd = fdalloc(f)) < 0){
    if(f)
      fileclose(f);
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  end_op();

  f->type = FD_INODE;
  f->ip = ip;
  f->off = 0;
  f->readable = !(omode & O_WRONLY);
  f->writable = (omode & O_WRONLY) || (omode & O_RDWR);
  return fd;
}

int
sys_mkdir(void)
{
  char *path;
  struct inode *ip;

  begin_op();
  if(argstr(0, &path) < 0 || (ip = create(path, T_DIR, 0, 0)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_mknod(void)
{
  struct inode *ip;
  char *path;
  int major, minor;

  begin_op();
  if((argstr(0, &path)) < 0 ||
     argint(1, &major) < 0 ||
     argint(2, &minor) < 0 ||
     (ip = create(path, T_DEV, major, minor)) == 0){
    end_op();
    return -1;
  }
  iunlockput(ip);
  end_op();
  return 0;
}

int
sys_chdir(void)
{
  char *path;
  struct inode *ip;
  struct proc *curproc = myproc();
  
  begin_op();
  if(argstr(0, &path) < 0 || (ip = namei(path)) == 0){
    end_op();
    return -1;
  }
  ilock(ip);
  if(ip->type != T_DIR){
    iunlockput(ip);
    end_op();
    return -1;
  }
  iunlock(ip);
  iput(curproc->cwd);
  end_op();
  curproc->cwd = ip;
  return 0;
}

int
sys_exec(void)
{
  char *path, *argv[MAXARG];
  int i;
  uint uargv, uarg;

  if(argstr(0, &path) < 0 || argint(1, (int*)&uargv) < 0){
    return -1;
  }
  memset(argv, 0, sizeof(argv));
  for(i=0;; i++){
    if(i >= NELEM(argv))
      return -1;
    if(fetchint(uargv+4*i, (int*)&uarg) < 0)
      return -1;
    if(uarg == 0){
      argv[i] = 0;
      break;
    }
    if(fetchstr(uarg, &argv[i]) < 0)
      return -1;
  }
  return exec(path, argv);
}

int
sys_pipe(void)
{
  int *fd;
  struct file *rf, *wf;
  int fd0, fd1;

  if(argptr(0, (void*)&fd, 2*sizeof(fd[0])) < 0)
    return -1;
  if(pipealloc(&rf, &wf) < 0)
    return -1;
  fd0 = -1;
  if((fd0 = fdalloc(rf)) < 0 || (fd1 = fdalloc(wf)) < 0){
    if(fd0 >= 0)
      myproc()->ofile[fd0] = 0;
    fileclose(rf);
    fileclose(wf);
    return -1;
  }
  fd[0] = fd0;
  fd[1] = fd1;
  return 0;
}

void*
sys_mmap(void)
{
  // Get args
  void *addr;
  int length;
  int prot;
  int flags;
  int fd;
  int offset;

  if ((argptr(0, (void*)&addr, sizeof(addr)) < 0) 
      || (argint(1, &length) < 0) 
      || (argint(2, &prot) < 0) 
      || (argint(3, &flags) < 0) 
      || (argint(4, &fd) < 0) 
      || (argint(5, &offset) < 0)) {
    return (void*) -1;
  }
 
  // Error Checking
  if (length <= 0 || (prot & ~(PROT_READ | PROT_WRITE)) || ((flags & MAP_ANONYMOUS) && fd != -1)) {
    return (void *) -1;
  }

  struct proc *curproc = myproc();
  int aligned_length = ((length + PGSIZE - 1) / PGSIZE) * PGSIZE;
  int found;
  int i;
  void *start_addr;

  // Align addr to page boundary if MAP_FIXED is set
  if ((flags & MAP_FIXED) && ((uint)addr % PGSIZE != 0)) {
    return (void *) -1;
  }

  // Find an available memory range and mapping slot
  for (i = 0; i < MAX_MMAPS; i++) {
    if (curproc->mmaps[i].is_used == 0) {
      found = 0;

      if (flags & MAP_ANONYMOUS) {
        // Handle anonymous mapping
        // Ignore fd and offset

        // Validate and find a suitable range if MAP_FIXED is not set
        if (!(flags & MAP_FIXED)) {
            addr = find_suitable_range(length);
            if (addr == 0) {
                return -1;  // Unable to find a suitable range
            }
        }

        // Create a new mmap region in the current process
        struct mmap_region region;
        region.addr = addr;
        region.length = length;
        region.flags = flags;
        region.prot = prot;

        // Add this region to the process's list of memory mappings
        if (add_mmap_region_to_process(myproc(), &region) < 0) {
            return (void*) -1;  // Failure in adding the region
        }

        // Return the start address of the mapped region
        return (void *)addr;
      }

      // If MAP_FIXED is set, use the specified addr
      if ((flags & MAP_FIXED) != 0) {
        start_addr = addr;
        void *new_end = start_addr + aligned_length;

        // Check for overlap with existing mappings
        int j;
        for (j = 0; j < MAX_MMAPS; j++) {
          if (curproc->mmaps[j].is_used) {
            void *existing_start = curproc->mmaps[j].addr;
            int existing_length = curproc->mmaps[j].length;
            void *existing_end = existing_start + existing_length;

            // Check for overlap
            if ((start_addr >= existing_start && start_addr < existing_end) ||
              (new_end > existing_start && new_end <= existing_end)) {
              return (void *) -1; // Overlap detected, range not available
            }
          }
        }
        found = 1; // No overlap, range is available
      } else {
        // Check for overlapping with existing mappings
        int j;
        for (j = 0; j < MAX_MMAPS; j++) {
          if (curproc->mmaps[j].is_used && j != i) {
            void *existing_start = curproc->mmaps[j].addr;
            int existing_length = curproc->mmaps[j].length;

            // Calculate the end addresses
            void *new_end = start_addr + aligned_length;
            void *existing_end = existing_start + existing_length;

            // Check for overlap
            if ((start_addr >= existing_start && start_addr < existing_end) ||
              (new_end > existing_start && new_end <= existing_end)) {
              found = 0; // Overlap detected, this range is not suitable
              break;
            }
          }
        }
      }

      if (found) {
        curproc->mmaps[i] = (struct mmap_region){start_addr, aligned_length, prot, flags, 1};
        return start_addr;
      }
    }
  }

  // Handle MAP_ANONYMOUS
  if (flags & MAP_ANONYMOUS) {
    // TODO: Allocate virtual memory (lazy allocation)
    // Return the starting address of allocated memory
  }

  // Handle file-backed mapping
  if (!(flags & MAP_ANONYMOUS)) {
    // TODO: Map the file into memory
    // This part is complex and requires file system modifications
  }

  // No available mapping slots
  return (void *) -1;

  //return mmap(addr, length, prot, flags, fd, offset);
}

int add_mmap_region_to_process(struct proc *p, struct mmap_region *region) {
    for (int i = 0; i < MAX_MMAPS; i++) {
        if (p->mmaps[i] == 0) {
            p->mmaps[i] = region;
            return 0; // Successfully added
        }
    }
    return -1; // No space left in the mmap_regions array
}

int
sys_munmap(void)
{
  // Get args
  void *addr;
  int length;

  if ((argptr(0, (void*)&addr, sizeof(addr)) < 0) 
      || (argint(1, &length) < 0)) {
    return -1;
  }

  // Error Checking
  if ((uint)addr % PGSIZE != 0 || length <= 0) {
    return -1;
  }

  struct proc *curproc = myproc();
  pte_t *pte;
  uint pa, start, end;

  // Iterate over all memory mappings
  for (int i = 0; i < MAX_MMAPS; i++) {
    if (curproc->mmaps[i].is_used) {
      start = (uint)curproc->mmaps[i].addr;
      end = start + curproc->mmaps[i].length;

      // Check if the unmapping request overlaps with this region
      if ((uint)addr + length > start && (uint)addr < end) {
        uint unmap_start = max(start, (uint)addr);
        uint unmap_end = min(end, (uint)addr + length);

        // Release the memory within the specified range
        for (uint a = unmap_start; a < unmap_end; a += PGSIZE) {
          pte = walkpgdir(curproc->pgdir, (void*)a, 0);
          if (pte && (*pte & PTE_P)) {
            pa = PTE_ADDR(*pte);
            if (pa) {
              kfree(P2V(pa));
              *pte = 0;
            }
          }
        }

        // Adjust the existing mmap region or split it
        if (unmap_start > start && unmap_end < end) {
          // Unmapping from the middle: split the region
          int new_index = -1;
          // Find a free mmap slot for the new region
          for (int j = 0; j < MAX_MMAPS; j++) {
            if (!curproc->mmaps[j].is_used) {
              new_index = j;
              break;
            }
          }
          if (new_index == -1) {
            // No space for new mmap region
            return -1;
          }

          // Create a new mmap region for the second part
          curproc->mmaps[new_index].addr = (void*)unmap_end;
          curproc->mmaps[new_index].length = end - unmap_end;
          curproc->mmaps[new_index].is_used = 1;

          // Adjust the length of the original region
          curproc->mmaps[i].length = unmap_start - start;
        } else if (unmap_start > start) {
          // Shrink from the start
          curproc->mmaps[i].length -= unmap_start - start;
          curproc->mmaps[i].addr = (void*)unmap_start;
        } else if (unmap_end < end) {
          // Shrink from the end
          curproc->mmaps[i].length -= end - unmap_end;
        }

        switchuvm(curproc); // Flush TLB
        return 0;
      }
    }
  }

  // No matching mapping found
  return -1;

  // return munmap(addr, length);
}