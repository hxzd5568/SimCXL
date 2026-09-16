#ifndef __SIMCXL_VIRT2PHY_H__
#define __SIMCXL_VIRT2PHY_H__

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>

#define PFN_MASK_SIZE 8

static inline uint64_t
virt2phy2(const void *virtaddr)
{
    int fd, retval;
    uint64_t page, physaddr;
    unsigned long virt_pfn;
    int page_size;
    off_t offset;

    page_size = getpagesize();

    fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) {
        return (uint64_t)-1;
    }

    virt_pfn = (unsigned long)virtaddr / page_size;
    offset = sizeof(uint64_t) * virt_pfn;
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        close(fd);
        return (uint64_t)-1;
    }

    retval = read(fd, &page, PFN_MASK_SIZE);
    close(fd);
    if (retval != PFN_MASK_SIZE) {
        return (uint64_t)-1;
    }

    if ((page & 0x7fffffffffffffULL) == 0) {
        return (uint64_t)-1;
    }

    physaddr = ((page & 0x7fffffffffffffULL) * page_size) +
        ((unsigned long)virtaddr % page_size);

    return physaddr;
}

#endif
