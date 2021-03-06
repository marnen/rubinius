#ifndef RBX_DETECTION
#define RBX_DETECTION

/*
 * This file, when included, defines a bunch of macros that have detected
 * values about the current machine.
 *
 * Towards the bottom, we also use those macros to setup some configuration
 * variables.
 *
 */


/** DETECT */

#if __ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__ >= 1050
#define OS_X_10_5
#elif defined(__APPLE__) && defined(__APPLE_CC__)
#define OS_X_ANCIENT
#endif

/** CONFIGURE */

#ifndef OS_X_ANCIENT
#define USE_EXECINFO
#endif

#endif
