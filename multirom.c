/*
 * This file is part of MultiROM.
 *
 * MultiROM is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * MultiROM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MultiROM.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/klog.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <linux/loop.h>
#include <ctype.h>
#include <unistd.h>

// clone libbootimg to /system/extras/ from
// https://github.com/Tasssadar/libbootimg.git
#include <libbootimg.h>

#if LIBBOOTIMG_VERSION  < 0x000200
#error "libbootimg version 0.2.0 or higher is required. Please update libbootimg."
#endif

#include "lib/containers.h"
#include "lib/framebuffer.h"
#include "lib/inject.h"
#include "lib/input.h"
#include "lib/log.h"
#include "lib/util.h"
#include "lib/mrom_data.h"
#include "multirom.h"
#include "multirom_ui.h"
#include "version.h"
#include "hooks.h"
#include "rom_quirks.h"
#include "kexec.h"

#ifdef MR_NO_KEXEC
#include "no_kexec.h"
#endif


#define REALDATA "/realdata"
#define BUSYBOX_BIN "busybox"
#define KEXEC_BIN "kexec"
#define NTFS_BIN "ntfs-3g"
#define EXFAT_BIN "exfat-fuse"
#define INTERNAL_ROM_NAME "Internal"
#define MAX_ROM_NAME_LEN 26
#define LAYOUT_VERSION "/data/.layout_version"

#define BATTERY_CAP "/sys/class/power_supply/battery/capacity"
#define DT_FSTAB_PATH "/proc/device-tree/firmware/android/fstab/"

static char busybox_path[64] = { 0 };
static char kexec_path[64] = { 0 };
static char ntfs_path[64] = { 0 };
static char exfat_path[64] = { 0 };
static char partition_dir[64] = { 0 };
static char datamedia_dir[64] = { 0 };

static volatile int run_usb_refresh = 0;
static pthread_t usb_refresh_thread;
static pthread_mutex_t parts_mutex = PTHREAD_MUTEX_INITIALIZER;
static void (*usb_refresh_handler)(void) = NULL;

char* read_file(char* file) {
    char* buf = calloc(1, 256);
    FILE* fp = fopen(file, "r");
    if (fp) {
        fread(buf, 1, 256, fp);
        fclose(fp);
    } else {
        ERROR("cannot open %s %s\n", file, strerror(errno));
    }
    return buf;
}

void disable_dtb_fstab(char* partition) {
    if (access("status", F_OK)) {
        FILE* fp = fopen("status", "w");
        fprintf(fp, "disabled");
        fclose(fp);
    }
    char mnt_pt[100];
    sprintf(mnt_pt, "%s%s/status", DT_FSTAB_PATH, partition);
    if (!mount("/status", mnt_pt, "ext4", MS_BIND, "discard,nomblk_io_submit")) {
        INFO("status node bind mounted in procfs\n");
    } else {
        ERROR("status node bind mount failed! %s\n", strerror(errno));
    }
}

void remove_dtb_fstab() {
    mkdir("/dummy_fw", S_IFDIR);
    if (!mount("/dummy_fw", "/proc/device-tree/firmware", "ext4", MS_BIND, "discard,nomblk_io_submit")) {
        INFO("dummy dtb node bind mounted in procfs\n");
    } else {
        ERROR("dummy dtb node bind mount failed! %s\n", strerror(errno));
    }
}

int mount_dtb_fstab(char* partition) {
    int rc = -1;

    char root_path [100] = {'\0'};
    char device[100] = {'\0'};
    char type[100] = {'\0'};
    char mnt_flags[100] = {'\0'};

    sprintf(root_path, "%s%s", DT_FSTAB_PATH, partition);
    sprintf(device, "%s%s", root_path, "/dev");
    sprintf(type, "%s%s", root_path, "/type");
    sprintf(mnt_flags, "%s%s", root_path, "/mnt_flags");

    if (!(rc = mount(read_file(device), partition, read_file(type), MS_RDONLY, "barrier=1,discard"))) {
        INFO("dtb %s mount successful\n", partition);
    } else {
        INFO("dtb %s mount failed %s\n", partition, strerror(errno));
    }

    return rc;
}

int multirom_find_base_dir(void)
{
    int i, j;
    char path[64];
    struct stat info;

    static const char *base_dirs[] = {
        REALDATA"/media/0",
        REALDATA"/media",
        "/data/media/0",
        "/data/media",
        NULL,
    };
    static const char *mrom_dirs[] = {
        "MultiROM/multirom",
        "multirom",
        NULL,
    };

    for(i = 0; base_dirs[i]; ++i) {
        for (j = 0; mrom_dirs[j]; ++j) {
            sprintf(path, "%s/%s", base_dirs[i], mrom_dirs[j]);
            if(stat(path, &info) < 0)
                continue;

            mrom_set_dir(path);

            strcpy(datamedia_dir, path);

            strncpy(partition_dir, path, strchr(path, '/') - path);

            sprintf(busybox_path, "%s/%s", path, BUSYBOX_BIN);
            sprintf(kexec_path, "%s/%s", path, KEXEC_BIN);
            sprintf(ntfs_path, "%s/%s", path, NTFS_BIN);
            sprintf(exfat_path, "%s/%s", path, EXFAT_BIN);

            chmod(kexec_path, 0755);
            chmod(ntfs_path, 0755);
            chmod(exfat_path, 0755);
            return 0;
        }
    }
    return -1;
}

#define MAX_LASTKMSG_LOGS 3
#define MAX_MROMKMSG_LOGS 5
enum
{
    BACKUP_LAST_KMSG    = 0,
    BACKUP_EARLY_KLOG   = 1,
    BACKUP_LATE_KLOG    = 2,
    BACKUP_LAST_KEXEC   = 3
};

static void set_mediarw_perms(const char *path)
{
    chmod(path, 0666);

    unsigned int media_rw_id = decode_uid("media_rw");
    if(media_rw_id != -1U)
        chown(path, (uid_t)media_rw_id, (gid_t)media_rw_id);
}

void multirom_kmsg_rename_logs(const char *path_logs_dir, const char *base_name, const int max_count)
{
    DIR *dp = opendir(path_logs_dir);
    if (!dp)
        return;

    int i;
    char path_file_1[256];
    char path_file_2[256];
    char tmp[5];
    char *pos = NULL;
    struct dirent *de = NULL;

    // delete last one (max_count), and rename the older ones
    while((de = readdir(dp)))
    {
        if (strncmp(de->d_name, base_name, strlen(base_name)) != 0)
            continue;

        sprintf(path_file_1, "%s/%s", path_logs_dir, de->d_name);
        sprintf(path_file_2, "%s/%s", path_logs_dir, de->d_name);

        for (i = max_count; i >= 0; i--)
        {
            sprintf(tmp, "_%i_", i);
            pos = strstr(path_file_2, tmp);
            if (pos != NULL)
            {
                sprintf(tmp, "_%i_", i+1);
                strncpy(pos, tmp, strlen(tmp));
                if (i == max_count)
                    remove(path_file_1); //INFO("Deleting oldest log '%s' res=%d\n", path_file_1, remove(path_file_1));
                else
                    rename(path_file_1, path_file_2); //INFO("Renaming older log '%s' to '%s' res=%d\n", path_file_1, path_file_2, rename(path_file_1, path_file_2));
            }
        }
    }
    closedir(dp);
}

void multirom_kmsg_logging(int kmsg_backup_type)
{
    // types of logging:
    //     BACKUP_LAST_KMSG  -> backup last_kmsg
    //     BACKUP_EARLY_KMSG -> current klog upon entering mrom
    //     BACKUP_LATE_KMSG  -> current klog upon exiting mrom
    //     BACKUP_LAST_KEXEC -> last kexec log
    char path_logs_dir[256];
    char path_log_file[256];

    static const char *kmsg_paths[] = {
        "/proc/last_kmsg",
        "/sys/fs/pstore/console-ramoops",
        NULL,
    };

    // make the logs folder visible outside multirom folder
    static const char log_dir_name[] = "multirom-klogs";
    static const char ext[] = "txt";

    // current date time
    char datetime[] = "yyyy-mm-dd-HHMMSS";
    time_t rawtime = time(NULL);
    struct tm  *timeinfo = localtime(&rawtime);
    strftime(datetime, sizeof(datetime), "%Y-%m-%d-%H%M%S", timeinfo);

    // filename: last_kmsg_n_2016-03-11-153600.txt
    // filename: early_klg_0_2016-03-11-153600.txt (this is current klog upon enterting mrom)
    // filename: mrom_klog_n_2016-03-11-153600.txt (this is current klog upon exiting mrom)
    // filename: last_kexec_0_2016-03-11-153600.txt (same klog as pulled in multirom_load_kexec function)
    char base_name[15];

    if (kmsg_backup_type == BACKUP_LAST_KMSG)
        strcpy(base_name, "last_kmsg");
    else if (kmsg_backup_type == BACKUP_EARLY_KLOG)
        strcpy(base_name, "early_klg");
    else if (kmsg_backup_type == BACKUP_LATE_KLOG)
        strcpy(base_name, "mrom_klog");
    else if (kmsg_backup_type == BACKUP_LAST_KEXEC)
        strcpy(base_name, "last_kexec");
    else
        return;

    sprintf(path_logs_dir, "%s/%s", datamedia_dir, log_dir_name);

    if (access(path_logs_dir, F_OK) < 0)
        mkdir(path_logs_dir, 0666);
    set_mediarw_perms(path_logs_dir);

    if (kmsg_backup_type == BACKUP_LAST_KMSG) {
        int i;
        multirom_kmsg_rename_logs(path_logs_dir, base_name, MAX_LASTKMSG_LOGS);
        sprintf(path_log_file, "%s/%s_%i_%s.%s", path_logs_dir, base_name, 0, datetime, ext);
        for(i = 0; kmsg_paths[i]; ++i) {
            if (access(kmsg_paths[i], R_OK) == 0) {
                INFO("Backing up last kmsg '%s' to '%s' res=%d\n", kmsg_paths[i], path_log_file, copy_file(kmsg_paths[i], path_log_file));
                break;
            }
        }
    }
    else {
        multirom_kmsg_rename_logs(path_logs_dir, base_name, (kmsg_backup_type == BACKUP_LATE_KLOG) ? MAX_MROMKMSG_LOGS : 0);
        sprintf(path_log_file, "%s/%s_%i_%s.%s", log_dir_name, base_name, 0, datetime, ext);
        INFO("Backing up current klog to '%s' res=%d\n", path_log_file, multirom_copy_log(NULL, path_log_file));
    }
    set_mediarw_perms(path_log_file);
}

int multirom(const char *rom_to_boot)
{
    if(multirom_find_base_dir() == -1)
    {
        ERROR("Could not find multirom dir\n");
        return -1;
    }

    struct multirom_status s;
    memset(&s, 0, sizeof(struct multirom_status));

    multirom_load_status(&s);
    multirom_dump_status(&s);

    if (s.enable_kmsg_logging != 0)
    {
        multirom_kmsg_logging(BACKUP_LAST_KMSG);
        multirom_kmsg_logging(BACKUP_EARLY_KLOG);
    }

    struct multirom_rom *to_boot = NULL;
    int exit = (EXIT_REBOOT | EXIT_UMOUNT);

#ifdef MR_NO_KEXEC
    // setup global variables here
    nokexec_set_struct(&s);


    // check if a partition mount failed, in which case multirom usually just boots internal
    if (s.current_rom && s.current_rom->type == ROM_DEFAULT && nokexec_is_second_boot())
    {
        ERROR(NO_KEXEC_LOG_TEXT ": Something went wrong in mounting the needed partition, so falling back...");
        nokexec()->selected_method = NO_KEXEC_BOOT_NORMAL;
        s.is_second_boot = 0;
        //ERROR(NO_KEXEC_LOG_TEXT ":    to Internal\n");     s.auto_boot_type = AUTOBOOT_FORCE_CURRENT;                // force reboot to Internal (this would be mrom default behaviour)
        //ERROR(NO_KEXEC_LOG_TEXT ":    to MultiROM GUI\n"); s.auto_boot_type = AUTOBOOT_NAME;                         // bring up the gui instead
        ERROR(NO_KEXEC_LOG_TEXT ":    to Recovery\n"); exit = (EXIT_REBOOT_RECOVERY | EXIT_UMOUNT);  goto finish;      // reboot to recovery
    }
#endif

    if(rom_to_boot != NULL)
    {
        struct multirom_rom *rom = NULL;

        // Parse rom_to_boot should be: Name_of_ROM++uuid=xxxx  (the '++uuid=xxxx' is optional but needed for external ROMs)
        char * uuid_ptr = strstr(rom_to_boot, "++uuid=");
        if(uuid_ptr == NULL)
        {
            rom = multirom_get_rom(&s, rom_to_boot, NULL);
        }
        else
        {
            char name[MAX_ROM_NAME_LEN + 1];
            char part_uuid[36+4+1];

            strncpy(name, rom_to_boot, uuid_ptr - rom_to_boot);
            name[uuid_ptr - rom_to_boot] = 0;

            strcpy(part_uuid, uuid_ptr + sizeof("++uuid=") - 1);

            // Note: if the current ROM is on the same external partition as the one being booted
            //       then this function would already have been called in multirom_load_status
            multirom_update_and_scan_for_external_roms(&s, part_uuid);

            rom = multirom_get_rom(&s, name, part_uuid);
        }

        if(rom)
        {
            // Two possible scenarios: this ROM has kexec-hardboot and target
            // ROM has boot image, so kexec it immediatelly or
            // reboot and then proceed as usuall
            if(((M(rom->type) & MASK_KEXEC) || rom->has_bootimg) && rom->type != ROM_DEFAULT && multirom_has_kexec())
            {
                to_boot = rom;
                s.is_second_boot = 0;
                INFO("Booting ROM %s...\n", rom_to_boot);
            }
#ifdef MR_NO_KEXEC
            else if(((M(rom->type) & MASK_KEXEC) || rom->has_bootimg) && rom->type != ROM_DEFAULT)
            {
                to_boot = rom;
                s.is_second_boot = 0;
                INFO(NO_KEXEC_LOG_TEXT " Booting ROM %s...\n", rom_to_boot);
            }
#endif
            else
            {
                s.current_rom = rom;

                free(s.curr_rom_part);
                s.curr_rom_part = NULL;

                if(rom->partition)
                    s.curr_rom_part = strdup(rom->partition->uuid);

                s.auto_boot_type |= AUTOBOOT_FORCE_CURRENT;
                INFO("Setting ROM %s to force autoboot\n", rom_to_boot);
            }
        }
        else
        {
            ERROR("ROM %s was not found, force autoboot was not set!\n", rom_to_boot);
            exit = EXIT_UMOUNT;
        }
    }
    else if(s.is_second_boot != 0 || (s.auto_boot_type & AUTOBOOT_FORCE_CURRENT))
    {
        ERROR("Skipping ROM selection, is_second_boot=%d, auto_boot_type=0x%x\n", s.is_second_boot, s.auto_boot_type);
        to_boot = s.current_rom;
    }
    else
    {
        // just to cache the result so that it does not take
        // any time when the UI is up
        multirom_has_kexec();

        switch(multirom_ui(&s, &to_boot))
        {
            case UI_EXIT_BOOT_ROM: break;
            case UI_EXIT_REBOOT:
                exit = (EXIT_REBOOT | EXIT_UMOUNT);
                break;
            case UI_EXIT_REBOOT_RECOVERY:
                exit = (EXIT_REBOOT_RECOVERY | EXIT_UMOUNT);
                break;
            case UI_EXIT_REBOOT_BOOTLOADER:
                exit = (EXIT_REBOOT_BOOTLOADER | EXIT_UMOUNT);
                break;
            case UI_EXIT_SHUTDOWN:
                exit = (EXIT_SHUTDOWN | EXIT_UMOUNT);
                break;
        }
    }

    if(to_boot)
    {
        s.auto_boot_type &= ~(AUTOBOOT_FORCE_CURRENT);

#ifdef MR_NO_KEXEC
        #define MR_NO_KEXEC_ABORT { \
                    if(rom_to_boot == NULL) { \
                        ERROR(NO_KEXEC_LOG_TEXT ": ERROR occurred in the above, aborting to recovery\n"); \
                        exit = (EXIT_REBOOT_RECOVERY | EXIT_UMOUNT); \
                    } \
                    else \
                    { \
                        ERROR(NO_KEXEC_LOG_TEXT ": ERROR occurred in the above, failed to boot '%s'\n", rom_to_boot); \
                        exit = EXIT_UMOUNT; \
                    } \
                    goto finish; \
                }

        if (s.is_second_boot != 0)
        {
            // Restore primary boot.img, and continue
            if (nokexec_restore_primary_and_cleanup() < 0)
                MR_NO_KEXEC_ABORT;
        }
        else
        {
            if ((to_boot->type != ROM_DEFAULT) && to_boot->has_bootimg)
            {
                // Flash secondary boot.img, and reboot
                // note: a secondary boot.img in primary slot will trigger second_boot=1
                //       so we don't need to worry about that any more
                if (nokexec_flash_secondary_bootimg(to_boot) < 0)
                    MR_NO_KEXEC_ABORT;

                s.current_rom = to_boot;

                free(s.curr_rom_part);
                s.curr_rom_part = NULL;

                if(to_boot->partition)
                    s.curr_rom_part = strdup(to_boot->partition->uuid);

                exit = (EXIT_REBOOT | EXIT_UMOUNT);
                goto finish;
            }
#ifdef MR_QSEECOMD_HAX
            else if ((to_boot->type == ROM_DEFAULT))
            {
                struct fstab_part *datap = fstab_find_first_by_path(s.fstab, "/data");

                // If data partition is encrypted perform workaround
                if(datap && strstr(datap->device, "dm-"))
                {
                    // Flash secondary boot.img, and reboot
                    // note: a secondary boot.img in primary slot will trigger second_boot=1
                    //       so we don't need to worry about that any more
                    if (nokexec_flash_enc_bootimg(to_boot) < 0)
                        MR_NO_KEXEC_ABORT;

                    s.current_rom = to_boot;

                    free(s.curr_rom_part);
                    s.curr_rom_part = NULL;

                    if(to_boot->partition)
                        s.curr_rom_part = strdup(to_boot->partition->uuid);

                    exit = (EXIT_REBOOT | EXIT_UMOUNT);
                    goto finish;
                }
            }
#endif
        }
#endif

        if(rom_to_boot == NULL)
            multirom_run_scripts("run-on-boot", to_boot);

        exit = multirom_prepare_for_boot(&s, to_boot);
        INFO("prepare for boot returned %d\n", exit);

        // Something went wrong, exit/reboot
        if(exit == -1)
        {
            INFO("SOMETHING HAS GONE WRONG !!");
            if(rom_to_boot == NULL)
            {
                multirom_emergency_reboot();
                exit = EXIT_REBOOT;
            }
            else
                exit = EXIT_UMOUNT;
            goto finish;
        }

        s.current_rom = to_boot;

        free(s.curr_rom_part);
        s.curr_rom_part = NULL;

        if(to_boot->partition)
            s.curr_rom_part = strdup(to_boot->partition->uuid);

        if(s.is_second_boot == 0 && (M(to_boot->type) & MASK_ANDROID) && (exit & EXIT_KEXEC))
        {
            s.is_second_boot = 1;

            // mrom_kexecd=1 param might be lost if kernel does not have kexec patches
            ERROR(SECOND_BOOT_KMESG);
        }
        else
            s.is_second_boot = 0;
    }

finish:
#ifdef MR_NO_KEXEC
    nokexec_free_struct();
#endif
    if (s.enable_kmsg_logging != 0)
        multirom_kmsg_logging(BACKUP_LATE_KLOG);
    multirom_save_status(&s);
    multirom_free_status(&s);

    sync();

    return exit;
}

int multirom_init_fb(int rotation)
{
    if(fb_open(rotation) < 0)
    {
        ERROR("Failed to open framebuffer!\n");
        return -1;
    }

    fb_fill(BLACK);
    return 0;
}

void multirom_emergency_reboot(void)
{
    char *klog;
    fb_text_proto *p;
    fb_img *t;
    char *tail;
    char *last_end;
    int cur_y;
    char path_log_file[64];

    if(multirom_init_fb(0) < 0)
    {
        ERROR("Failed to init framebuffer in emergency reboot\n");
        return;
    }
    fb_set_background(BLACK);

    klog = multirom_get_klog();

    t = fb_add_text(0, 120, WHITE, SIZE_NORMAL,
                "An error occured.\nShutting down MultiROM to avoid data corruption.\n"
                "Report this error to the developer!\nDebug info: /sdcard/multirom_log.txt\n\n"
                "Press POWER button to reboot.");

    t = fb_add_text(0, t->y + t->h + 100*DPI_MUL, GRAYISH, SIZE_SMALL, "Last lines from klog:");
    fb_add_rect(0, t->y + t->h + 5*DPI_MUL, fb_width, 1, GRAYISH);

    tail = klog+strlen(klog);
    last_end = tail;
    cur_y = fb_height;
    const int start_y = (t->y + t->h + 2);
    while(tail > klog)
    {
        --tail;
        if(*tail == '\n')
        {
            p = fb_text_create(0, cur_y, GRAYISH, 4*4, NULL);
            p->text = malloc(last_end - tail);
            memcpy(p->text, tail + 1, last_end - (tail + 1));
            p->text[last_end - (tail + 1)] = 0;
            p->style = STYLE_MONOSPACE;
            t = fb_text_finalize(p);

            cur_y -= t->h;
            t->y = cur_y;
            last_end = tail;

            if(cur_y < start_y)
            {
                fb_rm_text(t);
                break;
            }
        }
    }

    fb_force_draw();

    sprintf(path_log_file, "%s/multirom_log.txt", datamedia_dir);
    multirom_copy_log(klog, path_log_file);
    free(klog);

    set_mediarw_perms(path_log_file);

    // Wait for power key
    start_input_thread();
    while(wait_for_key() != KEY_POWER);
    stop_input_thread();

    fb_clear();
    fb_close();
}

static int find_idx(int c)
{
    static const char *capital = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char *normal  = "abcdefghijklmnopqrstuvwxyz";

    char *p;
    if((p = strchr(capital, c)))
        return p - capital;
    else if((p = strchr(normal, c)))
        return p - normal;

    return -128 + c;
}

static int compare_rom_names(const void *a, const void *b)
{
    struct multirom_rom *rom_a = *((struct multirom_rom **)a);
    struct multirom_rom *rom_b = *((struct multirom_rom **)b);

    if(rom_a->type == ROM_DEFAULT)
        return -1;
    else if(rom_b->type == ROM_DEFAULT)
        return 1;

    char *itr_a = rom_a->name;
    char *itr_b = rom_b->name;

    while(1)
    {
        if(*itr_a == 0)
            return -1;
        else if(*itr_b == 0)
            return 1;

        if(*itr_a == *itr_b)
        {
            ++itr_a;
            ++itr_b;
            continue;
        }

        int idx_a = find_idx(*itr_a);
        int idx_b = find_idx(*itr_b);

        if(idx_a == idx_b)
        {
            ++itr_a;
            ++itr_b;
            continue;
        }

        return idx_a < idx_b ? -1 : 1;
    }
    return 0;
}

int multirom_apk_get_roms(struct multirom_status *s)
{
    if(multirom_find_base_dir() == -1)
    {
        printf("Could not find multirom dir\n");
        return -1;
    }

    char current_rom[256] = { 0 };
    s->curr_rom_part = NULL;

    /* Read multirom.ini to find current_rom */
    char arg[256];
    sprintf(arg, "%s/multirom.ini", mrom_dir());

    FILE *f = fopen(arg, "re");
    if(f)
    {
        char line[1024];

        char name[64];
        char *pch;

        while((fgets(line, sizeof(line), f)))
        {
            pch = strtok (line, "=\n");
            if(!pch) continue;
            strcpy(name, pch);
            pch = strtok (NULL, "=\n");
            if(!pch) continue;
            strcpy(arg, pch);

            if(strstr(name, "current_rom"))
                strcpy(current_rom, arg);
            else if(strstr(name, "curr_rom_part"))
                s->curr_rom_part = strdup(arg);
        }

        printf("current_rom='%s' curr_rom_part='%s'\n", current_rom, (s->curr_rom_part ? s->curr_rom_part : ""));

        fclose(f);
    }
    else
    {
        printf("Failed to open config file, setting current_rom to null!\n");
    }

    /* Get Internal ROM */
    char roms_path[256];
    sprintf(roms_path, "%s/roms/"INTERNAL_ROM_NAME, mrom_dir());
    DIR *d = opendir(roms_path);
    if(!d)
    {
        printf("Failed to open Internal ROM's folder, creating one with ROM from internal memory...\n");
        multirom_import_internal();
    }
    else
        closedir(d);

    /* Get Internal Storage ROMs */
    sprintf(roms_path, "%s/roms", mrom_dir());
    d = opendir(roms_path);
    if(!d)
    {
        printf("Failed to open roms dir!\n");
        //return -1;
    }
    else
    {
        struct dirent *dr;
        char path[256];
        struct multirom_rom **add_roms = NULL;
        while((dr = readdir(d)))
        {
            if(dr->d_name[0] == '.')
                continue;

            if(dr->d_type != DT_DIR)
                continue;

            if(strlen(dr->d_name) > MAX_ROM_NAME_LEN)
            {
                printf("Skipping ROM %s, name is too long (max %d chars allowed)\n", dr->d_name, MAX_ROM_NAME_LEN);
                continue;
            }

            //printf("Adding ROM %s\n", dr->d_name);

            struct multirom_rom *rom = malloc(sizeof(struct multirom_rom));
            memset(rom, 0, sizeof(struct multirom_rom));

            rom->id = multirom_generate_rom_id();
            rom->name = strdup(dr->d_name);

            snprintf(path, sizeof(path), "%s/%s", roms_path, rom->name);
            rom->base_path = strdup(path);

            rom->type = multirom_get_rom_type(rom);

            snprintf(path, sizeof(path), "%s/boot.img", rom->base_path);
            rom->has_bootimg = access(path, R_OK) == 0 ? 1 : 0;

            multirom_find_rom_icon(rom);

            list_add(&add_roms, rom);
        }

        closedir(d);

        if(add_roms)
        {
            // sort roms
            qsort(add_roms, list_item_count(add_roms), sizeof(struct multirom_rom*), compare_rom_names);

            // add them to main list
            list_swap(&add_roms, &s->roms);
        }
    }

    /* Get External ROMs */
    multirom_update_partitions(s);

    int i;
    pthread_mutex_lock(&parts_mutex);
    for(i = 0; s->partitions && s->partitions[i]; ++i)
    {
        s->partitions[i]->keep_mounted = 1; // don't unmount on exit, the APK will need access to the folders
        multirom_scan_partition_for_roms(s, s->partitions[i]);
    }
    pthread_mutex_unlock(&parts_mutex);


    s->current_rom = multirom_get_rom(s, current_rom, s->curr_rom_part);
    if(!s->current_rom)
    {
        printf("Failed to find current rom (%s, part %s)!\n", current_rom, (s->curr_rom_part) ? s->curr_rom_part : "");
        free(s->curr_rom_part);
        s->curr_rom_part = NULL;
    }

    return 0;
}

int multirom_default_status(struct multirom_status *s)
{
    s->is_second_boot = 0;
    s->current_rom = NULL;
    s->roms = NULL;
#ifdef MR_NO_KEXEC
    s->no_kexec = MR_NO_KEXEC;
#endif
    s->colors = 0;
    s->brightness = MULTIROM_DEFAULT_BRIGHTNESS;
    s->enable_adb = 0;
    s->enable_kmsg_logging = 0;
    s->rotation = MULTIROM_DEFAULT_ROTATION;
    s->anim_duration_coef = 1.f;

    s->fstab = fstab_auto_load();
    if(!s->fstab)
        return -1;

    char roms_path[256];
    sprintf(roms_path, "%s/roms/"INTERNAL_ROM_NAME, mrom_dir());
    DIR *d = opendir(roms_path);
    if(!d)
    {
        ERROR("Failed to open Internal ROM's folder, creating one with ROM from internal memory...\n");
        multirom_import_internal();
    }
    else
        closedir(d);

    sprintf(roms_path, "%s/roms", mrom_dir());
    d = opendir(roms_path);
    if(!d)
    {
        ERROR("Failed to open roms dir!\n");
        return -1;
    }

    struct dirent *dr;
    char path[256];
    struct multirom_rom **add_roms = NULL;
    while((dr = readdir(d)))
    {
        if(dr->d_name[0] == '.')
            continue;

        if(dr->d_type != DT_DIR)
            continue;

        if(strlen(dr->d_name) > MAX_ROM_NAME_LEN)
        {
            ERROR("Skipping ROM %s, name is too long (max %d chars allowed)\n", dr->d_name, MAX_ROM_NAME_LEN);
            continue;
        }

        INFO("Adding ROM %s\n", dr->d_name);

        struct multirom_rom *rom = malloc(sizeof(struct multirom_rom));
        memset(rom, 0, sizeof(struct multirom_rom));

        rom->id = multirom_generate_rom_id();
        rom->name = strdup(dr->d_name);

        snprintf(path, sizeof(path), "%s/%s", roms_path, rom->name);
        rom->base_path = strdup(path);

        rom->type = multirom_get_rom_type(rom);

        snprintf(path, sizeof(path), "%s/boot.img", rom->base_path);
        rom->has_bootimg = access(path, R_OK) == 0 ? 1 : 0;

        multirom_find_rom_icon(rom);

        list_add(&add_roms, rom);
    }

    closedir(d);

    if(add_roms)
    {
        // sort roms
        qsort(add_roms, list_item_count(add_roms), sizeof(struct multirom_rom*), compare_rom_names);

        // add them to main list
        list_swap(&add_roms, &s->roms);
    }

    s->current_rom = multirom_get_internal(s);
    if(!s->current_rom)
    {
        ERROR("No internal rom found!\n");
        return -1;
    }

    s->auto_boot_rom = s->current_rom;
    s->auto_boot_seconds = 5;
    s->auto_boot_type = AUTOBOOT_NAME;

    return 0;
}

int multirom_load_status(struct multirom_status *s)
{
    INFO("Loading MultiROM status...\n");

    multirom_default_status(s);

    if(mrom_is_second_boot())
        s->is_second_boot = 1;

    // is_second_boot might be reset later, but we need to know if this
    // is second boot when filling in kexec info
    s->is_running_in_primary_rom = !s->is_second_boot;

    char arg[256];
    sprintf(arg, "%s/multirom.ini", mrom_dir());

    FILE *f = fopen(arg, "re");
    if(!f)
    {
        ERROR("Failed to open config file, using defaults!\n");
        return -1;
    }

    char line[1024];
    char current_rom[256] = { 0 };
    char auto_boot_rom[256] = { 0 };

    char name[64];
    char *pch;

    while((fgets(line, sizeof(line), f)))
    {
        pch = strtok (line, "=\n");
        if(!pch) continue;
        strcpy(name, pch);
        pch = strtok (NULL, "=\n");
        if(!pch) continue;
        strcpy(arg, pch);

        if(strstr(name, "current_rom"))
            strcpy(current_rom, arg);
        else if(strstr(name, "auto_boot_seconds"))
            s->auto_boot_seconds = atoi(arg);
        else if(strstr(name, "auto_boot_rom"))
            strcpy(auto_boot_rom, arg);
        else if(strstr(name, "auto_boot_type"))
            s->auto_boot_type = atoi(arg);
        else if(strstr(name, "curr_rom_part"))
            s->curr_rom_part = strdup(arg);
#ifdef MR_NO_KEXEC
        else if(strstr(name, "no_kexec"))
            s->no_kexec = atoi(arg);
#endif
        else if(strstr(name, "colors_v2"))
            s->colors = atoi(arg);
        else if(strstr(name, "brightness"))
            s->brightness = atoi(arg);
        else if(strstr(name, "enable_adb"))
            s->enable_adb = atoi(arg);
        else if(strstr(name, "enable_kmsg_logging"))
            s->enable_kmsg_logging = atoi(arg);
        else if(strstr(name, "hide_internal"))
            s->hide_internal = atoi(arg);
        else if(strstr(name, "int_display_name"))
            s->int_display_name = strdup(arg);
        else if(strstr(name, "rotation"))
            s->rotation = atoi(arg);
        else if(strstr(name, "force_generic_fb"))
            s->force_generic_fb = atoi(arg);
        else if(strstr(name, "anim_duration_coef_pct"))
            s->anim_duration_coef = ((float)atoi(arg)) / 100;
    }

    fclose(f);

    // find USB drive if we're booting from it
    if(s->curr_rom_part) // && s->is_second_boot)
    {
        multirom_update_and_scan_for_external_roms(s, s->curr_rom_part);
    }

    s->current_rom = multirom_get_rom(s, current_rom, s->curr_rom_part);
    if(!s->current_rom)
    {
        ERROR("Failed to select current rom (%s, part %s), using Internal!\n", current_rom, s->curr_rom_part);
        s->current_rom = multirom_get_internal(s);
        free(s->curr_rom_part);
        s->curr_rom_part = NULL;
        if(!s->current_rom)
        {
            ERROR("No internal rom found!\n");
            return -1;
        }
    }

    if((s->auto_boot_type & AUTOBOOT_LAST)) // && !s->curr_rom_part)
    {
        s->auto_boot_rom = s->current_rom;
    }
    else
    {
        s->auto_boot_rom = multirom_get_rom(s, auto_boot_rom, NULL);
        if(!s->auto_boot_rom)
            ERROR("Could not find rom %s to auto-boot\n", auto_boot_rom);
    }

    if(s->int_display_name)
    {
        struct multirom_rom *r = multirom_get_internal(s);
        r->name = realloc(r->name, strlen(s->int_display_name)+1);
        strcpy(r->name, s->int_display_name);
    }

    fb_force_generic_impl(s->force_generic_fb);

    if(s->anim_duration_coef == 0)
        s->anim_duration_coef = 1.f;

    return 0;
}

int multirom_save_status(struct multirom_status *s)
{
    INFO("Saving multirom status\n");

    char path[256];
    char auto_boot_name[MAX_ROM_NAME_LEN+1];
    char current_name[MAX_ROM_NAME_LEN+1];

    snprintf(path, sizeof(path), "%s/multirom.ini", mrom_dir());

    FILE *f = fopen(path, "we");
    if(!f)
    {
        ERROR("Failed to open/create status file!\n");
        return -1;
    }

    multirom_fixup_rom_name(s->auto_boot_rom, auto_boot_name, "");
    multirom_fixup_rom_name(s->current_rom, current_name, INTERNAL_ROM_NAME);

    fprintf(f, "current_rom=%s\n", current_name);
    fprintf(f, "auto_boot_seconds=%d\n", s->auto_boot_seconds);
    fprintf(f, "auto_boot_rom=%s\n", auto_boot_name);
    fprintf(f, "auto_boot_type=%d\n", s->auto_boot_type);
    fprintf(f, "curr_rom_part=%s\n", s->curr_rom_part ? s->curr_rom_part : "");
#ifdef MR_NO_KEXEC
    fprintf(f, "no_kexec=%d\n", s->no_kexec);
#endif
    fprintf(f, "colors_v2=%d\n", s->colors);
    fprintf(f, "brightness=%d\n", s->brightness);
    fprintf(f, "enable_adb=%d\n", s->enable_adb);
    fprintf(f, "enable_kmsg_logging=%d\n", s->enable_kmsg_logging);
    fprintf(f, "hide_internal=%d\n", s->hide_internal);
    fprintf(f, "int_display_name=%s\n", s->int_display_name ? s->int_display_name : "");
    fprintf(f, "rotation=%d\n", s->rotation);
    fprintf(f, "force_generic_fb=%d\n", s->force_generic_fb);
    fprintf(f, "anim_duration_coef_pct=%d\n", (int)(s->anim_duration_coef*100));

    fclose(f);
    return 0;
}

void multirom_fixup_rom_name(struct multirom_rom *rom, char *name, const char *def)
{
    if(rom)
    {
        if(rom->type == ROM_DEFAULT)
            strcpy(name, INTERNAL_ROM_NAME);
        else
            strcpy(name, rom->name);
    }
    else
    {
        strcpy(name, def);
    }
}

void multirom_dump_status(struct multirom_status *s)
{
    INFO("Dumping multirom status:\n");
    INFO("  is_second_boot=%d\n", s->is_second_boot);
    INFO("  is_running_in_primary_rom=%d\n", s->is_running_in_primary_rom);
    INFO("  current_rom=%s\n", s->current_rom ? s->current_rom->name : "NULL");
#ifdef MR_NO_KEXEC
    INFO("  no_kexec=%d\n", s->no_kexec);
#endif
    INFO("  colors_v2=%d\n", s->colors);
    INFO("  brightness=%d\n", s->brightness);
    INFO("  enable_adb=%d\n", s->enable_adb);
    INFO("  enable_kmsg_logging=%d\n", s->enable_kmsg_logging);
    INFO("  rotation=%d\n", s->rotation);
    INFO("  force_generic_fb=%d\n", s->force_generic_fb);
    INFO("  anim_duration_coef=%f\n", s->anim_duration_coef);
    INFO("  hide_internal=%d\n", s->hide_internal);
    INFO("  int_display_name=%s\n", s->int_display_name ? s->int_display_name : "NULL");
    INFO("  auto_boot_seconds=%d\n", s->auto_boot_seconds);
    INFO("  auto_boot_rom=%s\n", s->auto_boot_rom ? s->auto_boot_rom->name : "NULL");
    INFO("  auto_boot_type=%d\n", s->auto_boot_type);
    INFO("  curr_rom_part=%s\n", s->curr_rom_part ? s->curr_rom_part : "NULL");
    INFO("\n");

    int i;
    for(i = 0; s->roms && s->roms[i]; ++i)
    {
        INFO("  ROM: %s\n", s->roms[i]->name);
        INFO("    base_path: %s\n", s->roms[i]->base_path);
        INFO("    icon_path: %s\n", s->roms[i]->icon_path);
        INFO("    type: %d\n", s->roms[i]->type);
        INFO("    has_bootimg: %d\n", s->roms[i]->has_bootimg);
        if(s->roms[i]->partition)
            INFO("    partition: %s (%s)\n", s->roms[i]->partition->name, s->roms[i]->partition->fs);
    }
}

void multirom_free_status(struct multirom_status *s)
{
    list_clear(&s->partitions, &multirom_destroy_partition);
    list_clear(&s->roms, &multirom_free_rom);
    free(s->curr_rom_part);
    free(s->int_display_name);
    if (s->fstab) fstab_destroy(s->fstab);
}

void multirom_free_rom(void *rom)
{
    free(((struct multirom_rom*)rom)->name);
    free(((struct multirom_rom*)rom)->base_path);
    free(((struct multirom_rom*)rom)->icon_path);
    free(rom);
}

void multirom_find_usb_roms(struct multirom_status *s)
{
    char auto_boot_name[MAX_ROM_NAME_LEN+1];
    char current_name[MAX_ROM_NAME_LEN+1];
    multirom_fixup_rom_name(s->auto_boot_rom, auto_boot_name, "");
    multirom_fixup_rom_name(s->current_rom, current_name, INTERNAL_ROM_NAME);

    // remove USB roms
    int i;
    for(i = 0; s->roms && s->roms[i];)
    {
        if(s->roms[i]->partition)
        {
            list_rm_at(&s->roms, i, &multirom_free_rom);
            i = 0;
        }
        else ++i;
    }

    char path[256];
    struct usb_partition *p;

    pthread_mutex_lock(&parts_mutex);
    for(i = 0; s->partitions && s->partitions[i]; ++i)
        multirom_scan_partition_for_roms(s, s->partitions[i]);
    pthread_mutex_unlock(&parts_mutex);

    s->current_rom = multirom_get_rom(s, current_name, s->curr_rom_part);
    s->auto_boot_rom = multirom_get_rom(s, auto_boot_name, s->curr_rom_part);

    if (!s->current_rom)
         s->current_rom = multirom_get_internal(s);

    if (!s->auto_boot_rom)
        s->auto_boot_rom = multirom_get_internal(s);

    multirom_dump_status(s);
}

int multirom_scan_partition_for_roms(struct multirom_status *s, struct usb_partition *p)
{
    char path[256];
    int i;
    struct dirent *dr;
    struct multirom_rom **add_roms = NULL;

#ifdef MR_MOVE_USB_DIR
    // groupers will have old "multirom" folder on USB drive instead of "multirom-grouper".
    // We have to move it.
    sprintf(path, "%s/multirom", p->mount_path);
    if(access(path, F_OK) >= 0)
    {
        char dest[256];
        sprintf(dest, "%s/multirom-"TARGET_DEVICE, p->mount_path);

        INFO("Moving usb dir %s to %s!\n", path, dest);

        mkdir(dest, 0777);

        char *cmd[] = { busybox_path, "sh", "-c", malloc(1024), NULL };
        sprintf(cmd[3], "%s mv \"%s\"/* \"%s\"/", busybox_path, path, dest);

        run_cmd(cmd);

        rmdir(path);
        free(cmd[3]);

        sync();
    }
#endif

    sprintf(path, "%s/multirom-"TARGET_DEVICE, p->mount_path);
    if(access(path, F_OK) < 0)
        return -1;

    DIR *d = opendir(path);
    if(!d)
        return -1;

    while((dr = readdir(d)) != NULL)
    {
        if(dr->d_name[0] == '.')
            continue;

        struct multirom_rom *rom = malloc(sizeof(struct multirom_rom));
        memset(rom, 0, sizeof(struct multirom_rom));

        rom->id = multirom_generate_rom_id();
        rom->name = strdup(dr->d_name);

        sprintf(path, "%s/multirom-"TARGET_DEVICE"/%s", p->mount_path, rom->name);
        rom->base_path = strdup(path);

        rom->partition = p;
        rom->type = multirom_get_rom_type(rom);

        sprintf(path, "%s/boot.img", rom->base_path);
        rom->has_bootimg = access(path, R_OK) == 0 ? 1 : 0;

        multirom_find_rom_icon(rom);

        list_add(&add_roms, rom);
    }
    closedir(d);

    if(add_roms)
    {
        // sort roms
        qsort(add_roms, list_item_count(add_roms), sizeof(struct multirom_rom*), compare_rom_names);

        list_add_from_list(&s->roms, add_roms);
        list_clear(&add_roms, NULL);
    }
    return 0;
}

int multirom_path_exists(char *base, char *filename)
{
    char path[256];
    sprintf(path, "%s/%s", base, filename);
    if(access(path, R_OK) < 0)
        return -1;
    return 0;
}

int multirom_get_rom_type(struct multirom_rom *rom)
{
    if(!rom->partition && strcmp(rom->name, INTERNAL_ROM_NAME) == 0)
        return ROM_DEFAULT;

    char *b = rom->base_path;

    // Handle android ROMs
    if(!multirom_path_exists(b, "boot"))
    {
        if(!multirom_path_exists(b, "system") && !multirom_path_exists(b, "data") &&
            !multirom_path_exists(b, "cache"))
        {
            if(!rom->partition) return ROM_ANDROID_INTERNAL;
            else                return ROM_ANDROID_USB_DIR;
        }
        else if(!multirom_path_exists(b, "system.sparse.img") &&
                (!multirom_path_exists(b, "data") || !multirom_path_exists(b, "data.sparse.img")) &&
                (!multirom_path_exists(b, "cache") || !multirom_path_exists(b, "cache.sparse.img")))
        {
            if(!rom->partition) return ROM_ANDROID_INTERNAL_HYBRID;
            else                return ROM_ANDROID_USB_HYBRID;
        }
        else if(!multirom_path_exists(b, "system.img") && !multirom_path_exists(b, "data.img") &&
                !multirom_path_exists(b, "cache.img"))
        {
            return ROM_ANDROID_USB_IMG;
        }
    }

    // handle linux ROMs
    if(!multirom_path_exists(b, "rom_info.txt"))
    {
        if(!rom->partition)
            return ROM_LINUX_INTERNAL;
        else
            return ROM_LINUX_USB;
    }

    // Handle Ubuntu 13.04 - deprecated
    if ((!multirom_path_exists(b, "root") && multirom_path_exists(b, "boot.img")) ||
       (!multirom_path_exists(b, "root.img") && rom->partition))
    {
        // try to copy rom_info.txt in there, ubuntu is deprecated
        ERROR("Found deprecated Ubuntu 13.04, trying to copy rom_info.txt...\n");
        char *cmd[] = { busybox_path, "cp", malloc(256), malloc(256), NULL };
        sprintf(cmd[2], "%s/infos/ubuntu.txt", mrom_dir());
        sprintf(cmd[3], "%s/rom_info.txt", b);

        int res = run_cmd(cmd);

        free(cmd[2]);
        free(cmd[3]);

        if(res != 0)
        {
            ERROR("Failed to copy rom_info for Ubuntu!\n");
            if(!rom->partition) return ROM_UNSUPPORTED_INT;
            else                return ROM_UNSUPPORTED_USB;
        }
        else
        {
            if(!rom->partition) return ROM_LINUX_INTERNAL;
            else                return ROM_LINUX_USB;
        }
    }

    // Handle ubuntu 12.10
    if(!multirom_path_exists(b, "root") && !multirom_path_exists(b, "boot.img"))
    {
        if(!rom->partition) return ROM_UNSUPPORTED_INT;
        else                return ROM_UNSUPPORTED_USB;
    }

    return ROM_UNKNOWN;
}

void multirom_import_internal(void)
{
    char path[256];

    // multirom
    mkdir(mrom_dir(), 0777);

    // roms
    snprintf(path, sizeof(path), "%s/roms", mrom_dir());
    mkdir(path, 0777);

    // internal rom
    snprintf(path, sizeof(path), "%s/roms/%s", mrom_dir(), INTERNAL_ROM_NAME);
    mkdir(path, 0777);

    // set default icon if it doesn't exist yet
    snprintf(path, sizeof(path), "%s/roms/%s/.icon_data", mrom_dir(), INTERNAL_ROM_NAME);
    if(access(path, F_OK) < 0)
    {
        FILE *f = fopen(path, "we");
        if(f)
        {
            fputs("predef_set\ncom.tassadar.multirommgr:drawable/romic_android\n", f);
            fclose(f);
        }
    }
}

struct multirom_rom *multirom_get_internal(struct multirom_status *s)
{
    int i;
    for(i = 0; s->roms && s->roms[i]; ++i)
    {
        if(s->roms[i]->type == ROM_DEFAULT)
            return s->roms[i];
    }
    ERROR(" Something is wrong, multirom_get_internal returns NULL!\n");
    return NULL;
}

struct multirom_rom *multirom_get_rom(struct multirom_status *s, const char *name, const char *part_uuid)
{
    if(part_uuid == NULL && strcmp(name, INTERNAL_ROM_NAME) == 0)
        return multirom_get_internal(s);

    int i = 0;
    struct multirom_rom *r;
    for(; s->roms && s->roms[i]; ++i)
    {
        r = s->roms[i];
        if (r->type != ROM_DEFAULT && strcmp(r->name, name) == 0 &&
           (!part_uuid || (r->partition && strcmp(r->partition->uuid, part_uuid) == 0)))
        {
            return r;
        }
    }

    return NULL;
}

int multirom_generate_rom_id(void)
{
    static int id = 0;
    return id++;
}

struct multirom_rom *multirom_get_rom_by_id(struct multirom_status *s, int id)
{
    int i = 0;
    for(; s->roms && s->roms[i]; ++i)
        if(s->roms[i]->id == id)
            return s->roms[i];
    return NULL;
}

int multirom_prepare_for_boot(struct multirom_status *s, struct multirom_rom *to_boot)
{
    int exit = EXIT_UMOUNT;
    int type = to_boot->type;


#ifndef MR_NO_KEXEC
    if(((M(type) & MASK_KEXEC) || to_boot->has_bootimg) && type != ROM_DEFAULT && s->is_second_boot == 0)
#else
    if(((M(type) & MASK_KEXEC) || (to_boot->has_bootimg && (nokexec()->selected_method == NO_KEXEC_BOOT_NORMAL))) && type != ROM_DEFAULT && s->is_second_boot == 0)
#endif
    {
        if(multirom_load_kexec(s, to_boot) != 0)
            return -1;
        exit |= EXIT_KEXEC;
    }

    switch(type)
    {
        case ROM_DEFAULT:
        {
            if (!access(DT_FSTAB_PATH, F_OK)) {
                if (mount_dtb_fstab("system") == 0) {
                    disable_dtb_fstab("system");
                }
            }
            rom_quirks_on_initrd_finalized();
            break;
        }
        case ROM_LINUX_INTERNAL:
        case ROM_LINUX_USB:
            break;
        case ROM_ANDROID_USB_IMG:
        case ROM_ANDROID_USB_DIR:
        case ROM_ANDROID_INTERNAL:
        case ROM_ANDROID_INTERNAL_HYBRID:
        case ROM_ANDROID_USB_HYBRID:
        {
            if(!(exit & (EXIT_REBOOT | EXIT_KEXEC)))
            {
                exit &= ~(EXIT_UMOUNT);

                if(multirom_prep_android_mounts(s, to_boot) == -1)
                    return -1;

                if(multirom_create_media_link(s) == -1)
                    return -1;

                rom_quirks_on_initrd_finalized();

                rcadditions_write_to_files(&s->rc);
                rcadditions_free(&s->rc);
            }

            if(to_boot->partition)
                to_boot->partition->keep_mounted = 1;
            break;
        }
        default:
            ERROR("Unknown ROM type\n");
            return -1;
    }

    return exit;
}

#define EXEC_MASK (S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP)

char *multirom_find_fstab_in_rc(const char *rcfile)
{
    FILE *f = fopen(rcfile, "re");
    if(!f)
    {
        ERROR("Failed to open rcfile %s\n", rcfile);
        return NULL;
    }

    char *p,*e;
    char line[1024];
    while(fgets(line, sizeof(line), f))
    {
        for(p = line; isspace(*p); ++p) { }

        for(e = p+strlen(p)-1; isspace(*e); --e)
            *e = 0;

        if(*p == '#' || *p == 0)
            continue;

        if(strstr(p, "mount_all") == p)
        {
            fclose(f);

            p += sizeof("mount_all")-1;
            for(; isspace(*p); ++p) { }

            return strdup(p);
        }
    }
    fclose(f);
    return NULL;
}

// On L dev preview and presumably later android releases, the firmware image
// has mount option "context=...", which sets SELinux context for that whole
// mount. It needs initialized SELinux in order to successfully mount, which
// means it can't be done while in multirom (SELinux is initalized in real
// init, after multirom exits). Workaround as follows:
//  * inject 'start mrom_fw_mounter' before mount_all command in .rc file
//  * append service mrom_fw_mounter block into said .rc file. This service
//    starts binary 'fw_mounter', which is part of MultiROM and it just
//    mounts the FW image file.
//  * Copy fw_mounter to /sbin/ and setup its fstab
//  * Real init starts mrom_fw_mounter service which mounts the image
//
// SELinux compains about the fw_mounter not having context set, but it still
// works. There is a chance Google will disable all services which don't have
// context set in sepolicy. That will be a problem.

// UPDATE: fw_mounter gets shut down by SELinux on 6.0, inject .rc files and file_contexts instead.

static int multirom_inject_fw_mounter(struct multirom_status *s, struct fstab_part *fw_part)
{
    char buf[512];

    rcadditions_append_contexts(&s->rc, fw_part->device);
    rcadditions_append_contexts(&s->rc, " u:object_r:asec_image_file:s0\n");

    snprintf(buf, sizeof(buf), "    restorecon %s\n", fw_part->device);
    rcadditions_append_trigger(&s->rc, "fs", buf);

    snprintf(buf, sizeof(buf), "    mount %s loop@%s %s ", fw_part->type, fw_part->device, fw_part->path);
    rcadditions_append_trigger(&s->rc, "fs", buf);

    if(fw_part->options_raw)
    {
        char *c, *opts = strdup(fw_part->options_raw);
        for(c = opts; *c; ++c)
            if(*c == ',')
                *c = ' ';
        rcadditions_append_trigger(&s->rc, "fs", opts);
        free(opts);
    }

    rcadditions_append_trigger(&s->rc, "fs", "\n");
    return 0;
}

int multirom_prep_android_mounts(struct multirom_status *s, struct multirom_rom *rom)
{
    char in[128];
    char out[128];
    char path[256];
    char *fstab_name = NULL;
    struct stat stat;
    int has_fw = 0;
    int found_fstab = 0;
    struct fstab_part *fw_part = NULL;
    int res = -1;

    sprintf(path, "%s/firmware.img", rom->base_path);
    has_fw = (access(path, R_OK) >= 0);

    sprintf(path, "%s/boot", rom->base_path);

    DIR *d = opendir(path);
    char* prefix = NULL;
    if(!d)
    {
        ERROR("Failed to open rom path %s\n", path);
        return -1;
    }

    struct dirent *dp = NULL;

    while((dp = readdir(d)))
    {
        if((dp->d_name[0] == '.' && strlen(dp->d_name) == 1) || (dp->d_name[0] == '.' && (dp->d_name[1] == '.') || dp->d_name[1] == 0))
            continue;

        if (dp->d_type == DT_DIR) {
            sprintf(in, "%s/%s", path, dp->d_name);
            d = opendir(in);
            prefix = dp->d_name;
            continue;
        }

        if (prefix) {
            sprintf(out, "/%s/%s", prefix, dp->d_name);
            sprintf(in, "%s/%s/%s", path, prefix, dp->d_name);
        } else {
            sprintf(in, "%s/%s", path, dp->d_name);
            sprintf(out, "/%s", dp->d_name);
        }

        copy_file(in, out);

        if(strstr(dp->d_name, ".rc"))
        {
            // set permissions for .rc files
            chmod(out, EXEC_MASK);

            if(!fstab_name && strcmp(dp->d_name, "init."TARGET_DEVICE".rc") == 0)
                fstab_name = multirom_find_fstab_in_rc(out);
        }
    }
    closedir(d);

    if(multirom_process_android_fstab(fstab_name, has_fw, &fw_part, 0) != 0) {
        INFO("fstab couldnt be found in ramdisk. Rom maybe treble. Retry after vendor mount\n");
    } else {
        found_fstab = 1;
    }

    unlink("/cache");

    mkdir_with_perms("/system", 0755, NULL, NULL);
    mkdir_with_perms("/data", 0771, "system", "system");
    mkdir_with_perms("/cache", 0770, "system", "cache");
    mkdir_with_perms("/vendor", 0755, NULL, NULL);
    if(has_fw)
        mkdir_with_perms("/firmware", 0771, "system", "system");

    /* In case device is encrypted, vendor is renamed to vendor_boot
     * Now that decryption is done and partition is mounted, we dont need multirom's vendor folder.
     * So we can safely remove it */

    if (lstat("/vendor_boot", &stat) == 0) {
        remove("/vendor");
        rename("/vendor_boot", "/vendor");
    }
    static const char *folders[4] = { "system"   , "vendor"                 , "data"               , "cache"};
    unsigned long flags[4]        = { MS_RDONLY  ,  MS_RDONLY, MS_NOSUID | MS_NODEV  ,  MS_NOSUID | MS_NODEV};

    uint32_t i;
    char from[256];
    char to[256];

    for (i = 0; i < ARRAY_SIZE(folders); ++i) {
        snprintf(to, sizeof(to), "/%s", folders[i]);
        snprintf(from, sizeof(from), "%s/%s", rom->base_path, folders[i]);
        if (!access(from, R_OK)) {
            if (strstr(from, "vendor") && multirom_path_exists(rom->base_path, "vendor/etc")) {
                continue;
            }
            if(mount(from, to, "ext4", MS_BIND | flags[i], "discard,nomblk_io_submit") < 0) {
                ERROR("Failed to mount %s to %s (%d: %s)\n", from, to, errno, strerror(errno));
                goto exit;
            } else
                INFO("Bind mounted %s on %s\n", from, to);
            continue;
        }

        snprintf(from, sizeof(from), "%s/%s.img", rom->base_path, folders[i]);
        if (!access(from, R_OK)) {
            if(mount_image(from, to, "ext4", flags[i], NULL) < 0)
                goto exit;
            else
                INFO("Loop mounted %s on %s\n", from, to);
            continue;
        }

        snprintf(from, sizeof(from), "%s/%s.sparse.img", rom->base_path, folders[i]);
        if (!access(from, R_OK)) {
            if(multirom_mount_image(from, to, "ext4", flags[i], NULL) < 0)
                goto exit;
            else
                INFO("MultiROM Loop mounted %s on %s\n", from, to);
            continue;
        }

        if (strstr(from, "vendor")) {
            INFO("vendor not found, skipping\n");
            continue;
        }
        // Neither directory nor .img nor .sparse.img was found, panic
        goto exit;
    }

    if((multirom_path_exists("/system", "vendor/etc") == -1) &&
            (multirom_path_exists(rom->base_path, "vendor/etc") == -1) &&
            (multirom_path_exists("/", "vendor/etc") == -1)) {
        if (!access(DT_FSTAB_PATH, F_OK)) {
            mount_dtb_fstab("vendor");
        }
    }
    if (!access(DT_FSTAB_PATH, F_OK)) {
        remove_dtb_fstab();
    }
    if(!found_fstab && multirom_process_android_fstab(NULL, has_fw, &fw_part, 1) != 0) {
        INFO("fstab not found even in vendor!\n");
        goto exit;
    }

    // mount() does not necessarily obey the mount options (eg, ro on system or nosuid on data)
    // so attempt a remount on the partitions, but don't abort if the remount in unsuccessful,
    // unless MR_PANIC_ON_FAILED_REMOUNT is set: eg on the HTC 10 if system is not ro, then the
    // dir permissions may get changed to disallow 'x'
    struct statfs statfs_buf;
    for (i = 0; i < ARRAY_SIZE(folders); ++i) {
        snprintf(to, sizeof(to), "/%s", folders[i]);
        if(statfs(to, &statfs_buf) < 0)
            ERROR("Couldn't statfs %s (%d: %s)\n", to, errno, strerror(errno));
        else if((flags[i] & ~statfs_buf.f_flags)) {
            INFO("Mount flags of %s are 0x%08lX but should include 0x%08lX, attempting remount\n",
                 to, (unsigned long) statfs_buf.f_flags, flags[i]);

            if(mount(NULL, to, NULL, MS_REMOUNT | flags[i], NULL) < 0) {
                ERROR("Failed to remount %s (%d: %s)\n", to, errno, strerror(errno));
#ifdef MR_PANIC_ON_FAILED_REMOUNT
                goto exit;
#endif
            }
            else {
                if(statfs(to, &statfs_buf) < 0)
                    INFO("Remounted %s\n", to);
                else
                    INFO("Remounted %s flags=0x%08lX\n", to, (unsigned long) statfs_buf.f_flags);
            }
        }
    }

    if(has_fw && fw_part)
    {
        INFO("Mounting ROM's FW image instead of FW partition\n");
        snprintf(from, sizeof(from), "%s/firmware.img", rom->base_path);
        fw_part->device = realloc(fw_part->device, strlen(from)+1);
        strcpy(fw_part->device, from);
        multirom_inject_fw_mounter(s, fw_part);
    }

#if MR_DEVICE_HOOKS >= 1
    int hooks_res = mrom_hook_after_android_mounts(busybox_path, rom->base_path, rom->type);
    if(hooks_res < 0)
    {
        ERROR("mrom_hook_after_android_mounts returned %d!\n", hooks_res);
        goto exit;
    }
#endif

    res = 0;
exit:
    if(fw_part)
        fstab_destroy_part(fw_part);
    return res;
}

int multirom_process_android_fstab(char *fstab_name, int has_fw, struct fstab_part **fw_part, int treble_fstab)
{
    int res = -1;
    char* dirname;

    if(fstab_name != NULL)
        INFO("Using fstab %s from rc files\n", fstab_name);
    else
    {
        DIR *d;
        if (treble_fstab) {
            dirname = "/vendor/etc/";
            d = opendir(dirname);
        } else {
            d = opendir("/");
        }

        if(!d)
        {
            ERROR("Failed to open root folder!\n");
            return -1;
        }

        struct dirent *dp = NULL;
        while((dp = readdir(d)))
        {
            if(strstr(dp->d_name, "fstab.") == dp->d_name && strcmp(dp->d_name, "fstab.goldfish") != 0 && strcmp(dp->d_name, "fstab.ranchu") != 0)
            {
                fstab_name = realloc(fstab_name, strlen(dp->d_name)+1);
                strcpy(fstab_name, dp->d_name);
                // try to find specifically fstab.device
                if(strcmp(fstab_name, "fstab."TARGET_DEVICE) == 0)
                    break;
            }
        }
        closedir(d);

        if(!fstab_name)
        {
            ERROR("Failed to find fstab file in root!\n");
            return -1;
        }
    }

    if (treble_fstab) {
        char name[strlen(fstab_name) + 1];
        strcpy(name, fstab_name);
        fstab_name = realloc(fstab_name, strlen(name) + strlen(dirname) + 1);
        sprintf(fstab_name, "%s%s", dirname, name);
    }
    ERROR("Modifying fstab: %s\n", fstab_name);
    struct fstab *tab = fstab_load(fstab_name, 0);
    if(!tab)
        goto exit;

    int disable_sys = fstab_disable_parts(tab, "/system");
    int disable_data = fstab_disable_parts(tab, "/data");
    int disable_cache = fstab_disable_parts(tab, "/cache");
    int disable_vendor = fstab_disable_parts(tab, "/vendor");

    if((!treble_fstab) && disable_sys < 0 || disable_data < 0 || disable_cache < 0)
    {
#if MR_DEVICE_HOOKS >= 4
        if(!mrom_hook_allow_incomplete_fstab())
#endif
        {
            goto exit;
        }
    }

    if(has_fw)
    {
        struct fstab_part *p = fstab_find_first_by_path(tab, "/firmware");
        if(p)
        {
            *fw_part = fstab_clone_part(p);
            p->disabled = 1;
        }
    }

    // Android N needs a /data entry in fstab otherwise Developer Options won't work
    // because of a segmentation fault in cryptfs.c, detailed information can be
    // found here: http://forum.xda-developers.com/showpost.php?p=69224932&postcount=1706
    //
    // Possible option is to use a remount in fstab:
    //      fstab_add_part(tab, "/data", "/data", "auto", "remount", "defaults");
    // however this will actually execute, and the mount options may get changed, in my case
    // from     /dev/block/mmcblk0p47 /data ext4 rw,seclabel,nodev,relatime,discard,noauto_da_alloc,data=ordered 0 0
    // to       /dev/block/mmcblk0p47 /data ext4 rw,seclabel,relatime,discard,noauto_da_alloc,data=ordered 0 0
    // we could do
    //      fstab_add_part(tab, "/data", "/data", "auto", "remount,nodev", "defaults");
    //
    // A better solution is actually creating an entry that will be skipped, and fs_mgr will skip the
    // following types of partitions during mount_all:
    // 1) Partitions marked with fs_mgr_flags "recoveryonly", eg:    fstab_add_part(tab, "/data", "/data", "auto", "remount", "recoveryonly");
    // 2) Partitions having fs_type "swap" or "emmc" or "mtd", eg:   fstab_add_part(tab, "/data", "/data", "swap", "defaults", "defaults");
    //
    // opt for recoveryonly at this time
    fstab_add_part(tab, "/data", "/data", "auto", "remount", "recoveryonly");

    // Android considers empty fstab invalid
    if(tab->count <= 3 + has_fw)
    {
        INFO("fstab would be empty, adding dummy line\n");
        fstab_add_part(tab, "tmpfs", "/dummy_tmpfs", "tmpfs", "ro,nosuid,nodev", "defaults");
        mkdir("/dummy_tmpfs", 0644);
    }

    char* fstab_save_path = strstr(fstab_name, "fstab");
    if(fstab_save(tab, fstab_save_path) == 0)
        res = 0;

    if (treble_fstab) {
        if(!mount(fstab_save_path, fstab_name, "ext4", MS_BIND, "discard,nomblk_io_submit")) {
            INFO("fstab bind mounted in vendor\n");
        } else {
            ERROR("fstab bind mount failed on %s! %s\n", fstab_name, strerror(errno));
            res = -1;
            goto exit;
        }
    }

exit:
    if(tab)
        fstab_destroy(tab);
    free(fstab_name);
    return res;
}

int multirom_create_media_link(struct multirom_status *s)
{
    int media_new = 0;
    int api_level = multirom_get_api_level("/system/build.prop");
    if(api_level <= 0)
        return -1;

    struct stat info;
    if(stat(REALDATA"/media/0", &info) >= 0)
        media_new = 1;

    static const char *paths[] = {
        REALDATA"/media",      // 0
        REALDATA"/media/0",    // 1

        "/data/media",         // 2
        "/data/media/0",       // 3
    };

    int from, to;

    if(api_level <= 16)
    {
        to = 2;
        if(!media_new) from = 0;
        else           from = 1;
    }
    else if(api_level >= 17)
    {
        from = 0;
        if(!media_new) to = 3;
        else           to = 2;
    }

    ERROR("Making media dir: api %d, media_new %d, %s to %s\n", api_level, media_new, paths[from], paths[to]);
    if(mkdir_recursive(paths[to], 0775) == -1)
    {
        ERROR("Failed to make media dir on %s\n", paths[to]);
        return -1;
    }

    if(mount(paths[from], paths[to], "ext4", MS_BIND, "") < 0)
    {
        ERROR("Failed to bind media folder %d (%s)\n", errno, strerror(errno));
        return -1;
    }

    if(api_level >= 17)
    {
        char buf[16];
        buf[0] = 0;

        FILE *f = fopen(LAYOUT_VERSION, "re");
        const int rewrite = (!f || !fgets(buf, sizeof(buf), f) || atoi(buf) < 2);

        if(f)
            fclose(f);

        if(rewrite)
        {
            f = fopen(LAYOUT_VERSION, "we");
            if(!f)
            {
                ERROR("Failed to create .layout_version!\n");
                return -1;
            }

            fputc(api_level > 19 ? '3' : '2', f);
            fclose(f);
            chmod(LAYOUT_VERSION, 0600);
        }

        // We need to set SELinux context for this file in case it was created by multirom,
        // but can't do it here because selinux was not initialized
        rcadditions_append_trigger(&s->rc, "post-fs-data", "    restorecon " LAYOUT_VERSION "\n");
    }

    return 0;
}

int multirom_get_api_level(const char *path)
{
    FILE *f = fopen(path, "re");
    if(!f)
    {
        ERROR("Could not open %s to read api level!\n", path);
        return -1;
    }

    int res = -1;
    char line[256];
    while(res == -1 && (fgets(line, sizeof(line), f)))
    {
        if(strstr(line, "ro.build.version.sdk=") == line)
            res = atoi(strchr(line, '=')+1);
    }
    fclose(f);

    if(res == 0)
        ERROR("Invalid ro.build.version.sdk line in build.prop\n");

    return res;
}

int multirom_get_trampoline_ver(void)
{
    static int ver = -2;
    if(ver == -2)
    {
        ver = -1;

        char buff[128];
        char *cmd[] = { buff, "-v", NULL };

        // If we are booting into another ROM from already running system,
        // /main_init was moved to /init and we have to use trampoline from
        // /data/media
        if(access("/main_init", F_OK) >= 0)
            snprintf(buff, sizeof(buff), "/init");
        else
            snprintf(buff, sizeof(buff), "%s/trampoline", mrom_dir());

        char *res = run_get_stdout(cmd);
        if(res)
        {
            ver = atoi(res);
            free(res);
        }
        else
        {
            ERROR("Failed to get trampoline version, run_get_stdout returned NULL!\n");
        }
    }
    return ver;
}

int multirom_has_kexec(void)
{
    static int has_kexec = -1;
    if(has_kexec != -1)
        return has_kexec;

#if MR_DEVICE_HOOKS >= 5
    has_kexec = mrom_hook_has_kexec();
#endif

    if(has_kexec == -1)
    {
        if(access("/proc/config.gz", F_OK) >= 0)
        {
            char *cmd_cp[] = { busybox_path, "cp", "/proc/config.gz", "/ikconfig.gz", NULL };
            run_cmd(cmd_cp);

            char *cmd_gzip[] = { busybox_path, "gzip", "-d", "/ikconfig.gz", NULL };
            run_cmd(cmd_gzip);

            has_kexec = 1;

            uint32_t i;
            static const char *checks[] = {
                "CONFIG_KEXEC_HARDBOOT=y",
#ifndef MR_KEXEC_DTB
                "CONFIG_ATAGS_PROC=y",
#else
                "CONFIG_PROC_DEVICETREE=y",
#endif
            };
            //                   0             1       2     3
            char *cmd_grep[] = { busybox_path, "grep", NULL, "/ikconfig", NULL };
            for(i = 0; i < ARRAY_SIZE(checks); ++i)
            {
                cmd_grep[2] = (char*)checks[i];
                if(run_cmd(cmd_grep) != 0)
                {
                    has_kexec = 0;
                    ERROR("%s not found in /proc/config.gz!\n", checks[i]);
                }
            }

            remove("/ikconfig");
        }
        else
        {
            // Kernel without /proc/config.gz enabled - check for /proc/atags file,
            // if it is present, there is good change kexec-hardboot is enabled too.
            ERROR("/proc/config.gz is not available!\n");
#ifndef MR_KEXEC_DTB
            const char *checkfile = "/proc/atags";
#else
            const char *checkfile = "/proc/device-tree";
#endif
            if(access(checkfile, R_OK) < 0)
            {
                ERROR("%s was not found!\n", checkfile);
                has_kexec = 0;
            }
            else
                has_kexec = 1;
        }
    }

    if(has_kexec && mr_system("%s -u", kexec_path) != 0)
    {
        ERROR("kexec -u test has failed, kernel doesn't have kexec-hardboot patch enabled in config!\n");
        has_kexec = 0;
    }

    return has_kexec;
}

int multirom_get_bootloader_cmdline(struct multirom_status *s, char *str, size_t size)
{
    FILE *f;
    char *c, *e, *l;
    int res = -1;
    int bootimg_loaded = 0;
    struct boot_img_hdr hdr;
    struct fstab_part *boot;

    f = fopen("/proc/cmdline", "re");
    if(!f)
        return -1;

    str[0] = 0;

    if(fgets(str, size, f) == NULL)
        goto exit;

    for(c = str; *c; ++c)
        if(*c == '\n')
            *c = ' ';

    // Remove the part from boot.img
//#ifndef MR_NO_KEXEC
    if(s->is_running_in_primary_rom || !s->current_rom || !s->current_rom->has_bootimg)
//#else
//    // probably not needed, but for consistency
//    if(s->is_running_in_primary_rom || !s->current_rom || !(s->current_rom->has_bootimg && (nokexec()->selected_method == NO_KEXEC_BOOT_KEXEC)))
//#endif
    {
        boot = fstab_find_first_by_path(s->fstab, "/boot");
        if(boot && libbootimg_load_header(&hdr, boot->device) >= 0)
            bootimg_loaded = 1;
    }
    else
    {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s/boot.img", s->current_rom->base_path);
        if(libbootimg_load_header(&hdr, buf) >= 0)
            bootimg_loaded = 1;
    }

    if(bootimg_loaded)
    {
        l = (char*)hdr.cmdline;
        hdr.cmdline[BOOT_ARGS_SIZE-1] = 0;

#if MR_DEVICE_HOOKS >= 5
        mrom_hook_fixup_bootimg_cmdline(l, BOOT_ARGS_SIZE);
#endif

        if(*l != 0 && (c = strstr(str, l)))
        {
            e = c + strlen(l);
            if(*e == ' ')
                ++e;
            memmove(c, e, strlen(e)+1); // plus NULL
        }
    }

    res = 0;
exit:
    fclose(f);
    return res;
}

int multirom_find_file(char *res, const char *name_part, const char *path)
{
    DIR *d = opendir(path);
    if(!d)
        return -1;

    int wild = 0;
    int len = strlen(name_part);
    char *name = (char*)name_part;
    char *i;
    if((i = strchr(name_part, '*')))
    {
        wild = 1;
        name = strndup(name_part, i-name);
    }

    int ret= -1;
    struct dirent *dr;
    while(ret == -1 && (dr = readdir(d)))
    {
        if(dr->d_name[0] == '.')
            continue;

        if ((!wild && strcmp(dr->d_name, name)) ||
           ((wild && !strstr(dr->d_name, name))))
            continue;

        sprintf(res, "%s/%s", path, dr->d_name);
        ret = 0;
    }
    closedir(d);
    if(wild)
        free(name);
    return ret;
}

int multirom_load_kexec(struct multirom_status *s, struct multirom_rom *rom)
{
    int res = -1;
    struct kexec kexec;
    int loop_mounted = 0;

    // to find /data partition
    if(!rom->partition && multirom_update_partitions(s) < 0)
    {
        ERROR("Failed to update partitions\n");
        return -1;
    }

    kexec_init(&kexec, kexec_path);
    kexec_add_arg(&kexec, "--mem-min="MR_KEXEC_MEM_MIN);
#ifdef MR_KEXEC_DTB
    kexec_add_arg_prefix(&kexec, "--boardname=", TARGET_DEVICE);
#endif

    switch(rom->type)
    {
        case ROM_ANDROID_INTERNAL:
        case ROM_ANDROID_USB_DIR:
        case ROM_ANDROID_USB_IMG:
        case ROM_ANDROID_INTERNAL_HYBRID:
        case ROM_ANDROID_USB_HYBRID:
            if(multirom_fill_kexec_android(s, rom, &kexec) != 0)
                goto exit;
            break;
        case ROM_LINUX_INTERNAL:
        case ROM_LINUX_USB:
            loop_mounted = multirom_fill_kexec_linux(s, rom, &kexec);
            if(loop_mounted < 0)
                goto exit;
            break;
        default:
            ERROR("Unsupported rom type to kexec (%d)!\n", rom->type);
            goto exit;
    }

    res = kexec_load_exec(&kexec);

    char *cmd_cp[] = { busybox_path, "cp", kexec_path, "/kexec", NULL };
    run_cmd(cmd_cp);
    chmod("/kexec", 0755);

    if(loop_mounted)
        umount("/mnt/image");

    multirom_copy_log(NULL, "last_kexec_log.txt");
    if (s->enable_kmsg_logging != 0)
        multirom_kmsg_logging(BACKUP_LAST_KEXEC);

exit:
    kexec_destroy(&kexec);
    return res;
}

int multirom_fill_kexec_android(struct multirom_status *s, struct multirom_rom *rom, struct kexec *kexec)
{
    int res = -1;
    char img_path[256];
    snprintf(img_path, sizeof(img_path), "%s/boot.img", rom->base_path);

    // Trampolines in ROM boot images may get out of sync, so we need to check it and
    // update if needed. I can't do that during ZIP installation because of USB drives.
    if(inject_bootimg(img_path, 0) < 0)
    {
        ERROR("Failed to inject bootimg!\n");
        return -1;
    }

    struct bootimg img;
    if(libbootimg_init_load(&img, img_path, LIBBOOTIMG_LOAD_ALL) < 0)
    {
        ERROR("fill_kexec could not open boot image (%s)!\n", img_path);
        return -1;
    }

    if(libbootimg_dump_kernel(&img, "/zImage") < 0)
        goto exit;

    if(libbootimg_dump_ramdisk(&img, "/initrd.img") < 0)
        goto exit;

    kexec_add_kernel(kexec, "/zImage", 1);
    kexec_add_arg(kexec, "--initrd=/initrd.img");

#ifdef MR_KEXEC_DTB
    if(libbootimg_dump_dtb(&img, "/dtb.img") >= 0)
        kexec_add_arg(kexec, "--dtb=/dtb.img");
    else
        kexec_add_arg(kexec, "--dtb");
#endif

    char cmdline[1536];
    strcpy(cmdline, "--command-line=");

    if(img.hdr.cmdline[0] != 0)
    {
        img.hdr.cmdline[BOOT_ARGS_SIZE-1] = 0;

        // see multirom_get_bootloader_cmdline
#if MR_DEVICE_HOOKS >= 5
        mrom_hook_fixup_bootimg_cmdline((char*)img.hdr.cmdline, BOOT_ARGS_SIZE);
#endif

        strcat(cmdline, (char*)img.hdr.cmdline);
        strcat(cmdline, " ");
    }

    if(multirom_get_bootloader_cmdline(s, cmdline+strlen(cmdline), sizeof(cmdline)-strlen(cmdline)-1) == -1)
    {
        ERROR("Failed to get cmdline\n");
        goto exit;
    }

    if(!strstr(cmdline, " mrom_kexecd=1") && sizeof(cmdline)-strlen(cmdline)-1 >= sizeof("mrom_kexecd=1"))
        strcat(cmdline, "mrom_kexecd=1");

#if MR_DEVICE_HOOKS >= 6
    mrom_hook_fixup_full_cmdline(cmdline, sizeof(cmdline));
#endif

    kexec_add_arg(kexec, cmdline);

    res = 0;
exit:
    libbootimg_destroy(&img);
    return res;
}

static char *find_boot_file(char *path, char *root_path, char *base_path)
{
    if(!path)
        return NULL;

    struct stat info;
    char cmd[256];
    char *root = strstr(path, "%r");
    if(root)
        snprintf(cmd, sizeof(cmd), "%s/%s", root_path, root+2);
    else
        snprintf(cmd, sizeof(cmd), "%s/%s", base_path, path);

    char *last = strrchr(cmd, '/');
    if(!last)
    {
        ERROR("Failed to find boot file: %s\n", cmd);
        return NULL;
    }

    *last = 0;

    char *name = strdup(last+1);
    char res[256];
    if(multirom_find_file(res, name, cmd) < 0)
    {
        ERROR("Failed to find boot file: %s\n", cmd);
        free(name);
        return NULL;
    }
    return strdup(res);
}

int multirom_fill_kexec_linux(struct multirom_status *s, struct multirom_rom *rom, struct kexec *kexec)
{
    struct rom_info *info = multirom_parse_rom_info(s, rom);
    if(!info)
        return -1;

    int res = -1;
    int root_type = -1; // 0 = dir, 1 = img
    int loop_mounted = 0;
    char root_path[256];
    const char *base_path;

    if(!rom->partition)
        base_path = partition_dir;
    else
        base_path = rom->partition->mount_path;

    struct stat st;
    char path[256];
    char *tmp;

    if((tmp = map_get_val(info->str_vals, "root_img")))
    {
        sprintf(path, "%s/%s", base_path, tmp);
        if(stat(path, &st) >= 0)
        {
            root_type = 1;

            char *img_fs = map_get_val(info->str_vals, "root_img_fs");

            // mount the image file
            mkdir("/mnt", 0777);
            mkdir("/mnt/image", 0777);
            if(mount_image(path, "/mnt/image", img_fs ? img_fs : "ext4", MS_NOATIME, NULL) < 0)
                goto exit;

            loop_mounted = 1;
            strcpy(root_path, "/mnt/image");
        }
        else
            ERROR("Path %s not found!\n", path);
    }

    if(root_type == -1 && (tmp = map_get_val(info->str_vals, "root_dir")))
    {
        sprintf(path, "%s/%s", base_path, tmp);
        if(stat(path, &st) >= 0)
        {
            root_type = 0;
            strcpy(root_path, path);
        }
        else
            ERROR("Path %s not found!\n", path);
    }

    if(root_type == -1)
    {
        ERROR("Failed to find root of the ROM!\n");
        goto exit;
    }

    char *str = find_boot_file(map_get_val(info->str_vals, "kernel_path"), root_path, rom->base_path);
    if(str)
    {
        kexec_add_kernel(kexec, str, 1);
        free(str);
    }
    else
    {
        // kernel is required
        goto exit;
    }

#ifdef MR_KEXEC_DTB
    str = NULL;

    if (map_find(info->str_vals, "dtb_path") != -1)
    {
        str = find_boot_file(map_get_val(info->str_vals, "dtb_path"), root_path, rom->base_path);
        if(!str)
        {
            ERROR("failed to find dtb_path!\n");
            goto exit;
        }
    }
    else
    {
        str = find_boot_file("%r/dtb.img", root_path, rom->base_path);
    }

    if(!str)
        kexec_add_arg(kexec, "--dtb");
    else
    {
        kexec_add_arg_prefix(kexec, "--dtb=", str);
        free(str);
    }
#endif

    str = find_boot_file(map_get_val(info->str_vals, "initrd_path"), root_path, rom->base_path);
    if(str)
    {
        kexec_add_arg_prefix(kexec, "--initrd=", str);
        free(str);
    }

    char cmdline[1536];
    snprintf(cmdline, sizeof(cmdline), "--command-line=%s ", (char*)map_get_val(info->str_vals, "base_cmdline"));

    str = NULL;
    if(root_type == 0)
        str = map_get_val(info->str_vals, "dir_cmdline");
    else if(root_type == 1)
        str = map_get_val(info->str_vals, "img_cmdline");

    if(str)
    {
        if(strlen(str)+strlen(cmdline)+1 <= sizeof(cmdline))
            strcat(cmdline, str);
        else
        {
            ERROR("failed to fill kexec info, cmdline is too long!\n");
            goto exit;
        }
    }

    kexec_add_arg(kexec, cmdline);

    res = loop_mounted;
exit:
    multirom_destroy_rom_info(info);
    return res;
}

#define INFO_LINE_BUFF 4096
struct rom_info *multirom_parse_rom_info(struct multirom_status *s, struct multirom_rom *rom)
{
    char path[256];

    snprintf(path, sizeof(path), "%s/rom_info.txt", rom->base_path);
    ERROR("Parsing %s...\n", path);

    FILE *f = fopen(path, "re");
    if(!f)
    {
        ERROR("Failed to open %s!\n", path);
        return NULL;
    }

    struct rom_info *i = malloc(sizeof(struct rom_info));
    memset(i, 0, sizeof(struct rom_info));
    i->str_vals = map_create();

    char *line = malloc(INFO_LINE_BUFF);
    char key[32];
    int line_cnt = 1;
    for(; fgets(line, INFO_LINE_BUFF, f); ++line_cnt)
    {
        if(line[0] == '#')
            continue;

        char *val = strchr(line, '=');
        if(!val || val-line >= (int)(sizeof(key)-1))
            continue;

        strncpy(key, line, val-line);
        key[val-line] = 0;
        ++val; // skip '=' char

        // if string value
        {
            char *str = parse_string(val);
            if(str)
                map_add(i->str_vals, key, str, &free);
            else
                ERROR("Line %d: failed to parse string\n", line_cnt);
        }
    }
    free(line);
    fclose(f);

    static const char *roots[] = { "root_dir", "root_img", NULL };
    int found_root = 0;
    int y;
    for(y = 0; roots[y] && !found_root; ++y)
    {
        if(map_find(i->str_vals, (char*)roots[y]) >= 0)
            found_root = 1;
    }

    if(!found_root)
        ERROR("Failed to find any root key in %s\n", path);

    static const char *req_keys[] = { "type", "kernel_path", "base_cmdline", NULL};
    int failed = !found_root;
    for(y = 0; req_keys[y]; ++y)
    {
        if(map_find(i->str_vals, (char*)req_keys[y]) < 0)
        {
            ERROR("Key \"%s\" key not found in %s\n", req_keys[y], path);
            failed = 1;
        }
    }

    // Only supported type is kexec, check just to make sure older releases
    // can't try to run newer ROMs
    if(failed == 0)
    {
        char *val = map_get_val(i->str_vals, "type");
        if(strcmp(val, "kexec") != 0)
        {
            ERROR("Only supported rom_info type is \"kexec\", this rom_info has type \"%s\"!\n", val);
            failed = 1;
        }
    }

    if(failed == 1)
    {
        multirom_destroy_rom_info(i);
        return NULL;
    }

    char **ref;
    ERROR("Replacing aliases in root paths...\n");
    for(y = 0; roots[y]; ++y)
        if((ref = map_get_ref(i->str_vals, (char*)roots[y])))
            multirom_replace_aliases_root_path(ref, rom);

    ERROR("Replacing aliases in the cmdline...\n");
    static const char *cmdlines[] = { "base_cmdline", "img_cmdline", "dir_cmdline", NULL };
    for(y = 0; cmdlines[y]; ++y)
        if((ref = map_get_ref(i->str_vals, (char*)cmdlines[y])))
            multirom_replace_aliases_cmdline(ref, i, s, rom);

    return i;
}

void multirom_destroy_rom_info(struct rom_info *info)
{
    if(!info)
        return;

    map_destroy(info->str_vals, &free);
    free(info);
}

/*
# Set up the cmdline
# img_cmdline and dir_cmdline are appended to base_cmdline.
# Several aliases are used:
#  - %b - base command line from bootloader. You want this as first thing in cmdline.
#  - %d - root device. is either "UUID=..." (USB drive) or "/dev/mmcblk0p9" or "/dev/mmcblk0p10"
#  - %r - root fs type
#  - %s - root directory, from root of the root device
#  - %i - root image, from root of the root device
#  - %f - fs of the root image
*/
int multirom_replace_aliases_cmdline(char **s, struct rom_info *i, struct multirom_status *status, struct multirom_rom *rom)
{
    size_t c = strcspn (*s, "%");

    if(strlen(*s) == c)
        return 0;

    struct fstab_part *data_part = NULL;
    if(!rom->partition)
    {
        // FIXME: might have wrong fs type, because of those "multi-fs" bullshit fstabs
        // with multiple entries for /data
        data_part = fstab_find_first_by_path(status->fstab, "/data");
    }

    char *buff = mzalloc(4096);

    char *itr_o = buff;
    char *itr_i = *s;
    int res = -1;

    while(1)
    {
        memcpy(itr_o, itr_i, c);
        itr_o += c;
        itr_i += c;

        *itr_o = 0;

        if(*itr_i != '%')
            break;

        ++itr_i;
        switch(*itr_i)
        {
            // base command line from bootloader. You want this as first thing in cmdline.
            case 'b':
            {
                if(multirom_get_bootloader_cmdline(status, itr_o, 1024) == -1)
                {
                    ERROR("Failed to get cmdline\n");
                    goto fail;
                }
                break;
            }
            // root device. is either "UUID=..." (USB drive) or "/dev/mmcblk0p9" or "/dev/mmcblk0p10"
            case 'd':
            {
                if(data_part)
                {
                    // Only android's ueventd creates /dev/block, so try to remove it
                    // for _real_ linux OS. We can't use UUID, because it's the same for
                    // /system, /data and /cache partitions
                    char *blk = strstr(data_part->device, "/dev/block/");
                    if(blk)
                    {
                        strcpy(itr_o, "/dev/");
                        strcat(itr_o, blk+sizeof("/dev/block/")-1);
                    }
                    else
                        strcpy(itr_o, data_part->device);
                }
                else if(rom->partition)
                    sprintf(itr_o, "UUID=%s", rom->partition->uuid);
                else
                    ERROR("Failed to set root device\n");
                break;
            }
            // root fs type
            case 'r':
            {
                if(data_part)
                    strcpy(itr_o, data_part->type);
                else if(rom->partition)
                {
                    if(!strcmp(rom->partition->fs, "ntfs"))
                        strcpy(itr_o, "ntfs-3g");
                    else
                        strcpy(itr_o, rom->partition->fs);
                }
                else
                    ERROR("Failed to set root fs type\n");
                break;
            }
            // root directory, from root of the root device
            case 's':
            {
                char *d = map_get_val(i->str_vals, "root_dir");
                if(!d)
                {
                    ERROR("%%s alias found in cmdline, but root_dir key was not found!\n");
                    break;
                }
                sprintf(itr_o, "%s", d);
                break;
            }
            // root image, from root of the root device
            case 'i':
            {
                char *d = map_get_val(i->str_vals, "root_img");
                if(!d)
                {
                    ERROR("%%s alias found in cmdline, but root_img key was not found!\n");
                    break;
                }
                sprintf(itr_o, "%s", d);
                break;
            }
            // fs of the root image
            case 'f':
            {
                char *d = map_get_val(i->str_vals, "root_img_fs");
                if(!d)
                {
                    ERROR("%%s alias found in cmdline, but root_img_fs key was not found!\n");
                    break;
                }
                strcpy(itr_o, d);
                break;
            }
        }
        itr_o += strlen(itr_o);
        c = strcspn (++itr_i, "%");
    }

    free(*s);
    *s = realloc(buff, strlen(buff)+1);

    ERROR("Alias-replaced cmdline: %s\n", *s);
    return 0;

fail:
    free(buff);
    return -1;
}

// - %m - ROMs folder (eg. /sdcard/multirom/roms/*rom_name*)
int multirom_replace_aliases_root_path(char **s, struct multirom_rom *rom)
{
    char *alias = strstr(*s, "%m");
    if(!alias)
        return 0;

    char buff[256] = { 0 };
    memcpy(buff, *s, alias-*s);

    if(rom->partition)
        strcat(buff, rom->base_path + strlen(rom->partition->mount_path));
    else
        strcat(buff, rom->base_path + strlen(partition_dir));

    strcat(buff, alias+2);

    ERROR("Alias-replaced path: %s\n", buff);

    free(*s);
    *s = strdup(buff);
    return 0;
}

int multirom_extract_bytes(const char *dst, FILE *src, size_t size)
{
    FILE *f = fopen(dst, "we");
    if(!f)
    {
        ERROR("Failed to open dest file %s\n", dst);
        return -1;
    }

    char *buff = malloc(size);

    fread(buff, 1, size, src);
    fwrite(buff, 1, size, f);

    fclose(f);
    free(buff);
    return 0;
}

void multirom_destroy_partition(void *part)
{
    struct usb_partition *p = (struct usb_partition *)part;
    if(p->mount_path && p->keep_mounted == 0)
        umount(p->mount_path);

    free(p->name);
    free(p->uuid);
    free(p->mount_path);
    free(p->fs);
    free(p);
}

int multirom_update_partitions(struct multirom_status *s)
{
    pthread_mutex_lock(&parts_mutex);

    list_clear(&s->partitions, &multirom_destroy_partition);

    int exit_code = 0;
    char *cmd[] = { busybox_path, "blkid", NULL };
    char *res = run_get_stdout_with_exit(cmd, &exit_code);
    if(exit_code != 0 || res == NULL)
    {
        free(res);
        pthread_mutex_unlock(&parts_mutex);
        return exit_code == 0 ? 0 : -1;
    }

    char *tok;
    char *name;
    struct usb_partition *part;

    char *line = strtok(res, "\n");
    while(line != NULL)
    {
        if(strstr(line, "/dev/") != line)
        {
            ERROR("blkid line does not start with /dev/!\n");
            break;
        }

        tok = strrchr(line, '/')+1;
        name = strndup(tok, strchr(tok, ':') - tok);
        if(strncmp(name, "mmcblk0", 7) == 0 || strncmp(name, "dm-", 3) == 0 || strncmp(name, "sd", 2) == 0) // ignore internal nand
        {
            free(name);
            goto next_itr;
        }

        if(strncmp(name, "loop", 4) == 0) // ignore loop devices
        {
            free(name);
            goto next_itr;
        }

        part = mzalloc(sizeof(struct usb_partition));
        part->name = name;

        tok = strstr(line, "UUID=\"");
        if(tok)
        {
            tok += sizeof("UUID=\"")-1;
            part->uuid = strndup(tok, strchr(tok, '"') - tok);
        }
        else
        {
            ERROR("Part %s does not have UUID, line: \"%s\"\n", part->name, line);
            multirom_destroy_partition(part);
            goto next_itr;
        }

        tok = strstr(line, "TYPE=\"");
        if(tok)
        {
            tok += sizeof("TYPE=\"")-1;
            part->fs = strndup(tok, strchr(tok, '"') - tok);
        }

        if(part->fs && multirom_mount_usb(part) == 0)
        {
            list_add(&s->partitions, part);
            ERROR("Found part %s: %s, %s\n", part->name, part->uuid, part->fs);
        }
        else
        {
            ERROR("Failed to mount part %s %s, %s\n", part->name, part->uuid, part->fs);
            multirom_destroy_partition(part);
        }

next_itr:
        line = strtok(NULL, "\n");
    }
    pthread_mutex_unlock(&parts_mutex);
    free(res);

    return 0;
}

int is_mounted_properly(const char *src, const char *mnt_path)
{
    int res = 0;

    FILE *f;

    f = fopen("/proc/mounts", "re");
    if (f)
    {
        char mount_dev[256];
        char mount_dir[256];
        char mount_type[256];
        char mount_opts[256];
        int mount_freq;
        int mount_passno;
        int match;

        do {
            match = fscanf(f, "%255s %255s %255s %255s %d %d\n",
                           mount_dev, mount_dir, mount_type,
                           mount_opts, &mount_freq, &mount_passno);
            mount_dev[255] = 0;
            mount_dir[255] = 0;
            mount_type[255] = 0;
            mount_opts[255] = 0;
            if ((match == 6) && (strcmp(src, mount_dev) == 0) && (strcmp(mnt_path, mount_dir) == 0)) {
                // check if the dir is empty, if so even though the mount entry exists, the actual path isn't mounted
                int n = 0;
                DIR *dir = opendir(mnt_path);
                if (dir) {
                    while (readdir(dir)) {
                        if (++n > 2) {
                            res = 1;
                            break;
                        }
                    }
                    closedir(dir);
                }
                break;
            }
        } while (match != EOF);

        fclose(f);
    }

    return res;
}

int multirom_mount_usb(struct usb_partition *part)
{
    int res = 0;
    part->mount_path = NULL;

    mkdir("/mnt", 0777);
    mkdir("/mnt/mrom", 0777);

    char path[256];
    snprintf(path, sizeof(path), "/mnt/mrom/%s", part->name);
    if(mkdir(path, 0777) != 0 && errno != EEXIST)
    {
        ERROR("Failed to create dir for mount %s\n", path);
        return -1;
    }

    char src[256];
    snprintf(src, sizeof(src), "/dev/block/%s", part->name);

    if(is_mounted_properly(src, path))
    {
        INFO("Partition %s already mounted on %s\n", src, path);
        res = 0;
    }
    else if(strncmp(part->fs, "ntfs", 4) == 0)
    {
        char *cmd[] = { ntfs_path, src, path, NULL };
        if(run_cmd(cmd) != 0)
        {
            ERROR("Failed to mount %s with ntfs-3g\n", src);
            res = -1;
        }
    }
    else if(strcmp(part->fs, "exfat") == 0)
    {
        char *cmd[] = { exfat_path, "-o", "big_writes,max_read=131072,max_write=131072,nonempty", src, path, NULL };
        if(run_cmd(cmd) != 0)
        {
            ERROR("Failed to mount %s with exfat\n", src);
            res = -1;
        }
    }
    else if(mount(src, path, part->fs, MS_NOATIME, "") < 0)
    {
        ERROR("Failed to mount %s (%d: %s)\n", src, errno, strerror(errno));
        res = -1;
    }

    part->mount_path = strdup(path);
    return res;
}

void multirom_update_and_scan_for_external_roms(struct multirom_status *s, char *part_uuid)
{
    if (!s->partitions)
    {
        // no external partitions exist, so either there are none, or this is first run
        // so we can proceed in the normal fashion
        struct usb_partition *p = NULL;
        int tries = 0;
        while(!p && tries < 10) // is 10seconds enough for USB-OTG
        {
            multirom_update_partitions(s);

            p = multirom_get_partition(s, part_uuid);

            if(p)
            {
                multirom_scan_partition_for_roms(s, p);
                break;
            }

            ++tries;
            ERROR("part %s not found, waiting 1s (%d)\n", part_uuid, tries);
            sleep(1);
        }
    }
    else
    {
        // external partition exist, multirom_update_partitions will destroy all partitions information, so
        // so we'll just use multirom_find_usb_roms(s) to repopulate everything, the alternative is just too
        // much unnecessary code
        multirom_find_usb_roms(s);
    }
}

void *multirom_usb_refresh_thread_work(void *status)
{
    uint32_t timer = 0;
    struct stat info;

    // stat.st_ctime is defined as unsigned long instead
    // of time_t in android
    unsigned long last_ctime = 0;
    unsigned long last_ctime_nsec = 0;

    while(run_usb_refresh)
    {
        if(timer <= 50)
        {
            if (stat("/dev/block", &info) >= 0 &&
                (info.st_ctime != last_ctime || info.st_ctimensec != last_ctime_nsec))
            {
                multirom_update_partitions((struct multirom_status*)status);

                if(usb_refresh_handler)
                    (*usb_refresh_handler)();

                last_ctime = info.st_ctime;
                last_ctime_nsec = info.st_ctimensec;
            }
            timer = 500;
        }
        else
            timer -= 50;
        usleep(50000);
    }
    return NULL;
}

void multirom_set_usb_refresh_thread(struct multirom_status *s, int run)
{
    if(run_usb_refresh == run)
        return;

    run_usb_refresh = run;
    if(run)
        pthread_create(&usb_refresh_thread, NULL, multirom_usb_refresh_thread_work, s);
    else
        pthread_join(usb_refresh_thread, NULL);
}

void multirom_set_usb_refresh_handler(void (*handler)(void))
{
    usb_refresh_handler = handler;
}

char *multirom_get_klog(void)
{
    int len = klogctl(10, NULL, 0);
    if      (len < 16*1024)      len = 16*1024;
    else if (len > 16*1024*1024) len = 16*1024*1024;

    char *buff = malloc(len + 1);
    len = klogctl(3, buff, len);
    if(len <= 0)
    {
        ERROR("Could not get klog!\n");
        free(buff);
        return NULL;
    }
    buff[len] = 0;
    return buff;
}

int multirom_copy_log(char *klog, const char *dest_path_relative)
{
    int res = 0;
    int freeLog = (klog == NULL);

    if(!klog)
        klog = multirom_get_klog();

    if(klog)
    {
        char path[256];
        snprintf(path, sizeof(path), "%s/%s", mrom_dir(), dest_path_relative);
        FILE *f = fopen(path, "we");

        if(f)
        {
            fwrite(klog, 1, strlen(klog), f);
            fclose(f);
            chmod(path, 0777);
        }
        else
        {
            ERROR("Failed to open %s!\n", path);
            res = -1;
        }
    }
    else
    {
        ERROR("Could not get klog!\n");
        res = -1;
    }

    if(freeLog)
        free(klog);
    return res;
}

struct usb_partition *multirom_get_partition(struct multirom_status *s, char *uuid)
{
    // FIXME: This may need to be reconsidered, it is possible for 2 different partitions
    //        to have the same uuid which will lead to a false positive.
    //        Probably a good idea, to add mount point and uuid
    int i;
    for(i = 0; s->partitions && s->partitions[i]; ++i)
        if(strcmp(s->partitions[i]->uuid, uuid) == 0)
            return s->partitions[i];
    return NULL;
}

int multirom_get_battery(void)
{
    char buff[4];

    FILE *f = fopen(BATTERY_CAP, "re");
    if(!f)
        return -1;

    fgets(buff, sizeof(buff), f);
    fclose(f);

    return atoi(buff);
}

int multirom_run_scripts(const char *type, struct multirom_rom *rom)
{
    char buff[512];
    snprintf(buff, sizeof(buff), "%s/%s", rom->base_path, type);
    if(access(buff, (R_OK | X_OK)) < 0)
    {
        ERROR("No %s scripts for ROM %s\n", type, rom->name);
        return 0;
    }

    ERROR("Running %s scripts for ROM %s...\n", type, rom->name);

    int res = mr_system("B=\"%s\"; P=\"%s\"; for x in $(\"$B\" ls \"$P/%s/\"*.sh); do echo Running script $x; \"$B\" sh $x \"$B\" \"$P\" || exit 1; done", busybox_path, rom->base_path, type);
    if(res != 0)
    {
        ERROR("Error running scripts (%d)!\n", res);
        return res;
    }
    return 0;
}

#define IC_TYPE_PREDEF 0
#define IC_TYPE_USER   1
#define USER_IC_PATH "Android/data/com.tassadar.multirommgr/files"
#define DEFAULT_ICON "icons/romic_default.png"
#define DEFAULT_ANDROID_ICON "icons/romic_android_default.png"

void multirom_find_rom_icon(struct multirom_rom *rom)
{
    FILE *f;
    int type = 0, len;
    char buff[256];

    snprintf(buff, sizeof(buff), "%s/.icon_data", rom->base_path);

    f = fopen(buff, "re");
    if(!f)
        goto fail;

    if(!fgets(buff, sizeof(buff), f))
        goto fail;

    if(strcmp(buff, "predef_set\n") == 0)
        type = IC_TYPE_PREDEF;
    else if(strcmp(buff, "user_defined\n") == 0)
        type = IC_TYPE_USER;
    else
        goto fail;

    if(!fgets(buff, sizeof(buff), f))
        goto fail;
    fclose(f);
    f = NULL;

    len = strlen(buff);
    if(len < 2)
        goto fail;

    buff[--len] = 0; // remove \n

    switch(type)
    {
        case IC_TYPE_PREDEF:
        {
            char *ic_name = strrchr(buff, '/');
            if(!ic_name)
                goto fail;

            len = strlen(mrom_dir()) + 6 + strlen(ic_name) + 4 + 1; // + /icons + .png + \0
            rom->icon_path = malloc(len);
            snprintf(rom->icon_path, len, "%s/icons%s.png", mrom_dir(), ic_name);
            break;
        }
        case IC_TYPE_USER:
        {
            len = strlen(datamedia_dir) + 1 + sizeof(USER_IC_PATH) + 1 + len + 4; // + / + / + .png + \0 (sizeof() includes trailing null)
            rom->icon_path = malloc(len);
            snprintf(rom->icon_path, len, "%s/%s/%s.png", datamedia_dir, USER_IC_PATH, buff);
            break;
        }
    }

    if(access(rom->icon_path, F_OK) < 0)
        goto fail;

    return;
fail:
    if(f)
        fclose(f);

    if (rom->type & MASK_ANDROID) {
        len = strlen(mrom_dir()) + 1 + sizeof(DEFAULT_ANDROID_ICON);  // sizeof() includes trailing null
        rom->icon_path = realloc(rom->icon_path, len);
        snprintf(rom->icon_path, len, "%s/%s", mrom_dir(), DEFAULT_ANDROID_ICON);
    }
    else {
        len = strlen(mrom_dir()) + 1 + sizeof(DEFAULT_ICON); // sizeof() includes trailing null
        rom->icon_path = realloc(rom->icon_path, len);
        snprintf(rom->icon_path, len, "%s/%s", mrom_dir(), DEFAULT_ICON);
    }
}
