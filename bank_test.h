#define PAGE_SHIFT                      12
#define PAGE_SIZE                       (1 << PAGE_SHIFT)
#define PAGE_MASK                       (PAGE_SIZE - 1)

// Enable at most one of these option
// Priority order is: Kernel Allocator module > Huge Page > Simple Iterative mmap()
// Are we using kernel allocator module to allocate contiguous memory?
#define KERNEL_ALLOCATOR_MODULE         1
#define KERNEL_ALLOCATOR_MODULE_FILE    "/dev/kam"
#define KERNEL_HUGEPAGE_ENABLED         0
#define KERNEL_HUGEPAGE_SIZE            (2 * 1024 * 1024)    // 2 MB

// Using mmap(), we might/might not get contigous pages. We need to try multiple
// times.
// Using kernel module, if we can get, we wll get all contiguous on first attempt
#if (KERNEL_ALLOCATOR_MODULE == 1)
#define NUM_CONTIGOUS_PAGES             (MEM_SIZE / PAGE_SIZE)
#define MAX_MMAP_ITR                    1
#elif (KERNEL_HUGEPAGE_ENABLED == 1)
#define NUM_CONTIGOUS_PAGES             (MEM_SIZE > KERNEL_HUGEPAGE_SIZE ?      \
                                        (KERNEL_HUGEPAGE_SIZE / PAGE_SIZE) :    \
                                        (MEM_SIZE / PAGE_SIZE))
#define MAX_MMAP_ITR                    1
#endif

#define MEM_SIZE                        (1 << 22)

#define MAX_INNER_LOOP                  10
#define MAX_OUTER_LOOP                  1000

// Threshold for timing
#define HIGH_THRESHOLD_MULTIPLIER       3
#define LOW_THRESHOLD_MULTIPLIER        0.3

// By what percentage does a timing needs to be away from average to be considered
// outlier and hence we can assume that pair of address lie on same bank, different
// rows
#define OUTLIER_PERCENTAGE              30

// Following values need not be exact, just approximation. Limits used for
// memory allocation
#define MIN_BANKS                       8
#define MAX_BANKS                       128
#define MIN_BANK_SIZE                   (PAGE_SIZE/ 2)

// An entry is an address we tested to see on which address it lied
#define NUM_ENTRIES    ((NUM_CONTIGOUS_PAGES * PAGE_SIZE) / (MIN_BANK_SIZE))
#define MAX_NUM_ENTRIES_IN_BANK         (NUM_ENTRIES)
