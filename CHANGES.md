# CHANGES - mtwifi-adapter

2025-10-22 06:45:25 UTC - mtwifi-adapter changes merged/added
- Added: src/RadioSocket.h (abstract interface for socket-like radio/TCP adapter).
- Backups to be created for modified files (if any): src/mt_internals.h.off, src/mt_wifi.cpp.off
- Planned: add src/WiFiClientAdapter.h, modify src/mt_internals.h to expose RadioSocket pointer, modify src/mt_wifi.cpp to accept injected RadioSocket, enable MT_DEBUGGING, and add rate-limited debug logging.

Notes:
- All edits will include .off backups of original files.
- Debugging will be enabled with rate-limited messages for flood-prone logs.
- I will append precise commit SHAs and timestamps to this file after each change so you can see exactly what changed and when.
