#pragma once

#include <Arduino.h>

// NRF52 includes for sleep functions
#ifdef NRF52_PLATFORM
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
#endif

// ESP32 sleep and GPIO includes
#ifdef ESP32
  #include <esp_sleep.h>
  #include <driver/gpio.h>
  #include <driver/rtc_io.h>
  #include <WiFi.h>
#endif

/**
 * PowerManager - Centralized power management for MeshCore devices
 * 
 * Features:
 * - Detects serial/USB activity to prevent sleep when connected to host (e.g., Raspberry Pi)
 * - Adaptive loop delays based on activity level
 * - Dynamic CPU frequency scaling (ESP32) - 80MHz idle, 160MHz active
 * - NRF52 System ON sleep with SoftDevice integration
 * - Optional light sleep support (ESP32) with interrupt wake
 * - Safe defaults - power saving only when safe to do so
 * 
 * Configurable Parameters (at top of file):
 * - IDLE_TIMEOUT_MS: Time after last activity before entering IDLE mode
 * - LOW_POWER_TIMEOUT_MS: Time after last activity before entering LOW_POWER mode
 * - MAX_SLEEP_DURATION_MS: Maximum light sleep duration (for time accuracy)
 */

// ==================== CONFIGURABLE TIMEOUTS ====================
// Adjust these values to tune power saving behavior
//
// For stable low power on Heltec V4:
// - Use longer sleep with radio DIO1 wake (ext0 wakeup)
// - Radio stays in RX mode during light sleep
// - DIO1 goes HIGH on packet receive, waking the CPU instantly
// - Timer wake ensures millis()/RTC stays accurate

#define SERIAL_ACTIVITY_TIMEOUT_MS    30000   // 30 seconds - assume connected if recent serial activity
#define IDLE_TIMEOUT_MS               50     // 50ms after last activity -> IDLE mode
#define LOW_POWER_TIMEOUT_MS          200    // 200ms after last activity -> LOW_POWER mode

// Maximum sleep duration between timer wakes (radio wake is instant)
// Longer = more power savings, but millis() drifts slightly
// 10 seconds is a good balance for repeaters
#define MAX_SLEEP_DURATION_MS         10000   // 10 seconds max sleep

// Loop delays for each power mode (used when NOT entering light sleep)
#define ACTIVE_LOOP_DELAY_MS          0       // No delay when active
#define IDLE_LOOP_DELAY_MS            10      // Small delay in idle
#define LOW_POWER_LOOP_DELAY_MS       30      // Delay before sleep attempt

// ==================== END CONFIGURABLE SECTION ====================

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
#define ACTIVITY_CRYPTO        (1 << 5)   // Key exchange, signature verification

// CPU frequency settings (ESP32 only)
// Radio/SPI work fine at 80MHz, crypto benefits from higher speed
#define CPU_FREQ_LOW_POWER     80    // MHz - idle/low power mode
#define CPU_FREQ_ACTIVE        160   // MHz - active mode (good balance)
#define CPU_FREQ_BOOST         240   // MHz - crypto/OTA operations

// Callback type for checking if there are pending outbound packets
typedef bool (*HasPendingOutboundFn)();

class PowerManager {
private:
    unsigned long _last_serial_activity;
    unsigned long _last_radio_activity;
    unsigned long _last_any_activity;
    uint8_t _current_mode;
    bool _power_saving_enabled;
    bool _serial_connected_override;  // Force assume serial connected (for debugging)
    bool _serial_check_disabled;      // Disable serial activity check (allows sleep even with serial)
    
    // Configurable sleep parameters
    uint32_t _low_power_timeout_ms;   // Timeout before entering LOW_POWER mode
    uint32_t _max_sleep_duration_ms;  // Max sleep duration before timer wake
    
    // Callback to check for pending TX packets before sleeping
    HasPendingOutboundFn _has_pending_outbound;
    
    // CPU frequency scaling (ESP32)
    bool _cpu_scaling_enabled;
    uint16_t _current_cpu_freq;
    uint32_t _cpu_boost_until;        // millis() timestamp when boost expires
    
    // Statistics
    uint32_t _idle_loops;
    uint32_t _active_loops;
    uint32_t _sleep_count;
    uint32_t _sleep_skipped_tx;       // Times sleep was skipped due to pending TX
    uint32_t _cpu_scale_count;        // Number of CPU frequency changes
    
public:
    PowerManager() {
        _last_serial_activity = 0;
        _last_radio_activity = 0;
        _last_any_activity = 0;
        _current_mode = POWER_MODE_ACTIVE;
        _power_saving_enabled = false;  // SAFE DEFAULT - must be explicitly enabled via prefs
        _serial_connected_override = false;
        _serial_check_disabled = false;
        _low_power_timeout_ms = LOW_POWER_TIMEOUT_MS;
        _max_sleep_duration_ms = MAX_SLEEP_DURATION_MS;  // Default 1000ms (1 second) sleep duration for time accuracy
        _has_pending_outbound = nullptr;
        _cpu_scaling_enabled = true;
        _current_cpu_freq = CPU_FREQ_ACTIVE;
        _cpu_boost_until = 0;
        _idle_loops = 0;
        _active_loops = 0;
        _sleep_count = 0;
        _sleep_skipped_tx = 0;
        _cpu_scale_count = 0;
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
        
#ifdef ESP32
        // Initialize at active frequency
        _current_cpu_freq = getCpuFrequencyMhz();
#endif
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
        if (_serial_check_disabled) return false;  // Serial check disabled - allow sleep
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
            updateCpuFrequency();
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
        
        updateCpuFrequency();
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
     * Apply loop delay based on current power mode (conservative - delay only)
     * Call at end of main loop
     */
    void applyLoopDelay() {
        uint8_t delay_ms = getLoopDelayMs();
        if (delay_ms > 0) {
            _idle_loops++;
            delay(delay_ms);  // This allows FreeRTOS/RTOS to do power management
        } else {
            _active_loops++;
        }
    }
    
    /**
     * Apply power saving based on current mode (aggressive - uses actual sleep)
     * In LOW_POWER mode, this will enter actual CPU sleep (ESP32 light sleep / NRF52 System ON)
     * Call at end of main loop instead of applyLoopDelay() for maximum power savings
     * 
     * @param radio_dio_pin GPIO pin for radio DIO1 interrupt wake (ESP32 only, -1 to disable)
     * @param max_sleep_ms Maximum sleep time in ms (0 = wake on interrupt only, -1 = use member variable)
     */
    void applyPowerSaving(int radio_dio_pin = -1, int32_t max_sleep_ms = -1) {
        // Use constant default if not specified
        if (max_sleep_ms < 0) {
            max_sleep_ms = MAX_SLEEP_DURATION_MS;
        }
        
        if (_current_mode == POWER_MODE_LOW_POWER && _power_saving_enabled && !isSerialActive()) {
            // Enter actual sleep mode
#ifdef ESP32
            if (radio_dio_pin >= 0) {
                enterLightSleep(max_sleep_ms, radio_dio_pin);
                return;
            }
#endif
#ifdef NRF52_PLATFORM
            enterSystemOnSleep();
            return;
#endif
        }
        
        // Fall back to delay-based power saving
        applyLoopDelay();
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
    
    // ==================== CPU Frequency Scaling (ESP32) ====================
    
#ifdef ESP32
    /**
     * Update CPU frequency based on current power mode
     * Called automatically by updatePowerMode()
     * 
     * CPU scaling is gated behind BOTH _power_saving_enabled AND _cpu_scaling_enabled
     */
    void updateCpuFrequency() {
        // CPU scaling requires both master power saving AND cpu scaling to be enabled
        if (!_power_saving_enabled || !_cpu_scaling_enabled) {
            // Ensure we're at active frequency when power saving is disabled
            if (_current_cpu_freq != CPU_FREQ_ACTIVE) {
                setCpuFrequencyMhz(CPU_FREQ_ACTIVE);
                _current_cpu_freq = CPU_FREQ_ACTIVE;
            }
            return;
        }
        
        unsigned long now = millis();
        uint16_t target_freq;
        
        // Check if we're in a temporary boost period
        if (_cpu_boost_until > 0 && now < _cpu_boost_until) {
            target_freq = CPU_FREQ_BOOST;
        } else {
            _cpu_boost_until = 0;  // Clear expired boost
            
            // Select frequency based on power mode
            switch (_current_mode) {
                case POWER_MODE_LOW_POWER:
                    target_freq = CPU_FREQ_LOW_POWER;
                    break;
                case POWER_MODE_IDLE:
                    target_freq = CPU_FREQ_LOW_POWER;  // Also use low freq for idle
                    break;
                default:
                    target_freq = CPU_FREQ_ACTIVE;
                    break;
            }
        }
        
        // Only change if different
        if (target_freq != _current_cpu_freq) {
            setCpuFrequencyMhz(target_freq);
            _current_cpu_freq = target_freq;
            _cpu_scale_count++;
        }
    }
    
    /**
     * Temporarily boost CPU to maximum frequency
     * Use for crypto operations, OTA updates, or heavy processing
     * 
     * Note: Boost works even when power saving is enabled (it's an explicit request)
     * but respects the _cpu_scaling_enabled flag
     * 
     * @param duration_ms How long to maintain boost (max 30 seconds)
     */
    void boostCpu(uint32_t duration_ms = 5000) {
        if (!_cpu_scaling_enabled) return;
        
        // Cap at 30 seconds to prevent accidental permanent boost
        if (duration_ms > 30000) duration_ms = 30000;
        
        _cpu_boost_until = millis() + duration_ms;
        
        // Apply immediately
        if (_current_cpu_freq != CPU_FREQ_BOOST) {
            setCpuFrequencyMhz(CPU_FREQ_BOOST);
            _current_cpu_freq = CPU_FREQ_BOOST;
            _cpu_scale_count++;
        }
    }
    
    /**
     * Get current CPU frequency in MHz
     */
    uint16_t getCurrentCpuFreq() const {
        return _current_cpu_freq;
    }
    
#else
    // Non-ESP32 platforms: stub implementations
    void updateCpuFrequency() { }
    void boostCpu(uint32_t duration_ms = 5000) { (void)duration_ms; }
    uint16_t getCurrentCpuFreq() const { return 0; }
#endif

    /**
     * Enable/disable CPU frequency scaling
     */
    void setCpuScalingEnabled(bool enabled) {
        _cpu_scaling_enabled = enabled;
#ifdef ESP32
        if (!enabled) {
            // Restore to default active frequency
            setCpuFrequencyMhz(CPU_FREQ_ACTIVE);
            _current_cpu_freq = CPU_FREQ_ACTIVE;
        }
#endif
    }
    
    bool isCpuScalingEnabled() const {
        return _cpu_scaling_enabled;
    }
    
    /**
     * Lock CPU to lower frequency (most aggressive power saving for repeaters)
     * Disables frequency scaling and sets CPU to low power frequency
     * Useful for repeater-only devices that don't need higher speeds
     */
    void lockCpuLower() {
#ifdef ESP32
        _cpu_scaling_enabled = false;  // Disable scaling
        setCpuFrequencyMhz(CPU_FREQ_LOW_POWER);  // Lock at 80MHz
        _current_cpu_freq = CPU_FREQ_LOW_POWER;
#endif
    }
    
    /**
     * Enable/disable power saving
     * When disabled, CPU frequency is restored to active level
     */
    void setPowerSavingEnabled(bool enabled) {
        _power_saving_enabled = enabled;
        if (!enabled) {
            _current_mode = POWER_MODE_ACTIVE;
#ifdef ESP32
            // Restore CPU to active frequency when power saving is disabled
            if (_current_cpu_freq != CPU_FREQ_ACTIVE) {
                setCpuFrequencyMhz(CPU_FREQ_ACTIVE);
                _current_cpu_freq = CPU_FREQ_ACTIVE;
            }
#endif
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
     * Disable/enable serial activity check for power saving
     * When disabled, device can sleep even if serial appears connected
     * Useful if power adapter is detected as a serial device
     */
    void setSerialCheckDisabled(bool disabled) {
        _serial_check_disabled = disabled;
    }
    
    bool isSerialCheckDisabled() const {
        return _serial_check_disabled;
    }
    
    /**
     * Set LOW_POWER mode timeout (milliseconds after last activity)
     */
    void setLowPowerTimeout(uint32_t timeout_ms) {
        _low_power_timeout_ms = timeout_ms;
    }
    
    uint32_t getLowPowerTimeout() const {
        return _low_power_timeout_ms;
    }
    
    /**
     * Set maximum sleep duration before timer wake (milliseconds)
     */
    void setMaxSleepDuration(uint32_t duration_ms) {
        _max_sleep_duration_ms = duration_ms;
    }
    
    uint32_t getMaxSleepDuration() const {
        return _max_sleep_duration_ms;
    }
    
    /**
     * Set callback to check for pending outbound packets
     * If set, sleep will be skipped when packets are waiting to transmit
     */
    void setHasPendingOutboundCallback(HasPendingOutboundFn callback) {
        _has_pending_outbound = callback;
    }
    
    /**
     * Get statistics
     */
    uint32_t getIdleLoops() const { return _idle_loops; }
    uint32_t getActiveLoops() const { return _active_loops; }
    uint32_t getSleepCount() const { return _sleep_count; }
    uint32_t getSleepSkippedTx() const { return _sleep_skipped_tx; }
    uint32_t getCpuScaleCount() const { return _cpu_scale_count; }
    
    /**
     * Reset statistics
     */
    void resetStats() {
        _idle_loops = 0;
        _active_loops = 0;
        _sleep_count = 0;
        _sleep_skipped_tx = 0;
        _cpu_scale_count = 0;
    }
    
    /**
     * Format power stats for CLI reply
     */
    void formatStatsReply(char* reply) const {
#ifdef ESP32
        sprintf(reply, "mode=%s cpu=%uMHz serial=%s pwr=%s idle=%lu active=%lu scales=%lu sleeps=%lu skip_tx=%lu", 
                getModeName(),
                _current_cpu_freq,
                isSerialActive() ? "yes" : "no",
                _power_saving_enabled ? "on" : "off",
                _idle_loops,
                _active_loops,
                _cpu_scale_count,
                _sleep_count,
                _sleep_skipped_tx);
#else
        sprintf(reply, "mode=%s serial=%s pwr=%s idle=%lu active=%lu sleeps=%lu skip_tx=%lu", 
                getModeName(),
                isSerialActive() ? "yes" : "no",
                _power_saving_enabled ? "on" : "off",
                _idle_loops,
                _active_loops,
                _sleep_count,
                _sleep_skipped_tx);
#endif
    }
    
#ifdef ESP32
    /**
     * Enter light sleep (ESP32 only)
     * Wakes on: radio DIO interrupt, timer
     * 
     * NOTE: Will NOT sleep if WiFi is active (e.g., OTA mode, WiFi companion radio)
     * 
     * For ESP32/S2/S3: Uses ext0 wakeup with RTC GPIO
     * For ESP32-C3/C6: Uses GPIO wakeup (no RTC GPIO support)
     * 
     * @param max_sleep_ms Maximum time to sleep (0 = wake on interrupt only)
     * @param radio_dio_pin GPIO pin for radio DIO1 interrupt wake
     * @return true if entered sleep, false if sleep was skipped
     */
    bool enterLightSleep(uint32_t max_sleep_ms, int radio_dio_pin) {
        // Don't sleep if there are packets waiting to transmit
        if (_has_pending_outbound && _has_pending_outbound()) {
            _sleep_skipped_tx++;
            return false;
        }
        
        if (!canEnterLowPower()) return false;
        
        // Don't sleep if WiFi is active (OTA, companion radio WiFi mode, etc.)
        if (WiFi.getMode() != WIFI_MODE_NULL) return false;
        
        // Clear all previous wake sources first
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        
        // Configure radio DIO pin as wake source
        if (radio_dio_pin >= 0) {
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
            // ESP32-C3/C6: Use GPIO wakeup (ext0 not available on these chips)
            gpio_set_direction((gpio_num_t)radio_dio_pin, GPIO_MODE_INPUT);
            gpio_pulldown_en((gpio_num_t)radio_dio_pin);
            gpio_pullup_dis((gpio_num_t)radio_dio_pin);
            if (gpio_wakeup_enable((gpio_num_t)radio_dio_pin, GPIO_INTR_HIGH_LEVEL) == ESP_OK) {
                esp_sleep_enable_gpio_wakeup();
            }
#else
            // ESP32/S2/S3: Use ext0 with proper RTC GPIO init
            // ext0 is more reliable for handling SX1262 DIO1 behavior
            
            // Initialize RTC GPIO function for this pin
            rtc_gpio_init((gpio_num_t)radio_dio_pin);
            rtc_gpio_set_direction((gpio_num_t)radio_dio_pin, RTC_GPIO_MODE_INPUT_ONLY);
            rtc_gpio_pulldown_en((gpio_num_t)radio_dio_pin);
            rtc_gpio_pullup_dis((gpio_num_t)radio_dio_pin);
            
            // Enable ext0 wakeup on HIGH level (radio DIO1 goes HIGH on packet RX)
            esp_sleep_enable_ext0_wakeup((gpio_num_t)radio_dio_pin, 1);
#endif
        }
        
        // Configure timer wake if max_sleep_ms > 0
        if (max_sleep_ms > 0) {
            esp_sleep_enable_timer_wakeup(max_sleep_ms * 1000ULL);  // Convert to microseconds
        }
        
        // Enter light sleep
        _sleep_count++;
        esp_light_sleep_start();
        
        // Restore normal GPIO function after wakeup (ESP32/S2/S3 only)
#if !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32C6)
        if (radio_dio_pin >= 0) {
            // Deinit RTC GPIO to restore normal digital GPIO function
            // This is important for the radio driver to work correctly
            rtc_gpio_deinit((gpio_num_t)radio_dio_pin);
        }
#endif
        
        // Record activity to reset idle timer
        recordActivity(ACTIVITY_RADIO_RX);  // Assume woke due to radio
        
        return true;
    }
#endif

#ifdef NRF52_PLATFORM
    /**
     * Enter System ON sleep (NRF52 only)
     * CPU halts until interrupt occurs, peripherals remain active
     * Lower power than busy loop as CPU is halted
     * 
     * The function uses sd_app_evt_wait() if SoftDevice is enabled,
     * otherwise falls back to WFE instruction.
     * 
     * GPIO interrupts (like radio DIO1) will wake the CPU immediately.
     * millis() continues to run via RTC peripheral.
     * 
     * @return true if entered sleep, false if skipped (power saving disabled or serial active)
     */
    bool enterSystemOnSleep() {
        // Don't sleep if there are packets waiting to transmit
        if (_has_pending_outbound && _has_pending_outbound()) {
            _sleep_skipped_tx++;
            return false;
        }
        
        // Check if power saving allows sleep
        if (!_power_saving_enabled) return false;
        if (isSerialActive()) return false;
        
        _sleep_count++;
        
        // Check if SoftDevice is enabled
        uint8_t sd_enabled = 0;
        sd_softdevice_is_enabled(&sd_enabled);
        
        if (sd_enabled) {
            // SoftDevice is active - use its wait function
            // This properly integrates with BLE stack timing
            sd_app_evt_wait();
        } else {
            // No SoftDevice - use direct WFE
            // The SEV/WFE pattern clears any pending event flag first,
            // ensuring we actually sleep and don't just fall through
            __SEV();  // Set Event (ensures event register is set)
            __WFE();  // Clear Event (clears the event we just set)
            __WFE();  // Wait For Event (now actually sleeps)
        }
        
        // Woke up - record activity
        recordActivity(ACTIVITY_RADIO_RX);
        return true;
    }
    
    /**
     * Legacy function - calls enterSystemOnSleep()
     * @deprecated Use enterSystemOnSleep() instead
     */
    void waitForEvent() {
        enterSystemOnSleep();
    }
#endif
};

