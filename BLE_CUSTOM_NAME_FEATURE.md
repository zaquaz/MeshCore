# Bluetooth Custom Name Feature

## Overview

This document describes the implementation of a custom Bluetooth device name feature for MeshCore devices. This feature allows users to set a custom Bluetooth Low Energy (BLE) advertisement name, separate from their user identity (node_name), to enhance privacy and prevent stalking through Bluetooth device discovery.

## Problem Statement

Previously, MeshCore devices advertised their user identity (node_name) as the Bluetooth device name. This posed a privacy and security risk because:
1. Anyone with Bluetooth scanning capabilities could identify the device owner
2. The device could be easily tracked by observing BLE advertisements over time
3. No separation existed between the user's mesh identity and their BLE advertisement name

## Solution Design

### Data Structure Changes

A new field has been added to the `NodePrefs` structure:

```cpp
char ble_custom_name[32+16];  // custom Bluetooth device name (if empty, defaults to "Meshcore - <StandardID>")
```

**Size**: 48 bytes (32+16), matching the maximum possible length of the default format (BLE_NAME_PREFIX + node_name)

**Storage Offset**: 170 bytes into the preferences file (CommonCLI), 85 bytes (companion_radio)

### Bluetooth Name Generation Logic

The `getBluetoothName()` method implements the following logic:

1. **If `ble_custom_name` is set (non-empty)**:
   - Use the custom name as-is
   - Example: If user sets "MyDevice", BLE name = "MyDevice"

2. **If `ble_custom_name` is empty (default)**:
   - Generate default name: `BLE_NAME_PREFIX` + "-" + `node_name`
   - `BLE_NAME_PREFIX` defaults to "Meshcore" (can be overridden via compile-time define)
   - If `node_name` is empty or set to default "NONAME", uses the first 4 bytes of the pubkey in hex instead
   - Example: If prefix is "Meshcore" and pubkey starts with `A1B2C3D4...`, BLE name = "Meshcore-A1B2C3D4"
   - Example: If prefix is "Meshcore" and node_name is "Alice", BLE name = "Meshcore-Alice"

### Implementation Details

#### Core Components Modified

1. **CommonCLI.h/cpp**:
   - Added `ble_custom_name[32]` field to `NodePrefs` struct
   - Updated `loadPrefsInt()` to read the new field at byte offset 170
   - Updated `savePrefs()` to write the new field at byte offset 170
   - Implemented `getBluetoothName(char* buffer)` method with the above logic
   - Added CLI commands for managing the custom name

2. **examples/companion_radio/MyMesh.h/cpp**:
   - Added `getBluetoothName(char* buffer)` method
   - Mirrors the logic from CommonCLI for consistency
   - Used during BLE interface initialization

3. **examples/companion_radio/main.cpp**:
   - Updated both NRF52 and ESP32 initialization paths to use the new `getBluetoothName()` method
   - Increased buffer size from `32+16` to `48` to accommodate the full prefix + node_name combination
   - Changed from: `sprintf(dev_name, "%s%s", BLE_NAME_PREFIX, the_mesh.getNodeName())`
   - Changed to: `the_mesh.getBluetoothName(dev_name)` (which implements the fallback logic)

### CLI Commands

Two new CLI commands have been added:

#### Get Current BLE Name
```
> ble_name
> Meshcore - A1B2C3
```

#### Set Custom BLE Name
```
> ble_name MyCustomDevice
> ok - BLE name set to: MyCustomDevice
```

#### Clear Custom Name (use default)
```
> ble_name 
> ok - using default BLE name
```

**Constraints**:
- Maximum 47 characters (32+16-1 for null terminator, matching default name format capacity)
- Empty string clears the custom name
- No special validation of characters (BLE allows most printable characters)

### Binary Protocol Command (Mobile App)

For mobile/web applications communicating with the companion_radio firmware:

#### CMD_SET_BLE_NAME (0x39 / 57)

**Request Format**:
| Byte | Description |
|------|-------------|
| 0    | Command ID: `0x39` (57) |
| 1-47 | Custom BLE name (optional, 0-47 bytes) |

**Response**: `RESP_CODE_OK` (0x00) on success

**Examples**:
- Set custom name "MyDevice": `[0x39, 0x4D, 0x79, 0x44, 0x65, 0x76, 0x69, 0x63, 0x65]` ("MyDevice" in ASCII)
- Clear custom name: `[0x39]` (command byte only, no payload)

**Notes**:
- Maximum 47 characters (truncated if longer)
- Empty payload clears the custom name (reverts to default naming)
- Requires BLE stack restart to update the advertised name
- Changes persist across reboots

## Backward Compatibility

### File Format
The preferences file format has been extended to include the new 32-byte field. Old preferences files will:
- Continue to load correctly (the new field will be uninitialized/zero)
- Be migrated to the new format when saved
- The new field will default to empty string (zero bytes)

### Migration Path
When an old preferences file is loaded:
1. All existing fields load correctly
2. The new `ble_custom_name[32]` field is uninitialized (contains garbage)
3. On first save, the field is properly initialized
4. The device will use the original BLE name format (prefix + node_name) since custom_name will be empty

## Companion BLE Firmware Feasibility Analysis

### Architecture Overview

The MeshCore project includes a companion application (`examples/companion_radio`) that:
- Runs on NRF52 or ESP32 microcontrollers
- Provides a BLE interface to mesh network
- Communicates with mobile/web applications (via BLE UART or similar)
- Maintains its own preferences and configuration

### Implementation Status for Companion Firmware

#### ✅ Already Implemented
- Custom BLE name field in `NodePrefs` structure
- Loading/saving of custom BLE name in persistent preferences
- `getBluetoothName()` method in `MyMesh` class
- Updated BLE interface initialization to use custom names
- CLI commands for getting/setting custom names

#### 📝 Additional Considerations

The companion BLE firmware implementation is straightforward because:

1. **Single BLE Advertisement Name**: The companion firmware only exposes ONE BLE advertisement name (for the device as a whole), not per-user or per-contact, so no complex name management is needed

2. **Direct Integration**: The custom name is directly integrated into the BLE interface initialization:
   ```cpp
   char dev_name[32];
   the_mesh.getBluetoothName(dev_name);
   serial_interface.begin(dev_name, the_mesh.getBLEPin());
   ```

3. **Mobile App Binary Command**: A new binary command has been added for mobile apps to set the custom BLE name:
   - **Command**: `CMD_SET_BLE_NAME` (0x39 / 57)
   - **Payload**: 0-31 bytes of custom name (empty payload clears the name)
   - **Response**: `RESP_CODE_OK` on success
   - **Effect**: Sets the custom BLE name and saves preferences; BLE stack restart required to update advertised name

4. **Mobile App Awareness**: Mobile/web applications that communicate with the companion firmware will:
   - See the custom BLE name during device scanning
   - Not need any special modifications (the name appears in standard BLE scanning)
   - Can set the custom name using `CMD_SET_BLE_NAME` (0x39 / 57) binary command
   - Can optionally display the device's Meshcore ID separately if needed

4. **No Recompilation Needed**: Users can change the BLE name via CLI without recompiling firmware

5. **Runtime Updates**: The BLE name can be changed at runtime via CLI commands, though the BLE stack needs to be restarted to update the advertised name (handled by `serial_interface.disable()` followed by `serial_interface.enable()`)

### Security Properties

- **Privacy Control**: Users have explicit control over what name is advertised:
  - Default: BLE name matches the user's configured mesh identity (node_name), same as before
  - Custom: Users can set a completely different name that doesn't reveal their mesh identity
  - This addresses the stalking concern by allowing users to obscure their identity if desired

- **Backward Compatible**: Existing deployments continue to work without any change - devices advertise `BLE_NAME_PREFIX + node_name` by default

- **Opt-in Privacy**: Users who are concerned about BLE-based stalking can set a generic custom name that doesn't reveal their identity

- **Network Activity Still Visible**: Note that this only controls the BLE advertisement name; BLE scanning can still detect radio activity and BLE devices nearby. This is a general BLE limitation, not specific to this feature.

### Compatibility Notes

- NRF52 Boards: Full support (uses Adafruit Bluefruit library with standard BLE GAP naming)
- ESP32 Boards: Full support (uses Arduino ESP32 BLE library with standard BLE naming)
- STM32 Boards: Would need to implement their own SerialBLEInterface if needed
- RP2040 Boards: Would need custom BLE implementation

## Testing Recommendations

### Unit Testing
1. Verify `getBluetoothName()` returns custom name when set
2. Verify `getBluetoothName()` returns default format when empty
3. Verify preferences save/load cycle preserves custom name
4. Verify max length (31 chars) is enforced

### Integration Testing
1. Set custom BLE name via CLI
2. Scan with phone/tablet to verify new name appears
3. Clear custom name and verify default name appears
4. Restart device and verify name persists

### Real-World Testing
1. Test with actual BLE scanner apps (iOS/Android)
2. Verify name changes don't require firmware recompilation
3. Test edge cases (special characters, whitespace, unicode)

## Future Enhancements

Possible future improvements:
1. **Dynamic Name Prefixes**: Allow customization of the default prefix (currently "Meshcore - ")
2. **Context-Aware Names**: Different names for different connection contexts
3. **Name Expiry**: Temporary names that reset after a time period
4. **QR Code Support**: Encode device info in BLE advertisement (requires raw AD manipulation)
5. **Per-Device Nicknames**: Mesh nodes could have optional nicknames displayed by apps

## Summary

This custom Bluetooth name feature provides:
- ✅ Privacy protection by allowing users to hide their mesh identity from BLE scanners
- ✅ Sensible defaults that prevent accidental information leakage
- ✅ Easy runtime configuration via CLI
- ✅ Full backward compatibility with existing deployments
- ✅ Complete implementation for companion BLE firmware
- ✅ No changes required to mobile/web applications (transparent to clients)
