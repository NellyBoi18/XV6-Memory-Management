# xv6 Memory Mapping

Implemented the `mmap` system call in xv6. The version I implemented is a simplified version of the actual `mmap` in the Linux kernel. This simplification makes my version of mmap slightly different than what you find in Linux.

Memory mapping, as the name suggests, is the process of mapping memory in a process **"virtual"** address space to some physical memory. It is supported through a pair of system calls, `mmap` and `munmap`. First, you call mmap to get a pointer to a memory region (with the size you specified) that you can access and use. When you no longer need that memory, you call `munmap` to remove that mapping from the process address space.

# Project details

## mmap

`mmap` has two modes of operation. First is **"anonymous"** memory allocation, aspects of `mmap` that are similar to `malloc`. The real power of mmap comes through the support of **"file-backed"** mapping. Wait - what does file-backed mapping mean? It means you create a memory representation of a file. Reading data from that memory region is the same as reading data from the file. What happens if we write to the memory backed by a file? Will it be reflected in the file? Well, that depends on the flags you use for `mmap`. When the flag MAP_SHARED is used, you need to write the (perhaps modified) contents of the memory back to the file upon `munmap`.

This is the `mmap` system call signature:
`void *mmap(void *addr, size_t length, int prot, int flags, int fd, off_t offset)`

* addr: Depending on the flags (MAP_FIXED, more on that later), it could be a hint for what "virtual" address the mmap should place the mapping at, or the "virtual" address that mmap MUST place the mapping.

* length: The length of the mapping in bytes

* prot: It indicates what operations are allowed in this region (read/write). It can be the bitwise OR of one or more protection flags (e.g., `PROT_READ | PROT_WRITE`). For this project, we assume the protection flag will always be `PROT_READ | PROT_WRITE`.

* fd: If it's a file-backed mapping, this is the file descriptor for the file to be mapped. In case of MAP_ANONYMOUS (see flags), I ignored this argument.

* offset: The offset into the file. For simplicity, I assumed that this argument will always be 0.

* flags: The kind of memory mapping you are requesting for. Flags can be *ORed* together (e.g., `MAP_PRIVATE | MAP_ANONYMOUS`). You should define these flags as constants in `mmap.h` header file. Use the snippet provided in the *Hints* section. If you look at the man page, there are many flags for various purposes. In my implementation, I only implemented these flags:

a) MAP_ANONYMOUS: It's NOT a file-backed mapping. Ignore the last two arguments (fd and offset) if this flag is provided.

b) MAP_SHARED: This flag tells mmap that the mapping is shared. Memory mappings are copied from the parent to the child across the `fork` system call. If the mapping is MAP_SHARED, then changes made by the child will be visible to the parent, and vice versa. However, if the mapping is MAP_PRIVATE, each process will have its own copy of the mapping.

c) MAP_PRIVATE: Mapping is not shared. Still need to copy the mappings from parent to child, but these mappings should use different "physical" pages. In other words, the same virtual addresses are mapped in child, but to a different set of physical pages. That will cause modifications to memory be invisible to other processes. Moreover, if it's a file-backed mapping, modification to memory are NOT carried through to the underlying file. See 'Best Practices' section for a guide on implementation. Between the flags MAP_SHARED and MAP_PRIVATE, one of them must be specified in flags.

d) MAP_FIXED: This has to do with the first argument of the `mmap` - the *addr* argument. Without MAP_FIXED, this address would be interpreted as a *hint* to where the mapping should be placed. If MAP_FIXED is set, then the mapping MUST be placed at exactly *"addr"*. In this project, I only implemented the latter. In other words, I don't care about the addr argument, unless MAP_FIXED has been set. <!-- Also, a valid `addr` will be a multiple of page size. -->

e) MAP_GROWSUP: When it is set, it makes the memory mapping grow upward as needed. Specifically, if you have a mapping at *addr = X* with *length = L*, this means you can access virtual addresses in the range [X, X + L) without getting segmentation fault from the OS. In fact, that's the case when you don't have the MAP_GROWSUP flag. If you have MAP_GROWSUP set, of course you can access to pages that you have allocated, but additionally, touching the **"guard page"** can grow the mapping upward. The guard page is defined to be the page right above the mapping - i.e. the first page above the high end of the mapping. So in this case, touching any virtual address in the range [X + L, X + L + PGSIZE) will extend the mapping by one page (PGSIZE is the page size of the system).
For example, if you try to write to the address *X + L*, since it's within the guard page, mapping will grow by one page. Hence the new mapping becomes [X, X + L + PGSIZE). Please note that this growing can happen again if you touch the guard page (e.g. access to address = X + L + PGSIZE). This growing can continue until it grows to within a page of the low end of the next higher mapping. Simply put, there should be a minimum *"distance"* of 1 page between this mapping and the low end of a higher mapping. This also means that when you're allocating a mapping with MAP_GROWSUP being set, you should make sure there's at least ONE page of margin in the high end of the mapping.

The following scenario should clarify things about MAP_GROWSUP:
Suppose we have 10 pages of virtual memory that we can use for `mmap`. Initially, no mapping exists, so it will look like the following:
| Page # | Mapping ID |
|:--------:|:------------:|
10 |  |
9 |  |
8 |  |
7 |  |
6 |  |
5 |  |
4 |  |
3 |  |
2 |  |
1 |  |


Then we do a mapping of *length = 1* page with MAP_GROWSUP set. The system decides to place this mapping at the sixth page (page # = 6). Now, the memory is as follows:
| Page # | Mapping ID |
|:--------:|:------------:|
10 |  |
9 |  |
8 |  |
7 |  |
6 | A |
5 |  | 
4 |  |
3 |  |
2 |  |
1 |  |

In this state, touching any page except pages 6 and 7 will result in a page fault and kills the program. For page number 6 it's obvious why it should not fault - because we have that memory allocated for us. But for page number 7, it's due to the MAP_GROWSUP flag. It's the guard page here - touching it will result the mapping to become:
| Page # | Mapping ID |
|:--------:|:------------:|
10 |  |
9 |  |
8 |  |
7 | A |
6 | A |
5 |  | 
4 |  |
3 |  |
2 |  |
1 |  |

Now, page number 8 becomes the guard page by definition. But before we access any pages, we call mmap again, with the length = 2 pages. The system decides to place this mapping at the high end of the memory. So the memory will look like this:
| Page # | Mapping ID |
|:--------:|:------------:|
10 | B |
9 | B |
8 |  |
7 | A |
6 | A |
5 |  | 
4 |  |
3 |  |
2 |  |
1 |  |

If we touch page 8 now, it will try to grow the mapping because it's the guard page. But it cannot - since it would violate the margin constraint. The low end of the upper mapping is page 9, and the high end of the lower mapping is page 7. Page 8 is the margin here - we cannot append it to the upper mapping. As a result, an access to page 8 results in a segmentation fault and kills the program.


## munmap
The signature of the `munmap()` system call looks like the following:

`int munmap(void *addr, size_t length)`

It removes `length` bytes starting at addr from the process virtual address space. If it's a file-backed mapping with MAP_SHARED, it writes memory data back to the file to ensure the file remains up-to-date. <!-- If `length` is not a multiple of the page size, then you need to round it *up* to the closest multiple of page size. -->

## Return values
`mmap` returns the starting virtual address of the allocated memory on success, and `(void *)-1` on failure. *That virtual address must be a multiple of page size.*

`munmap` returns 0 to indicate success, and -1 for failure.

## Modified `fork()` system call
Memory mappings should be inherited by the child. To do this, I modified the `fork()` system call to copy the mappings from the parent process to the child across `fork()`.

## Modified `exit()` system call
I modified the `exit()` so that it removes all the mappings from current process address space. This is to prevent memory leaks when a user program forgets to call `munmap` before exiting.

## A note on address allocation
If the MAP_FIXED flag is not set, then I needed to find an available region in the virtual address space. To do this, you should first understand the memory layout of xv6. In xv6, higher memory addresses are always used to map the kernel. Specifically, there's a constant defined as **KERNBASE** and has the value of **0x80000000**. This means that only "virtual" addresses between 0 and **KERNBASE** are used for user space processes. Lower addresses in this range are used to map text, data and stack sections. `mmap` will use higher addresses in the user space, which are inside the heap section.
For this project, I only used addresses between **0x60000000** and **0x80000000** to serve `mmap` requests.

### Best practices
My implementation of `mmap` does "*lazy allocation*". Lazy allocation means that you don't actually allocate any physical pages when `mmap` is called. You just keep track of the allocated region using some structure. Later, when the process tries to access that memory, a page fault is generated which is handled by the kernel. In the kernel, you can now allocate a physical page and let the user resume execution. Lazy allocation makes large memory mappings fast.

For MAP_PRIVATE, a naive implementation may just duplicate all the physical pages at the beginning. Note that we don't want writes from one process to be visible to the other one, and that's why we duplicate the physical pages. However, an idea similar to "lazy allocation" can be applied to make it more efficient. It is called "*copy-on-write*". The idea is that as long as both processes are only reading memory pages, having a single copy of the physical pages would be sufficient. When one of the process tries a write to a virtual page, we duplicate the physical page that virtual page maps, so that both processes now have their own physical copy.