/**
 * @file time.h
 * @brief Kernel Timekeeping Declarations
 *
 * Provides basic types and function prototypes for kernel time services.
 * Assumes an underlying time source (like RTC) is initialized and managed elsewhere.
 */

 #ifndef _KERNEL_TIME_H_
 #define _KERNEL_TIME_H_
 
 // Include kernel-specific standard types if not globally available
 // If you have a central types header, include that instead.
 #include "libc/stdint.h" // Assuming a kernel-provided stdint.h or equivalent exists
                     // If not, you might need definitions like: typedef unsigned long long uint64_t;
 
 // Define the Epoch Year used by kernel_time_t
 #define KERNEL_EPOCH_YEAR 1970
 
 /**
  * @brief Kernel time representation.
  * Represents seconds elapsed since the KERNEL_EPOCH_YEAR (e.g., 1970).
  * Using 64 bits avoids the Year 2038 problem.
  */
 typedef uint64_t kernel_time_t;
 
 /**
  * @brief Gets the current kernel time.
  *
  * Retrieves the current time as seconds elapsed since the KERNEL_EPOCH_YEAR.
  * The underlying implementation should query the system's time source (e.g., RTC).
  *
  * @return The current time as a kernel_time_t value. Returns 0 if the time
  * source is not available or initialized.
  */
 kernel_time_t kernel_get_time(void);
 
 /*
  * NOTE: Functions for converting kernel_time_t to specific formats
  * (like FAT timestamps) belong in the modules that require those formats.
  *
  * Example (should be declared in fat_utils.h):
  * void fat_pack_timestamp(kernel_time_t time, uint16_t *fat_date, uint16_t *fat_time);
  */
 
 /*
  * NOTE: Functions for timers, delays, ticks etc. might also go here
  * or in a separate timer-specific header depending on kernel design.
  * Example:
  * uint64_t kernel_get_ticks(void); // Ticks since boot
  * void kernel_sleep_ms(uint32_t milliseconds);
  */
 
 #endif /* _KERNEL_TIME_H_ */