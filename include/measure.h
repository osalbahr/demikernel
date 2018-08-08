/*
 * measure.h
 *
 *  Created on: Aug 7, 2018
 *      Author: amanda
 */

#ifndef INCLUDE_MEASURE_H_
#define INCLUDE_MEASURE_H_

#include <stdint.h>

#define _CPUFREQ 3500LU /* MHz */

#define NS2CYCLE(__ns) (((__ns) * _CPUFREQ) / 1000)
#define CYCLE2NS(__cycles) (((__cycles) * 1000) / _CPUFREQ)

struct timer_info {
    uint64_t libos_pop_start;
    uint64_t device_read_start;
    uint64_t device_read_end;
    uint64_t libos_pop_end;
    uint64_t libos_push_start;
    uint64_t device_send_start;
    uint64_t device_send_end;
    uint64_t libos_push_end;
};

extern struct timer_info ti;

static inline uint64_t rdtsc(void)
{
    uint32_t eax, edx;
    __asm volatile ("rdtsc" : "=a" (eax), "=d" (edx) :: "memory");
    return ((uint64_t)edx << 32) | eax;
}

static inline void print_timer_info()
{
    uint64_t pop_duration = ti.libos_pop_end - ti.libos_pop_start;
    uint64_t push_duration = ti.libos_push_end - ti.libos_push_start;
    uint64_t recv_duration = ti.device_read_end - ti.device_read_start;
    uint64_t send_duration = ti.device_send_end - ti.device_send_start;
    uint64_t pop_to_push_duration = ti.libos_push_end - ti.libos_pop_start;
    uint64_t push_overhead = push_duration - send_duration;
    uint64_t pop_overhead = pop_duration - recv_duration;

    printf("======================\n");
    printf("pop duration: %4.2f\n", CYCLE2NS(pop_duration) / 1000.0);
    printf("read duration: %4.2f\n", CYCLE2NS(recv_duration) / 1000.0);
    printf("push duration: %4.2f\n", CYCLE2NS(push_duration) / 1000.0);
    printf("send duration: %4.2f\n", CYCLE2NS(send_duration) / 1000.0);
    printf("push overhead: %4.2f\n", CYCLE2NS(push_overhead) / 1000.0);
    printf("pop overhead: %4.2f\n", CYCLE2NS(pop_overhead) / 1000.0);
    printf("pop to push duration: %4.2f\n", CYCLE2NS(pop_to_push_duration) / 1000.0);
}

#endif /* INCLUDE_MEASURE_H_ */
