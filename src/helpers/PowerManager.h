#pragma once

#include <Arduino.h>

/**
 * PowerManager - Centralized power management for MeshCore devices
 * 
 * Features:
 * - Detects serial/USB activity to prevent sleep when connected to host (e.g., Raspberry Pi)
 * - Adaptive loop delays based on activity level
 * - Optional light sleep support (ESP32) with interrupt wake
 * - Safe defaults - power saving only when safe to do so
 * 
 * Clock Skew Note:
 * - ESP32 light sleep: millis() continues via hardware timer
 * - NRF52 System ON sleep: millis() continues via RTC peripheral
 * - No clock drift concerns for these sleep modes
 */

// Power modes
#define POWER_MODE_ACTIVE      0   // Full speed, no delays
#define POWER_MODE_IDLE        1   // Short delays in loop
#define POWER_MODE_LOW_POWER   2   // Longer delays, optional light sleep

// Activity types that affect power state
#define ACTIVITY_SERIAL_RX     (1 << 0)
#define ACTIVITY_SERIAL_TX     (1 << 1)
#define ACTIVITY_RADIO_RX      (1 << 2)
#define ACTIVITY_RADIO_TX      (1 << 3)
#define ACTIVITY_USER_INPUT    (1 << 4)

// Default timeouts (milliseconds)
#define SERIAL_ACTIVITY_TIMEOUT_MS    30000   // 30 seconds - assume connected if recent serial activity
#define IDLE_TIMEOUT_MS               10000   // 10 seconds before entering idle mode
#define LOW_POWER_TIMEOUT_MS          60000   // 60 seconds before entering low power mode

// Loop delays for each power mode
#define ACTIVE_LOOP_DELAY_MS          0       // No delay when active
#define IDLE_LOOP_DELAY_MS            5       // 5ms delay in idle
#define LOW_POWER_LOOP_DELAY_MS       20      // 20ms delay in low power

class PowerManager {
private:
    unsigned long _last_serial_activity;
    unsigned long _last_radio_activity;
    unsigned long _last_any_activity;
    uint8_t _current_mode;
    bool _power_saving_enabled;
    bool _serial_connected_override;  // Force assume serial connected (for debugging)
    
    // Statistics
    uint32_t _idle_loops;
    uint32_t _active_loops;
    uint32_t _sleep_count;
    
public:
    PowerManager() {
        _last_serial_activity = 0;
        _last_radio_activity = 0;
        _last_any_activity = 0;
        _current_mode = POWER_MODE_ACTIVE;
        _power_saving_enabled = true;
        _serial_connected_override = false;
        _idle_loops = 0;
        _active_loops = 0;
        _sleep_count = 0;
    }
    
    /**
     * Initialize power manager
     * Call once in setup()
     */
    void begin() {
        unsigned long now = millis();
        _last_serial_activity = now;
        _last_radio_activity = now;
        _last_any_activity = now;
        _current_mode = POWER_MODE_ACTIVE;
    }
    
    /**
     * Record activity of a specific type
     * Call this whenever relevant activity occurs
     */
    void recordActivity(uint8_t activity_type) {
        unsigned long now = millis();
        _last_any_activity = now;
        
        if (activity_type & (ACTIVITY_SERIAL_RX | ACTIVITY_SERIAL_TX)) {
            _last_serial_activity = now;
        }
        if (activity_type & (ACTIVITY_RADIO_RX | ACTIVITY_RADIO_TX)) {
            _last_radio_activity = now;
        }
    }
    
    /**
     * Check if serial appears to be connected (recent activity)
     * Used to prevent sleep when device is connected to a host like Raspberry Pi
     */
    bool isSerialActive() const {
        if (_serial_connected_override) return true;
        
        unsigned long now = millis();
        return (now - _last_serial_activity) < SERIAL_ACTIVITY_TIMEOUT_MS;
    }
    
    /**
     * Check if power saving should be active
     * Returns false if serial is active or power saving is disabled
     */
    bool canEnterLowPower() const {
        if (!_power_saving_enabled) return false;
        if (isSerialActive()) return false;
        return true;
    }
    
    /**
     * Update power mode based on activity
     * Call this at the start of each loop iteration
     */
    void updatePowerMode() {
        if (!_power_saving_enabled || isSerialActive()) {
            _current_mode = POWER_MODE_ACTIVE;
            return;
        }
        
        unsigned long now = millis();
        unsigned long idle_time = now - _last_any_activity;
        
        if (idle_time < IDLE_TIMEOUT_MS) {
            _current_mode = POWER_MODE_ACTIVE;
        } else if (idle_time < LOW_POWER_TIMEOUT_MS) {
            _current_mode = POWER_MODE_IDLE;
        } else {
            _current_mode = POWER_MODE_LOW_POWER;
        }
    }
    
    /**
     * Get the appropriate loop delay for current power mode
     * Apply this delay at the end of each loop iteration
     */
    uint8_t getLoopDelayMs() const {
        switch (_current_mode) {
            case POWER_MODE_IDLE:
                return IDLE_LOOP_DELAY_MS;
            case POWER_MODE_LOW_POWER:
                return LOW_POWER_LOOP_DELAY_MS;
            default:
                return ACTIVE_LOOP_DELAY_MS;
        }
    }
    
    /**
     * Apply loop delay based on current power mode
     * Call at end of main loop
     */
    void applyLoopDelay() {
        uint8_t delay_ms = getLoopDelayMs();
        if (delay_ms > 0) {
            _idle_loops++;
            delay(delay_ms);  // This allows FreeRTOS to do power management
        } else {
            _active_loops++;
        }
    }
    
    /**
     * Get current power mode
     */
    uint8_t getCurrentMode() const {
        return _current_mode;
    }
    
    /**
     * Get mode name string for debugging
     */
    const char* getModeName() const {
        switch (_current_mode) {
            case POWER_MODE_ACTIVE: return "ACTIVE";
            case POWER_MODE_IDLE: return "IDLE";
            case POWER_MODE_LOW_POWER: return "LOW_POWER";
            default: return "UNKNOWN";
        }
    }
    
    /**
     * Enable/disable power saving
     */
    void setPowerSavingEnabled(bool enabled) {
        _power_saving_enabled = enabled;
        if (!enabled) {
            _current_mode = POWER_MODE_ACTIVE;
        }
    }
    
    bool isPowerSavingEnabled() const {
        return _power_saving_enabled;
    }
    
    /**
     * Force serial connected state (for debugging)
     */
    void setSerialConnectedOverride(bool connected) {
        _serial_connected_override = connected;
    }
    
    /**
     * Get statistics
     */
    uint32_t getIdleLoops() const { return _idle_loops; }
    uint32_t getActiveLoops() const { return _active_loops; }
    uint32_t getSleepCount() const { return _sleep_count; }
    
    /**
     * Reset statistics
     */
    void resetStats() {
        _idle_loops = 0;
        _active_loops = 0;
        _sleep_count = 0;
    }
    
    /**
     * Format power stats for CLI reply
     */
    void formatStatsReply(char* reply) const {
        sprintf(reply, "mode=%s serial=%s pwr_save=%s idle=%lu active=%lu", 
                getModeName(),
                isSerialActive() ? "yes" : "no",
                _power_saving_enabled ? "on" : "off",
                _idle_loops,
                _active_loops);
    }
    
#ifdef ESP32
    /**
     * Enter light sleep (ESP32 only)
     * Wakes on: radio DIO interrupt, timer, or serial RX
     * 
     * NOTE: Only call this when canEnterLowPower() returns true
     * and there are no pending TX operations
     * 
     * @param max_sleep_ms Maximum time to sleep (0 = wake on interrupt only)
     * @param radio_dio_pin GPIO pin for radio DIO1 interrupt wake
     * @return true if entered sleep, false if sleep was skipped
     */
    bool enterLightSleep(uint32_t max_sleep_ms, int radio_dio_pin) {
        if (!canEnterLowPower()) return false;
        
        // Configure wake sources
        esp_sleep_enable_gpio_wakeup();
        
        // Configure radio DIO pin as wake source (active high)
        if (radio_dio_pin >= 0) {
            gpio_wakeup_enable((gpio_num_t)radio_dio_pin, GPIO_INTR_HIGH_LEVEL);
        }
        
        // Configure timer wake if max_sleep_ms > 0
        if (max_sleep_ms > 0) {
            esp_sleep_enable_timer_wakeup(max_sleep_ms * 1000);  // Convert to microseconds
        }
        
        // Enter light sleep
        _sleep_count++;
        esp_light_sleep_start();
        
        // Woke up - record activity to reset idle timer
        recordActivity(ACTIVITY_RADIO_RX);  // Assume woke due to radio
        
        return true;
    }
#endif

#ifdef NRF52_PLATFORM
    /**
     * Wait for event (NRF52 only)
     * CPU halts until interrupt occurs
     * Much lower power than busy loop
     */
    void waitForEvent() {
        if (!canEnterLowPower()) return;
        
        _sleep_count++;
        __WFE();  // Wait For Event - ARM instruction
        
        // Woke up - record activity
        recordActivity(ACTIVITY_RADIO_RX);
    }
#endif
};
