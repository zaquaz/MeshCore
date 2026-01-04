#pragma once

/**
 * PowerManagerIntegration.h - Integration helpers for PowerManager
 * 
 * This file provides helper functions and macros to simplify integrating
 * PowerManager into main.cpp with minimal boilerplate code.
 * 
 * Usage in main.cpp:
 *   #include <helpers/PowerManagerIntegration.h>
 *   
 *   void setup() {
 *     the_mesh.begin(fs);
 *     initPowerManager(the_mesh, the_mesh.getNodePrefs());
 *   }
 *   
 *   void loop() {
 *     powerManagerLoopStart();
 *     // ... your loop code ...
 *     recordSerialActivity(ACTIVITY_SERIAL_RX);  // when reading serial
 *     // ...
 *     powerManagerLoopEnd();
 *   }
 */

#include "PowerManager.h"
#include "CommonCLI.h"

// Global power manager instance (singleton pattern)
static PowerManager _global_power_manager;

/**
 * Get reference to the global power manager instance
 */
inline PowerManager& getPowerManager() {
    return _global_power_manager;
}

/**
 * Initialize power manager with mesh integration
 * Call this in setup() after mesh.begin()
 * 
 * @param mesh Reference to the mesh instance (must have setPowerManager and hasPendingOutbound methods)
 * @param prefs Pointer to NodePrefs for restoring saved settings
 */
template<typename MeshType>
void initPowerManager(MeshType& mesh, NodePrefs* prefs) {
    auto& pm = getPowerManager();
    pm.begin();
    mesh.setPowerManager(&pm);
    
    // Set callback to check for pending outbound packets before sleeping
    // Use a static pointer and a non-capturing lambda so it can convert to
    // the C-style function pointer type expected by PowerManager.
    static MeshType* _pm_mesh_ptr = nullptr;
    _pm_mesh_ptr = &mesh;
    pm.setHasPendingOutboundCallback([]() -> bool {
        return _pm_mesh_ptr->hasPendingOutbound();
    });
    
    // Restore power saving state from persistent preferences
    if (prefs && prefs->power_saving_enabled) {
        pm.setPowerSavingEnabled(true);
        MESH_DEBUG_PRINTLN("Power saving: enabled (from prefs)");
    } else {
        pm.setPowerSavingEnabled(false);
        MESH_DEBUG_PRINTLN("Power saving: disabled");
    }
    
    // Restore serial check state from persistent preferences
    if (prefs && prefs->serial_check_disabled) {
        pm.setSerialCheckDisabled(true);
        MESH_DEBUG_PRINTLN("Serial check: disabled (from prefs)");
    } else {
        pm.setSerialCheckDisabled(false);
        MESH_DEBUG_PRINTLN("Serial check: enabled");
    }
}

/**
 * Call at the start of loop() to update power mode
 */
inline void powerManagerLoopStart() {
    getPowerManager().updatePowerMode();
}

/**
 * Record serial activity (wraps the serial check logic)
 * Only records if serial check is not disabled
 * 
 * @param activity_type ACTIVITY_SERIAL_RX or ACTIVITY_SERIAL_TX
 */
inline void recordSerialActivity(uint8_t activity_type) {
    auto& pm = getPowerManager();
    if (!pm.isSerialCheckDisabled()) {
        pm.recordActivity(activity_type);
    }
}

/**
 * Record any type of activity
 * 
 * @param activity_type One of ACTIVITY_* constants
 */
inline void recordActivity(uint8_t activity_type) {
    getPowerManager().recordActivity(activity_type);
}

/**
 * Call at end of loop() to apply power saving
 * Handles platform-specific sleep modes automatically
 * 
 * For ESP32: Only uses light sleep if P_LORA_DIO_1 is an RTC-capable GPIO
 * (required for ext0 wakeup with SX1262). Falls back to delay otherwise.
 * 
 * For custom behavior, call getPowerManager().applyPowerSaving() directly
 */
inline void powerManagerLoopEnd() {
    auto& pm = getPowerManager();
#ifdef NRF52_PLATFORM
    pm.applyPowerSaving();  // Uses System ON sleep on NRF52
#elif defined(ESP32) && defined(P_LORA_DIO_1)
    // Only use light sleep if DIO1 is an RTC-capable GPIO (required for ext0 wakeup)
    // SX1262 DIO1 has short pulses that require ext0/ext1, not regular GPIO wakeup
    if (rtc_gpio_is_valid_gpio((gpio_num_t)P_LORA_DIO_1)) {
        pm.applyPowerSaving(P_LORA_DIO_1, pm.getMaxSleepDuration());
    } else {
        pm.applyLoopDelay();  // Fall back to delay if not RTC GPIO
    }
#else
    pm.applyLoopDelay();    // Conservative delay-based fallback
#endif
}

/**
 * Check if power manager allows low power mode
 * Useful for conditional behavior based on power state
 */
inline bool canEnterLowPower() {
    return getPowerManager().canEnterLowPower();
}

/**
 * Check if serial check is disabled
 * Useful for conditional serial activity recording
 */
inline bool isSerialCheckDisabled() {
    return getPowerManager().isSerialCheckDisabled();
}
