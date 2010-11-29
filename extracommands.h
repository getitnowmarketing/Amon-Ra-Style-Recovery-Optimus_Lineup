extern int do_verify;

void
do_verify_switch();

int
__system(const char *command);

int
format_non_mtd_device(const char* root);

//void usb_toggle_emmc();

void usb_toggle_sdcard();

void
run_script(char *str1,char *str2,char *str3,char *str4,char *str5,char *str6,char *str7);

void
wipe_battery_stats();

void
wipe_rotate_settings();

//void show_choose_zip_menu_emmc();

void key_logger_test();

//void check_my_battery_level();

