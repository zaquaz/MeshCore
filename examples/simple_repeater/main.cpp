#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>
#include <helpers/PowerManager.h>

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(display);
#endif

// Power manager for optimizing battery life
// Tracks serial activity to avoid sleeping when connected to host (e.g., Raspberry Pi)
static PowerManager power_manager;

StdRNG fast_rng;
SimpleMeshTables tables;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];

void setup() {
  Serial.begin(115200);
  delay(1000);

  board.begin();

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

  if (!radio_init()) {
    halt();
  }

  fast_rng.begin(radio_get_rng_seed());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_new_identity();   // create new random identity
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_new_identity(); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;

  sensors.begin();

  the_mesh.begin(fs);

  // Initialize power manager and wire it up to the mesh for remote commands
  power_manager.begin();
  the_mesh.setPowerManager(&power_manager);
  
  // Restore power saving state from persistent preferences
  NodePrefs* prefs = the_mesh.getNodePrefs();
  if (prefs->power_saving_enabled) {
    power_manager.setPowerSavingEnabled(true);
    MESH_DEBUG_PRINTLN("Power saving: enabled (from prefs)");
  } else {
    MESH_DEBUG_PRINTLN("Power saving: disabled");
  }

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

  // send out initial Advertisement to the mesh
  the_mesh.sendSelfAdvertisement(16000);
}

void loop() {
  // Update power mode based on recent activity
  power_manager.updatePowerMode();

  int len = strlen(command);
  while (Serial.available() && len < sizeof(command)-1) {
    // Serial activity detected - record it to prevent sleep
    power_manager.recordActivity(ACTIVITY_SERIAL_RX);
    
    char c = Serial.read();
    if (c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r') break;
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (len > 0 && command[len - 1] == '\r') {  // received complete line
    Serial.print('\n');
    command[len - 1] = 0;  // replace newline with C string null terminator
    
    // Record TX activity (response will be sent)
    power_manager.recordActivity(ACTIVITY_SERIAL_TX);
    
    // Check for power management commands
    char reply[160];
    if (strncmp(command, "power", 5) == 0) {
      // Built-in power management command
      NodePrefs* prefs = the_mesh.getNodePrefs();
      if (strcmp(command, "power") == 0 || strcmp(command, "power status") == 0) {
        power_manager.formatStatsReply(reply);
      } else if (strcmp(command, "power on") == 0) {
        power_manager.setPowerSavingEnabled(true);
        prefs->power_saving_enabled = 1;
        the_mesh.savePrefs();
        strcpy(reply, "power saving enabled (saved)");
      } else if (strcmp(command, "power off") == 0) {
        power_manager.setPowerSavingEnabled(false);
        prefs->power_saving_enabled = 0;
        the_mesh.savePrefs();
        strcpy(reply, "power saving disabled (saved)");
      } else if (strcmp(command, "power reset") == 0) {
        power_manager.resetStats();
        strcpy(reply, "power stats reset");
      } else {
        strcpy(reply, "usage: power [on|off|status|reset]");
      }
    } else {
      the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
    }
    
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

    command[0] = 0;  // reset command buffer
  }

  the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();
  
  // Apply power-efficient delay based on activity level
  // On NRF52: Uses System ON sleep in LOW_POWER mode (wakes on radio interrupt)
  // On ESP32: Uses delay() which allows FreeRTOS idle (light sleep requires DIO pin config)
  // When serial is active (e.g., connected to Raspberry Pi), stays in ACTIVE mode
#ifdef NRF52_PLATFORM
  power_manager.applyPowerSaving();  // Uses System ON sleep on NRF52
#else
  power_manager.applyLoopDelay();    // Conservative delay-based on other platforms
#endif
}
