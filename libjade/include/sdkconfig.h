#ifndef _SW_JADE_SDKCONFIG_H_
#define _SW_JADE_SDKCONFIG_H_ 1

// Config defines for a software Jade device

// Export debug mode functions for testing
#define CONFIG_DEBUG_MODE 1

// Auto "press" OK buttons when tasks are run (after 1ms)
#define CONFIG_DEBUG_UNATTENDED_CI 1
#define CONFIG_DEBUG_UNATTENDED_CI_TIMEOUT_MS 1

// Default to no logging
#define CONFIG_LOG_DEFAULT_LEVEL_NONE

// Tell the firmware code we are building libjade
#define CONFIG_LIBJADE 1

// libjade currently has no GUI support
#define CONFIG_LIBJADE_NO_GUI 1

// We have plenty of RAM
#define CONFIG_SPIRAM 1

// Provide values in order to compile (we don't actually have a screen)
#define CONFIG_DISPLAY_WIDTH 320
#define CONFIG_DISPLAY_HEIGHT 200

#endif // _SW_JADE_SDKCONFIG_H_
