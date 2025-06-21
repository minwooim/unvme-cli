#!/usr/bin/env python3
"""
Python client for communicating with unvmed daemon via System V message queue.
Based on struct unvme_msg from unvme.h
"""

import os
import sys
import struct
import tempfile
import sysv_ipc
from typing import Optional, List
import time

# Constants from unvme.h
UNVME_PWD_STRLEN = 256
UNVME_BDF_STRLEN = 13
UNVME_DAEMON_PID = "/var/run/unvmed.pid"

class UnvmeMsg:
    """
    Python representation of struct unvme_msg (fully packed, 545 bytes)
    """
    # Struct format: little-endian, packed, matches C struct (545 bytes)
    STRUCT_FORMAT = '<qi256s13s256sii'
    STRUCT_SIZE = struct.calcsize(STRUCT_FORMAT)  # Should be 545
    
    def __init__(self, msg_type: int = 0, argc: int = 0, argv_file: str = "", 
                 bdf: str = "", pwd: str = "", pid: int = 0, signum: int = 0):
        self.msg_type = msg_type
        self.argc = argc
        self.argv_file = argv_file
        self.bdf = bdf
        self.pwd = pwd
        self.pid = pid
        self.signum = signum  # Also used as ret in responses
    
    def pack(self) -> bytes:
        """Pack the message into bytes for sending via message queue"""
        return struct.pack(
            self.STRUCT_FORMAT,
            self.msg_type,
            self.argc,
            self.argv_file.encode('utf-8')[:UNVME_PWD_STRLEN-1].ljust(UNVME_PWD_STRLEN, b'\x00'),
            self.bdf.encode('utf-8')[:UNVME_BDF_STRLEN-1].ljust(UNVME_BDF_STRLEN, b'\x00'),
            self.pwd.encode('utf-8')[:UNVME_PWD_STRLEN-1].ljust(UNVME_PWD_STRLEN, b'\x00'),
            self.pid,
            self.signum
        )
    
    @classmethod
    def unpack(cls, data: bytes) -> 'UnvmeMsg':
        """Unpack bytes received from message queue into UnvmeMsg"""
        unpacked = struct.unpack(cls.STRUCT_FORMAT, data)
        msg = cls()
        msg.msg_type = unpacked[0]
        msg.argc = unpacked[1]
        msg.argv_file = unpacked[2].rstrip(b'\x00').decode('utf-8')
        msg.bdf = unpacked[3].rstrip(b'\x00').decode('utf-8')
        msg.pwd = unpacked[4].rstrip(b'\x00').decode('utf-8')
        msg.pid = unpacked[5]
        msg.signum = unpacked[6]
        return msg
    
    def __str__(self):
        return f"UnvmeMsg(type={self.msg_type}, argc={self.argc}, argv_file='{self.argv_file}', bdf='{self.bdf}', pwd='{self.pwd}', pid={self.pid}, signum={self.signum})"

class UnvmeMsgQClient:
    """Client for communicating with unvmed daemon via System V message queue"""
    
    def __init__(self, keyfile: str = "/dev/mqueue/unvmed"):
        self.keyfile = keyfile
        self.msg_queue = None
        self.client_pid = os.getpid()
    
    def get_daemon_pid(self) -> Optional[int]:
        """Get daemon PID from the PID file"""
        try:
            with open(UNVME_DAEMON_PID, 'r') as f:
                return int(f.read().strip())
        except (FileNotFoundError, ValueError):
            return None
    
    def is_daemon_running(self) -> bool:
        """Check if daemon is running"""
        daemon_pid = self.get_daemon_pid()
        if daemon_pid is None:
            return False
        
        try:
            # Send signal 0 to check if process exists
            os.kill(daemon_pid, 0)
            return True
        except (OSError, ProcessLookupError):
            return False
    
    def connect(self) -> bool:
        """Connect to the message queue"""
        try:
            # Generate key from keyfile (similar to ftok)
            key = sysv_ipc.ftok(self.keyfile, 0, silence_warning=True)
            self.msg_queue = sysv_ipc.MessageQueue(key, sysv_ipc.IPC_CREAT, 0o666)
            return True
        except sysv_ipc.ExistentialError:
            print(f"Failed to connect to message queue with keyfile: {self.keyfile}")
            return False
    
    def disconnect(self):
        """Disconnect from message queue"""
        if self.msg_queue:
            self.msg_queue = None
    
    def send_command(self, argv: List[str], bdf: str = "", timeout: float = 10.0) -> Optional[int]:
        """
        Send command to daemon and wait for response
        
        Args:
            argv: Command arguments list
            bdf: Device BDF string (optional)
            timeout: Timeout in seconds
            
        Returns:
            Return code from daemon, or None if failed
        """
        if not self.connect():
            return None
        
        if not self.is_daemon_running():
            print("Daemon is not running")
            return None
        
        daemon_pid = self.get_daemon_pid()
        if daemon_pid is None:
            print("Failed to get daemon PID")
            return None
        
        # Ensure argv[0] is always 'unvme'
        if not argv or argv[0] != 'unvme':
            argv = ['unvme'] + argv
        # Create temporary file for argv
        argv_file = ""
        if argv:
            try:
                with tempfile.NamedTemporaryFile(mode='w', delete=False, suffix='.argv') as f:
                    for arg in argv:
                        f.write(f"{arg}\n")
                    argv_file = f.name
            except Exception as e:
                print(f"Failed to create argv file: {e}")
                return None
        
        # Prepare message to daemon
        msg = UnvmeMsg(
            msg_type=daemon_pid,
            argc=len(argv),
            argv_file=argv_file,
            bdf=bdf,
            pwd=os.getcwd(),
            pid=self.client_pid,
            signum=0  # Normal request
        )
        
        try:
            # Send message to daemon
            self.msg_queue.send(msg.pack()[8:], type=daemon_pid)
            
            # Wait for response from daemon (manual timeout loop)
            start_time = time.time()
            while True:
                try:
                    response_data, _ = self.msg_queue.receive(type=self.client_pid, block=False)
                    break
                except sysv_ipc.BusyError:
                    if time.time() - start_time > timeout:
                        print("Timeout waiting for daemon response")
                        return None
                    time.sleep(0.05)

            response = UnvmeMsg.unpack(bytes(8) + response_data)

            # Print daemon output files if they exist
            stdout_path = f"/var/log/unvmed/stdout-{self.client_pid}"
            stderr_path = f"/var/log/unvmed/stderr-{self.client_pid}"
            if os.path.exists(stdout_path):
                with open(stdout_path, 'r', errors='replace') as f:
                    print(f.read(), end='', file=sys.stdout)
            if os.path.exists(stderr_path):
                with open(stderr_path, 'r', errors='replace') as f:
                    print(f.read(), end='', file=sys.stderr)

            return response.signum
            
        except Exception as e:
            print(f"Error communicating with daemon: {e}")
            return None
        finally:
            # Clean up argv file
            if argv_file and os.path.exists(argv_file):
                try:
                    os.unlink(argv_file)
                except:
                    pass
            self.disconnect()
    
    def send_signal(self, signum: int) -> bool:
        """
        Send signal to daemon
        
        Args:
            signum: Signal number to send
            
        Returns:
            True if signal sent successfully
        """
        if not self.connect():
            return False
        
        if not self.is_daemon_running():
            print("Daemon is not running")
            return False
        
        daemon_pid = self.get_daemon_pid()
        if daemon_pid is None:
            print("Failed to get daemon PID")
            return False
        
        # Prepare signal message to daemon
        msg = UnvmeMsg(
            msg_type=daemon_pid,
            argc=0,
            argv_file="",
            bdf="",
            pwd=os.getcwd(),
            pid=self.client_pid,
            signum=signum
        )
        
        try:
            self.msg_queue.send(msg.pack()[8:], type=daemon_pid)
            print(f"Sent signal {signum} to daemon")
            return True
        except Exception as e:
            print(f"Error sending signal to daemon: {e}")
            return False
        finally:
            self.disconnect()

def main():
    """Example usage of UnvmeMsgQClient"""
    if len(sys.argv) < 2:
        print("Usage:")
        print("  python3 unvme_msgq_client.py <command> [args...]")
        print("  python3 unvme_msgq_client.py --signal <signum>")
        print("")
        print("Examples:")
        print("  python3 unvme_msgq_client.py list")
        print("  python3 unvme_msgq_client.py id-ctrl --bdf 0000:01:00.0")
        print("  python3 unvme_msgq_client.py --signal 15  # Send SIGTERM")
        sys.exit(1)
    
    client = UnvmeMsgQClient(keyfile="/dev/mqueue/unvmed")
    
    # Check if daemon is running
    if not client.is_daemon_running():
        print("Error: unvmed daemon is not running")
        sys.exit(1)
    
    # Handle signal sending
    if sys.argv[1] == "--signal":
        if len(sys.argv) != 3:
            print("Usage: python3 unvme_msgq_client.py --signal <signum>")
            sys.exit(1)
        
        try:
            signum = int(sys.argv[2])
            if client.send_signal(signum):
                print("Signal sent successfully")
            else:
                print("Failed to send signal")
                sys.exit(1)
        except ValueError:
            print("Invalid signal number")
            sys.exit(1)
        return
    
    # Handle normal command
    argv = sys.argv[1:]
    
    # Extract BDF if provided
    bdf = ""
    if "--bdf" in argv:
        try:
            bdf_idx = argv.index("--bdf")
            if bdf_idx + 1 < len(argv):
                bdf = argv[bdf_idx + 1]
        except ValueError:
            pass
    
    start = time.time()
    breakpoint()
    ret = client.send_command(argv, bdf)
    end = time.time()
    print(end - start)

    if ret is not None:
        sys.exit(ret)
    else:
        print("Failed to execute command")
        sys.exit(1)

if __name__ == "__main__":
    main() 
