/*
Copyright (c) 2016, Al Poole <netstar@gmail.com>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/ioctl.h>

#if defined(__OpenBSD__) || defined(__NetBSD__)
#include <sys/swap.h>
#include <sys/mount.h>
#include <sys/sensors.h>
#include <sys/audioio.h>
#endif

#if defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/soundcard.h>
#include <vm/vm_param.h>
#endif

static bool simple_mode = true;

#define MAX_BATTERIES 5

int mibs[5];
int *bat_mibs[MAX_BATTERIES];

#if defined(__OpenBSD__) || defined(__NetBSD__)
static char *sensor_name = NULL;
size_t slen = sizeof(struct sensor);
struct sensor snsr;
struct sensordev snsrdev;
size_t sdlen = sizeof(struct sensordev);
int devn;
#endif

typedef struct {
        unsigned long total;
        unsigned long used;
        unsigned long cached;
        unsigned long buffered;
        unsigned long shared;
        unsigned long swap_total;
        unsigned long swap_used;
} meminfo_t;

typedef struct {
        bool have_ac;
        int battery_index;
        uint8_t percent;
        double last_full_charge;
        double current_charge;
} power_t;

typedef struct {
        bool enabled;
        uint8_t volume_left;
        uint8_t volume_right;
} mixer_t;

typedef struct results_t results_t;
struct results_t {
        meminfo_t memory;
        power_t power;
        mixer_t mixer;
        uint8_t temperature;
};

#if defined(__FreeBSD__) || defined(__DragonFly__)
static long int
_sysctlfromname(const char *name, void *mib, int depth, size_t * len)
{
        long int result;

        if (sysctlnametomib(name, mib, len) < 0)
                return -1;

        *len = sizeof(result);
        if (sysctl(mib, depth, &result, len, NULL, 0) < 0)
                return -1;

        return result;
}
#endif

void _memsize_bytes_to_kb(unsigned long *bytes)
{
        *bytes = (unsigned int) *bytes >> 10;
}

#define _memsize_kb_to_mb _memsize_bytes_to_kb

static void bsd_generic_meminfo(meminfo_t * memory)
{
        size_t len;
        int i = 0;
        memset(memory, 0, sizeof(meminfo_t));
#if defined(__FreeBSD__) || defined(__DragonFly__)
        int total_pages = 0, free_pages = 0, inactive_pages = 0;
        long int result = 0;
        int page_size = getpagesize();

        int *mib = malloc(sizeof(int) * 4);
        if (mib == NULL)
                return;

        mib[0] = CTL_HW;
        mib[1] = HW_PHYSMEM;
        len = sizeof(memory->total);
        if (sysctl(mib, 2, &memory->total, &len, NULL, 0) == -1)
                return;
        memory->total /= 1024;

        total_pages =
            _sysctlfromname("vm.stats.vm.v_page_count", mib, 4, &len);
        if (total_pages < 0)
                return;

        free_pages =
            _sysctlfromname("vm.stats.vm.v_free_count", mib, 4, &len);
        if (free_pages < 0)
                return;

        inactive_pages =
            _sysctlfromname("vm.stats.vm.v_inactive_count", mib, 4, &len);
        if (inactive_pages < 0)
                return;

        memory->used =
            (total_pages - free_pages - inactive_pages) * page_size;
        _memsize_bytes_to_kb(&memory->used);

        result = _sysctlfromname("vfs.bufspace", mib, 2, &len);
        if (result < 0)
                return;
        memory->buffered = (result);
        _memsize_bytes_to_kb(&memory->buffered);

        result =
            _sysctlfromname("vm.stats.vm.v_active_count", mib, 4, &len);
        if (result < 0)
                return;
        memory->cached = (result * page_size);
        _memsize_bytes_to_kb(&memory->cached);

        result =
            _sysctlfromname("vm.stats.vm.v_cache_count", mib, 4, &len);
        if (result < 0)
                return;
        memory->shared = (result * page_size);
        _memsize_bytes_to_kb(&memory->shared);

        result = _sysctlfromname("vm.swap_total", mib, 2, &len);
        if (result < 0)
                return;
        memory->swap_total = (result / 1024);

        struct xswdev xsw;
        // previous mib is important for this one...

        while (i++) {
                mib[2] = i;
                len = sizeof(xsw);
                if (sysctl(mib, 3, &xsw, &len, NULL, 0) == -1)
                        break;

                memory->swap_used += xsw.xsw_used * page_size;
        }

        memory->swap_used >>= 10;

        free(mib);
#elif defined(__OpenBSD__)
        static int mib[] = { CTL_HW, HW_PHYSMEM64 };
        static int bcstats_mib[] =
            { CTL_VFS, VFS_GENERIC, VFS_BCACHESTAT };
        struct bcachestats bcstats;
        static int uvmexp_mib[] = { CTL_VM, VM_UVMEXP };
        struct uvmexp uvmexp;
        int nswap, rnswap;
        struct swapent *swdev = NULL;

        len = sizeof(memory->total);
        if (sysctl(mib, 2, &memory->total, &len, NULL, 0) == -1)
                return;

        len = sizeof(uvmexp);
        if (sysctl(uvmexp_mib, 2, &uvmexp, &len, NULL, 0) == -1)
                return;

        len = sizeof(bcstats);
        if (sysctl(bcstats_mib, 3, &bcstats, &len, NULL, 0) == -1)
                return;

        // Don't fail if there's not swap!
        nswap = swapctl(SWAP_NSWAP, 0, 0);
        if (nswap == 0)
                goto swap_out;

        swdev = calloc(nswap, sizeof(*swdev));
        if (swdev == NULL)
                goto swap_out;

        rnswap = swapctl(SWAP_STATS, swdev, nswap);
        if (rnswap == -1)
                goto swap_out;

        for (i = 0; i < nswap; i++)     // nswap; i++)
        {
                if (swdev[i].se_flags & SWF_ENABLE) {
                        memory->swap_used +=
                            (swdev[i].se_inuse / (1024 / DEV_BSIZE));
                        memory->swap_total +=
                            (swdev[i].se_nblks / (1024 / DEV_BSIZE));
                }
        }
      swap_out:
        if (swdev)
                free(swdev);

        memory->total /= 1024;

        memory->cached = (uvmexp.pagesize * bcstats.numbufpages);
        _memsize_bytes_to_kb(&memory->cached);

        memory->used = (uvmexp.active * uvmexp.pagesize);
        _memsize_bytes_to_kb(&memory->used);

        memory->buffered =
            (uvmexp.pagesize * (uvmexp.npages - uvmexp.free));
        _memsize_bytes_to_kb(&memory->buffered);

        memory->shared = (uvmexp.pagesize * uvmexp.wired);
        _memsize_bytes_to_kb(&memory->shared);
#endif
}

static int bsd_generic_audio_state_master(mixer_t *mixer)
{
#if defined(__OpenBSD__) || defined(__NetBSD__)
        int i;
        mixer_devinfo_t dinfo;
        mixer_ctrl_t *values = NULL;
        mixer_devinfo_t *info = NULL;

        int fd = open("/dev/mixer", O_RDONLY);
        if (fd < 0)
                return (0);

        for (devn = 0;; devn++) {
                dinfo.index = devn;
                if (ioctl(fd, AUDIO_MIXER_DEVINFO, &dinfo))
                        break;
        }

        info = calloc(devn, sizeof(*info));
        if (!info)
                return (0);

        for (i = 0; i < devn; i++) {
                info[i].index = i;
                if (ioctl(fd, AUDIO_MIXER_DEVINFO, &info[i]) == -1) {
                        --devn;
                        --i;
                        mixer->enabled = true;
                        continue;
                }
        }

        values = calloc(devn, sizeof(*values));
        if (!values)
                return (0);

        for (i = 0; i < devn; i++) {
                values[i].dev = i;
                values[i].type = info[i].type;
                if (info[i].type != AUDIO_MIXER_CLASS) {
                        values[i].un.value.num_channels = 2;
                        if (ioctl(fd, AUDIO_MIXER_READ, &values[i]) == -1) {
                                values[i].un.value.num_channels = 1;
                                if (ioctl(fd, AUDIO_MIXER_READ, &values[i])
                                    == -1)
                                        return (0);
                        }
                }
        }

        char name[64];

        for (i = 0; i < devn; i++) {
                strlcpy(name, info[i].label.name, sizeof(name));
                if (!strcmp("master", name)) {
                        mixer->volume_left =
                            values[i].un.value.level[0];
                        mixer->volume_right =
                            values[i].un.value.level[1];
                        mixer->enabled = true;
                        break;
                }
        }

        close(fd);

        if (values)
                free(values);
        if (info)
                free(info);

#elif defined(__FreeBSD__) || defined(__DragonFly__)
        int bar;
        int fd = open("/dev/mixer", O_RDONLY);
        if (fd == -1)
                return (0);

        if ((ioctl(fd, MIXER_READ(0), &bar)) == -1) {
                return (0);
        }
        mixer->enabled = true;
        mixer->volume_left = bar & 0x7f;
        mixer->volume_right = (bar >> 8) & 0x7f;
        close(fd);
#endif
        return (mixer->enabled);

}

static void bsd_generic_temperature_state(uint8_t *temperature)
{
#if defined(__OpenBSD__) || defined(__NetBSD__)
        int mib[5] = { CTL_HW, HW_SENSORS, 0, 0, 0 };
        memcpy(&mibs, mib, sizeof(int) * 5);

        for (devn = 0;; devn++) {
                mibs[2] = devn;

                if (sysctl
                    (mibs, 3, &snsrdev, &sdlen,
                     NULL, 0) == -1) {
                        if (errno == ENOENT)
                                break;
                        else
                                continue;
                }
                if (!strcmp("cpu0", snsrdev.xname)) {
                        sensor_name = strdup("cpu0");
                        break;
                } else if (!strcmp("km0", snsrdev.xname)) {
                        sensor_name = strdup("km0");
                        break;
                }
        }

        int numt;

        for (numt = 0; numt < snsrdev.maxnumt[SENSOR_TEMP]; numt++) {
                mibs[4] = numt;

                if (sysctl
                    (mibs, 5, &snsr, &slen, NULL,
                     0) == -1)
                        continue;

                if (slen > 0 && (snsr.flags & SENSOR_FINVALID) == 0)
                        break;
        }

        int temp = 0;

        if (sysctl(mibs, 5, &snsr, &slen, NULL, 0)
            != -1) {
                temp = (snsr.value - 273150000) / 1000000.0;
        }
        *temperature = temp;

#elif defined(__FreeBSD__) || defined(__DragonFly__)
        unsigned int value;
        size_t len = sizeof(value);
        if ((sysctlbyname
             ("hw.acpi.thermal.tz0.temperature", &value, &len, NULL,
              0)) != -1) {
                *temperature = (value - 2732) / 10;
        }
#endif
}

/* just add the values for all batteries. */


static int bsd_generic_mibs_power_get(results_t * results)
{
        int result = 0;
#if defined(__OpenBSD__) || defined(__NetBSD__)
        int i;
        int mib[5] = { CTL_HW, HW_SENSORS, 0, 0, 0 };
#elif defined(__FreeBSD__) || defined(__DragonFly__)
        size_t len;
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
        for (devn = 0;; devn++) {
                mib[2] = devn;
                if (sysctl(mib, 3, &snsrdev, &sdlen, NULL, 0) == -1) {
                        if (errno == ENXIO)
                                continue;
                        if (errno == ENOENT)
                                break;
                }

                for (i = 0; i < MAX_BATTERIES; i++) {
                        char buf[64];
                        snprintf(buf, sizeof(buf), "acpibat%d", i);
                        if (!strcmp(buf, snsrdev.xname)) {
                                bat_mibs[results->power.
                                                      battery_index] =
                                    malloc(sizeof(int) * 5);
                                int *tmp =
                                    bat_mibs[results->power.
                                                          battery_index++];
                                tmp[0] = mib[0];
                                tmp[1] = mib[1];
                                tmp[2] = mib[2];
                        }
                        result++;
                }

                if (!strcmp("acpiac0", snsrdev.xname)) {
                        mibs[0] = mib[0];
                        mibs[1] = mib[1];
                        mibs[2] = mib[2];
                }
        }
#elif defined(__FreeBSD__) || defined(__DragonFly__)
        if ((sysctlbyname("hw.acpi.battery.life", NULL, &len, NULL, 0)) !=
            -1) {
                bat_mibs[results->power.battery_index] =
                    malloc(sizeof(int) * 5);
                sysctlnametomib("hw.acpi.battery.life",
                                bat_mibs[results->power.
                                                      battery_index],
                                &len);
                result++;
        }

        if ((sysctlbyname("hw.acpi.acline", NULL, &len, NULL, 0)) != -1) {
                sysctlnametomib("hw.acpi.acline", mibs,
                                &len);
        }
#endif

        return (result);
}


static void bsd_generic_battery_state_get(int *mib, power_t *power)
{
#if defined(__OpenBSD__) || defined(__NetBSD__)
        double last_full_charge = 0;
        double current_charge = 0;

        mib[3] = 7;
        mib[4] = 0;

        if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
                last_full_charge = (double) snsr.value;

        mib[3] = 7;
        mib[4] = 3;

        if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
                current_charge = (double) snsr.value;

        /* There is a bug in the OS so try again... */
        if (current_charge == 0 || last_full_charge == 0) {
                mib[3] = 8;
                mib[4] = 0;

                if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
                        last_full_charge = (double) snsr.value;

                mib[3] = 8;
                mib[4] = 3;

                if (sysctl(mib, 5, &snsr, &slen, NULL, 0) != -1)
                        current_charge = (double) snsr.value;
        }

        power->last_full_charge += last_full_charge;
        power->current_charge += current_charge;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
        unsigned int value;
        size_t len = sizeof(value);
        if ((sysctl(mib, 4, &value, &len, NULL, 0)) != -1)
                power->percent = value;

#endif
}

static void bsd_generic_power_state(power_t * power)
{
        int i;
#if defined(__OpenBSD__) || defined(__NetBSD__)
        int have_ac = 0;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
        unsigned int value;
        size_t len;
#endif

#if defined(__OpenBSD__) || defined(__NetBSD__)
        mibs[3] = 9;
        mibs[4] = 0;

        if (sysctl(mibs, 5, &snsr, &slen, NULL, 0) != -1)
                have_ac = (int) snsr.value;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
        len = sizeof(value);
        if ((sysctl(mibs, 3, &value, &len, NULL, 0)) ==
            -1) {
                return;
        }
        power->have_ac = value;
#endif

        // get batteries here
        for (i = 0; i < power->battery_index; i++) {
                bsd_generic_battery_state_get(bat_mibs[i],
                                              power);
        }

        for (i = 0; i < power->battery_index; i++)
                free(bat_mibs[i]);

#if defined(__OpenBSD__) || defined(__NetBSD__)
        double percent =
            100 * (power->current_charge /
                   power->last_full_charge);

        power->percent = (int) percent;
        power->have_ac = have_ac;
#elif defined(__FreeBSD__) || defined(__DragonFly__)
        len = sizeof(value);
        if ((sysctl(bat_mibs[0], 4, &value, &len, NULL, 0)) ==
            -1) {
                return;
        }

        power->percent = value;

#endif
}

static int get_percent(int value, int max)
{
        double avg = (max / 100.0);
        double tmp = value / avg;

        int result = round(tmp);

        return (result);
}

static void results_show(results_t results)
{
        _memsize_kb_to_mb(&results.memory.used);
        _memsize_kb_to_mb(&results.memory.total);
        printf("[MEM] %luM/%luM (used/total) ", results.memory.used,
               results.memory.total);

        if (results.power.have_ac)
                printf("[AC]: %d%%", results.power.percent);
        else
                printf("[DC]: %d%%", results.power.percent);

        printf(" [TEMP]: %dC", results.temperature);

        if (results.mixer.enabled) {
                uint8_t high =
                    results.mixer.volume_right >
                    results.mixer.volume_left ? results.mixer.
                    volume_right : results.mixer.volume_left;
#if defined(__OpenBSD__) || defined(__NetBSD__)
                if (simple_mode) {
                        uint8_t perc = get_percent(high, 255);
                        printf(" [AUDIO]: %d%%", perc);
                } else
                        printf(" [AUDIO] L: %d R: %d",
                               results.mixer.volume_left,
                               results.mixer.volume_right);
#elif defined(__FreeBSD__) || defined(__DragonFly__)
                if (simple_mode) {
                        uint8_t perc = get_percent(high, 100);
                        printf(" [AUDIO]: %d%%", perc);
                } else
                        printf(" [AUDIO] L: %d R: %d",
                               results.mixer.volume_left,
                               results.mixer.volume_right);
#endif
        }
        printf("\n");
}

int main(int argc, char **argv)
{
        results_t results;
        bool have_battery;

        memset(&results, 0, sizeof(results_t));

        bsd_generic_meminfo(&results.memory);

        have_battery = bsd_generic_mibs_power_get(&results);
        if (have_battery) {
                bsd_generic_power_state(&results.power);
        }

        bsd_generic_temperature_state(&results.temperature);

        bsd_generic_audio_state_master(&results.mixer);

        results_show(results);

        return (EXIT_SUCCESS);
}
