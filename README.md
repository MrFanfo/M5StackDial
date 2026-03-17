# M5StackDial PlatformIO Project

This repository is now structured as a standard PlatformIO project, so you can:

1. keep code on GitHub,
2. pull changes locally in VS Code,
3. build firmware with PlatformIO,
4. upload by USB once,
5. then upload by OTA for daily iteration.

## Project structure

- `platformio.ini` - environments and dependencies
- `src/main.cpp` - firmware source entrypoint

## Local workflow (GitHub -> local build -> OTA flash)

### 1) Clone and build

```bash
git clone <your-repo-url>
cd M5StackDial
pio run -e m5dial_usb
```

### 2) First flash by USB

```bash
pio run -e m5dial_usb -t upload
```

### 3) OTA updates after device is on Wi-Fi

Set environment variables (so OTA host/auth are not committed in git):

```bash
export M5DIAL_OTA_HOST=192.168.1.37
export M5DIAL_OTA_AUTH=your_ota_password
```

Then upload OTA:

```bash
pio run -e m5dial_ota -t upload
```

## Is your desired flow possible?

Yes. Your desired flow is valid:

- ask for code changes committed to GitHub,
- pull locally,
- build locally from this repo,
- flash OTA,
- test,
- repeat.

That is a common and recommended PlatformIO workflow.
