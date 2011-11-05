/* Getitnowmarketing 10/29/11 
added to define recovery keys so recovery keys can easily be changed for new devices */

#ifdef DEFAULT_RECOVERY_UI_KEYS

#define CONFIRM "Menu"
#define UNCONFIRM_TXT "or press BACK to return"
#define UPDOWNTXT "Vol Up/Down"

/* these are defined in device's kernel input.h */
#define SELECT KEY_MENU
#define UP KEY_VOLUMEUP
#define DN KEY_VOLUMEDOWN
#define GO_BACK KEY_BACK

#endif

#ifdef ALOHA_RECOVERY_UI_KEYS

#define CONFIRM "Power"
#define UNCONFIRM_TXT "or press BACK to return"
#define UPDOWNTXT "Vol Up/Down"

/* these are defined in device's kernel input.h */
#define SELECT KEY_END
#define UP KEY_VOLUMEUP
#define DN KEY_VOLUMEDOWN
#define GO_BACK 248

#endif

#ifdef HTC_TRACKBALL_RECOVERY_UI_KEYS

#define CONFIRM "Trackball"
#define UNCONFIRM_TXT "or press VOL-DOWN to return"
#define UPDOWNTXT "Up/Down"

#define SELECT BTN_MOUSE
#define UP KEY_UP
#define DN KEY_DOWN
#define GO_BACK KEY_VOLUMEDOWN

#endif

#ifdef HTC_NO_TRACKBALL_RECOVERY_UI_KEYS

#define CONFIRM "Power"
#define UNCONFIRM_TXT "or press BACK to return"
#define UPDOWNTXT "Vol Up/Down"

/* these are defined in device's kernel input.h */
#define SELECT KEY_POWER
#define UP KEY_VOLUMEUP
#define DN KEY_VOLUMEDOWN
#define GO_BACK KEY_BACK

#endif




