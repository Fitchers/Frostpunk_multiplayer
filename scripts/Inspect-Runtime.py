"""Read-only native code inspection for the supported local Frostpunk build."""
import argparse
import ctypes as c
import re
import struct
import capstone
import pefile

p = argparse.ArgumentParser()
p.add_argument('pid', type=int)
p.add_argument('base', type=lambda s: int(s, 0))
p.add_argument('mode', choices=['strings', 'disasm', 'refs', 'calls', 'read', 'globals', 'session', 'weather'])
p.add_argument('target')
p.add_argument('--size', type=lambda s: int(s, 0), default=0x200)
a = p.parse_args()
k = c.WinDLL('kernel32', use_last_error=True)
k.OpenProcess.argtypes = [c.c_uint32, c.c_int, c.c_uint32]
k.OpenProcess.restype = c.c_void_p
k.ReadProcessMemory.argtypes = [c.c_void_p,c.c_void_p,c.c_void_p,c.c_size_t,c.POINTER(c.c_size_t)]
k.CloseHandle.argtypes = [c.c_void_p]
h = k.OpenProcess(0x410, False, a.pid)
def read(addr, size):
    b = c.create_string_buffer(size); n = c.c_size_t()
    if not k.ReadProcessMemory(h,addr,b,size,c.byref(n)) or n.value != size:
        raise RuntimeError('Read failed at '+hex(addr))
    return b.raw
try:
    pe = pefile.PE(data=read(a.base,0x1000), fast_load=True)
    if a.mode == 'weather':
        def ptr(address): return struct.unpack('<Q', read(address, 8))[0]
        weather = ptr(a.base + 0x2B687A0)
        print('weather_system', hex(weather), 'current_entry', hex(ptr(weather + 0x150)))
        timeline = ptr(weather + 0x118)
        count = struct.unpack('<i', read(weather + 0x120, 4))[0]
        print('timeline_count', count, 'index', struct.unpack('<i', read(weather + 0x148, 4))[0])
        if 0 <= count <= 256:
            for index in range(min(count, 12)):
                tick, entry = struct.unpack('<qQ', read(timeline + index * 16, 16))
                name = read(ptr(entry), 80).split(b'\0')[0].decode('ascii', errors='replace')
                print(index, 'ticks', tick, 'weather', name, 'guid', read(entry + 8, 16).hex())
    elif a.mode == 'session':
        controller = struct.unpack('<Q', read(a.base + 0x2B6A710, 8))[0]
        timer = struct.unpack('<Q', read(a.base + 0x2B68510, 8))[0]
        ticks = struct.unpack('<q', read(controller + 0xB0, 8))[0]
        scale = struct.unpack('<i', read(a.base + 0x2B70CC0, 4))[0]
        print('calendar_ms', int(ticks / 2147483648 * 3600000 / scale),
              'user_pause', read(controller + 0xE0, 1)[0],
              'loading', read(a.base + 0x2A602DD, 1)[0])
        reasons = struct.unpack('<Q', read(timer + 0xB8, 8))[0]
        count = struct.unpack('<i', read(timer + 0xC0, 4))[0]
        print('reason_count', count)
        if 0 <= count <= 128:
            for index in range(count):
                pointer = struct.unpack('<Q', read(reasons + index * 8, 8))[0]
                try:
                    print('reason', read(pointer, 80).split(b'\0')[0].decode('ascii', errors='replace'))
                except RuntimeError:
                    print('unreadable reason')
    elif a.mode == 'globals':
        start = int(a.target,0)
        block = read(a.base+start,a.size)
        for off in range(0,len(block)-7,8):
            obj = struct.unpack_from('<Q',block,off)[0]
            if not 0x10000 < obj < 0x7fffffffffff: continue
            try:
                vt = struct.unpack('<Q',read(obj,8))[0]
                if not a.base < vt < a.base+pe.OPTIONAL_HEADER.SizeOfImage: continue
                col = struct.unpack('<Q',read(vt-8,8))[0]
                td = struct.unpack('<I',read(col+12,4))[0]
                name = read(a.base+td+16,160).split(b'\0')[0].decode('ascii')
                if 'Frostpunk' in name or 'Timer' in name:
                    print(hex(start+off),hex(obj),'vtable',hex(vt-a.base),name)
            except (RuntimeError,UnicodeError): pass
    elif a.mode == 'read':
        addr = int(a.target,0)
        for off in range(0,a.size,16):
            b = read(addr+off,min(16,a.size-off))
            print(hex(addr+off), b.hex(' '))
    elif a.mode == 'disasm':
        addr = a.base+int(a.target,0)
        md = capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64)
        for i in md.disasm(read(addr,a.size),addr):
            print(hex(i.address-a.base),i.mnemonic,i.op_str)
    elif a.mode == 'strings':
        for s in pe.sections:
            if s.Name.startswith((b'.rdata',b'.data')):
                b = read(a.base+s.VirtualAddress,s.Misc_VirtualSize)
                for m in re.finditer(rb'[ -~]{5,}',b):
                    if re.search(a.target,m[0].decode(),re.I):
                        print(hex(s.VirtualAddress+m.start()),m[0].decode()[:200])
    elif a.mode == 'calls':
        target = a.base + int(a.target, 0)
        for s in pe.sections:
            if not s.Characteristics & 0x20000000: continue
            b = read(a.base+s.VirtualAddress,s.Misc_VirtualSize)
            for m in re.finditer(b'\xe8', b):
                off = m.start()
                if off + 5 <= len(b) and a.base+s.VirtualAddress+off+5+struct.unpack_from('<i',b,off+1)[0] == target:
                    print('possible direct call',hex(s.VirtualAddress+off))
    else:
        target = a.base+int(a.target,0)
        # RIP-relative displacements, then decode a bounded window to inspect hits.
        for s in pe.sections:
            if not s.Characteristics & 0x20000000: continue
            b = read(a.base+s.VirtualAddress,s.Misc_VirtualSize)
            for off in range(len(b)-4):
                disp = struct.unpack_from('<i',b,off)[0]
                if a.base+s.VirtualAddress+off+4+disp == target:
                    print('possible RIP ref',hex(s.VirtualAddress+off-3))
finally:
    k.CloseHandle(h)
