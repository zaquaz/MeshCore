#pragma once

#include <Arduino.h>

// NRF52 SoftDevice includes for sleep functions
#ifdef NRF52_PLATFORM
  #include <nrf_sdm.h>
  #include <nrf_soc.h>
#endif

// ESP32 sleep and GPIO includes
#ifdef ESP32
  #include <esp_sleep.h>
  #include <driver/gpio.h>
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
#define ACTIVITY_CRYPTO        (1 << 5)   // Key exchange, signature verification

// Default timeouts (milliseconds)
#define SERIAL_ACTIVITY_TIMEOUT_MS    30000   // 30 seconds - assume connected if recent serial activity
#define IDLE_TIMEOUT_MS               10000   // 10 seconds before entering idle mode
#define LOW_POWER_TIMEOUT_MS          60000   // 60 seconds before entering low power mode

// Loop delays for each power mode
#define ACTIVE_LOOP_DELAY_MS          0       // No delay when active
#define IDLE_LOOP_DELAY_MS            5       // 5ms delay in idle
#define LOW_POWER_LOOP_DELAY_MS       20      // 20ms delay in low power

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
     * @param max_sleep_ms Maximum sleep time in ms (0 = wake on interrupt only)
     */
    void applyPowerSaving(int radio_dio_pin = -1, uint32_t max_sleep_ms = 100) {
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
        
        // Ensure DIO pin is configured as input
        if (radio_dio_pin >= 0) {
            pinMode(radio_dio_pin, INPUT);
        }
        
        // Clear all previous wake sources first
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
        
        // Configure radio DIO pin as wake source (active high)
        if (radio_dio_pin >= 0) {
            gpio_wakeup_enable((gpio_num_t)radio_dio_pin, GPIO_INTR_HIGH_LEVEL);
        }
        esp_sleep_enable_gpio_wakeup();
        
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

