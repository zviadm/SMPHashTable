#define _XOPEN_SOURCE 500

#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "ia32msr.h"

int
ReadMSR(int cpu, uint32_t reg, uint64_t* value)
{
  // code comes from msr-tools readmsr
  int fd;
  char msr_file_name[64];

  sprintf(msr_file_name, "/dev/cpu/%d/msr", cpu);
  fd = open(msr_file_name, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  
  if (pread(fd, value, sizeof(*value), reg) != sizeof(*value)) {
    close(fd);
    return -1;
  }

  close(fd);
  return 0;
}

int
WriteMSR(int cpu, uint32_t reg, uint64_t value)
{
  // code comes from msr-tools writemsr
  int fd;
  char msr_file_name[64];

  sprintf(msr_file_name, "/dev/cpu/%d/msr", cpu);
  fd = open(msr_file_name, O_WRONLY);
  if (fd < 0) {
    return -1;
  }
  
  if (pwrite(fd, &value, sizeof(value), reg) != sizeof(value)) {
    close(fd);
    return -1;
  }

  close(fd);
  return 0;
}
