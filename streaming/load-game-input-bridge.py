"""Load our locally built input transport into the specified CP launcher only."""
import ctypes
from ctypes import wintypes as W
from pathlib import Path
import sys

pid=int(sys.argv[1])
dll=Path(__file__).with_name('game-input-bridge.dll').resolve()
assert dll.is_file()
k=ctypes.WinDLL('kernel32',use_last_error=True)
k.OpenProcess.argtypes=[W.DWORD,W.BOOL,W.DWORD];k.OpenProcess.restype=W.HANDLE
k.QueryFullProcessImageNameW.argtypes=[W.HANDLE,W.DWORD,W.LPWSTR,ctypes.POINTER(W.DWORD)]
k.GetModuleHandleW.argtypes=[W.LPCWSTR];k.GetModuleHandleW.restype=W.HMODULE
k.GetProcAddress.argtypes=[W.HMODULE,ctypes.c_char_p];k.GetProcAddress.restype=ctypes.c_void_p
k.GetModuleHandleExW.argtypes=[W.DWORD,ctypes.c_void_p,ctypes.POINTER(W.HMODULE)]
k.GetModuleFileNameW.argtypes=[W.HMODULE,W.LPWSTR,W.DWORD]
k.CreateToolhelp32Snapshot.argtypes=[W.DWORD,W.DWORD];k.CreateToolhelp32Snapshot.restype=W.HANDLE
class MODULE(ctypes.Structure):
    _fields_=[('size',W.DWORD),('id',W.DWORD),('pid',W.DWORD),('globalUse',W.DWORD),('processUse',W.DWORD),('base',ctypes.c_void_p),('bytes',W.DWORD),('module',W.HMODULE),('name',W.WCHAR*256),('path',W.WCHAR*260)]
k.Module32FirstW.argtypes=[W.HANDLE,ctypes.POINTER(MODULE)]
k.Module32NextW.argtypes=[W.HANDLE,ctypes.POINTER(MODULE)]
k.VirtualAllocEx.argtypes=[W.HANDLE,ctypes.c_void_p,ctypes.c_size_t,W.DWORD,W.DWORD];k.VirtualAllocEx.restype=ctypes.c_void_p
k.WriteProcessMemory.argtypes=[W.HANDLE,ctypes.c_void_p,ctypes.c_void_p,ctypes.c_size_t,ctypes.POINTER(ctypes.c_size_t)]
k.CreateRemoteThread.argtypes=[W.HANDLE,ctypes.c_void_p,ctypes.c_size_t,ctypes.c_void_p,ctypes.c_void_p,W.DWORD,ctypes.POINTER(W.DWORD)];k.CreateRemoteThread.restype=W.HANDLE
k.WaitForSingleObject.argtypes=[W.HANDLE,W.DWORD]
k.VirtualFreeEx.argtypes=[W.HANDLE,ctypes.c_void_p,ctypes.c_size_t,W.DWORD]
k.CloseHandle.argtypes=[W.HANDLE]
process=k.OpenProcess(0x43a,False,pid)
if not process: raise ctypes.WinError(ctypes.get_last_error())
try:
    image=ctypes.create_unicode_buffer(32768);length=W.DWORD(len(image))
    if not k.QueryFullProcessImageNameW(process,0,image,ctypes.byref(length)): raise ctypes.WinError(ctypes.get_last_error())
    expected=Path(r'D:\Cyber punk\binaries\bin\x64_DLL.Release\cpLauncher.exe')
    if Path(image.value).resolve()!=expected.resolve(): raise RuntimeError('Target is not the expected local CP launcher')
    function=k.GetProcAddress(k.GetModuleHandleW('kernel32.dll'),b'LoadLibraryW')
    owner=W.HMODULE()
    if not k.GetModuleHandleExW(6,function,ctypes.byref(owner)): raise ctypes.WinError(ctypes.get_last_error())
    owner_path=ctypes.create_unicode_buffer(32768);k.GetModuleFileNameW(owner,owner_path,len(owner_path))
    name=Path(owner_path.value).name.lower()
    snapshot=k.CreateToolhelp32Snapshot(0x18,pid)
    remote_base=None
    try:
        entry=MODULE();entry.size=ctypes.sizeof(MODULE)
        valid=k.Module32FirstW(snapshot,ctypes.byref(entry))
        while valid:
            if entry.name.lower()==name: remote_base=entry.base;break
            valid=k.Module32NextW(snapshot,ctypes.byref(entry))
    finally:k.CloseHandle(snapshot)
    if not remote_base: raise RuntimeError('LoadLibrary module absent from target')
    address=remote_base+(function-owner.value)
    data=ctypes.create_string_buffer((str(dll)+'\0').encode('utf-16-le'))
    remote=k.VirtualAllocEx(process,None,len(data),0x3000,4)
    if not remote: raise ctypes.WinError(ctypes.get_last_error())
    copied=ctypes.c_size_t()
    if not k.WriteProcessMemory(process,remote,data,len(data),ctypes.byref(copied)) or copied.value!=len(data): raise ctypes.WinError(ctypes.get_last_error())
    thread=k.CreateRemoteThread(process,None,0,address,remote,0,None)
    if not thread: raise ctypes.WinError(ctypes.get_last_error())
    try:
        if k.WaitForSingleObject(thread,10000)!=0: raise RuntimeError('Bridge loader still running; do not repeat')
        k.VirtualFreeEx(process,remote,0,0x8000)
    finally:k.CloseHandle(thread)
    print('Local input bridge loaded into verified game PID',pid)
finally:k.CloseHandle(process)
