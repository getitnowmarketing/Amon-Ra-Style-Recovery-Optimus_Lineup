/*
 * Copyright (C) 2007 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input.h>
#include <stdio.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <termios.h>
#include <sys/stat.h>

#include "bootloader.h"
#include "commands.h"
#include "common.h"
#include "cutils/properties.h"
#include "firmware.h"
#include "install.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "roots.h"

#include "extracommands.h"
#include "recovery_ui_keys.h"

static const struct option OPTIONS[] = {
  { "send_intent", required_argument, NULL, 's' },
  { "update_package", required_argument, NULL, 'u' },
  { "wipe_data", no_argument, NULL, 'w' },
  { "wipe_cache", no_argument, NULL, 'c' },
};

static const char *COMMAND_FILE = "CACHE:recovery/command";
static const char *INTENT_FILE = "CACHE:recovery/intent";
static const char *LOG_FILE = "CACHE:recovery/log";
static const char *SDCARD_PACKAGE_FILE = "SDCARD:update.zip";
static const char *SDCARD_PATH = "SDCARD:";
static const char *NANDROID_PATH = "SDCARD:/nandroid/";
#define SDCARD_PATH_LENGTH 7
#define NANDROID_PATH_LENGTH 17
static const char *TEMPORARY_LOG_FILE = "/tmp/recovery.log";
static const char *CLOCKWORK_PATH = "SDCARD:/clockworkmod/backup/";
#define CLOCKWORK_PATH_LENGTH 28
void free_string_array(char** array);
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[]);
char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles);

/*
 * The recovery tool communicates with the main system through /cache files.
 *   /cache/recovery/command - INPUT - command line for tool, one arg per line
 *   /cache/recovery/log - OUTPUT - combined log file from recovery run(s)
 *   /cache/recovery/intent - OUTPUT - intent that was passed in
 *
 * The arguments which may be supplied in the recovery.command file:
 *   --send_intent=anystring - write the text out to recovery.intent
 *   --update_package=root:path - verify install an OTA package file
 *   --wipe_data - erase user data (and cache), then reboot
 *   --wipe_cache - wipe cache (but not user data), then reboot
 *
 * After completing, we remove /cache/recovery/command and reboot.
 * Arguments may also be supplied in the bootloader control block (BCB).
 * These important scenarios must be safely restartable at any point:
 *
 * FACTORY RESET
 * 1. user selects "factory reset"
 * 2. main system writes "--wipe_data" to /cache/recovery/command
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--wipe_data"
 *    -- after this, rebooting will restart the erase --
 * 5. erase_root() reformats /data
 * 6. erase_root() reformats /cache
 * 7. finish_recovery() erases BCB
 *    -- after this, rebooting will restart the main system --
 * 8. main() calls reboot() to boot main system
 *
 * OTA INSTALL
 * 1. main system downloads OTA package to /cache/some-filename.zip
 * 2. main system writes "--update_package=CACHE:some-filename.zip"
 * 3. main system reboots into recovery
 * 4. get_args() writes BCB with "boot-recovery" and "--update_package=..."
 *    -- after this, rebooting will attempt to reinstall the update --
 * 5. install_package() attempts to install the update
 *    NOTE: the package install must itself be restartable from any point
 * 6. finish_recovery() erases BCB
 *    -- after this, rebooting will (try to) restart the main system --
 * 7. ** if install failed **
 *    7a. prompt_and_wait() shows an error icon and waits for the user
 *    7b; the user reboots (pulling the battery, etc) into the main system
 * 8. main() calls maybe_install_firmware_update()
 *    ** if the update contained radio/hboot firmware **:
 *    8a. m_i_f_u() writes BCB with "boot-recovery" and "--wipe_cache"
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8b. m_i_f_u() writes firmware image into raw cache partition
 *    8c. m_i_f_u() writes BCB with "update-radio/hboot" and "--wipe_cache"
 *        -- after this, rebooting will attempt to reinstall firmware --
 *    8d. bootloader tries to flash firmware
 *    8e. bootloader writes BCB with "boot-recovery" (keeping "--wipe_cache")
 *        -- after this, rebooting will reformat cache & restart main system --
 *    8f. erase_root() reformats /cache
 *    8g. finish_recovery() erases BCB
 *        -- after this, rebooting will (try to) restart the main system --
 * 9. main() calls reboot() to boot main system
 */

static const int MAX_ARG_LENGTH = 4096;
static const int MAX_ARGS = 100;

static int do_reboot = 1;

// open a file given in root:path format, mounting partitions as necessary
static FILE*
fopen_root_path(const char *root_path, const char *mode) {
    if (ensure_root_path_mounted(root_path) != 0) {
        LOGE("Can't mount %s\n", root_path);
        return NULL;
    }

    char path[PATH_MAX] = "";
    if (translate_root_path(root_path, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s\n", root_path);
        return NULL;
    }

    // When writing, try to create the containing directory, if necessary.
    // Use generous permissions, the system (init.rc) will reset them.
    if (strchr("wa", mode[0])) dirCreateHierarchy(path, 0777, NULL, 1);

    FILE *fp = fopen(path, mode);
    return fp;
}

// close a file, log an error if the error indicator is set
static void
check_and_fclose(FILE *fp, const char *name) {
    fflush(fp);
    if (ferror(fp)) LOGE("Error in %s\n(%s)\n", name, strerror(errno));
    fclose(fp);
}

// command line args come from, in decreasing precedence:
//   - the actual command line
//   - the bootloader control block (one per line, after "recovery")
//   - the contents of COMMAND_FILE (one per line)
static void
get_args(int *argc, char ***argv) {
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    get_bootloader_message(&boot);  // this may fail, leaving a zeroed structure

    if (boot.command[0] != 0 && boot.command[0] != 255) {
        LOGI("Boot command: %.*s\n", sizeof(boot.command), boot.command);
    }

    if (boot.status[0] != 0 && boot.status[0] != 255) {
        LOGI("Boot status: %.*s\n", sizeof(boot.status), boot.status);
    }

    // --- if arguments weren't supplied, look in the bootloader control block
    if (*argc <= 1) {
        boot.recovery[sizeof(boot.recovery) - 1] = '\0';  // Ensure termination
        const char *arg = strtok(boot.recovery, "\n");
        if (arg != NULL && !strcmp(arg, "recovery")) {
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = strdup(arg);
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if ((arg = strtok(NULL, "\n")) == NULL) break;
                (*argv)[*argc] = strdup(arg);
            }
            LOGI("Got arguments from boot message\n");
        } else if (boot.recovery[0] != 0 && boot.recovery[0] != 255) {
            LOGW("Bad boot message\n\"%.20s\"\n", boot.recovery);
#ifdef LGE_RESET_BOOTMODE
	    lge_direct_mtd_access("6");
	    LOGW("LGE FACT_RESET_6 SET");
#endif
        }
    }

    // --- if that doesn't work, try the command file
    if (*argc <= 1) {
        FILE *fp = fopen_root_path(COMMAND_FILE, "r");
        if (fp != NULL) {
            char *argv0 = (*argv)[0];
            *argv = (char **) malloc(sizeof(char *) * MAX_ARGS);
            (*argv)[0] = argv0;  // use the same program name

            char buf[MAX_ARG_LENGTH];
            for (*argc = 1; *argc < MAX_ARGS; ++*argc) {
                if (!fgets(buf, sizeof(buf), fp)) break;
                (*argv)[*argc] = strdup(strtok(buf, "\r\n"));  // Strip newline.
            }

            check_and_fclose(fp, COMMAND_FILE);
            LOGI("Got arguments from %s\n", COMMAND_FILE);
        }
    }

    // --> write the arguments we have back into the bootloader control block
    // always boot into recovery after this (until finish_recovery() is called)
    strlcpy(boot.command, "boot-recovery", sizeof(boot.command));
    strlcpy(boot.recovery, "recovery\n", sizeof(boot.recovery));
    int i;
    for (i = 1; i < *argc; ++i) {
        strlcat(boot.recovery, (*argv)[i], sizeof(boot.recovery));
        strlcat(boot.recovery, "\n", sizeof(boot.recovery));
    }
    set_bootloader_message(&boot);
}


// clear the recovery command and prepare to boot a (hopefully working) system,
// copy our log file to cache as well (for the system to read), and
// record any intent we were asked to communicate back to the system.
// this function is idempotent: call it as many times as you like.
static void
finish_recovery(const char *send_intent)
{
    // By this point, we're ready to return to the main system...
    if (send_intent != NULL) {
        FILE *fp = fopen_root_path(INTENT_FILE, "w");
        if (fp == NULL) {
            LOGE("Can't open %s\n", INTENT_FILE);
        } else {
            fputs(send_intent, fp);
            check_and_fclose(fp, INTENT_FILE);
        }
    }

    // Copy logs to cache so the system can find out what happened.
    FILE *log = fopen_root_path(LOG_FILE, "a");
    if (log == NULL) {
        LOGE("Can't open %s\n", LOG_FILE);
    } else {
        FILE *tmplog = fopen(TEMPORARY_LOG_FILE, "r");
        if (tmplog == NULL) {
            LOGE("Can't open %s\n", TEMPORARY_LOG_FILE);
        } else {
            static long tmplog_offset = 0;
            fseek(tmplog, tmplog_offset, SEEK_SET);  // Since last write
            char buf[4096];
            while (fgets(buf, sizeof(buf), tmplog)) fputs(buf, log);
            tmplog_offset = ftell(tmplog);
            check_and_fclose(tmplog, TEMPORARY_LOG_FILE);
        }
        check_and_fclose(log, LOG_FILE);
    }

    // Reset the bootloader message to revert to a normal main system boot.
    struct bootloader_message boot;
    memset(&boot, 0, sizeof(boot));
    set_bootloader_message(&boot);

    // Remove the command file, so recovery won't repeat indefinitely.
    char path[PATH_MAX] = "";
    if (ensure_root_path_mounted(COMMAND_FILE) != 0 ||
        translate_root_path(COMMAND_FILE, path, sizeof(path)) == NULL ||
        (unlink(path) && errno != ENOENT)) {
        LOGW("Can't unlink %s\n", COMMAND_FILE);
    }

    sync();  // For good measure.
}

#define TEST_AMEND 0
#if TEST_AMEND
static void
test_amend()
{
    extern int test_symtab(void);
    extern int test_cmd_fn(void);
    int ret;
    LOGD("Testing symtab...\n");
    ret = test_symtab();
    LOGD("  returned %d\n", ret);
    LOGD("Testing cmd_fn...\n");
    ret = test_cmd_fn();
    LOGD("  returned %d\n", ret);
}
#endif  // TEST_AMEND

static int
erase_root(const char *root)
{
    ui_set_background(BACKGROUND_ICON_INSTALLING);
    ui_show_indeterminate_progress();
    ui_print("Formatting %s...\n", root);
    return format_root_device(root);
}



static void
choose_nandroid_file(const char *nandroid_folder)
{
    static char* headers[] = { "Choose nandroid-backup,",
			       UNCONFIRM_TXT,
                               "",
                               NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    char **list;
    int total = 0;
    int i;

    if (ensure_root_path_mounted(nandroid_folder) != 0) {
        LOGE("Can't mount %s\n", nandroid_folder);
        return;
    }

    if (translate_root_path(nandroid_folder, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return;
    }

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return;
    }

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        } else {
            total++;
        }
    }

    if (total==0) {
        LOGE("No nandroid-backup files found\n");
    		if (closedir(dir) < 0) {
		  LOGE("Failure closing directory %s", path);
	          goto out;
    		}
        return;
    }

    /* allocate the array for the file list menu */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    list = (char **) malloc((total + 1) * sizeof(*files));
    list[total] = NULL;

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        } else {

            files[i] = (char *) malloc(strlen(nandroid_folder) + strlen(de->d_name) + 1);
            strcpy(files[i], nandroid_folder);
            strcat(files[i], de->d_name);

            list[i] = (char *) malloc(strlen(de->d_name) + 1);
            strcpy(list[i], de->d_name);

            i++;

        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    ui_start_menu(headers, list);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            ui_print("\nRestore ");
            ui_print(list[chosen_item]);
            ui_clear_key_queue();
            ui_print(" ?\nPress %s to confirm,", CONFIRM);
            ui_print("\nany other key to abort.\n");
            int confirm_apply = ui_wait_key();
            if (confirm_apply == SELECT) {
                      
                            ui_print("\nRestoring : ");
       		            char nandroid_command[200]="/sbin/nandroid-mobile.sh -r -e -a --bigdata --wimax --norecovery --nomisc --nosplash1 --nosplash2 --defaultinput -s ";

			    strlcat(nandroid_command, list[chosen_item], sizeof(nandroid_command));

                            pid_t pid = fork();
                            if (pid == 0) {
                                char *args[] = {"/sbin/sh", "-c", nandroid_command , "1>&2", NULL};
                                execv("/sbin/sh", args);
                                fprintf(stderr, "\nCan't run nandroid-mobile.sh\n(%s)\n", strerror(errno));
        	                _exit(-1);
                            }

                            int status3;

                            while (waitpid(pid, &status3, WNOHANG) == 0) {
                                ui_print(".");
                                sleep(1);
                            } 
                            ui_print("\n");

                           if (!WIFEXITED(status3) || (WEXITSTATUS(status3) != 0)) {
                               ui_print("\nOops... something went wrong!\nPlease check the recovery log!\n\n");
                          } else {
                                ui_print("\nRestore complete!\n\n");
                          }

                        
            } else {
                ui_print("\nRestore aborted.\n");
            }
            if (!ui_text_visible()) break;
            break;
        }
    }

out:

    for (i = 0; i < total; i++) {
        free(files[i]);
	free(list[i]);
    }
    free(files);
    free(list);
}

static void
choose_clockwork_file()
{
    static char* headers[] = { "Choose clockworkmod nandroid-backup,",
			       UNCONFIRM_TXT,
                               "",
                               NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    char **list;
    int total = 0;
    int i;

    if (ensure_root_path_mounted(CLOCKWORK_PATH) != 0) {
        LOGE("Can't mount %s\n", CLOCKWORK_PATH);
        return;
    }

    if (translate_root_path(CLOCKWORK_PATH, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return;
    }

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return;
    }

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
	if (de->d_name[0] == '.') {
            continue;
        } else {
            total++;
        }
    }

    if (total==0) {
        LOGE("No clockworkmod nandroid-backup files found\n");
    		if (closedir(dir) < 0) {
		  LOGE("Failure closing directory %s", path);
	          goto out;
    		}
        return;
    }

    /* allocate the array for the file list menu (+1 for exit) */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    

    list = (char **) malloc((total + 1) * sizeof(*files));
    list[total] = NULL;

    

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        } else {

            files[i] = (char *) malloc(CLOCKWORK_PATH_LENGTH + strlen(de->d_name) + 1);
            strcpy(files[i], CLOCKWORK_PATH);
            strcat(files[i], de->d_name);

            list[i] = (char *) malloc(strlen(de->d_name) + 1);
            strcpy(list[i], de->d_name);

            i++;

        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    ui_start_menu(headers, list);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        

	if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            ui_print("\nRestore Clockwork Backup ");
            ui_print(list[chosen_item]);
            ui_clear_key_queue();
            ui_print(" ?\nPress %s to confirm,", CONFIRM);
            ui_print("\nany other key to abort.\n");
#ifdef HAS_WIMAX
	    ui_print("\nThis will not restore wimax backup!!\n");
#endif
            int confirm_apply = ui_wait_key();
            if (confirm_apply == SELECT) {
                      
                            ui_print("\nRestoring : ");
       		            char cw_nandroid_command[200]="/sbin/nandroid-mobile.sh -r -e -a --cwmcompat --norecovery --nomisc --nosplash1 --nosplash2 --defaultinput -s ";

			    strlcat(cw_nandroid_command, list[chosen_item], sizeof(cw_nandroid_command));

                            pid_t pid = fork();
                            if (pid == 0) {
                                char *args[] = {"/sbin/sh", "-c", cw_nandroid_command , "1>&2", NULL};
                                execv("/sbin/sh", args);
                                fprintf(stderr, "\nCan't run nandroid-mobile.sh\n(%s)\n", strerror(errno));
        	                _exit(-1);
                            }

                            int status3;

                            while (waitpid(pid, &status3, WNOHANG) == 0) {
                                ui_print(".");
                                sleep(1);
                            } 
                            ui_print("\n");

                           if (!WIFEXITED(status3) || (WEXITSTATUS(status3) != 0)) {
                               ui_print("\nOops... something went wrong!\nPlease check the recovery log!\n\n");
                          } else {
                                ui_print("\nRestore complete!\n\n");
                          }

                        
            } else {
                ui_print("\nRestore aborted.\n");
            }
            if (!ui_text_visible()) break;
            break;
        }
    }

out:

    for (i = 0; i < total; i++) {
        free(files[i]);
	free(list[i]);
    }
    free(files);
    free(list);
}


static void
choose_nandroid_folder()
{
    static char* headers[] = { "Choose Device-ID,",
			       UNCONFIRM_TXT,
                               "",
                               NULL };

    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    char **files;
    char **list;
    int total = 0;
    int i;

    if (ensure_root_path_mounted(NANDROID_PATH) != 0) {
        LOGE("Can't mount %s\n", NANDROID_PATH);
        return;
    }

    if (translate_root_path(NANDROID_PATH, path, sizeof(path)) == NULL) {
        LOGE("Bad path %s", path);
        return;
    }

    dir = opendir(path);
    if (dir == NULL) {
        LOGE("Couldn't open directory %s", path);
        return;
    }

    /* count how many files we're looking at */
    while ((de = readdir(dir)) != NULL) {
        char *extension = strrchr(de->d_name, '.');
        if (de->d_name[0] == '.') {
            continue;
        } else {
            total++;
        }
    }

    if (total==0) {
        LOGE("No Device-ID folder found\n");
    		if (closedir(dir) < 0) {
		  LOGE("Failure closing directory %s", path);
	          goto out;
    		}
        return;
    }

    /* allocate the array for the file list menu */
    files = (char **) malloc((total + 1) * sizeof(*files));
    files[total] = NULL;

    list = (char **) malloc((total + 1) * sizeof(*files));
    list[total] = NULL;

    /* set it up for the second pass */
    rewinddir(dir);

    /* put the names in the array for the menu */
    i = 0;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        } else {
            files[i] = (char *) malloc(NANDROID_PATH_LENGTH + strlen(de->d_name) + 1);
            strcpy(files[i], NANDROID_PATH);
            strcat(files[i], de->d_name);

            list[i] = (char *) malloc(strlen(de->d_name) + 1);
            strcpy(list[i], de->d_name);

            i++;
        }
    }

    /* close directory handle */
    if (closedir(dir) < 0) {
        LOGE("Failure closing directory %s", path);
        goto out;
    }

    ui_start_menu(headers, list);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            choose_nandroid_file(files[chosen_item]);
            if (!ui_text_visible()) break;
            break;
        }
    }

out:

    for (i = 0; i < total; i++) {
        free(files[i]);
        free(list[i]);
    }
    free(files);
    free(list);
}

char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles)
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
        ui_print("Couldn't open directory.\n");
        return NULL;
    }

    int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de=readdir(dir)) != NULL) {
            // skip hidden files
               if (de->d_name[0] == '.' && de->d_name[1] != '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL)
            {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            }
            else
            {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                stat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0)
            {
                total++;
                continue;
            }

            files[i] = (char*) malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**) malloc((total+1)*sizeof(char*));
        files[total]=NULL;
    }

    if(closedir(dir) < 0) {
        LOGE("Failed to close directory.");
    }

    if (total==0) {
        return NULL;
    }
	// sort the result
	 if (files != NULL) {
		for (i = 0; i < total; i++) {
			int curMax = -1;
			int j;
			for (j = 0; j < total - i; j++) {
				if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
					curMax = j;
			}
			char* temp = files[curMax];
			files[curMax] = files[total - i - 1];
			files[total - i - 1] = temp;
		}
	}

    return files;
}

int get_file_selection(char** headers, char** list) {

    // throw away keys pressed previously, so user doesn't
    // accidentally trigger menu items.
    ui_clear_key_queue();

    ui_start_menu(headers, list);
    int selected = 0;
    int chosen_item = -1;

    while (chosen_item < 0 && chosen_item != -9) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

            switch (key) {
                case UP:
                    --selected;
                    selected = ui_menu_select(selected);
                    break;
                case DN:
                    ++selected;
                    selected = ui_menu_select(selected);
                    break;
                case SELECT:
                    chosen_item = selected;
		    if (chosen_item==0) chosen_item = -9;
                    break;
                case GO_BACK:
                    chosen_item = -9;
                    break;
            }

    }

    ui_end_menu();
    ui_clear_key_queue();
    return chosen_item;
}


// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
char* choose_file_menu(const char* directory, const char* fileExtensionOrDirectory, const char* headers[])
{
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    int dir_len = strlen(directory);

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0)
    {
        ui_print("No files found.\n");
    }
    else
    {
        char** list = (char**) malloc((total + 1) * sizeof(char*));
        list[total] = NULL;

        for (i = 0 ; i < numDirs; i++)
        {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0 ; i < numFiles; i++)
        {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;)
        {

            int chosen_item = get_file_selection(headers, list);
            if (chosen_item == -9)
                break;

            static char ret[PATH_MAX];
            if (chosen_item < numDirs)
            {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL)
                {
                    strcpy(ret, subret);
                    return_value = ret;
                    break;
                }
                continue;
            }
            strcpy(ret, files[chosen_item - numDirs]);
            return_value = ret;
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}



void free_string_array(char** array)
{
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL)
    {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

static void
show_menu_nandroid()
{
   static char* headers[] = { "What do you want to backup?",
			       "",
			       NULL };
				   
	char* items[] = {       "- [X] boot",
				"- [X] system",
				"- [X] data",
				"- [X] cache",
				"- [ ] recovery",
				"- [ ] sd-ext",
				"- [ ] .android_secure",
#ifdef HAS_WIMAX		
				"- [ ] wimax",
#endif

#ifdef USE_BIGDATA	
				"- [ ] ext-data.img",
#endif
				"- Perform Backup",
		NULL};

	static char* items_in[] = { 
				"- [X] boot",
				"- [X] system",
				"- [X] data",
				"- [X] cache",
				"- [X] recovery",
				"- [X] sd-ext",
				"- [X] .android_secure",
#ifdef HAS_WIMAX		
				"- [X] wimax",
#endif

#ifdef USE_BIGDATA
				"- [X] ext-data.img",
#endif
				"- Perform Backup",
		NULL};
	
	static char* items_out[] = { 
				"- [ ] boot",
				"- [ ] system",
				"- [ ] data",
				"- [ ] cache",
				"- [ ] recovery",
				"- [ ] sd-ext",
				"- [ ] .android_secure",
#ifdef HAS_WIMAX		
				"- [ ] wimax",
#endif

#ifdef USE_BIGDATA
				"- [ ] ext-data.img",
#endif
				"- Perform Backup",
               	NULL};

	
	ui_start_menu(headers, items);
        int selected = 0;
        int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }
        
        if (chosen_item >= 0) {

            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();
#if defined (USE_BIGDATA) && defined (HAS_WIMAX)
	    if (chosen_item < 9) {
#elif defined (USE_BIGDATA)
            if (chosen_item < 8) {
#elif defined (HAS_WIMAX)
	    if (chosen_item < 8) {
#else
	    if (chosen_item < 7) {
#endif
		   // Rebuild items
		   if (items[chosen_item]==items_in[chosen_item]) {
	               items[chosen_item]=items_out[chosen_item];
	           } else {
	               items[chosen_item]=items_in[chosen_item];
	           }

            } else {

	      char nandroid_command[1024];
	      strcpy(nandroid_command, "/sbin/nandroid-mobile.sh -b --nomisc --nosplash1 --nosplash2 --defaultinput");

                int i=0;
		while (items[i])
		{


				if (strcmp( items[i], "- [X] sd-ext") == 0) strcat(nandroid_command, " -e");
				if (strcmp( items[i], "- [X] .android_secure") == 0) strcat(nandroid_command, " -a");
				if (strcmp( items[i], "- [ ] recovery") == 0) strcat(nandroid_command, " --norecovery");
				if (strcmp( items[i], "- [ ] boot") == 0) strcat(nandroid_command, " --noboot");
				if (strcmp( items[i], "- [ ] data") == 0) strcat(nandroid_command, " --nodata");
				if (strcmp( items[i], "- [ ] system") == 0) strcat(nandroid_command, " --nosystem");
				if (strcmp( items[i], "- [ ] cache") == 0) strcat(nandroid_command, " --nocache");
#ifdef HAS_WIMAX		
				if (strcmp( items[i], "- [X] wimax")  == 0) strcat(nandroid_command, " --wimax");
#endif

#ifdef USE_BIGDATA		
				if (strcmp( items[i], "- [X] ext-data.img")  == 0) strcat(nandroid_command, " --bigdata");
#endif
                	        
		i++;	
		}

			run_script("\nCreate Nandroid backup?",
				   "\nPerforming backup : ",
				   nandroid_command,
				   "\nuNnable to execute nandroid-mobile.sh!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n",
				   "\nBackup complete!\n\n",
				   "\nBackup aborted!\n\n");

            }

            ui_start_menu(headers, items);
            chosen_item = -1;
            selected = 0;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        } 

    }
	
}

void show_choose_zip_menu()
{
    if (ensure_root_path_mounted("SDCARD:") != 0) {
        LOGE ("Can't mount /sdcard\n");
        return;
    }

    static char* headers[] = {  "Choose a zip to apply",
			        UNCONFIRM_TXT,
                                "",
                                NULL 
    };
    
    char* file = choose_file_menu("/sdcard/", ".zip", headers);

    if (file == NULL)
        return;

    char sdcard_package_file[1024];
    strcpy(sdcard_package_file, "SDCARD:");
    strcat(sdcard_package_file,  file + strlen("/sdcard/"));

    ui_end_menu();

    ui_print("\nInstall : ");
    ui_print(file + strlen("/sdcard/"));
    ui_clear_key_queue();
    ui_print(" ? \nPress %s to confirm,", CONFIRM);
    ui_print("\nany other key to abort.\n");

    int confirm_apply = ui_wait_key();
    if (confirm_apply == SELECT) {
    	ui_print("\nInstall from sdcard...\n");
        int status = install_package(sdcard_package_file);
	        if (status != INSTALL_SUCCESS) {
                    ui_set_background(BACKGROUND_ICON_ERROR);
                    ui_print("\nInstallation aborted.\n");
                } else {
                    if (firmware_update_pending()) {
                        ui_print("\nReboot via vol-up+vol-down or menu\n"
                                 "to complete installation.\n");
                    } else {
                        ui_print("\nInstall from sdcard complete.\n");
                    }
                }
    } else {
        ui_print("\nInstallation aborted.\n");
    }

}

static void
show_menu_wipe()
{

    static char* headers[] = { "Choose wipe item,",
			       UNCONFIRM_TXT,
			       "",
			       NULL };


// these constants correspond to elements of the items[] list.
#define ITEM_WIPE_ALL      0
#define ITEM_WIPE_DATA     1
#define ITEM_WIPE_CACHE    2
#define ITEM_WIPE_SECURE   3
#define ITEM_WIPE_EXT      4
#define ITEM_WIPE_SYSTEM   5
#define ITEM_WIPE_DALVIK   6
#define ITEM_WIPE_BAT      7
#define ITEM_WIPE_ROT      8
#define ITEM_WIPE_SDCARD   9

#ifdef USE_BIGDATA
#define ITEM_WIPE_EXTDATA  10
#endif

    static char* items[] = { "- Wipe ALL data/factory reset",
			     "- Wipe /data",
                             "- Wipe /cache",
			     "- Wipe /sdcard/.android_secure",
                             "- Wipe /sd-ext",
                             "- Wipe /system",
                             "- Wipe Dalvik-cache",
                             "- Wipe battery stats",
                             "- Wipe rotate settings",
			     "- Wipe SDCard",
#ifdef USE_BIGDATA	    
			     "- Wipe ext data.img",
#endif
                             NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {

                case ITEM_WIPE_ALL:
                    ui_clear_key_queue();
		    ui_print("\nWipe ALL userdata");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_all = ui_wait_key();
                    if (confirm_wipe_all == SELECT) {
                        erase_root("DATA:");
                        erase_root("SDCARD:.android_secure");
                        erase_root("CACHE:");

			struct stat st;
        		if (0 != stat("/dev/block/mmcblk0p2", &st))
		        {
                        ui_print("Skipping format of /sd-ext.\n");
		        } else {
	                        erase_root("SDEXT:");
			}
                        ui_print("Userdata wipe complete!\n\n");			

                    } else {
                        ui_print("Userdata wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

                case ITEM_WIPE_DATA:
                    ui_clear_key_queue();
		    ui_print("\nWipe /data");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_data = ui_wait_key();
                    if (confirm_wipe_data == SELECT) {
                        erase_root("DATA:");
                        ui_print("/data wipe complete!\n\n");
                    } else {
                        ui_print("/data wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

                case ITEM_WIPE_EXT:
                    ui_clear_key_queue();
		    ui_print("\nWipe /sd-ext");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_ext = ui_wait_key();
                    if (confirm_wipe_ext == SELECT) {
                        
			struct stat st;
        		if (0 != stat("/dev/block/mmcblk0p2", &st))
		        {
                        ui_print("Skipping format of /sd-ext.\n");
		        } else {
	                        erase_root("SDEXT:");
	                        ui_print("/sd-ext wipe complete!\n\n");			
			}
                    } else {
                        ui_print("/sd-ext wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

                case ITEM_WIPE_SECURE:
                    ui_clear_key_queue();
		    ui_print("\nWipe /sdcard/.android_secure");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_secure = ui_wait_key();
                    if (confirm_wipe_secure == SELECT) {
                        erase_root("SDCARD:.android_secure");
                        ui_print("/sdcard/.android_secure wipe complete!\n\n");
                    } else {
                        ui_print("/sdcard/.android_secure wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

                case ITEM_WIPE_CACHE:
                    ui_clear_key_queue();
		    ui_print("\nWipe /cache");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_cache = ui_wait_key();
                    if (confirm_wipe_cache == SELECT) {
                        erase_root("CACHE:");
                        ui_print("/cache wipe complete!\n\n");
                    } else {
                        ui_print("/cache wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

                case ITEM_WIPE_DALVIK:
                    ui_clear_key_queue();
		    ui_print("\nWipe Dalvik-cache");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_dalvik = ui_wait_key();
                    if (confirm_wipe_dalvik == SELECT) {
                        ui_print("Formatting DATA:dalvik-cache...\n");
                        format_non_mtd_device("DATA:dalvik-cache");
   
                        ui_print("Formatting CACHE:dalvik-cache...\n");
                        format_non_mtd_device("CACHE:dalvik-cache");

			struct stat st;
        		if (0 != stat("/dev/block/mmcblk0p2", &st))
		        {
                        ui_print("Skipping format SDEXT:dalvik-cache.\n");
		        } else {
	                        erase_root("SDEXT:dalvik-cache");
			}
                        ui_print("Dalvik-cache wipe complete!\n\n");
                    } else {
                        ui_print("Dalvik-cache wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

		case ITEM_WIPE_BAT:
                    ui_clear_key_queue();
		    ui_print("\nWipe battery stats");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_bat = ui_wait_key();
                    if (confirm_wipe_bat == SELECT) {
                        ui_print("Wiping battery stats...\n");
                        wipe_battery_stats();
                        ui_print("Battery wipe complete!\n\n");
                    } else {
                        ui_print("Battery wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;


		case ITEM_WIPE_ROT:
		    ui_clear_key_queue();
		    ui_print("\nWipe rotate settings");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_rot = ui_wait_key();
                    if (confirm_wipe_rot == SELECT) {
                        ui_print("Wiping rotate settings...\n");
                        wipe_rotate_settings();
                        ui_print("Rotate settings wipe complete!\n\n");
                    } else {
                        ui_print("Rotate settings wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

#ifdef USE_BIGDATA
		case ITEM_WIPE_EXTDATA:
                    	run_script("\nWipe ext data.img",
				   "\nWiping ext data.img : ",
				   "/sbin/bigdata.sh wipe-dataimg",
				   "\nuNnable to execute bigdata.sh!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n",
				   "\nData.img wipe complete!\n\n",
				   "\nData.img wipe aborted!\n\n");

				   bigdata_cleanup();
                    break;
#endif

                case ITEM_WIPE_SDCARD:
                    ui_clear_key_queue();
		    ui_print("\nWipe Sdcard");
                    ui_print("\nThis is Irreversible!!!\n");
		    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_mysd = ui_wait_key();
                    if (confirm_wipe_mysd == SELECT) {
                        erase_root("SDCARD:");
                        ui_print("/Sdcard wipe complete!\n\n");
                    } else {
                        ui_print("/Sdcard wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;

		case ITEM_WIPE_SYSTEM:
                    ui_clear_key_queue();
		    ui_print("\nWipe /system");
                    ui_print("\nPress %s to confirm,", CONFIRM);
                    ui_print("\nany other key to abort.\n\n");
                    int confirm_wipe_mysys = ui_wait_key();
                    if (confirm_wipe_mysys == SELECT) {
                        erase_root("SYSTEM:");
                        ui_print("/system wipe complete!\n\n");
                    } else {
                        ui_print("/system wipe aborted!\n\n");
                    }
                    if (!ui_text_visible()) return;
                    break;
            }

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}

static void
show_menu_br()
{

    static char* headers[] = { "Choose backup/restore item;",
			       UNCONFIRM_TXT,
			       "",
			       NULL };


// these constants correspond to elements of the items[] list.
#define ITEM_NANDROID_BCK  0
#define ITEM_NANDROID_RES  1
#define ITEM_CWM_NANDROID 2
#define ITEM_GOOG_BCK  3
#define ITEM_GOOG_RES  4



    static char* items[] = { "- Nand backup",
			     "- Nand restore",
			     "- Nand restore clockworkmod backup",
			     "- Backup Google proprietary system files",
                             "- Restore Google proprietary system files",
                             NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {

                case ITEM_NANDROID_BCK:
		    ui_print("\n\n*** WARNING ***");
		    ui_print("\nNandroid backups require minimum");
		    ui_print("\n300mb SDcard space and may take a few");
		    ui_print("\nminutes to back up!\n\n");
		    ui_print("\nUse Other/recoverylog2sd for errors.\n\n"); 
		    show_menu_nandroid();
                    break;


                case ITEM_NANDROID_RES:
                    	choose_nandroid_folder();
	                break;

                case ITEM_GOOG_BCK:
			run_script("\nBackup Google proprietary system files?",
				   "\nPerforming backup : ",
				   "/sbin/backuptool.sh backup",
				   "\nuNnable to execute backuptool.sh!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n",
				   "\nBackup complete!\n\n",
				   "\nBackup aborted!\n\n");
			break;

                case ITEM_GOOG_RES:
			run_script("\nRestore Google proprietary system files?",
				   "\nPerforming restore : ",
				   "/sbin/backuptool.sh restore",
				   "\nuNnable to execute backuptool.sh!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n",
				   "\nRestore complete!\n\n",
				   "\nRestore aborted!\n\n");
			break;

		case ITEM_CWM_NANDROID:
			ui_print("\nExperimental Beta Feature\n\n");
			make_clockwork_path();
			choose_clockwork_file();
			break;
		             
            }

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}


static void
show_menu_partition()
{

    static char* headers[] = { "Choose partition item,",
			       UNCONFIRM_TXT,
			       "",
			       NULL };

// these constants correspond to elements of the items[] list.
#define ITEM_PART_SD       0
#define ITEM_PART_REP      1
#define ITEM_PART_EXT3     2
#define ITEM_PART_EXT4     3

#ifdef USE_BIGDATA
#define ITEM_PART_SD_FAT2  4
#define ITEM_PART_DATAIMG_EXT4 5
#endif

    static char* items[] = { "- Partition SD",
			     "- Repair SD:ext",
			     "- SD:ext2 to ext3",
                             "- SD:ext3 to ext4",
#ifdef USE_BIGDATA
			     "- Partition SD for bigdata",
			     "- Upgrade ext3 data.img to ext4",
#endif
                             NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {

		case ITEM_PART_SD:
                        ui_clear_key_queue();
			ui_print("\nPartition sdcard?");
			ui_print("\nPress %s to confirm,", CONFIRM);
		       	ui_print("\nany other key to abort.");
			int confirm = ui_wait_key();
				if (confirm == SELECT) {
	                                ui_clear_key_queue();
				       	ui_print("\n\nUse %s", UPDOWNTXT);
				       	ui_print("\nto increase/decrease size,");
				       	ui_print("\n%s to set (0=NONE) :\n\n", CONFIRM);
					char swapsize[32];
					int swap = 32;
					for (;;) {
						sprintf(swapsize, "%4d", swap);
						ui_print("\rSwap-size  = %s MB",swapsize);
        	                        	int key = ui_wait_key();
						if (key == SELECT) {
	           	                                ui_clear_key_queue();
							if (swap==0){
								ui_print("\rSwap-size  = %s MB : NONE\n",swapsize);
							} else {
								ui_print("\rSwap-size  = %s MB : SET\n",swapsize);
							}
							break;
					        } else if ((key == DN)) {
								swap=swap-32;
					        } else if ((key == UP)) {
								swap=swap+32;
			                        }
						if (swap < 0) { swap=0; }
					} 
                			
					char extsize[32];
					int ext = 512;
					for (;;) {
						sprintf(extsize, "%4d", ext);
						ui_print("\rExt2-size  = %s MB",extsize);
        	                        	int key = ui_wait_key();
						if (key == SELECT) {
	           	                                ui_clear_key_queue();
							if (ext==0){
								ui_print("\rExt2-size  = %s MB : NONE\n",extsize);
							} else {
								ui_print("\rExt2-size  = %s MB : SET\n",extsize);
							}
							ui_print(" FAT32-size = Remainder\n");
							break;
					        } else if ((key == DN)) {
								ext=ext-128;
					        } else if ((key == UP)) {
								ext=ext+128;
			                        }
						if (ext < 0) { ext=0; }
					}

					char es[64];
					sprintf(es, "/sbin/sdparted -s -es %dM -ss %dM",ext,swap);
					run_script("\nContinue partitioning?",
				   		   "\nPartitioning sdcard : ",
				   		   es,
	   					   "\nuNnable to execute parted!\n(%s)\n",
						   "\nOops... something went wrong!\nPlease check the recovery log!\n",
						   "\nPartitioning complete!\n\n",
						   "\nPartitioning aborted!\n\n");

				} else {
	       				ui_print("\nPartitioning aborted!\n\n");
       	        		}
				if (!ui_text_visible()) return;
			break;

#ifdef USE_BIGDATA
	        case ITEM_PART_SD_FAT2:
                        ui_clear_key_queue();
			ui_print("\nPartition sdcard for 2nd Fat32?");
			ui_print("\nPress %s to confirm,", CONFIRM);
		       	ui_print("\nany other key to abort.");
			int confirm2 = ui_wait_key();
				if (confirm2 == SELECT) {
	                                ui_clear_key_queue();
				       	ui_print("\n\nUse %s", UPDOWNTXT);
				       	ui_print("\nto increase/decrease size,");
				       	ui_print("\n%s to set (0=NONE) :\n\n", CONFIRM);
					char fat2size[32];
					int fat2 = 1096;
					for (;;) {
						sprintf(fat2size, "%4d", fat2);
						ui_print("\rFat2-size  = %s MB",fat2size);
        	                        	int key = ui_wait_key();
						if (key == SELECT) {
	           	                                ui_clear_key_queue();
							if (fat2==0){
								ui_print("\rFat2-size  = %s MB : NONE\n",fat2size);
							} else {
								ui_print("\rFat2-size  = %s MB : SET\n",fat2size);
							}
							break;
					        } else if ((key == DN)) {
								fat2=fat2-128;
					        } else if ((key == UP)) {
								fat2=fat2+128;
			                        }
						if (fat2 < 0) { fat2=0; }
					} 
                			
					char ext2size[32];
					int ext2 = 32;
					for (;;) {
						sprintf(ext2size, "%4d", ext2);
						ui_print("\rExt2-size  = %s MB",ext2size);
        	                        	int key = ui_wait_key();
						if (key == SELECT) {
	           	                                ui_clear_key_queue();
							if (ext2==0){
								ui_print("\rExt2-size  = %s MB : NONE\n",ext2size);
							} else {
								ui_print("\rExt2-size  = %s MB : SET\n",ext2size);
							}
							ui_print(" FAT32-size = Remainder\n");
							break;
					        } else if ((key == DN)) {
								ext2=ext2-32;
					        } else if ((key == UP)) {
								ext2=ext2+32;
			                        }
						if (ext2 < 0) { ext2=0; }
					}

					char ds[64];
					sprintf(ds, "/sbin/fat2parted -s -es %dM -f2s %dM",ext2,fat2);
					run_script("\nContinue partitioning?",
				   		   "\nPartitioning sdcard : ",
				   		   ds,
	   					   "\nuNnable to execute fat2parted!\n(%s)\n",
						   "\nOops... something went wrong!\nPlease check the recovery log!\n",
						   "\nPartitioning complete!\n\n",
						   "\nPartitioning aborted!\n\n");

				} else {
	       				ui_print("\nPartitioning aborted!\n\n");
       	        		}
				if (!ui_text_visible()) return;
			break;
#endif

		case ITEM_PART_REP:
			run_script("\nRepair ext filesystem",
				   "\nRepairing ext filesystem : ",
				   "/sbin/fs repair",
				   "\nUnable to execute fs!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n\n",
				   "\nExt repairing complete!\n\n",
				   "\nExt repairing aborted!\n\n");
			break;
                   
		case ITEM_PART_EXT3:
			run_script("\nUpgrade ext2 to ext3",
				   "\nUpgrading ext2 to ext3 : ",
				   "/sbin/fs ext3",
				   "\nUnable to execute fs!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n\n",
				   "\nExt upgrade complete!\n\n",
				   "\nExt upgrade aborted!\n\n");
			break;

		case ITEM_PART_EXT4:
			run_script("\nUpgrade ext3 to ext4",
				   "\nUpgrading ext3 to ext4 : ",
				   "/sbin/fs ext4",
				   "\nUnable to execute fs!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n\n",
				   "\nExt upgrade complete!\n\n",
				   "\nExt upgrade aborted!\n\n");
			break;

#ifdef USE_BIGDATA
		case ITEM_PART_DATAIMG_EXT4:
			run_script("\nUpgrade data.img ext3 to ext4",
				   "\nUpgrading data.img ext3 to ext4 : ",
				   "/sbin/bigdata.sh upgrade-ext4",
				   "\nUnable to execute bigdata.sh!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n\n",
				   "\nExt4 data.img upgrade complete!\n\n",
				   "\nExt4 data.img upgrade aborted!\n\n");
			break;
#endif

            }

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}

static void
show_menu_other()
{

    static char* headers[] = { "Choose item,",
			       UNCONFIRM_TXT,
			       "",
			       NULL };

// these constants correspond to elements of the items[] list.
#define ITEM_OTHER_FIXUID 0
#define ITEM_OTHER_RE2SD  1
#define ITEM_OTHER_KEY_TEST 2
#define ITEM_OTHER_BATTERY_LEVEL 3

#ifdef USE_BIGDATA
#define ITEM_OTHER_CREATEEXTDATA 4
#define ITEM_OTHER_RMEXTDATA 5
#endif

    static char* items[] = { "- Fix Permissions",
			     "- Move recovery.log to SD",
                             "- Debugging Test Key Codes",
			     "- Check Battery Level",
#ifdef USE_BIGDATA
			     "- Create new ext3 data.img",
			     "- Delete existing ext3 data.img",
#endif	     
			     NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {

	        case ITEM_OTHER_FIXUID:
			run_script("\nFix package uid mismatches",
				   "\nFixing package uid mismatches : ",
				   "/sbin/fix_permissions",
				   "\nUnable to execute fix_permissions!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n\n",
				   "\nUid mismatches fixed!\n\n",
				   "\nFixing aborted!\n\n");
			break;

		case ITEM_OTHER_RE2SD:
			run_script("\nMove recovery.log to SD",
				   "\nMoving : ",
				   "/sbin/log2sd",
				   "\nUnable to execute log2sd!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n\n",
				   "\nMoving complete!\n\n",
				   "\nMoving aborted!\n\n");
			break;
		
		case ITEM_OTHER_KEY_TEST:
				key_logger_test();
				break;
		
		case ITEM_OTHER_BATTERY_LEVEL:
				check_my_battery_level();
				break;

#ifdef USE_BIGDATA				
		case ITEM_OTHER_CREATEEXTDATA:
                    	ui_print("\nThis may take 5 to 10 min ...\n\n");
			run_script("\nCreate ext3 data.img",
				   "\nMaking ext3 data.img : ",
				   "/sbin/bigdata.sh make-dataimg",
				   "\nuNnable to execute bigdata.sh!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n",
				   "\nData.img created!\n\n",
				   "\nData.img create aborted!\n\n");

				   bigdata_cleanup();
                    break;

		case ITEM_OTHER_RMEXTDATA:
                    	run_script("\nDelete ext3 data.img",
				   "\nDeleting ext3 data.img : ",
				   "/sbin/bigdata.sh rm-dataimg",
				   "\nuNnable to execute bigdata.sh!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n",
				   "\nData.img deleted!\n\n",
				   "\nData.img delete aborted!\n\n");

				   bigdata_cleanup();
                    break;
#endif

		}

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}

static void
show_menu_flash()
{

    static char* headers[] = { "Choose item,",
			       UNCONFIRM_TXT,
			       "",
			       	NULL };

// these constants correspond to elements of the items[] list.
#define ITEM_FLASH_FLASHZIP 0
#define ITEM_FLASH_TOGGLE   1

    static char* items[] = { "- Choose zip from sdcard",
                             "- Toggle signature verification",
				NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {

		case ITEM_FLASH_FLASHZIP:
        	        show_choose_zip_menu();
        	        break;
		
		case ITEM_FLASH_TOGGLE:
      			toggle_signature_check();
			break;   
           
            }

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}



static void
create_mount_items(char *items[],int item)
{
	int i=0;
	
	static char* roots[] = { 
				"SYSTEM:",
				"CACHE:",
				"DATA:",
				"SDEXT:",
				"SDCARD:",
             	NULL};
	
	static char* items_m[] = { 
				"- Mount /system",
				"- Mount /cache",
				"- Mount /data",
				"- Mount /sd-ext",
				"- Mount /sdcard",
		NULL};
	
	static char* items_u[] = { 
				"- Unmount /system",
				"- Unmount /cache",
				"- Unmount /data",
				"- Unmount /sd-ext",
				"- Unmount /sdcard",
               	NULL};
	
	while (roots[i])
	{
		if (item!=-1&&i==item)
		{
			// Mounted ?
			if (is_root_path_mounted(roots[i]))
				ensure_root_path_unmounted(roots[i]);
			else
				ensure_root_path_mounted(roots[i]);
		}
		
		if (is_root_path_mounted(roots[i]))
			items[i]=items_u[i];
		else
			items[i]=items_m[i];
			
		i++;
	}
}

            	 
static void
show_menu_mount()
{
   static char* headers[] = { "Choose mount item,",
			       UNCONFIRM_TXT,
			       "",
			       NULL };
				   
	char* items[]={NULL,NULL,NULL,NULL,NULL,NULL};
	
	create_mount_items(items,-1);
	ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
}
        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

			// Rebuild items
            create_mount_items(items, selected);
						
            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
	
}

static void
show_menu_usb()
{

    static char* headers[] = { "Choose item,",
			       UNCONFIRM_TXT,
			       "",
			       	NULL };

// these constants correspond to elements of the items[] list.
#define ITEM_USB_SD 0



    static char* items[] = { "- USB-MS Toggle SDCard",
			     
                             	NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {

		case ITEM_USB_SD:
			usb_toggle_sdcard();
			break;
		

		
	
			}
// if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}

static void
show_menu_developer()
{

    static char* headers[] = { "Choose Developer item,",
			       UNCONFIRM_TXT,
			       "",
			       	NULL };

// these constants correspond to elements of the items[] list.

#define ITEM_DEV_MKBOOT 0
#define ITEM_DEV_SU 1
#define ITEM_DEV_SU_ENG 2
#define ITEM_DEV_RB_BOOT 3
#define ITEM_DEV_RB_REC 4

#ifdef LGE_RESET_BOOTMODE
#define ITEM_DEV_RESET_LGE_BM 5
#endif

    static char* items[] = { "- Make and flash boot from zimage",
			     "- Install su & superuser",
			     "- Install eng (unguarded) su",
			     "- Reboot to bootloader",
			     "- Reboot recovery",
#ifdef LGE_RESET_BOOTMODE
			     "- Reset LGE boot mode",
#endif	
                             	NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == GO_BACK) {
            break;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {


	
		
		case ITEM_DEV_MKBOOT:
			ui_clear_key_queue();
			ui_print("\nMake new boot from zImage?");
			ui_print("\nMust be plugged into pc as");
			ui_print("\nsdcard is ejected as mass storage");
			ui_print("\nPress %s to confirm,", CONFIRM);
		       	ui_print("\nany other key to abort.\n\n");
			int confirm_mkboot = ui_wait_key();
				if (confirm_mkboot == SELECT) {
				do_make_new_boot();
				} else {
					 ui_print("\nAborted make new boot.\n\n");
				}
				if (!ui_text_visible()) return;  
			
			break;

		case ITEM_DEV_SU:
			ui_clear_key_queue();
			ui_print("\nInstall or fix su & superuser?");
			ui_print("\nPress %s to confirm,", CONFIRM);
		       	ui_print("\nany other key to abort.\n\n");
			int confirm_su_super = ui_wait_key();
				if (confirm_su_super == SELECT) {
				install_su(0);
				} else {
					 ui_print("\nInstall of su & superuser aborted.\n\n");
				}
				if (!ui_text_visible()) return; 
			
			break;

		case ITEM_DEV_SU_ENG:
			ui_clear_key_queue();
			ui_print("\nInstall eng (unguarded) su?");
			ui_print("\nPress %s to confirm,", CONFIRM);
		       	ui_print("\nany other key to abort.\n\n");
			int confirm_su_eng = ui_wait_key();
				if (confirm_su_eng == SELECT) {
				install_su(1);
				} else {
					 ui_print("\nInstall of su aborted.\n\n");
				}
				if (!ui_text_visible()) return; 
			
			break;

		case ITEM_DEV_RB_REC:
				rb_recovery();
			break;

		case ITEM_DEV_RB_BOOT:
				rb_bootloader();
			break;

#ifdef LGE_RESET_BOOTMODE
		case ITEM_DEV_RESET_LGE_BM:
				lge_direct_mtd_access("6");
			break;
#endif
			

			}
// if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
	  }
	 }	
	}


static void
prompt_and_wait()
{
	
  
	
    static char* headers[] = { "Android system recovery",
			       "",
			       NULL };

// these constants correspond to elements of the items[] list.
#define ITEM_REBOOT        0
#define ITEM_USBTOGGLE     1
#define ITEM_BR            2
#define ITEM_FLASH         3
#define ITEM_WIPE          4
#define ITEM_PARTITION     5
#define ITEM_MOUNT	   6
#define ITEM_OTHER         7
#define ITEM_DEVELOPER	   8
#define ITEM_POWEROFF      9


    static char* items[] = { "- Reboot system now",
                             "- USB-MS toggle",
                             "- Backup/Restore",
                             "- Flash zip menu",
                             "- Wipe",
                             "- Partition sdcard",
                             "- Mounts",
			     "- Other",
			     "- Developer menu",
                             "- Power off",
                             NULL };

    ui_start_menu(headers, items);
    int selected = 0;
    int chosen_item = -1;

    finish_recovery(NULL);
    ui_reset_progress();
    for (;;) {
        int key = ui_wait_key();
        int alt = ui_key_pressed(KEY_LEFTALT) || ui_key_pressed(KEY_RIGHTALT);
        int visible = ui_text_visible();

        if (key == DN && ui_key_pressed(SELECT)) {
            // Wait for the keys to be released, to avoid triggering
            // special boot modes (like coming back into recovery!).
            while (ui_key_pressed(SELECT) ||
                   ui_key_pressed(DN)) {
                usleep(1000);
            }
            chosen_item = ITEM_REBOOT;
        } else if ((key == DN) && visible) {
            ++selected;
            selected = ui_menu_select(selected);
        } else if ((key == UP) && visible) {
            --selected;
            selected = ui_menu_select(selected);
        } else if ((key == SELECT) && visible ) {
            chosen_item = selected;
        }

        if (chosen_item >= 0) {
            // turn off the menu, letting ui_print() to scroll output
            // on the screen.
            ui_end_menu();

            switch (chosen_item) {
                case ITEM_REBOOT:
                    return;

                case ITEM_USBTOGGLE:
                    show_menu_usb();	
                    break;  

		case ITEM_BR:
                    show_menu_br();
                    break;

		case ITEM_FLASH:
                    show_menu_flash();
                    break;

                case ITEM_WIPE:
                    show_menu_wipe();
                    break;

                case ITEM_PARTITION:
                    show_menu_partition();
                    break;

		case ITEM_MOUNT:
			show_menu_mount();
			break;

		case ITEM_OTHER:
                    show_menu_other();
        	    break; 

	        case ITEM_POWEROFF:
			run_script("\nPower off phone?",
				   "\nShutting down : ",
				   "/sbin/reboot -p",
				   "\nUnable to power off phone!\n(%s)\n",
				   "\nOops... something went wrong!\nPlease check the recovery log!\n\n",
				   "\nPower off complete!\n\n",
				   "\nPower off aborted!\n\n");
			break;

		case ITEM_DEVELOPER:
			show_menu_developer();
			break;

          
            }

            // if we didn't return from this function to reboot, show
            // the menu again.
            ui_start_menu(headers, items);
            selected = 0;
            chosen_item = -1;

            finish_recovery(NULL);
            ui_reset_progress();

            // throw away keys pressed while the command was running,
            // so user doesn't accidentally trigger menu items.
            ui_clear_key_queue();
        }
    }
}


static void
print_property(const char *key, const char *name, void *cookie)
{
    fprintf(stderr, "%s=%s\n", key, name);
}

int
main(int argc, char **argv)
{
    time_t start = time(NULL);

    // If these fail, there's not really anywhere to complain...
    freopen(TEMPORARY_LOG_FILE, "a", stdout); setbuf(stdout, NULL);
    freopen(TEMPORARY_LOG_FILE, "a", stderr); setbuf(stderr, NULL);
    fprintf(stderr, "Starting recovery on %s", ctime(&start));

    tcflow(STDIN_FILENO, TCOOFF);

    char prop_value[PROPERTY_VALUE_MAX];
    property_get("ro.modversion", &prop_value[0], "not set");
 
    ui_init();
    ui_print("Build : ");
    ui_print(prop_value);
    ui_print("\n");

    get_args(&argc, &argv);
    
    int previous_runs = 0;
    const char *send_intent = NULL;
    const char *update_package = NULL;
    int wipe_data = 0, wipe_cache = 0;

    int arg;
    while ((arg = getopt_long(argc, argv, "", OPTIONS, NULL)) != -1) {
        switch (arg) {
        case 'p': previous_runs = atoi(optarg); break;
        case 's': send_intent = optarg; break;
        case 'u': update_package = optarg; break;
        case 'w': wipe_data = wipe_cache = 1; break;
        case 'c': wipe_cache = 1; break;
        case '?':
            LOGE("Invalid command argument\n");
            continue;
        }
    }

    fprintf(stderr, "Command:");
    for (arg = 0; arg < argc; arg++) {
        fprintf(stderr, " \"%s\"", argv[arg]);
    }
    fprintf(stderr, "\n\n");

    property_list(print_property, NULL);
    fprintf(stderr, "\n");

#if TEST_AMEND
    test_amend();
#endif

    RecoveryCommandContext ctx = { NULL };
    if (register_update_commands(&ctx)) {
        LOGE("Can't install update commands\n");
    }

    int status = INSTALL_SUCCESS;

    if (update_package != NULL) {
        status = install_package(update_package);
        if (status != INSTALL_SUCCESS) ui_print("Installation aborted.\n");
    } else if (wipe_data || wipe_cache) {
        if (wipe_data && erase_root("DATA:")) status = INSTALL_ERROR;
        if (wipe_cache && erase_root("CACHE:")) status = INSTALL_ERROR;
        if (status != INSTALL_SUCCESS) ui_print("Data wipe failed.\n");
    } else {
        status = INSTALL_ERROR;  // No command specified
    }

    if (status != INSTALL_SUCCESS) ui_set_background(BACKGROUND_ICON_ERROR);
    if (status != INSTALL_SUCCESS || ui_text_visible()) prompt_and_wait();

    // If there is a radio image pending, reboot now to install it.
    maybe_install_firmware_update(send_intent);

    // Otherwise, get ready to boot the main system...
    finish_recovery(send_intent);
    sync();
    if (do_reboot)
    {
    	ui_print("Rebooting...\n");
    	reboot(RB_AUTOBOOT);
	}
	
	tcflush(STDIN_FILENO, TCIOFLUSH);	
	tcflow(STDIN_FILENO, TCOON);
	
    return EXIT_SUCCESS;
}
