# Image Display Server Scripts

This directory contains a client-server system for remotely displaying images on a dedicated monitor. You can run the client and server on the same machine, or on different machines. The installation steps are the same for both.

## Overview

- **`image-display.py`** - Server that listens for commands and displays images in fullscreen
- **`send-image.py`** - Client that sends commands to the server
- **`image-display-config.yaml.template`** - Configuration template for the server

## Installation

### Prerequisites
- Python 3.7 or later

### Step 1: Install Python

#### Windows

1. **Open PowerShell:**
   - Press `Win + X` and select **Windows PowerShell** or **Terminal**
   - Or search for "PowerShell" in the Start menu
   - (Admin privileges are not required - winget will prompt when needed)

2. **Install Python using winget:**
   ```powershell
   winget install Python.Python.3.12
   ```
   Winget will automatically prompt for admin privileges if needed.

3. **Restart PowerShell/Terminal:**
   Close the terminal completely and open a new one. This allows the terminal to recognize the new Python installation and PATH updates.

4. **Verify installation:**
   ```powershell
   python --version
   ```

#### macOS

1. **Open Terminal:**
   - Press `Cmd + Space` and type "Terminal", then press Enter
   - Or open Applications → Utilities → Terminal

2. **Install Python using Homebrew (if not already installed):**
   ```bash
   # First install Homebrew if needed
   /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   
   # Then install Python
   brew install python@3.12
   ```

3. **Verify installation:**
   ```bash
   python3 --version
   ```

#### Linux

1. **Open Terminal:**
   - Press `Ctrl + Alt + T` (Ubuntu/Debian)
   - Or search for "Terminal" in your application menu

2. **Install Python:**

   **Ubuntu/Debian:**
   ```bash
   sudo apt update
   sudo apt install python3 python3-venv python3-pip
   ```

   **Fedora/RHEL:**
   ```bash
   sudo dnf install python3 python3-venv python3-pip
   ```

3. **Verify installation:**
   ```bash
   python3 --version
   ```

### Step 2: Navigate to the Scripts Directory

In your terminal, navigate to the scripts directory:

**Windows (PowerShell):**
```powershell
cd "C:\yourpathhere\BioSlicer\scripts"
```

**macOS/Linux:**
```bash
cd /yourpathhere/BioSlicer/scripts
```

### Step 3: Create and Activate a Virtual Environment

1. **Create virtual environment:**

   On Windows:
   ```powershell
   python -m venv venv
   ```

   On macOS/Linux:
   ```bash
   python3 -m venv venv
   ```

2. **Activate virtual environment:**

   On Windows (PowerShell):
   ```powershell
   .\venv\Scripts\Activate.ps1
   ```

   On macOS/Linux:
   ```bash
   source venv/bin/activate
   ```

   You should see `(venv)` appear at the beginning of your terminal prompt when activated.

### Step 4: Install Dependencies

With the virtual environment activated, run:
```bash
pip install -r requirements.txt
```

### Step 5: Create Configuration File

```bash
cp image-display-config.yaml.template image-display-config.yaml
```

### Step 6: Customize Configuration

Edit `image-display-config.yaml` with your preferred text editor (see Configuration section below)

## Configuration Guide

Create `image-display-config.yaml` from the template and configure the following:

### Network Settings
```yaml
host: '0.0.0.0'   # Listen on all interfaces (or specify a specific IP)
port: 5555        # TCP port number for the server
```

### Finding Your Monitor Configuration

To see available monitors and their settings, run:
```bash
python image-display.py --enum-monitors
```

This will output something like:
```
Available Monitors: index: widthxheight@(x,y)
Monitor 0: 1920x1080@(0,0)
Monitor 1: 2560x1440@(1920,0)
```

### Monitor Selection

Choose one of the following options to select which monitor to display on:

#### Option 1: By Index (Simplest)
```yaml
monitor_index: 0  # Use the first monitor, 1 for second, etc.
```

#### Option 2: By Size
```yaml
monitor_size: [1920, 1080]  # Width and height in pixels
```

#### Option 3: By Position
```yaml
monitor_position: [0, 0]  # X and Y coordinates
```

#### Option 4: By Size and Position
```yaml
monitor_size: [1920, 1080]
monitor_position: [0, 0]
```

## Usage

### Starting the Server

```bash
python image-display.py
```

The server will:
- Load the configuration from `image-display-config.yaml`
- Start listening on the configured host and port
- Display a fullscreen image window on the selected monitor
- Accept incoming commands via TCP

**To stop the server:** Press `ESC` on the image display window.

### Sending Commands

In a separate terminal/window, use `send-image.py` to send commands to the running server.

#### Display a Local Image
```bash
python send-image.py localhost 5555 '{"type":"DISPLAY_IMAGE","path":"C:\Users\yourusername\Pictures\photo.png"}'
```

#### Display an Image from URL
```bash
python send-image.py localhost 5555 '{"type":"DISPLAY_IMAGE","path":"https://picsum.photos/1920/1080"}'
```

#### Rotate Image
Add the `rotation` parameter (0, 90, 180, 270):
```bash
python send-image.py localhost 5555 '{"type":"DISPLAY_IMAGE","path":"photo.png","rotation":180}'
```

#### Clear the Display
```bash
python send-image.py localhost 5555 '{"type":"CLEAR"}'
```

**Command Types:**
- `DISPLAY_IMAGE` - Display an image (local path or URL)
- `CLEAR` - Clear the display

**Fields:**
- `type` (required) - Command type
- `path` (required for DISPLAY_IMAGE) - Local file path or HTTP(S) URL
- `rotation` (optional) - Rotation in degrees: 0, 90, 180, 270

## Troubleshooting

### Server Won't Start
- Check that `image-display-config.yaml` exists and is properly formatted
- Verify the port is not already in use
- Try a different port number

### Monitor Not Found
- Run `python image-display.py --enum-monitors` to list available monitors
- Update the configuration with the correct monitor_index, monitor_size, or monitor_position

### Connection Refused
- Ensure the server is running before sending commands
- Check the host and port match the server configuration
- Verify firewall settings allow the connection

### Image Not Displaying
- Check the file path is correct and the file exists
- For URLs, ensure internet connectivity
- Verify the image format is supported (PNG, JPG, GIF, BMP, etc.)