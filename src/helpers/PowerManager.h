#pragma once

#include <Arduino.h>

// ============================================================================
// Platform Includes
// ============================================================================

#ifdef NRF52_PLATFORM
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
#endif

#ifdef ESP32
  #include <esp_sleep.h>
  #include <driver/gpio.h>
  #include <driver/rtc_io.h>
  #include <WiFi.h>
#endif

// ============================================================================
// Configuration
// ============================================================================

namespace PowerConfig {
    // Serial activity timeout - assume connected if recent activity
    constexpr uint32_t SERIAL_ACTIVITY_TIMEOUT_MS = 30000;  // 30 seconds
    
    // Power mode transition timeouts (after last activity)
    constexpr uint32_t IDLE_TIMEOUT_MS = 50;       // -> IDLE mode
    constexpr uint32_t LOW_POWER_TIMEOUT_MS = 3000; // -> LOW_POWER mode
    
    // Maximum sleep duration between timer wakes
    // Longer = more power savings, but millis() drifts slightly
    constexpr uint32_t MAX_SLEEP_DURATION_MS = 60000;  // 60 seconds
    
    // Loop delays for each power mode (when NOT entering light sleep)
    constexpr uint8_t ACTIVE_LOOP_DELAY_MS = 0;
    constexpr uint8_t IDLE_LOOP_DELAY_MS = 10;
    constexpr uint8_t LOW_POWER_LOOP_DELAY_MS = 30;
    
    // CPU frequency settings (ESP32 only)
    constexpr uint16_t CPU_FREQ_LOW_POWER = 80;   // MHz - idle/low power
    constexpr uint16_t CPU_FREQ_ACTIVE = 160;     // MHz - normal operation
    constexpr uint16_t CPU_FREQ_BOOST = 240;      // MHz - crypto/OTA
}

// ============================================================================
// Type Definitions
// ============================================================================

// Power modes
enum PowerMode : uint8_t {
    POWER_MODE_ACTIVE = 0,    // Full speed, no delays
    POWER_MODE_IDLE = 1,      // Short delays in loop
    POWER_MODE_LOW_POWER = 2  // Longer delays, optional light sleep
};

// Activity types that affect power state (bitmask)
enum ActivityType : uint8_t {
    ACTIVITY_SERIAL_RX  = (1 << 0),
    ACTIVITY_SERIAL_TX  = (1 << 1),
    ACTIVITY_RADIO_RX   = (1 << 2),
    ACTIVITY_RADIO_TX   = (1 << 3),
    ACTIVITY_USER_INPUT = (1 << 4),
    ACTIVITY_CRYPTO     = (1 << 5)
};

// Callback type for checking pending outbound packets
typedef bool (*HasPendingOutboundFn)();

// ============================================================================
// PowerManager Class
// ============================================================================

/**
 * PowerManager - Centralized power management for MeshCore devices
 * 
 * Features:
 * - Detects serial/USB activity to prevent sleep when connected to host
 * - Adaptive loop delays based on activity level
 * - Dynamic CPU frequency scaling (ESP32)
 * - Light sleep with interrupt wake (ESP32)
 * - System ON sleep with SoftDevice integration (NRF52)
 * 
 * Key Methods:
 *   begin()              - Initialize (call in setup)
 *   recordActivity()     - Track activity to delay sleep
 *   updatePowerMode()    - Update mode (call at loop start)
 *   applyPowerSaving()   - Apply sleep/delay (call at loop end)
 *   
 * Configuration:
 *   setPowerSavingEnabled()           - Master enable/disable
 *   setMaxSleepDuration()             - Max light sleep time
 *   setHasPendingOutboundCallback()   - Skip sleep when TX pending
 */
class PowerManager {

// ============================================================================
// Private Members
// ============================================================================
private:
    // --- Activity Tracking ---
    unsigned long _last_serial_activity;
    unsigned long _last_radio_activity;
    unsigned long _last_any_activity;
    
    // --- Power State ---
    PowerMode _current_mode;
    bool _power_saving_enabled;
    
    // --- Serial Detection ---
    bool _serial_connected_override;
    bool _serial_check_disabled;
    
    // --- Configurable Parameters ---
    uint32_t _low_power_timeout_ms;
    uint32_t _max_sleep_duration_ms;
    HasPendingOutboundFn _has_pending_outbound;
    
    // --- CPU Scaling (ESP32) ---
    bool _cpu_scaling_enabled;
    uint16_t _current_cpu_freq;
    uint32_t _cpu_boost_until;
    
    // --- Statistics ---
    uint32_t _idle_loops;
    uint32_t _active_loops;
    uint32_t _sleep_count;
    uint32_t _sleep_skipped_tx;
    uint32_t _cpu_scale_count;

// ============================================================================
// Public Interface
// ============================================================================
public:

    // ========== Constructor ==========
    
    PowerManager() :
        _last_serial_activity(0),
        _last_radio_activity(0),
        _last_any_activity(0),
        _current_mode(POWER_MODE_ACTIVE),
        _power_saving_enabled(false),  // Safe default - must be explicitly enabled
        _serial_connected_override(false),
        _serial_check_disabled(false),
        _low_power_timeout_ms(PowerConfig::LOW_POWER_TIMEOUT_MS),
        _max_sleep_duration_ms(PowerConfig::MAX_SLEEP_DURATION_MS),
        _has_pending_outbound(nullptr),
        _cpu_scaling_enabled(true),
        _current_cpu_freq(PowerConfig::CPU_FREQ_ACTIVE),
        _cpu_boost_until(0),
        _idle_loops(0),
        _active_loops(0),
        _sleep_count(0),
        _sleep_skipped_tx(0),
        _cpu_scale_count(0)
    {}
    
    // ========== Initialization ==========
    
    /**
     * Initialize power manager. Call once in setup().
     */
    void begin() {
        unsigned long now = millis();
        _last_serial_activity = now;
        _last_radio_activity = now;
        _last_any_activity = now;
        _current_mode = POWER_MODE_ACTIVE;
        
#ifdef ESP32
        _current_cpu_freq = getCpuFrequencyMhz();
#endif
    }

    // ========== Activity Tracking ==========
    
    /**
     * Record activity of a specific type.
     * Call this whenever relevant activity occurs.
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
     * Check if serial appears to be connected (recent activity).
     * Used to prevent sleep when device is connected to a host.
     */
    bool isSerialActive() const {
        if (_serial_check_disabled) return false;
        if (_serial_connected_override) return true;
        
        unsigned long now = millis();
        return (now - _last_serial_activity) < PowerConfig::SERIAL_ACTIVITY_TIMEOUT_MS;
    }
    
    /**
     * Check if power saving should be active.
     * Returns false if serial is active or power saving is disabled.
     */
    bool canEnterLowPower() const {
        if (!_power_saving_enabled) return false;
        if (isSerialActive()) return false;
        return true;
    }

    // ========== Power Mode Management ==========
    
    /**
     * Update power mode based on activity.
     * Call this at the start of each loop iteration.
     */
    void updatePowerMode() {
        if (!_power_saving_enabled || isSerialActive()) {
            _current_mode = POWER_MODE_ACTIVE;
            updateCpuFrequency();
            return;
        }
        
        unsigned long now = millis();
        unsigned long idle_time = now - _last_any_activity;
        
        if (idle_time < PowerConfig::IDLE_TIMEOUT_MS) {
            _current_mode = POWER_MODE_ACTIVE;
        } else if (idle_time < PowerConfig::LOW_POWER_TIMEOUT_MS) {
            _current_mode = POWER_MODE_IDLE;
        } else {
            _current_mode = POWER_MODE_LOW_POWER;
        }
        
        updateCpuFrequency();
    }
    
    /**
     * Get current power mode.
     */
    PowerMode getCurrentMode() const {
        return _current_mode;
    }
    
    /**
     * Get mode name string for debugging.
     */
    const char* getModeName() const {
        switch (_current_mode) {
            case POWER_MODE_ACTIVE:    return "ACTIVE";
            case POWER_MODE_IDLE:      return "IDLE";
            case POWER_MODE_LOW_POWER: return "LOW_POWER";
            default:                   return "UNKNOWN";
        }
    }
    
    /**
     * Get the appropriate loop delay for current power mode.
     */
    uint8_t getLoopDelayMs() const {
        switch (_current_mode) {
            case POWER_MODE_IDLE:      return PowerConfig::IDLE_LOOP_DELAY_MS;
            case POWER_MODE_LOW_POWER: return PowerConfig::LOW_POWER_LOOP_DELAY_MS;
            default:                   return PowerConfig::ACTIVE_LOOP_DELAY_MS;
        }
    }

    // ========== Power Saving Actions ==========
    
    /**
     * Apply loop delay based on current power mode (conservative).
     * Call at end of main loop.
     */
    void applyLoopDelay() {
        uint8_t delay_ms = getLoopDelayMs();
        if (delay_ms > 0) {
            _idle_loops++;
            delay(delay_ms);
        } else {
            _active_loops++;
        }
    }
    
    /**
     * Apply power saving based on current mode (aggressive - uses actual sleep).
     * In LOW_POWER mode, enters CPU sleep (ESP32 light sleep / NRF52 System ON).
     * Call at end of main loop instead of applyLoopDelay() for maximum savings.
     * 
     * @param radio_dio_pin GPIO pin for radio DIO1 interrupt wake (ESP32, -1 to disable)
     * @param max_sleep_ms Maximum sleep time in ms (-1 = use configured default)
     */
    void applyPowerSaving(int radio_dio_pin = -1, int32_t max_sleep_ms = -1) {
        if (max_sleep_ms < 0) {
            max_sleep_ms = _max_sleep_duration_ms;
        }
        
        if (_current_mode == POWER_MODE_LOW_POWER && canEnterLowPower()) {
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

    // ========== ESP32 Light Sleep ==========
    
#ifdef ESP32
    /**
     * Enter light sleep (ESP32 only).
     * Wakes on: radio DIO interrupt, timer.
     * 
     * NOTE: Will NOT sleep if WiFi is active.
     * 
     * For ESP32/S2/S3: Uses ext0 wakeup with RTC GPIO
     * For ESP32-C3/C6: Uses GPIO wakeup (no ext0 support)
     * 
     * @param max_sleep_ms Maximum time to sleep (0 = interrupt only)
     * @param radio_dio_pin GPIO pin for radio DIO1 interrupt
     * @return true if entered sleep, false if skipped
     */
    bool enterLightSleep(uint32_t max_sleep_ms, int radio_dio_pin) {
        // Don't sleep if packets waiting to transmit
        if (_has_pending_outbound && _has_pending_outbound()) {
            _sleep_skipped_tx++;
            return false;
        }
        
        if (!canEnterLowPower()) return false;
        
        // Don't sleep if WiFi is active
        if (WiFi.getMode() != WIFI_MODE_NULL) return false;
        
        // Clear previous wake sources
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        
        // Configure radio DIO pin as wake source
        if (radio_dio_pin >= 0) {
            configureWakePin(radio_dio_pin);
        }
        
        // Configure timer wake
        if (max_sleep_ms > 0) {
            esp_sleep_enable_timer_wakeup(max_sleep_ms * 1000ULL);
        }
        
        // Enter light sleep
        _sleep_count++;
        esp_light_sleep_start();
        
        // Restore GPIO after wakeup
        restoreWakePin(radio_dio_pin);
        
        // Record activity to reset idle timer
        recordActivity(ACTIVITY_RADIO_RX);
        
        return true;
    }

private:
    /**
     * Configure wake pin for light sleep (ESP32 internal).
     */
    void configureWakePin(int pin) {
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
        // ESP32-C3/C6: Use GPIO wakeup (no ext0)
        gpio_set_direction((gpio_num_t)pin, GPIO_MODE_INPUT);
        gpio_pulldown_en((gpio_num_t)pin);
        gpio_pullup_dis((gpio_num_t)pin);
        if (gpio_wakeup_enable((gpio_num_t)pin, GPIO_INTR_HIGH_LEVEL) == ESP_OK) {
            esp_sleep_enable_gpio_wakeup();
        }
#else
        // ESP32/S2/S3: Use ext0 with RTC GPIO (more reliable for short pulses)
        rtc_gpio_init((gpio_num_t)pin);
        rtc_gpio_set_direction((gpio_num_t)pin, RTC_GPIO_MODE_INPUT_ONLY);
        rtc_gpio_pulldown_en((gpio_num_t)pin);
        rtc_gpio_pullup_dis((gpio_num_t)pin);
        esp_sleep_enable_ext0_wakeup((gpio_num_t)pin, 1);  // Wake on HIGH
        //esp_sleep_enable_ext1_wakeup((1ULL << radio_dio_pin), ESP_EXT1_WAKEUP_ANY_HIGH);
        //esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
        //esp_sleep_enable_ext1_wakeup((1L << P_LORA_DIO_1), ESP_EXT1_WAKEUP_ANY_HIGH);
#endif
    }
    
    /**
     * Restore wake pin after light sleep (ESP32 internal).
     */
    void restoreWakePin(int pin) {
#if !defined(CONFIG_IDF_TARGET_ESP32C3) && !defined(CONFIG_IDF_TARGET_ESP32C6)
        if (pin >= 0) {
            rtc_gpio_deinit((gpio_num_t)pin);
        }
#else
        (void)pin;  // Unused on C3/C6
#endif
    }

public:
#endif  // ESP32

    // ========== NRF52 System ON Sleep ==========
    
#ifdef NRF52_PLATFORM
    /**
     * Enter System ON sleep (NRF52 only).
     * CPU halts until interrupt occurs, peripherals remain active.
     * GPIO interrupts (like radio DIO1) wake the CPU immediately.
     * 
     * @return true if entered sleep, false if skipped
     */
    bool enterSystemOnSleep() {
        // Don't sleep if packets waiting to transmit
        if (_has_pending_outbound && _has_pending_outbound()) {
            _sleep_skipped_tx++;
            return false;
        }
        
        if (!_power_saving_enabled) return false;
        if (isSerialActive()) return false;
        
        _sleep_count++;
        
        // Check if SoftDevice is enabled
        uint8_t sd_enabled = 0;
        sd_softdevice_is_enabled(&sd_enabled);
        
        if (sd_enabled) {
            // SoftDevice active - use its wait function
            sd_app_evt_wait();
        } else {
            // No SoftDevice - use direct WFE
            __SEV();  // Set Event
            __WFE();  // Clear Event
            __WFE();  // Wait For Event
        }
        
        recordActivity(ACTIVITY_RADIO_RX);
        return true;
    }
    
    /**
     * @deprecated Use enterSystemOnSleep() instead
     */
    void waitForEvent() {
        enterSystemOnSleep();
    }
#endif  // NRF52_PLATFORM

    // ========== CPU Frequency Scaling ==========
    
#ifdef ESP32
    /**
     * Update CPU frequency based on current power mode.
     * Called automatically by updatePowerMode().
     */
    void updateCpuFrequency() {
        if (!_power_saving_enabled || !_cpu_scaling_enabled) {
            if (_current_cpu_freq != PowerConfig::CPU_FREQ_ACTIVE) {
                setCpuFrequencyMhz(PowerConfig::CPU_FREQ_ACTIVE);
                _current_cpu_freq = PowerConfig::CPU_FREQ_ACTIVE;
            }
            return;
        }
        
        unsigned long now = millis();
        uint16_t target_freq;
        
        // Check for temporary boost
        if (_cpu_boost_until > 0 && now < _cpu_boost_until) {
            target_freq = PowerConfig::CPU_FREQ_BOOST;
        } else {
            _cpu_boost_until = 0;
            
            // Select frequency based on power mode
            if (_current_mode == POWER_MODE_ACTIVE) {
                target_freq = PowerConfig::CPU_FREQ_ACTIVE;
            } else {
                target_freq = PowerConfig::CPU_FREQ_LOW_POWER;
            }
        }
        
        if (target_freq != _current_cpu_freq) {
            setCpuFrequencyMhz(target_freq);
            _current_cpu_freq = target_freq;
            _cpu_scale_count++;
        }
    }
    
    /**
     * Temporarily boost CPU to maximum frequency.
     * Use for crypto operations, OTA updates, or heavy processing.
     * 
     * @param duration_ms How long to maintain boost (max 30 seconds)
     */
    void boostCpu(uint32_t duration_ms = 5000) {
        if (!_cpu_scaling_enabled) return;
        
        if (duration_ms > 30000) duration_ms = 30000;
        
        _cpu_boost_until = millis() + duration_ms;
        
        if (_current_cpu_freq != PowerConfig::CPU_FREQ_BOOST) {
            setCpuFrequencyMhz(PowerConfig::CPU_FREQ_BOOST);
            _current_cpu_freq = PowerConfig::CPU_FREQ_BOOST;
            _cpu_scale_count++;
        }
    }
    
    /**
     * Get current CPU frequency in MHz.
     */
    uint16_t getCurrentCpuFreq() const {
        return _current_cpu_freq;
    }
    
#else
    // Non-ESP32 stubs
    void updateCpuFrequency() {}
    void boostCpu(uint32_t duration_ms = 5000) { (void)duration_ms; }
    uint16_t getCurrentCpuFreq() const { return 0; }
#endif

    /**
     * Enable/disable CPU frequency scaling.
     */
    void setCpuScalingEnabled(bool enabled) {
        _cpu_scaling_enabled = enabled;
#ifdef ESP32
        if (!enabled && _current_cpu_freq != PowerConfig::CPU_FREQ_ACTIVE) {
            setCpuFrequencyMhz(PowerConfig::CPU_FREQ_ACTIVE);
            _current_cpu_freq = PowerConfig::CPU_FREQ_ACTIVE;
        }
#endif
    }
    
    bool isCpuScalingEnabled() const {
        return _cpu_scaling_enabled;
    }
    
    /**
     * Lock CPU to lower frequency (aggressive power saving for repeaters).
     */
    void lockCpuLower() {
#ifdef ESP32
        _cpu_scaling_enabled = false;
        setCpuFrequencyMhz(PowerConfig::CPU_FREQ_LOW_POWER);
        _current_cpu_freq = PowerConfig::CPU_FREQ_LOW_POWER;
#endif
    }

    // ========== Configuration ==========
    
    /**
     * Enable/disable power saving.
     */
    void setPowerSavingEnabled(bool enabled) {
        _power_saving_enabled = enabled;
        if (!enabled) {
            _current_mode = POWER_MODE_ACTIVE;
#ifdef ESP32
            if (_current_cpu_freq != PowerConfig::CPU_FREQ_ACTIVE) {
                setCpuFrequencyMhz(PowerConfig::CPU_FREQ_ACTIVE);
                _current_cpu_freq = PowerConfig::CPU_FREQ_ACTIVE;
            }
#endif
        }
    }
    
    bool isPowerSavingEnabled() const {
        return _power_saving_enabled;
    }
    
    /**
     * Force serial connected state (for debugging).
     */
    void setSerialConnectedOverride(bool connected) {
        _serial_connected_override = connected;
    }
    
    /**
     * Disable/enable serial activity check.
     * When disabled, device can sleep even if serial appears connected.
     */
    void setSerialCheckDisabled(bool disabled) {
        _serial_check_disabled = disabled;
    }
    
    bool isSerialCheckDisabled() const {
        return _serial_check_disabled;
    }
    
    /**
     * Set LOW_POWER mode timeout (ms after last activity).
     */
    void setLowPowerTimeout(uint32_t timeout_ms) {
        _low_power_timeout_ms = timeout_ms;
    }
    
    uint32_t getLowPowerTimeout() const {
        return _low_power_timeout_ms;
    }
    
    /**
     * Set maximum sleep duration before timer wake (ms).
     */
    void setMaxSleepDuration(uint32_t duration_ms) {
        _max_sleep_duration_ms = duration_ms;
    }
    
    uint32_t getMaxSleepDuration() const {
        return _max_sleep_duration_ms;
    }
    
    /**
     * Set callback to check for pending outbound packets.
     */
    void setHasPendingOutboundCallback(HasPendingOutboundFn callback) {
        _has_pending_outbound = callback;
    }

    // ========== Statistics ==========
    
    uint32_t getIdleLoops() const { return _idle_loops; }
    uint32_t getActiveLoops() const { return _active_loops; }
    uint32_t getSleepCount() const { return _sleep_count; }
    uint32_t getSleepSkippedTx() const { return _sleep_skipped_tx; }
    uint32_t getCpuScaleCount() const { return _cpu_scale_count; }
    
    /**
     * Reset statistics.
     */
    void resetStats() {
        _idle_loops = 0;
        _active_loops = 0;
        _sleep_count = 0;
        _sleep_skipped_tx = 0;
        _cpu_scale_count = 0;
    }
    
    /**
     * Format power stats for CLI reply.
     */
    void formatStatsReply(char* reply) const {
#ifdef ESP32
        sprintf(reply, 
            "mode=%s cpu=%uMHz serial=%s pwr=%s idle=%lu active=%lu scales=%lu sleeps=%lu skip_tx=%lu", 
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
        sprintf(reply, 
            "mode=%s serial=%s pwr=%s idle=%lu active=%lu sleeps=%lu skip_tx=%lu", 
            getModeName(),
            isSerialActive() ? "yes" : "no",
            _power_saving_enabled ? "on" : "off",
            _idle_loops,
            _active_loops,
            _sleep_count,
            _sleep_skipped_tx);
#endif
    }
};

// ============================================================================
// PowerManagerCallbacks - Mixin for CommonCLICallbacks
// ============================================================================

/**
 * Inherit from this class to get power management callback implementations.
 * Reduces boilerplate in MyMesh.h and similar files.
 * 
 * Usage:
 *   class MyMesh : public mesh::Mesh, 
 *                  public CommonCLICallbacks, 
 *                  public PowerManagerCallbacks {
 *     // Power methods inherited from PowerManagerCallbacks
 *   };
 */
class PowerManagerCallbacks {
protected:
    PowerManager* _power_manager = nullptr;

public:
    /**
     * Set the power manager instance. Call in setup().
     */
    void setPowerManager(PowerManager* pm) { 
        _power_manager = pm; 
    }
    
    /**
     * Get the power manager instance.
     */
    PowerManager* getPowerManager() const {
        return _power_manager;
    }

    // --- CommonCLICallbacks implementations ---
    
    void setPowerSavingEnabled(bool enable) {
        if (_power_manager) _power_manager->setPowerSavingEnabled(enable);
    }

    bool getPowerSavingEnabled() {
        return _power_manager ? _power_manager->isPowerSavingEnabled() : false;
    }

    void formatPowerStatsReply(char* reply) {
        if (_power_manager) {
            _power_manager->formatStatsReply(reply);
        } else {
            strcpy(reply, "> power management not available");
        }
    }

    void resetPowerStats() {
        if (_power_manager) _power_manager->resetStats();
    }
    
    void setSerialCheckDisabled(bool disabled) {
        if (_power_manager) _power_manager->setSerialCheckDisabled(disabled);
    }
    
    void lockCpuLower() {
        if (_power_manager) _power_manager->lockCpuLower();
    }
    
    void setCpuScalingEnabled(bool enabled) {
        if (_power_manager) _power_manager->setCpuScalingEnabled(enabled);
    }
};
