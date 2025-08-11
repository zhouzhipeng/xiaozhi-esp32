# SD Card MCP Tools for Xiaozhi ESP32

## Overview

This documentation describes the SD card MCP (Model Context Protocol) tools integrated into the Xiaozhi ESP32 project for the Waveshare ESP32-S3-Touch-AMOLED-2.06 board. These tools enable AI assistants to interact with the SD card storage through standardized MCP commands.

## Hardware Configuration

The SD card interface uses SPI mode with the following GPIO connections:
- MOSI: GPIO 1
- MISO: GPIO 3  
- SCK: GPIO 2
- CS: GPIO 17

## Available MCP Tools

### 1. `self.sdcard.list_files`
Lists files and directories in the SD card.

**Parameters:**
- `path` (string, optional): Directory path to list (default: `/sdcard`)

**Returns:**
JSON object containing:
- `success`: Boolean indicating operation success
- `path`: The listed directory path
- `files`: Array of file objects with:
  - `name`: File/directory name
  - `is_directory`: Boolean indicating if it's a directory
  - `size`: File size in bytes
  - `modified`: Last modification timestamp

**Example:**
```json
{
  "name": "self.sdcard.list_files",
  "arguments": {
    "path": "/sdcard/photos"
  }
}
```

### 2. `self.sdcard.read_file`
Reads the content of a text file from the SD card.

**Parameters:**
- `filepath` (string, required): Full path of the file to read
- `max_size` (integer, optional): Maximum bytes to read (default: 4096, range: 1-102400)

**Returns:**
JSON object containing:
- `success`: Boolean indicating operation success
- `filepath`: The file path
- `size`: Total file size
- `bytes_read`: Number of bytes actually read
- `content`: File content as string

**Example:**
```json
{
  "name": "self.sdcard.read_file",
  "arguments": {
    "filepath": "/sdcard/config.txt",
    "max_size": 1024
  }
}
```

### 3. `self.sdcard.write_file`
Writes content to a file on the SD card.

**Parameters:**
- `filepath` (string, required): Full path of the file to write
- `content` (string, required): Content to write to the file
- `append` (boolean, optional): Whether to append to existing file (default: false)

**Returns:**
JSON object containing:
- `success`: Boolean indicating operation success
- `filepath`: The file path
- `bytes_written`: Number of bytes written

**Example:**
```json
{
  "name": "self.sdcard.write_file",
  "arguments": {
    "filepath": "/sdcard/notes.txt",
    "content": "This is a test note",
    "append": false
  }
}
```

### 4. `self.sdcard.delete_file`
Deletes a file or empty directory from the SD card.

**Parameters:**
- `path` (string, required): Full path of the file or directory to delete

**Returns:**
JSON object containing:
- `success`: Boolean indicating operation success
- `message`: Status message

**Example:**
```json
{
  "name": "self.sdcard.delete_file",
  "arguments": {
    "path": "/sdcard/temp.txt"
  }
}
```

### 5. `self.sdcard.get_info`
Gets SD card information including capacity and usage.

**Parameters:** None

**Returns:**
JSON object containing:
- `success`: Boolean indicating operation success
- `total_bytes`: Total capacity in bytes
- `free_bytes`: Available space in bytes
- `used_bytes`: Used space in bytes
- `total_mb`: Total capacity in MB
- `free_mb`: Available space in MB
- `used_mb`: Used space in MB
- `name`: SD card name (from CID)
- `capacity_mb`: Card capacity from CSD
- `is_mmc`: Boolean indicating if it's MMC
- `is_sdio`: Boolean indicating if it's SDIO

**Example:**
```json
{
  "name": "self.sdcard.get_info",
  "arguments": {}
}
```

### 6. `self.sdcard.display_image`
Displays a JPEG image file from the SD card on the screen.

**Parameters:**
- `filepath` (string, required): Full path of the JPEG image file

**Returns:**
JSON object containing:
- `success`: Boolean indicating operation success
- `message`: Status message
- `filepath`: The image file path
- `size`: File size in bytes

**Example:**
```json
{
  "name": "self.sdcard.display_image",
  "arguments": {
    "filepath": "/sdcard/_C3A8676.jpg"
  }
}
```

## Implementation Details

### SD Card Initialization
The SD card is automatically initialized on first access using:
- SPI3 host controller
- Initial frequency: 400 kHz (for compatibility)
- Operating frequency: Up to 5 MHz after successful initialization
- File system: FAT32 (required for cards > 32GB)

### Image Display Features
The image display tool includes:
- ESP-JPEG hardware-accelerated decoding
- Automatic scaling for large images:
  - Images > 960x1000px: Scaled to 1/4
  - Images > 480x500px: Scaled to 1/2
  - Smaller images: Original size
- RGB565 format for optimal display performance
- Scrolling support for images larger than screen
- Memory optimization using PSRAM when available

### Error Handling
All tools return structured JSON responses with:
- `success`: Boolean indicating if the operation succeeded
- `error`: Error message if operation failed (when success is false)

Common error conditions:
- SD card not initialized
- File/directory not found
- Memory allocation failure
- Permission/access errors

## Usage Examples

### List all files in root directory
```javascript
// Request
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.sdcard.list_files",
    "arguments": {
      "path": "/sdcard"
    }
  },
  "id": 1
}
```

### Display an image
```javascript
// Request
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.sdcard.display_image",
    "arguments": {
      "filepath": "/sdcard/photos/vacation.jpg"
    }
  },
  "id": 2
}
```

### Check SD card status
```javascript
// Request
{
  "jsonrpc": "2.0",
  "method": "tools/call",
  "params": {
    "name": "self.sdcard.get_info",
    "arguments": {}
  },
  "id": 3
}
```

## Notes

1. **File System**: The SD card must be formatted as FAT32. Cards larger than 32GB need special formatting tools as they come formatted as exFAT by default.

2. **Thread Safety**: MCP tool calls are executed in separate threads with configurable stack size (default: 6144 bytes).

3. **Image Formats**: Currently only JPEG images are supported for display. Other formats may be read as binary/text data.

4. **Path Format**: All paths should use forward slashes (/) and start with `/sdcard`.

5. **Performance**: File operations may take several seconds for large files. The system uses DMA for improved performance where possible.

## Integration with Xiaozhi

These MCP tools are automatically registered when the device starts up. The AI assistant can use them to:
- Browse and manage files on the SD card
- Display photos to the user
- Store and retrieve notes or configuration
- Check storage capacity
- Create backups of conversations or settings

The tools integrate seamlessly with Xiaozhi's existing MCP infrastructure, allowing natural language requests like:
- "Show me the photos on the SD card"
- "Save this conversation to a file"
- "How much space is left on the SD card?"
- "Display the image _C3A8676.jpg"