extern int signature_check_enabled;

void
toggle_signature_check();

void
run_script(char *str1,char *str2,char *str3,char *str4,char *str5,char *str6,char *str7);

void
usb_toggle_sdcard();

int
__system(const char *command);

int
format_non_mtd_device(const char* root);


void
wipe_battery_stats();

void
wipe_rotate_settings();

void 
key_logger_test();

void
check_my_battery_level();

void
make_clockwork_path();

void
unpack_boot();

int
dump_device(const char *device);

void
setup_mkboot();

int
check_file_exists(const char* file_path);

int
copy_file(const char* source, const char* dest);

void
do_module();

void
do_make_new_boot();

int
is_dir(const char* file_path);

int
delete_file(const char* file);

void
install_su(int eng_su);

void
rb_bootloader();

void
rb_recovery();

#ifdef USE_BIGDATA

void
bigdata_cleanup();

#endif

#ifdef LGE_RESET_BOOTMODE
int
lge_direct_mtd_access(char *boot_mode);
#endif


