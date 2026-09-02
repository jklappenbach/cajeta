#!/usr/bin/env python3
"""Vendor-userspace-free NPU capability probe.
Path: open /dev/accel/N -> generic DRM_IOCTL_VERSION to identify driver
      -> driver-specific capability query. No XRT, no vendor runtime."""
import ctypes, fcntl, os, glob, struct

DRM_COMMAND_BASE = 0x40
def _iowr(nr, size): return (3 << 30) | (size << 16) | (0x64 << 8) | nr

class DrmVersion(ctypes.Structure):
    _fields_ = [("major", ctypes.c_int), ("minor", ctypes.c_int), ("patch", ctypes.c_int),
                ("name_len", ctypes.c_size_t), ("name", ctypes.c_char_p),
                ("date_len", ctypes.c_size_t), ("date", ctypes.c_char_p),
                ("desc_len", ctypes.c_size_t), ("desc", ctypes.c_char_p)]

class GetInfo(ctypes.Structure):
    _fields_ = [("param", ctypes.c_uint32), ("buffer_size", ctypes.c_uint32), ("buffer", ctypes.c_uint64)]

class TileMeta(ctypes.Structure):
    _fields_ = [("row_count", ctypes.c_uint16), ("row_start", ctypes.c_uint16),
                ("dma_channel_count", ctypes.c_uint16), ("lock_count", ctypes.c_uint16),
                ("event_reg_count", ctypes.c_uint16), ("pad", ctypes.c_uint16 * 3)]

class AieMeta(ctypes.Structure):
    _fields_ = [("col_size", ctypes.c_uint32), ("cols", ctypes.c_uint16), ("rows", ctypes.c_uint16),
                ("ver_major", ctypes.c_uint32), ("ver_minor", ctypes.c_uint32),
                ("core", TileMeta), ("mem", TileMeta), ("shim", TileMeta)]

def drm_identify(fd):
    v = DrmVersion(); fcntl.ioctl(fd, 0xc0406400, v)
    nb = ctypes.create_string_buffer(v.name_len + 1); db = ctypes.create_string_buffer(v.date_len + 1)
    sb = ctypes.create_string_buffer(v.desc_len + 1)
    v.name = ctypes.cast(nb, ctypes.c_char_p); v.date = ctypes.cast(db, ctypes.c_char_p)
    v.desc = ctypes.cast(sb, ctypes.c_char_p)
    fcntl.ioctl(fd, 0xc0406400, v)
    return nb.value.decode(), sb.value.decode(), (v.major, v.minor, v.patch)

def amdxdna_query(fd, param, out_struct):
    buf = out_struct()
    gi = GetInfo(param=param, buffer_size=ctypes.sizeof(buf),
                 buffer=ctypes.cast(ctypes.pointer(buf), ctypes.c_void_p).value)
    fcntl.ioctl(fd, _iowr(DRM_COMMAND_BASE + 7, ctypes.sizeof(GetInfo)), gi)  # 7 = GET_INFO
    return buf

for node in sorted(glob.glob("/dev/accel/accel*")):
    print(f"=== {node}")
    fd = os.open(node, os.O_RDWR)
    try:
        name, desc, ver = drm_identify(fd)
        print(f"  driver      : {name}  v{ver[0]}.{ver[1]}.{ver[2]}")
        print(f"  description : {desc}")
        if name.startswith("amdxdna"):
            m = amdxdna_query(fd, 1, AieMeta)   # 1 = QUERY_AIE_METADATA
            print(f"  AIE version : {m.ver_major}.{m.ver_minor}")
            print(f"  array       : {m.cols} cols x {m.rows} rows, col_size={m.col_size} B")
            for lbl, t in (("core", m.core), ("mem", m.mem), ("shim", m.shim)):
                print(f"    {lbl:5s} rows={t.row_count} start={t.row_start} "
                      f"dma_ch={t.dma_channel_count} locks={t.lock_count} events={t.event_reg_count}")
    except Exception as e:
        print(f"  probe failed: {e}")
    finally:
        os.close(fd)
