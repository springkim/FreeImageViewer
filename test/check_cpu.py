import os
import platform
import subprocess
import json


def run(*args):
    try:
        return subprocess.check_output(args, text=True, stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def get_windows_cpu_name():
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r"HARDWARE\DESCRIPTION\System\CentralProcessor\0") as key:
            return winreg.QueryValueEx(key, "ProcessorNameString")[0].strip()
    except OSError:
        return platform.processor() or None


def get_windows_cpu_sets():
    import ctypes
    import struct
    func = ctypes.WinDLL("kernel32", use_last_error=True).GetSystemCpuSetInformation
    func.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_ulong), ctypes.c_void_p, ctypes.c_ulong]
    func.restype = ctypes.c_bool
    size = ctypes.c_ulong()
    func(None, 0, ctypes.byref(size), None, 0)
    if not size.value:
        return []
    buf = ctypes.create_string_buffer(size.value)
    if not func(buf, size.value, ctypes.byref(size), None, 0):
        raise ctypes.WinError(ctypes.get_last_error())
    data, cpus, offset = memoryview(buf.raw[:size.value]), [], 0
    while offset + 8 <= len(data):
        entry_size, entry_type = struct.unpack_from("<II", data, offset)
        if not entry_size:
            break
        if entry_type == 0 and entry_size >= 20:
            _, group, _, core, _, _, efficiency, _ = struct.unpack_from("<I H B B B B B B", data, offset + 8)
            cpus.append((group, core, efficiency))
        offset += entry_size
    return cpus


def get_windows_cpu_info():
    cpus = get_windows_cpu_sets()
    info = {"model": get_windows_cpu_name(), "cores": None, "threads": os.cpu_count(), "p_cores": None, "e_cores": None}
    if cpus:
        cores = {(group, core): efficiency for group, core, efficiency in cpus}
        info["cores"], info["threads"] = len(cores), len(cpus)
        if len(set(cores.values())) > 1:
            info["p_cores"] = sum(x == max(cores.values()) for x in cores.values())
            info["e_cores"] = len(cores) - info["p_cores"]
    return info


def sysctl(name):
    return run("sysctl", "-n", name)


def get_macos_cpu_info():
    model = sysctl("machdep.cpu.brand_string") or sysctl("hw.model")
    cores, threads = sysctl("hw.physicalcpu"), sysctl("hw.logicalcpu")
    info = {"model": model, "cores": int(cores) if cores else None,
            "threads": int(threads) if threads else os.cpu_count(), "p_cores": None, "e_cores": None}
    try:
        info["p_cores"], info["e_cores"] = int(sysctl("hw.perflevel0.physicalcpu")), int(
            sysctl("hw.perflevel1.physicalcpu"))
    except (TypeError, ValueError):
        pass
    return info


def read_text(path):
    try:
        with open(path, encoding="utf-8") as file:
            return file.read().strip()
    except OSError:
        return None


def get_linux_cpu_name():
    text = read_text("/proc/cpuinfo")
    if text:
        lines = text.splitlines()
        for key in ("model name", "Hardware", "Processor", "Model"):
            for line in lines:
                if line.startswith(key) and ":" in line:
                    return line.split(":", 1)[1].strip()
    return platform.processor() or None


def get_linux_topology():
    root, cores, types = "/sys/devices/system/cpu", set(), {}
    try:
        names = os.listdir(root)
    except OSError:
        return None, {}
    for name in names:
        if not (name.startswith("cpu") and name[3:].isdigit()):
            continue
        path = f"{root}/{name}/topology"
        core, package = read_text(f"{path}/core_id"), read_text(f"{path}/physical_package_id") or "0"
        if core is None:
            continue
        key = package, core
        cores.add(key)
        core_type = read_text(f"{path}/core_type")
        if core_type is not None:
            types[key] = int(core_type)
    return len(cores) or None, types


def get_linux_physical_cores():
    return get_linux_topology()[0]


def hybrid_counts(types):
    p, e = sum(x == 2 for x in types.values()), sum(x == 1 for x in types.values())
    return (p, e) if p or e else (None, None)


def get_linux_hybrid_cores():
    return hybrid_counts(get_linux_topology()[1])


def get_linux_cpu_info():
    cores, types = get_linux_topology()
    p, e = hybrid_counts(types)
    return {"model": get_linux_cpu_name(), "cores": cores, "threads": os.cpu_count(), "p_cores": p, "e_cores": e}


def get_cpu_info():
    function = {"Windows": get_windows_cpu_info, "Darwin": get_macos_cpu_info, "Linux": get_linux_cpu_info}.get(
        platform.system())
    return function() if function else {"model": platform.processor() or None, "cores": None, "threads": os.cpu_count(),
                                        "p_cores": None, "e_cores": None}


info = get_cpu_info()
print(f"CPU     : {info['model'] or 'Unknown'}")
print(f"Cores   : {info['cores'] or 'Unknown'}")
print(f"Threads : {info['threads'] or 'Unknown'}")
if info["p_cores"] is not None:
    print(f"P-Cores : {info['p_cores']}")
if info["e_cores"] is not None:
    print(f"E-Cores : {info['e_cores']}")
