from __future__ import annotations

import ctypes
import ctypes.util
import os
import pathlib
import platform
from dataclasses import dataclass
from typing import Optional, Union


PathLike = Union[pathlib.Path, str]

WIN32_SCREENCAP_GDI = 1
WIN32_SCREENCAP_FRAME_POOL = 1 << 1
WIN32_SCREENCAP_DXGI_DESKTOP_DUP = 1 << 2
WIN32_SCREENCAP_DXGI_DESKTOP_DUP_WINDOW = 1 << 3
WIN32_SCREENCAP_PRINT_WINDOW = 1 << 4
WIN32_SCREENCAP_SCREEN_DC = 1 << 5


@dataclass(frozen=True)
class Screenshot:
    data: bytes
    width: int
    height: int
    channels: int

    @property
    def shape(self) -> tuple[int, ...]:
        if self.channels == 1:
            return (self.height, self.width)
        return (self.height, self.width, self.channels)

    @property
    def format(self) -> str:
        if self.channels == 1:
            return "gray"
        if self.channels == 3:
            return "bgr"
        if self.channels == 4:
            return "bgra"
        return "raw"

    def to_numpy(self, copy: bool = False):
        import numpy as np

        array = np.frombuffer(self.data, dtype=np.uint8).reshape(self.shape)
        return array.copy() if copy else array


class _Native:
    _lib = None
    _lib_path: Optional[pathlib.Path] = None
    _dll_dirs = []

    @classmethod
    def load(cls, path: Optional[PathLike] = None):
        if cls._lib is not None:
            return cls._lib

        lib_path = cls._resolve_library_path(path)
        if lib_path.parent != pathlib.Path("."):
            lib_path = lib_path.resolve()
            lib_dir = lib_path.parent
        else:
            lib_dir = pathlib.Path.cwd()

        if platform.system().lower() == "windows":
            if lib_dir.exists():
                os.environ["PATH"] = str(lib_dir) + os.pathsep + os.environ.get("PATH", "")
                if hasattr(os, "add_dll_directory"):
                    cls._dll_dirs.append(os.add_dll_directory(str(lib_dir)))
            cls._lib = ctypes.WinDLL(str(lib_path))
        else:
            cls._lib = ctypes.CDLL(str(lib_path))

        cls._lib_path = lib_path
        cls._set_properties()
        return cls._lib

    @staticmethod
    def _resolve_library_path(path: Optional[PathLike]) -> pathlib.Path:
        system = platform.system().lower()
        lib_names = {
            "windows": ("MaaScreencap.dll", "MaaCore.dll"),
            "darwin": ("libMaaScreencap.dylib", "libMaaCore.dylib"),
            "linux": ("libMaaScreencap.so", "libMaaCore.so"),
        }
        if system not in lib_names:
            raise RuntimeError(f"unsupported platform: {platform.system()}")
        preferred_name, fallback_name = lib_names[system]

        if path is None:
            found = ctypes.util.find_library("MaaScreencap") or ctypes.util.find_library("MaaCore")
            return pathlib.Path(found) if found else pathlib.Path(preferred_name)

        lib_path = pathlib.Path(path)
        if lib_path.is_dir():
            preferred = lib_path / preferred_name
            return preferred if preferred.exists() else lib_path / fallback_name
        return lib_path

    @classmethod
    def _set_properties(cls) -> None:
        cls._lib.AsstCreateMumuScreencap.restype = ctypes.c_void_p
        cls._lib.AsstCreateMumuScreencap.argtypes = (
            ctypes.c_char_p,
            ctypes.c_int32,
            ctypes.c_char_p,
        )

        if platform.system().lower() == "windows":
            cls._lib.AsstCreateWin32Screencap.restype = ctypes.c_void_p
            cls._lib.AsstCreateWin32Screencap.argtypes = (
                ctypes.c_void_p,
                ctypes.c_uint64,
                ctypes.c_char_p,
            )

        cls._lib.AsstDestroyScreencap.restype = None
        cls._lib.AsstDestroyScreencap.argtypes = (ctypes.c_void_p,)

        cls._lib.AsstScreencapCapture.restype = ctypes.c_bool
        cls._lib.AsstScreencapCapture.argtypes = (ctypes.c_void_p,)

        cls._lib.AsstGetScreencapImageInfo.restype = ctypes.c_bool
        cls._lib.AsstGetScreencapImageInfo.argtypes = (
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_int32),
            ctypes.POINTER(ctypes.c_int32),
            ctypes.POINTER(ctypes.c_int32),
            ctypes.POINTER(ctypes.c_uint64),
        )

        cls._lib.AsstGetScreencapImage.restype = ctypes.c_uint64
        cls._lib.AsstGetScreencapImage.argtypes = (
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_uint64,
        )


def load(path: Optional[PathLike] = None) -> None:
    _Native.load(path)


def _encode_path(path: PathLike) -> bytes:
    return str(pathlib.Path(path)).encode("utf-8")


def _encode_optional_path(path: Optional[PathLike], default_file: Optional[str] = None):
    if path is None:
        return None

    resolved = pathlib.Path(path)
    if default_file and resolved.is_dir():
        resolved = resolved / default_file
    return str(resolved).encode("utf-8")


class ScreencapBackend:
    def __init__(self, handle: int):
        if not handle:
            raise RuntimeError("failed to create screencap backend")
        self._handle = ctypes.c_void_p(handle)

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, traceback) -> None:
        self.close()

    def __del__(self):
        self.close()

    def close(self) -> None:
        handle = getattr(self, "_handle", None)
        if handle:
            if _Native._lib is not None:
                _Native._lib.AsstDestroyScreencap(handle)
            self._handle = None

    def capture(self) -> Screenshot:
        lib = _Native.load()
        if not lib.AsstScreencapCapture(self._handle):
            raise RuntimeError("screencap failed")

        width = ctypes.c_int32()
        height = ctypes.c_int32()
        channels = ctypes.c_int32()
        data_size = ctypes.c_uint64()

        if not lib.AsstGetScreencapImageInfo(
            self._handle,
            ctypes.byref(width),
            ctypes.byref(height),
            ctypes.byref(channels),
            ctypes.byref(data_size),
        ):
            raise RuntimeError("failed to query screencap image info")

        buffer = (ctypes.c_ubyte * data_size.value)()
        got = lib.AsstGetScreencapImage(self._handle, buffer, data_size.value)
        if got != data_size.value:
            raise RuntimeError("failed to copy screencap image data")

        return Screenshot(
            data=bytes(buffer),
            width=width.value,
            height=height.value,
            channels=channels.value,
        )


class MumuScreencapBackend(ScreencapBackend):
    def __init__(
        self,
        mumu_path: PathLike,
        index: int = 0,
        package_name: str = "default",
        library_path: Optional[PathLike] = None,
    ):
        lib = _Native.load(library_path)
        handle = lib.AsstCreateMumuScreencap(
            _encode_path(mumu_path),
            int(index),
            package_name.encode("utf-8") if package_name else None,
        )
        super().__init__(handle)


class WgcScreencapBackend(ScreencapBackend):
    def __init__(
        self,
        hwnd: int,
        library_path: Optional[PathLike] = None,
        control_unit_path: Optional[PathLike] = None,
    ):
        if platform.system().lower() != "windows":
            raise RuntimeError("WGC screencap backend is only available on Windows")

        lib = _Native.load(library_path)
        handle = lib.AsstCreateWin32Screencap(
            ctypes.c_void_p(int(hwnd)),
            WIN32_SCREENCAP_FRAME_POOL,
            _encode_optional_path(control_unit_path, "MaaWin32ControlUnit.dll"),
        )
        super().__init__(handle)
