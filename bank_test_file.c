/**
 * Read timing and generate bank map file (input to the algo)
 * 
 * Input file format: <paddr1> <paddr2> <cycles>
 *
 * How to generate the input file
 *  # ./bank_test_nomap > x.out 2> x.err
 *  # grep Reading x.err | awk '{ print $4 " " $6 " " $9 }'  > x.time.txt
 *  # ./bank_test_file x.time.txt 0x2c200000 > x.bank.txt
 *
 *  NOTE: MEM_SIZE of both bank_test_nomap and bank_test_file must be the same
 */ 

/**************************************************************************
 * Conditional Compilation Options
 **************************************************************************/
#define _GNU_SOURCE
#define DEBUG                           0
#define KERNEL_ALLOCATOR_MODULE         1
#define KERNEL_HUGEPAGE_ENABLED         0

/**************************************************************************
 * Included Files
 **************************************************************************/
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <sys/sysinfo.h>
#include <sched.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <pthread.h>

/**************************************************************************
 * Public Definitions
 **************************************************************************/

#if (DEBUG == 1)
#define dprintf(...)                    fprintf(stderr, "DEBUG:" __VA_ARGS__)
#else
#define dprintf(...)
#endif

#define eprint(...)	                    fprintf(stderr, "ERROR:" __VA_ARGS__)


FILE *time_fp = NULL;
#define tprintf(...)                     fprintf(time_fp, __VA_ARGS__)

#define PAGE_SHIFT                      12
#define PAGE_SIZE                       (1 << PAGE_SHIFT)
#define PAGE_MASK                       (PAGE_SIZE - 1)

// Enable at most one of these option
// Priority order is: Kernel Allocator module > Huge Page > Simple Iterative mmap()
// Are we using kernel allocator module to allocate contiguous memory?
#define KERNEL_ALLOCATOR_MODULE_FILE    "/dev/kam"
#define KERNEL_HUGEPAGE_SIZE            (2 * 1024 * 1024)    // 2 MB

#define MEM_SIZE                        (1 << 23) // 1<<23 = 8 MB, 1<<24 = 16M, 1<<25 = 32M

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

#define MAX_INNER_LOOP                  10
#define MAX_OUTER_LOOP                  1000

// Threshold for timing
#define HIGH_THRESHOLD_MULTIPLIER            3
#define LOW_THRESHOLD_MULTIPLIER            0.3

// By what percentage does a timing needs to be away from average to be considered
// outlier and hence we can assume that pair of address lie on same bank, different
// rows
#define OUTLIER_PERCENTAGE              30

// CORE to run on : -1 for last processor
#define CORE                            -1
#define IA32_MISC_ENABLE_OFFSET         0x1a4
#define DISBALE_PREFETCH(msr)           (msr |= 0xf)

// On some systems, HW prefetch details are not well know. Use BIOS setting for
// disabling it
#define SOFTWARE_CONTROL_HWPREFETCH     0

// Following values need not be exact, just approximation. Limits used for
// memory allocation
#define MIN_BANKS                       8
#define MAX_BANKS                       128
#define MIN_BANK_SIZE                   (PAGE_SIZE/ 2)

// An entry is an address we tested to see on which address it lied
#define NUM_ENTRIES    ((NUM_CONTIGOUS_PAGES * PAGE_SIZE) / (MIN_BANK_SIZE))
#define MAX_NUM_ENTRIES_IN_BANK         (NUM_ENTRIES)

/**************************************************************************
 * Public Types
 **************************************************************************/

typedef struct entry {
   
    uint64_t virt_addr;
    uint64_t phy_addr;                              // Physical address of entry
    int bank;                                       // Bank on which this lies
    struct entry *siblings[MAX_NUM_ENTRIES_IN_BANK];// Entries that lie on same banks
    int num_sibling;
    int associated;                                 // Is this someone's sibling?
} entry_t;

/**************************************************************************
 * Global Variables
 **************************************************************************/

entry_t entries[NUM_ENTRIES];

// DRAM bank
typedef struct banks_t {
    entry_t *main_entry;        // Master entry that belongs to this bank
} bank_t;

bank_t banks[MAX_BANKS];

/**************************************************************************
 * Public Function Prototypes
 **************************************************************************/

static void init_banks(void)
{
    int i;
    for (i = 0; i < MAX_BANKS; i++) {
        banks[i].main_entry = NULL;
    }
}

static void init_entries(uint64_t virt_start, uintptr_t phy_start)
{
    uintptr_t inter_bank_spacing = MIN_BANK_SIZE;
    int i;
    for (i = 0; i < NUM_ENTRIES; i++) {
        entry_t *entry = &entries[i];
        memset(entry, 0, sizeof(*entry));
        entry->virt_addr = virt_start + i * inter_bank_spacing;
        entry->phy_addr = phy_start + i * inter_bank_spacing;
        entry->bank = -1;
        entry->num_sibling = 0;
        entry->associated = false;
    }
}


// static int comparator(const void *p, const void *q)
//{
//   return *(int *)p > *(int *)q;
//}


double find_read_time_from_file(uintptr_t *a, uintptr_t*b)
{
    char *line = NULL;
    size_t len = 0;
    int c;
    int read = getline(&line, &len, time_fp);
    assert (read > 0);
    sscanf(line, "0x%lx 0x%lx %d", a, b, &c);
    return (double)c;
}

void print_binary(uint64_t v)
{
    char buffer[100];
    int index;

    buffer[99] = '\0';
    for (index = 98; v > 0; index--) {
        if (v & 1)
            buffer[index] = '1';
        else
            buffer[index] = '0';

        v = v >> 1;
    }

    printf("%s", &buffer[index + 1]);
}

void run_exp(uint64_t virt_start, uint64_t phy_start)
{
    uintptr_t a, b;
    double sum, running_avg, running_threshold, nearest_nonoutlier;
    double *avgs;
    int i, j, num_outlier;
    
    // avgs = calloc(sizeof(double), NUM_ENTRIES);
    avgs = malloc(sizeof(double) * NUM_ENTRIES);
    assert(avgs != NULL);

    // run the experiment: up to n*(n-1)/2 iterations
    for (i = 0; i < NUM_ENTRIES; i++) {

        entry_t *entry = &entries[i]; 
        int sub_entries = NUM_ENTRIES - (i + 1);
        
        if (entry->associated)
            continue;

        dprintf("Master Entry: %d\n", i);
        
        for (j = i + 1, sum = 0; j < NUM_ENTRIES; j++) {
            avgs[j] = find_read_time_from_file(&a, &b);
            entries[i].virt_addr = a;
            entries[j].virt_addr = b;
            dprintf("Reading Time: PhyAddr1: 0x%lx\t PhyAddr2: 0x%lx\t Avg Ticks: %.0f\n",
                    entries[i].phy_addr, entries[j].phy_addr, avgs[j]);
            sum += avgs[j];
        }

        running_avg = sum / sub_entries;
        running_threshold = (running_avg * (100.0 + OUTLIER_PERCENTAGE)) / 100.0;
        // dprintf("running_threshold: %.0f\n", running_threshold);
        entry->associated = false;
        for (j = i + 1, num_outlier = 0, nearest_nonoutlier = 0;
                j < NUM_ENTRIES; j++) {
            if (avgs[j] >= running_threshold) {
                if (entries[j].associated) {
					eprint("Entry being mapped to multiple siblings\n");
					eprint("Entry: PhyAddr: 0x%lx,"
						   " Prior Sibling: PhyAddr: 0x%lx,"
						   " Current Sibling: PhyAddr: 0x%lx\n",
						   entries[j].phy_addr, entries[j].siblings[0]->phy_addr,
						   entry->phy_addr);
                } else {
                    entry->siblings[num_outlier] = &entries[j];
                    num_outlier++;
                    entries[j].associated = true;
                    entries[j].siblings[0] = entry;
                    entries[j].num_sibling = 1;
                }   
            } else {
                nearest_nonoutlier = avgs[j] > nearest_nonoutlier ?
                                    avgs[j] : nearest_nonoutlier;
            }
        }

        if (entry->associated == false) {
            entry->num_sibling = num_outlier;
#if 0	    
            for (k = 0; k < entry->num_sibling; k++) {
                printf("Siblings: PhyAddr: 0x%lx\tPhyAddr: 0x%lx\t\t", entry->phy_addr, 
                    entry->siblings[k]->phy_addr);
                print_binary(entry->siblings[k]->phy_addr);
                printf("\n");
            }
#endif        
        }
        dprintf("Nearest Nonoutlier: %f, Avg: %f, Threshold: %f\n",
                nearest_nonoutlier, running_avg, running_threshold);
        dprintf("Found %d siblings\n", num_outlier);
    }

    free(avgs);
	
}


// Checks mapping/hypothesis
// TODO: Check if all the bits of address have been accounted for
void check_mapping(void)
{
    int i, j;
    int main_bank = 0; // bank;

    for (i = 0; i < NUM_ENTRIES; i++) {
        entry_t *entry = &entries[i];

        // Look for only master siblings
        if (entry->associated == true)
            continue;

        entry->bank = main_bank;
        for (j = 0; j < entry->num_sibling; j++) {
            entry_t *sibling = entry->siblings[j];
            sibling->bank = main_bank;
        }
        banks[main_bank].main_entry = entry;
        main_bank++;
	assert(main_bank <= MAX_BANKS);
    }

    // All entries should be assigned a bank
    for (i = 0; i < NUM_ENTRIES; i++) {
        entry_t *entry = &entries[i];
        if (entry->bank < 0) {
            eprint("Entry not assigned any bank: PhyAddr: 0x%lx\n",
                    entry->phy_addr);
        }

        if (entry->associated)
            continue;

        printf("Bank %d\n0x%lx\n", entry->bank,
	       entry->phy_addr);
        for (j = 0; j < entry->num_sibling; j++) {
            printf("0x%lx\n", entry->siblings[j]->phy_addr);
        }
    }

#if 0
    // Print bank stats
    fprintf(stderr, "Banks in use: Total Entries: %d\n", NUM_ENTRIES);
    for (i = 0; i < MAX_BANKS; i++) {
        if (banks[i].main_entry == NULL)
            continue;
        fprintf(stderr, "Bank:%d, Entries:%d\n", i, banks[i].main_entry->num_sibling + 1);
    }
#endif    
}

int main(int argc, char *argv[])
{
    void *virt_start = 0;
    uint64_t phy_start = 0;

    if (argc != 3) {
	fprintf(stderr, "Usage: %s <time file> <phyaddr>\n", argv[0]);
	exit(1);
    }
    
    time_fp = fopen(argv[1], "r");
    if (time_fp == NULL) {
	fprintf(stderr, "failed to open %s\n", argv[1]);
	exit(1);
    }

    init_banks();

    virt_start = 0;
    phy_start = strtol(argv[2], NULL, 0);
    
    fprintf(stderr, "mem_size: %d\tnum_entries: %d\tmin_bank_sz: %d\tsizeof(entires): %d\n",
           MEM_SIZE, NUM_ENTRIES, MIN_BANK_SIZE, (int)sizeof(entries));
    
    init_entries((uint64_t)virt_start, phy_start);
   
    run_exp((uint64_t)virt_start, phy_start);

    check_mapping();

    fclose(time_fp);
    return 0;
}
