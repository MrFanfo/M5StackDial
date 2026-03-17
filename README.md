# M5StackDial PlatformIO Project

This project is set up for a simple workflow:

1. Keep source code on GitHub.
2. Pull latest code on your computer.
3. Build firmware locally in VS Code with PlatformIO.
4. Flash first time by USB.
5. Flash next updates over Wi-Fi (OTA).

If you are new to this: no problem—follow this guide exactly, step by step.

---

## 0) What you need before starting

- A computer with **VS Code** installed.
- The **PlatformIO IDE extension** in VS Code.
- **Git** installed.
- Your M5Dial connected to your Wi-Fi.
- This repository cloned locally.

### Install VS Code + PlatformIO extension

1. Open VS Code.
2. Click Extensions (left sidebar, square icon).
3. Search for **PlatformIO IDE**.
4. Click **Install**.
5. Restart VS Code if prompted.

### Install Git (if needed)

- Windows: install “Git for Windows”.
- macOS: `xcode-select --install` or install Git from website.
- Linux: install via your package manager.

Check Git is installed:

```bash
git --version
```

---

## 1) Clone the repository locally

In a terminal:

```bash
git clone <your-repo-url>
cd M5StackDial
```

Then open this folder in VS Code:

```bash
code .
```

If `code .` does not work, open VS Code manually and use **File -> Open Folder...** and pick `M5StackDial`.

---

## 2) Understand the two PlatformIO environments

This repo uses two envs in `platformio.ini`:

- `m5dial_usb` -> build/upload over USB cable.
- `m5dial_ota` -> upload over Wi-Fi OTA.

`m5dial_usb` is the default and is used for the first flash.

---

## 3) First build locally (safe check)

From terminal in repo root:

```bash
pio run -e m5dial_usb
```

What this does:

- downloads required libraries/tools,
- compiles your firmware,
- outputs `.bin` files in `.pio/build/m5dial_usb/`.

If this succeeds, your local setup is working.

---

## 4) First flash by USB (required once)

OTA only works reliably after your device already has OTA-enabled firmware and Wi-Fi credentials.

1. Connect M5Dial with USB.
2. Confirm the serial port (example Linux: `/dev/ttyACM0`, Windows: `COM3`, macOS: `/dev/cu.usbmodem...`).
3. Update `upload_port` in `platformio.ini` if needed for USB env.
4. Upload:

```bash
pio run -e m5dial_usb -t upload
```

Optional: open serial monitor to check boot logs:

```bash
pio device monitor -b 115200
```

---

## 5) Find device IP for OTA

After USB flash and boot, your firmware should connect to Wi-Fi.

You need the device OTA host value:

- either IP address (example `192.168.1.37`),
- or mDNS host (example `M5Dial.local`, if mDNS resolves in your network).

You can usually get the IP from:

- serial logs,
- router DHCP client list,
- device web/status output (if your firmware provides it).

---

## 6) Configure OTA upload credentials locally (important)

This repo expects OTA settings from environment variables so secrets are not stored in Git.

### Linux/macOS (bash/zsh)

```bash
export M5DIAL_OTA_HOST=192.168.1.37
export M5DIAL_OTA_AUTH=your_ota_password
```

### Windows PowerShell

```powershell
$env:M5DIAL_OTA_HOST = "192.168.1.37"
$env:M5DIAL_OTA_AUTH = "your_ota_password"
```

> You must set these in the same terminal session before OTA upload.

---

## 7) Build and upload over OTA

Run:

```bash
pio run -e m5dial_ota -t upload
```

If successful, PlatformIO sends the firmware over Wi-Fi and the device reboots into new firmware.

---

## 8) Your day-to-day workflow (recommended)

Every time you want to test new changes from GitHub:

1. Pull latest:
   ```bash
   git pull
   ```
2. Build locally:
   ```bash
   pio run -e m5dial_usb
   ```
3. OTA upload:
   ```bash
   pio run -e m5dial_ota -t upload
   ```
4. Test on device.
5. Repeat.

Yes: this is exactly the flow you asked for, and it is a normal PlatformIO workflow.

---

## 9) Troubleshooting (beginner friendly)

### `pio: command not found`

PlatformIO CLI is not available in your shell.

- Easiest fix: use PlatformIO tasks inside VS Code (PlatformIO sidebar -> Project Tasks).
- Or install PlatformIO Core CLI and reopen terminal.

### OTA upload timeout / device not found

Check:

- PC and M5Dial are on the same network/VLAN.
- `M5DIAL_OTA_HOST` is correct.
- firewall is not blocking local network traffic.
- device is powered and online.

Try using IP instead of `.local` name.

### Wrong OTA password/auth failed

Set `M5DIAL_OTA_AUTH` again and retry.

### USB upload port errors

- pick correct serial port,
- close other serial monitors/apps using that port,
- retry upload.

---

## 10) VS Code GUI path (no terminal, optional)

If terminal commands feel hard, you can do everything from PlatformIO UI:

1. Open PlatformIO (alien head icon).
2. Under **Project Tasks**:
   - `env:m5dial_usb -> Build`
   - `env:m5dial_usb -> Upload` (first flash)
   - `env:m5dial_ota -> Upload` (later OTA flashes)
3. Use **Monitor** task to watch serial logs.

You still need OTA host/auth environment variables set in your VS Code environment/session.

---

## Files in this repo

- `platformio.ini` - build environments, dependencies, upload settings.
- `src/main.cpp` - firmware source code.
- `.gitignore` - local generated files excluded from git.
