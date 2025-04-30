import ctypes
import ctypes.wintypes
import struct
import sys
import time
import os # For clearing screen

# Try importing psutil, provide instructions if missing
try:
    import psutil # Requires 'pip install psutil' [cite: 227]
except ImportError:
    print("Error: 'psutil' library not found.")
    print("Please install it using: pip install psutil")
    sys.exit(1)


# --- Driver Interaction ---

# Define IOCTL codes (MUST match driver's .h file) [cite: 227]
FILE_DEVICE_NETWORK = 0x00000012 [cite: 227]
METHOD_BUFFERED = 0 [cite: 227]
FILE_WRITE_ACCESS = 0x0002 [cite: 227]
FILE_READ_ACCESS = 0x0001 [cite: 227]

def CTL_CODE(DeviceType, Function, Method, Access):
    """Replicates the CTL_CODE macro from Windows."""
    return ((DeviceType << 16) | (Access << 14) | (Function << 2) | Method) [cite: 228]

IOCTL_ADD_RULE = CTL_CODE(FILE_DEVICE_NETWORK, 0x801, METHOD_BUFFERED, FILE_WRITE_ACCESS) [cite: 228]
IOCTL_REMOVE_RULE = CTL_CODE(FILE_DEVICE_NETWORK, 0x802, METHOD_BUFFERED, FILE_WRITE_ACCESS) [cite: 228]
# IOCTL_QUERY_RULES = CTL_CODE(FILE_DEVICE_NETWORK, 0x803, METHOD_BUFFERED, FILE_READ_ACCESS) # Optional [cite: 228]


# Define the rule structure (MUST match driver's .h file) [cite: 228]
class UserRuleData(ctypes.Structure):
    _pack_ = 1 # Ensure packing matches driver's #pragma pack(push, 1) [cite: 228]
    _fields_ = [
        ("ProcessId", ctypes.wintypes.ULONG), [cite: 228]
        ("UploadLimitKBps", ctypes.wintypes.ULONG), [cite: 228]
        ("DownloadLimitKBps", ctypes.wintypes.ULONG) [cite: 228]
    ]

# Define necessary WinAPI constants and functions using ctypes [cite: 228]
GENERIC_READ = 0x80000000 [cite: 228]
GENERIC_WRITE = 0x40000000 [cite: 228]
OPEN_EXISTING = 3 [cite: 228]
INVALID_HANDLE_VALUE = ctypes.wintypes.HANDLE(-1).value [cite: 228]
FILE_ATTRIBUTE_NORMAL = 0x80 [cite: 228]

# Load kernel32.dll
kernel32 = ctypes.windll.kernel32 [cite: 229]

# Define CreateFileW prototype [cite: 229]
CreateFileW = kernel32.CreateFileW
CreateFileW.argtypes = [
    ctypes.wintypes.LPWSTR,  # lpFileName
    ctypes.wintypes.DWORD,   # dwDesiredAccess
    ctypes.wintypes.DWORD,   # dwShareMode
    ctypes.c_void_p,         # lpSecurityAttributes
    ctypes.wintypes.DWORD,   # dwCreationDisposition
    ctypes.wintypes.DWORD,   # dwFlagsAndAttributes
    ctypes.wintypes.HANDLE   # hTemplateFile
] [cite: 229]
CreateFileW.restype = ctypes.wintypes.HANDLE [cite: 229]

# Define DeviceIoControl prototype [cite: 229]
DeviceIoControl = kernel32.DeviceIoControl
DeviceIoControl.argtypes = [
    ctypes.wintypes.HANDLE,            # hDevice
    ctypes.wintypes.DWORD,             # dwIoControlCode
    ctypes.c_void_p,                   # lpInBuffer
    ctypes.wintypes.DWORD,             # nInBufferSize
    ctypes.c_void_p,                   # lpOutBuffer
    ctypes.wintypes.DWORD,             # nOutBufferSize
    ctypes.POINTER(ctypes.wintypes.DWORD), # lpBytesReturned [cite: 230]
    ctypes.c_void_p                    # lpOverlapped
] [cite: 230]
DeviceIoControl.restype = ctypes.wintypes.BOOL [cite: 230]

# Define CloseHandle prototype [cite: 230]
CloseHandle = kernel32.CloseHandle
CloseHandle.argtypes = [ctypes.wintypes.HANDLE] [cite: 230]
CloseHandle.restype = ctypes.wintypes.BOOL [cite: 231]

# Define GetLastError prototype [cite: 231]
GetLastError = kernel32.GetLastError
GetLastError.restype = ctypes.wintypes.DWORD

# Driver Symbolic Link Name (MUST match driver's DOS_DEVICE_NAME without prefix) [cite: 231]
DRIVER_SYMBOLIC_LINK = r"\\.\BandwidthThrottler" [cite: 231]

# Global handle to the driver [cite: 231]
driver_handle = None [cite: 231]
# Dictionary to store rules managed by this UI {pid: (up_kbps, down_kbps)} [cite: 231]
active_rules = {}

def connect_to_driver():
    """Opens a handle to the kernel driver."""
    global driver_handle
    print(f"Attempting to open device: {DRIVER_SYMBOLIC_LINK}") [cite: 231]
    handle = CreateFileW(
        DRIVER_SYMBOLIC_LINK, [cite: 232]
        GENERIC_READ | GENERIC_WRITE, [cite: 232]
        0,                      # No sharing [cite: 232]
        None,                   # Default security
        OPEN_EXISTING, [cite: 232]
        FILE_ATTRIBUTE_NORMAL, [cite: 232]
        None                    # No template file
    ) [cite: 232]

    if handle == INVALID_HANDLE_VALUE: [cite: 232]
        error_code = GetLastError() [cite: 232]
        print(f"Error opening driver handle: {error_code}") [cite: 232]
        # FormatMessage might be useful here for a textual description
        driver_handle = None [cite: 232]
        return False [cite: 232]
    else:
        print("Successfully opened driver handle.") [cite: 232]
        driver_handle = handle [cite: 233]
        return True [cite: 233]

def close_driver_connection():
    """Closes the handle to the driver."""
    global driver_handle
    if driver_handle and driver_handle != INVALID_HANDLE_VALUE: [cite: 233]
        print("Closing driver handle.") [cite: 233]
        CloseHandle(driver_handle) [cite: 233]
        driver_handle = None [cite: 233]
        active_rules.clear() # Clear local rule cache on disconnect [cite: 233]

def send_ioctl(ioctl_code, input_buffer_obj=None):
    """
    Sends an IOCTL request to the driver.
    Takes a ctypes structure/object or raw bytes as input_buffer_obj.
    """
    if not driver_handle or driver_handle == INVALID_HANDLE_VALUE: [cite: 233]
        print("Driver handle is not valid.") [cite: 233]
        return False, None [cite: 233]

    in_buffer_ptr = ctypes.c_void_p(None) [cite: 233]
    in_buffer_size = 0 [cite: 234]

    if input_buffer_obj is not None:
        if isinstance(input_buffer_obj, ctypes.Structure):
             in_buffer_size = ctypes.sizeof(input_buffer_obj) [cite: 234]
             in_buffer_ptr = ctypes.byref(input_buffer_obj) [cite: 234]
        elif isinstance(input_buffer_obj, bytes):
             in_buffer_size = len(input_buffer_obj) [cite: 234]
             # Create a mutable buffer from the input bytes
             c_buffer = ctypes.create_string_buffer(input_buffer_obj, in_buffer_size) [cite: 234]
             in_buffer_ptr = ctypes.byref(c_buffer) [cite: 234]
        else:
             print("Error: Invalid input_buffer_obj type for IOCTL.")
             return False, None

    bytes_returned = ctypes.wintypes.DWORD(0) [cite: 234]

    # For IOCTLs that don't expect output (like ADD/REMOVE in this simple case) [cite: 234]
    out_buffer_ptr = ctypes.c_void_p(None) [cite: 234]
    out_buffer_size = 0 [cite: 235]

    success = DeviceIoControl(
        driver_handle, [cite: 235]
        ioctl_code, [cite: 235]
        in_buffer_ptr, [cite: 235]
        in_buffer_size, [cite: 235]
        out_buffer_ptr,       # No output buffer needed for Add/Remove [cite: 235]
        out_buffer_size,      # Output buffer size 0 [cite: 235]
        ctypes.byref(bytes_returned), [cite: 235]
        None                    # No overlapped I/O
    ) [cite: 235]

    if not success: [cite: 235]
        error_code = GetLastError() [cite: 235]
        print(f"DeviceIoControl failed for IOCTL {ioctl_code:#X} with error: {error_code}") [cite: 235]
        return False, error_code [cite: 235]
    else:
        # print(f"DeviceIoControl succeeded for IOCTL {ioctl_code:#X}.") [cite: 236]
        return True, 0 [cite: 236]


# --- Application Logic ---

def clear_screen():
    """Clears the console screen."""
    os.system('cls' if os.name == 'nt' else 'clear')

def get_process_list():
    """Returns a list of (pid, name) for running processes.""" [cite: 236]
    processes = [] [cite: 236]
    try:
        # Iterate over all running process PIDs
        for proc in psutil.process_iter(['pid', 'name']): [cite: 236]
            try:
                 # Append tuple (pid, name) to the list
                 processes.append((proc.info['pid'], proc.info['name'])) [cite: 236]
            except (psutil.NoSuchProcess, psutil.AccessDenied, psutil.ZombieProcess): [cite: 236]
                 # Ignore processes that ended or we can't access [cite: 236]
                 pass
    except Exception as e:
        print(f"Error getting process list: {e}") [cite: 236]
    # Sort by process name (case-insensitive) [cite: 236]
    return sorted(processes, key=lambda x: x[1].lower() if x[1] else '') [cite: 236]

def add_or_update_rule_ui():
    """Handles adding/updating a rule via UI interaction.""" [cite: 236]
    print("-" * 20) [cite: 236]
    try:
        pid_str = input("Enter Process ID (PID) to throttle: ") [cite: 236]
        pid = int(pid_str) [cite: 236]
        if pid <= 0: [cite: 236]
            print("Invalid PID.") [cite: 237]
            return [cite: 237]

        # Check if process exists (optional but good UX) [cite: 237]
        try:
            if not psutil.pid_exists(pid): [cite: 237]
                print(f"Warning: Process with PID {pid} does not currently exist.") [cite: 237]
                if input("Continue anyway? (y/n): ").lower() != 'y': [cite: 237]
                    return [cite: 237]
        except Exception as e:
             print(f"Warning: Could not check if PID {pid} exists ({e}). Proceeding anyway.")

        up_limit_str = input("Enter Upload Limit (KB/s) [Default 24]: ") [cite: 237]
        down_limit_str = input("Enter Download Limit (KB/s) [Default 24]: ") [cite: 237]

        # Use default if input is empty, otherwise convert to int
        up_limit = int(up_limit_str) if up_limit_str.strip() else 24 [cite: 237]
        down_limit = int(down_limit_str) if down_limit_str.strip() else 24 [cite: 237]

        if up_limit < 0 or down_limit < 0: [cite: 237]
            print("Limits cannot be negative.") [cite: 237]
            return [cite: 237]

        # Create the structure [cite: 237]
        rule = UserRuleData() [cite: 237]
        rule.ProcessId = pid [cite: 237]
        rule.UploadLimitKBps = up_limit [cite: 237]
        rule.DownloadLimitKBps = down_limit [cite: 237]

        # Send IOCTL using the structure directly
        print(f"Sending ADD_RULE for PID={pid}, Up={up_limit}, Down={down_limit}") [cite: 238]
        success, err_code = send_ioctl(IOCTL_ADD_RULE, rule) # Pass structure directly [cite: 238]

        if success: [cite: 238]
            print("Rule added/updated successfully in driver.") [cite: 238]
            active_rules[pid] = (up_limit, down_limit) # Update local cache [cite: 238]
        else:
            print(f"Failed to add/update rule in driver (Error: {err_code}).") [cite: 238]

    except ValueError: [cite: 238]
        print("Invalid input. Please enter numbers.") [cite: 239]
    except Exception as e: [cite: 239]
        print(f"An error occurred: {e}") [cite: 239]
    print("-" * 20) [cite: 240]

def remove_rule_ui():
    """Handles removing a rule via UI interaction.""" [cite: 240]
    print("-" * 20) [cite: 240]
    try:
        pid_str = input("Enter Process ID (PID) to remove throttle rule: ") [cite: 240]
        pid = int(pid_str) [cite: 240]
        if pid <= 0: [cite: 240]
            print("Invalid PID.") [cite: 240]
            return [cite: 240]

        # Pack PID into bytes (ULONG = 4 bytes, unsigned long) [cite: 240]
        # Use '<L' for little-endian unsigned long [cite: 240]
        pid_bytes = struct.pack('<L', pid) [cite: 240]

        # Send IOCTL [cite: 240]
        print(f"Sending REMOVE_RULE for PID={pid}") [cite: 240]
        success, err_code = send_ioctl(IOCTL_REMOVE_RULE, pid_bytes) # Pass bytes [cite: 240]

        if success: [cite: 240]
            print("Rule removal request sent successfully to driver.") [cite: 240]
            # Note: Driver returns success even if PID wasn't found [cite: 241]
            # A QUERY IOCTL would be needed to confirm rule state reliably. [cite: 241]
            if pid in active_rules: [cite: 240]
                del active_rules[pid] # Remove from local cache [cite: 241]
                print("(Removed from UI cache)")
            else:
                 print("(PID was not in UI cache)")
        else:
            print(f"Rule removal request failed in driver (Error: {err_code}).") [cite: 241]
            # Still remove from local cache if it was there
            if pid in active_rules: [cite: 241]
                del active_rules[pid] [cite: 241]
                print("(Removed from UI cache despite driver error)")


    except ValueError: [cite: 241]
        print("Invalid input. Please enter a number for PID.") [cite: 242]
    except Exception as e: [cite: 242]
        print(f"An error occurred: {e}") [cite: 242]
    print("-" * 20) [cite: 242]

def display_processes():
    """Displays the list of running processes.""" [cite: 242]
    print("\n--- Running Processes ---") [cite: 242]
    procs = get_process_list() [cite: 242]
    if not procs: [cite: 242]
        print("Could not retrieve process list.") [cite: 243]
        return [cite: 243]

    # Determine max width for names for formatting [cite: 244]
    try:
        max_name_len = max(len(name) for _, name in procs if name) if procs else 20 [cite: 244]
    except ValueError: # Handle case where all names are None or empty
        max_name_len = 20

    # Print header [cite: 244]
    header = f"{'PID':>8} | {'Name':<{max_name_len}} | {'Throttled (Up / Down KBps)'}" [cite: 244]
    print(header) [cite: 244]
    print("-" * len(header)) [cite: 244]

    count = 0 [cite: 244]
    for pid, name in procs: [cite: 244]
        name = name or "<Unknown>" # Handle potential None names
        throttle_info = "NO" [cite: 244]
        if pid in active_rules: [cite: 244]
            up, down = active_rules[pid] [cite: 244]
            throttle_info = f"YES ({up} / {down})" [cite: 245]

        print(f"{pid:>8} | {name:<{max_name_len}} | {throttle_info}") [cite: 245]
        count += 1 [cite: 245]

        # Optional: Pause every ~30 lines [cite: 245]
        if count % 30 == 0 and count < len(procs): [cite: 245]
            try:
                input("--- Press Enter to continue listing processes ---") [cite: 245]
                print(header) # Reprint header after pause
                print("-" * len(header))
            except EOFError: # Handle environments where input might fail
                 break

    print(f"--- Total Processes: {len(procs)} ---") [cite: 245]

def display_active_rules():
    """Displays rules currently being managed by this UI.""" [cite: 245]
    print("\n--- Active Throttling Rules (UI Cache) ---") [cite: 245]
    if not active_rules: [cite: 245]
        print("No rules are currently active in this UI session.") [cite: 245]
    else:
        header = f"{'PID':>8} | {'Upload KBps':>12} | {'Download KBps':>14}" [cite: 245]
        print(header) [cite: 245]
        print("-" * len(header)) [cite: 245]
        # Sort by PID for consistent display [cite: 245]
        for pid in sorted(active_rules.keys()): [cite: 245]
            up, down = active_rules[pid] [cite: 246]
            print(f"{pid:>8} | {up:>12} | {down:>14}") [cite: 246]
    print("-" * 40) [cite: 246]

# --- Main Loop ---
if __name__ == "__main__": [cite: 246]
    clear_screen()
    print("--- Bandwidth Throttler Control UI ---") [cite: 246]
    print("NOTE: Requires the Bandwidth Throttler driver to be loaded and running.") [cite: 246]
    print("Ensure 'bcdedit /set testsigning on' is enabled and system rebooted.") [cite: 246]

    if not connect_to_driver(): [cite: 247]
        print("\nFailed to connect to the driver.") [cite: 247]
        print("Make sure the driver (BandwidthThrottler.sys) is loaded.") [cite: 247]
        print("You might need to install it using the .inf file (e.g., right-click -> Install)") [cite: 247]
        print("or using 'devcon install BandwidthThrottler.inf ROOT\\BandwidthThrottler'") [cite: 247]
        sys.exit(1) [cite: 247]

    try:
        while True: [cite: 247]
            print("\nOptions:") [cite: 247]
            print("  1. List Running Processes") [cite: 247]
            print("  2. Add / Update Throttling Rule") [cite: 247]
            print("  3. Remove Throttling Rule") [cite: 247]
            print("  4. Show Active Rules (from this UI)") [cite: 247]
            print("  Q. Quit") [cite: 247]

            choice = input("Enter choice: ").strip().lower() [cite: 247]

            if choice == '1': [cite: 247]
                display_processes() [cite: 247]
            elif choice == '2': [cite: 247]
                add_or_update_rule_ui() [cite: 247]
            elif choice == '3': [cite: 247]
                remove_rule_ui() [cite: 247]
            elif choice == '4': [cite: 247]
                display_active_rules() [cite: 247]
            elif choice == 'q': [cite: 247]
                break [cite: 247]
            else:
                print("Invalid choice.") [cite: 247]

    except KeyboardInterrupt: [cite: 247]
        print("\nExiting...") [cite: 247]
    finally:
        close_driver_connection() [cite: 247]
        print("Program terminated.") [cite: 247]
