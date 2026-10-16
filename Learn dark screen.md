# M-VAVE FM-1: Dark Screen / Soft-Brick Recovery & Windows 11 Fixes

This document records the exact root causes, debugging discoveries, and recovery procedures for when an M-VAVE FM-1 (or custom firmware like Evander's Music Box, Felucca, SLOOP, or ChoralRoot) boots with a **dark / black screen** and fails to flash over the web installer.

---

## 1. Symptoms & Initial Failure

1. **Physical symptom:**
   - Turning the power switch ON produces a completely black/dark screen.
   - No crest, no LEDs, no audio output.

2. **Web installer symptom (`installer.html` or `isod89.github.io/sloop-fm1`):**
   - Clicking **Install** or **Install file** immediately fails with:
     ```text
     # No FM-1 answered. Check the cable, close other MIDI apps, and press Install again.
     Error: FM-1 not found (USB cable, and no other app using it?)
         at fail (vendor/fm1ota.js)
         at Updater.install (vendor/fm1ota.js)
     ```
   - On SLOOP installer: `(MIDI: none)`.

---

## 2. Root Cause: WL82 UBOOT Mode vs. WebMIDI

- When the FM-1 crashes on boot 5 times in a row, when firmware in flash is incomplete, or when powered on while holding `OCT-`, the JieLi AC791N / WL82 chip automatically enters its internal mask ROM bootloader: **WL82 UBOOT mode**.
- In UBOOT mode:
  - Hardware ID: `USB\VID_4C4A&PID_8057`
  - Windows Device: `WL82 UBOOT1.00 USB Device` (Class: `DiskDrive`, Interface: `USBSTOR`)
- **Crucial Conflict:** In this mode, the hardware presents as a **raw USB Mass Storage disk drive**, NOT a USB-MIDI interface.
- Web installers rely on the browser's WebMIDI API (`navigator.requestMIDIAccess`). Because the device exposes zero MIDI endpoints while in UBOOT mode, WebMIDI reports `(MIDI: none)`.
- **Conclusion:** **The web installer cannot flash an FM-1 that is in UBOOT mode.** It must be recovered using the low-level JieLi SCSI bootloader interface.

---

## 3. Why `MvaveFM1Unbricker` / `jl-uboot-tool` Failed on Windows 11

When running the community recovery script `fm1_unbrick.py restore <package.fwsc>`, the tool crashed with:

```text
File "MvaveFM1Unbricker\jl-uboot-tool\scsiio\win32port.py", line 133, in execute
  raise SCSIException('Transfer failed: %s' % ctypes.WinError())
scsiio.common.SCSIException: Transfer failed: [WinError 5] Access is denied.
```

Investigation revealed four distinct bugs and OS security barriers on modern Windows 11:

### Bug A: Incorrect Access Mask Constants in `win32port.py`
In `jl-uboot-tool\scsiio\win32port.py`:
```python
# BUGGY ORIGINAL CODE:
GENERIC_READ     = 1
GENERIC_WRITE    = 2
```
In the Win32 API, `1` is `FILE_READ_DATA` and `2` is `FILE_WRITE_DATA`. The proper Win32 constants are:
```python
# CORRECTED CONSTANTS:
GENERIC_READ     = 0x80000000
GENERIC_WRITE    = 0x40000000
```
Because the handle was opened with `DesiredAccess = 3`, Windows kernel storage drivers rejected `IOCTL_SCSI_PASS_THROUGH_DIRECT` (which explicitly requires `GENERIC_READ | GENERIC_WRITE`) with `ERROR_ACCESS_DENIED` (`WinError 5`).

### Bug B: Hardcoded SCSI Target ID
In `win32port.py`, the target ID was hardcoded to `1`:
```python
# BUGGY ORIGINAL CODE:
sptd.path_id   = 0
sptd.target_id = 1
sptd.lun       = 0
```
On Windows USBSTOR, the FM-1 enumerates at **`TargetId = 0`**, `PathId = 0`, `Lun = 0`. Sending SCSI commands to Target 1 was rejected by the driver.
**Fix:** Dynamically query the actual hardware SCSI address using `IOCTL_SCSI_GET_ADDRESS` (`0x00041018`) upon opening the drive handle, defaulting to `0` instead of `1`.

### Bug C: Unaligned DMA Memory Buffers in 64-bit Python
`IOCTL_SCSI_PASS_THROUGH_DIRECT` requires adapter-aligned user buffers for direct memory access (DMA). Allocating buffers on the standard Python CRT heap via `ctypes.create_string_buffer` produces unaligned addresses that the storage stack rejects.
**Fix:** Allocate DMA transfer memory with `VirtualAlloc` (`MEM_COMMIT | MEM_RESERVE`, `PAGE_READWRITE`), which guarantees 4096-byte page alignment, and release with `VirtualFree`.

### Bug D: Windows Disk "Offline" State
Windows Storage Management frequently marks raw USB disks as `IsOffline : True` (`OperationalStatus : No Media`). When offline, Windows kernel blocks raw I/O control codes.
**Fix:** Bring the disk online in elevated PowerShell before flashing:
```powershell
Set-Disk -Number <DiskNumber> -IsOffline $false
```

---

## 4. Complete Working Solution for `win32port.py`

Replace `MvaveFM1Unbricker\jl-uboot-tool\scsiio\win32port.py` with this fully corrected implementation:

```python
from scsiio.common import *
import ctypes

from ctypes.wintypes import HANDLE, BOOL, LPCWSTR, BYTE, WORD, DWORD

class SCSI_ADDRESS(ctypes.Structure):
    _fields_ = [
        ('Length', DWORD),
        ('PortNumber', BYTE),
        ('PathId', BYTE),
        ('TargetId', BYTE),
        ('Lun', BYTE)
    ]

class SCSI_PASS_THROUGH_DIRECT(ctypes.Structure):
    _fields_ = [
        ('len',       WORD),
        ('status',    BYTE),
        ('path_id',   BYTE),
        ('target_id', BYTE),
        ('lun',       BYTE),
        ('cdb_len',   BYTE),
        ('sense_len', BYTE),
        ('dir',       BYTE),
        ('data_len',  DWORD),
        ('timeout',   DWORD),
        ('data_ptr',  ctypes.c_void_p),
        ('sense_off', DWORD),
        ('cdb',       BYTE * 16)
    ]

LPSECURITY_ATTRIBUTES = ctypes.c_void_p
INVALID_HANDLE_VALUE = HANDLE(-1).value

GENERIC_READ     = 0x80000000
GENERIC_WRITE    = 0x40000000
FILE_SHARE_READ  = 1
FILE_SHARE_WRITE = 2
OPEN_EXISTING    = 3

IOCTL_SCSI_GET_ADDRESS         = 0x00041018
IOCTL_SCSI_PASS_THROUGH_DIRECT = 0x0004D014

SCSI_IOCTL_DATA_OUT         = 0
SCSI_IOCTL_DATA_IN          = 1
SCSI_IOCTL_DATA_UNSPECIFIED = 2

MEM_COMMIT  = 0x1000
MEM_RESERVE = 0x2000
MEM_RELEASE = 0x8000
PAGE_READWRITE = 0x04

kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)

CreateFile = kernel32.CreateFileW
CreateFile.restype  = HANDLE
CreateFile.argtypes = [LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE]

CloseHandle = kernel32.CloseHandle
CloseHandle.restype  = BOOL
CloseHandle.argtypes = [HANDLE]

DeviceIoControl = kernel32.DeviceIoControl
DeviceIoControl.restype  = BOOL
DeviceIoControl.argtypes = [HANDLE, DWORD, ctypes.c_void_p, DWORD, ctypes.c_void_p, DWORD, ctypes.POINTER(DWORD), ctypes.c_void_p]

VirtualAlloc = kernel32.VirtualAlloc
VirtualAlloc.restype = ctypes.c_void_p
VirtualAlloc.argtypes = [ctypes.c_void_p, ctypes.c_size_t, DWORD, DWORD]

VirtualFree = kernel32.VirtualFree
VirtualFree.restype = BOOL
VirtualFree.argtypes = [ctypes.c_void_p, ctypes.c_size_t, DWORD]

class SCSIDev(SCSIDevBase):
    def open(self, path):
        self.fhandle = CreateFile(path, GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE, None,
                                  OPEN_EXISTING, 0, None)

        if self.fhandle == INVALID_HANDLE_VALUE or self.fhandle == -1:
            raise ctypes.WinError(ctypes.get_last_error())

        self.path_id = 0
        self.target_id = 0
        self.lun = 0

        addr = SCSI_ADDRESS()
        addr.Length = ctypes.sizeof(addr)
        xlen = DWORD()
        if DeviceIoControl(self.fhandle, IOCTL_SCSI_GET_ADDRESS, None, 0, ctypes.byref(addr), addr.Length, ctypes.byref(xlen), None):
            self.path_id = addr.PathId
            self.target_id = addr.TargetId
            self.lun = addr.Lun

        self.is_open = True

    def close(self):
        if self.is_open:
            if self.fhandle is not None and self.fhandle != INVALID_HANDLE_VALUE:
                CloseHandle(self.fhandle)
                self.fhandle = None
            self.is_open = False

    def execute(self, cdb, data_out, data_in, max_sense_len=32, return_sense_buffer=False):
        sptd = SCSI_PASS_THROUGH_DIRECT()
        sptd.len = ctypes.sizeof(sptd)
        sptd.path_id   = self.path_id
        sptd.target_id = self.target_id
        sptd.lun       = self.lun
        sptd.timeout = 5
        sptd.cdb_len = len(cdb)
        for i in range(sptd.cdb_len):
            sptd.cdb[i] = cdb[i]

        page = None
        if data_out and data_in:
            raise NotImplemented('Indirect I/O is not supported')
        elif data_out:
            sptd.dir = SCSI_IOCTL_DATA_OUT
            sptd.data_len = len(data_out)
            alloc_sz = max(sptd.data_len, 4096)
            page = VirtualAlloc(None, alloc_sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
            if not page:
                raise MemoryError('VirtualAlloc failed')
            ctypes.memmove(page, bytes(data_out), sptd.data_len)
            sptd.data_ptr = page
        elif data_in:
            sptd.dir = SCSI_IOCTL_DATA_IN
            sptd.data_len = len(data_in)
            alloc_sz = max(sptd.data_len, 4096)
            page = VirtualAlloc(None, alloc_sz, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)
            if not page:
                raise MemoryError('VirtualAlloc failed')
            sptd.data_ptr = page
        else:
            sptd.dir = SCSI_IOCTL_DATA_UNSPECIFIED
            sptd.data_len = 0
            sptd.data_ptr = None

        datalen = sptd.data_len
        xlen = DWORD()

        try:
            res = DeviceIoControl(self.fhandle, IOCTL_SCSI_PASS_THROUGH_DIRECT,
                                  ctypes.byref(sptd), sptd.len, ctypes.byref(sptd), sptd.len,
                                  ctypes.byref(xlen), None)
            if not res:
                err = ctypes.get_last_error()
                raise SCSIException(f'Transfer failed: [WinError {err}] {ctypes.FormatError(err).strip()}')

            if data_in and page:
                inlen = min(sptd.data_len, len(data_in))
                data_in[:inlen] = ctypes.string_at(page, inlen)

            if return_sense_buffer:
                return datalen - sptd.data_len, b''
            else:
                return datalen - sptd.data_len
        finally:
            if page:
                VirtualFree(page, 0, MEM_RELEASE)
```

---

## 5. Step-by-Step Soft-Brick Recovery Procedure

1. **Verify device presence in UBOOT mode:**
   - Plug the FM-1 in with a data USB-C cable and turn it ON.
   - Screen stays black.
   - Open PowerShell and check:
     ```powershell
     Get-CimInstance Win32_DiskDrive | Where-Object { $_.Model -match 'WL82' }
     ```
   - Note the disk index (e.g. Disk `6`).

2. **Open Administrator PowerShell:**
   - Right-click Start button $\rightarrow$ **Terminal (Admin)** or **PowerShell (Admin)**.

3. **Bring the disk online:**
   ```powershell
   Set-Disk -Number 6 -IsOffline $false
   ```

4. **Run the restore command:**
   ```powershell
   cd C:\Users\mixan\MvaveFM1Unbricker
   python fm1_unbrick.py restore "x:\Projects\Evanders Music Box- FM-1 LARP firmware\build\fw\emb.fwsc"
   ```
   - The tool will read `WL82 UBOOT1.00`, upload the second-stage RAM loader, and create a full 1 MiB double-backup in `backups/`.
   - When prompted, type `WRITE`.
   - It erases and flashes `0x4000..0x92FFF` and verifies every byte against the package.

5. **Reboot the FM-1:**
   - Unplug the USB cable.
   - Turn power switch **OFF**.
   - Wait 5 seconds.
   - Turn power switch **ON**.
   - The FM-1 boots up into the firmware with screen, sound, and USB-MIDI functional.
