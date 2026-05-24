#ifndef SBI_H
#define SBI_H

#include "types.h"

//#define SBI_EXT_SET_TIMER 0x0 //legacy
#define SBI_EXT_SHUTDOWN  0x8
#define SBI_EXT_BASE      0x10
#define SBI_EXT_SET_TIMER 0x54494D45
#define SBI_FID_SET_TIMER 0x0


enum sbi_ext_base_fid {
    SBI_EXT_BASE_GET_SPEC_VERSION,
    SBI_EXT_BASE_GET_IMP_ID,
    SBI_EXT_BASE_GET_IMP_VERSION,
    SBI_EXT_BASE_PROBE_EXT,
    SBI_EXT_BASE_GET_MVENDORID,
    SBI_EXT_BASE_GET_MARCHID,
    SBI_EXT_BASE_GET_MIMPID,
};

struct sbiret {
    long error;
    long value;
};

struct sbiret sbi_ecall(int ext,
                        int fid,
                        unsigned long arg0,
                        unsigned long arg1,
                        unsigned long arg2,
                        unsigned long arg3,
                        unsigned long arg4,
                        unsigned long arg5);
                        
long sbi_get_spec_version(void);

long sbi_get_impl_id(void);

long sbi_get_impl_version(void);

long sbi_set_timer_legacy(uint64_t stime_value);

struct sbiret sbi_set_timer(uint64_t stime_value);

#endif
