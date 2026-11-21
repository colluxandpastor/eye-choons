This is slop but it works for my use case. 
# Eye Choons (iPhone Album & Library Manager)

A fast, lightweight, native Windows application written in pure C (Win32) for direct, album-centric music synchronization and management on iPhones without destructive full-library wipes.

---

## Features

* **Zero Apple Proprietary DLLs:** Completely independent of `MobileDevice.dll`, `CoreFoundation.dll`, or any bundled Apple runtimes.
* **Driverless Native Protocol Stack:** Built-in pure C implementation of the Usbmuxd, Lockdown, Schannel TLS 1.2 mutual authentication, and AFC (Apple File Conduit) protocols using Windows system libraries (`ws2_32`, `secur32`, `crypt32`).
* **Built-in System SQLite Engine:** Utilizes Windows' built-in `winsqlite3.dll` with atomic transactions, WAL flushing, and calibrated collation indexes.
* **Album-Centric Direct Push:** Transfer full album folders or individual songs directly to your device without requiring a full library wipe or sync. Existing tracks and playlists remain completely intact.
* **Full Apple MediaLibrary Specification Compliance:** Automatically creates and updates SQLite relational records (`item`, `item_extra`, `item_artist`, `album`, `album_artist`, `artwork`, `artwork_token`, `best_artwork_token`) with authentic collation ordering, Unicode normalization, section hashes, and Mac Epoch timestamps.
* **Direct USB Streaming via AFC:** Transfers audio files directly to `/iTunes_Control/Music/Fxx/` over USB via the Apple File Conduit protocol.
* **Lossless Artwork Extraction & Caching:** Extracts embedded ID3/APIC JPEG artwork and installs full-resolution originals and thumbnails into device artwork caches with proper color tags.
* **Compilation & Multi-Artist Handling:** Accurately groups multi-artist albums and compilations under unified album artist containers to prevent fragmented or split albums.
* **Device Library Browser:** View, search, and filter all tracks currently stored on the device with real-time title, artist, and album queries.
* **Export to PC:** Export selected audio files from the device back to any folder on your computer.
* **Keyboard Shortcuts & Context Menus:** Native Win32 keyboard navigation, right-click context menus, and File Explorer integration.

---

## Keyboard Shortcuts & Controls

| Shortcut | Action |
| :--- | :--- |
| `Ctrl + O` | Add individual audio files to transfer queue |
| `Ctrl + Shift + O` | Select and queue an entire album folder |
| `Ctrl + F` | Jump focus to real-time search box |
| `Escape` | Clear search filter and restore full list view |
| `Ctrl + A` | Select all tracks in active view |
| `Ctrl + C` | Copy formatted track information to clipboard |
| `F5` | Refresh device library |
| `Delete` | Remove selected track(s) from transfer queue |
| `Double-Click (Queue)` | Reveal local audio file in Windows File Explorer |
| `Right-Click` | Context menu (Reveal, Export, Copy, Select All) |

---

## Prerequisites & Building

### Requirements
- **OS:** Windows 10 / 11 (64-bit)
- **Compiler:** GCC (via MinGW-w64 or w64devkit)
- **Zero Third-Party Dependencies:** Relies strictly on standard Windows system DLLs (`winsqlite3.dll`, `gdiplus.dll`, `secur32.dll`, `crypt32.dll`, `ws2_32.dll`).

### Build from Source
Run the provided build script:
```cmd
build.bat
```
Or compile directly with GCC:
```cmd
gcc -O2 -mwindows -o EyeChoons.exe main.c apple_conduit.c -lcomctl32 -lsetupapi -lws2_32 -lole32 -lshell32 -lgdi32 -lcomdlg32 -lmsimg32 -lsecur32 -lcrypt32
```

---

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.
