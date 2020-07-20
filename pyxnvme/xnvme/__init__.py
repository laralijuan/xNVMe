"""
    xNVMe libraries for Python

    Wrapping the shared version of xNVMe v.0.16
"""
import ctypes
import time
import sys
import os

XNVME_SHARED_LIB_FN = "libxnvme-shared.so"

CAPI = None
if CAPI is None:
    CAPI = ctypes.cdll.LoadLibrary(XNVME_SHARED_LIB_FN)

VERSION_MAJOR = CAPI.xnvme_ver_major()
VERSION_MINOR = CAPI.xnvme_ver_minor()
VERSION_PATCH = CAPI.xnvme_ver_patch()
VERSION = "%d.%d.%d" % (VERSION_MAJOR, VERSION_MINOR, VERSION_PATCH)

# pylint: disable=too-few-public-methods
class BackendAttributes(ctypes.Structure):
    """Encapsulation of backend attributes"""

    _fields_ = [
        ("name", ctypes.c_char * 8),
        ("scheme", ctypes.c_char * 7),
        ("enabled", ctypes.c_uint8)
    ]

# pylint: disable=too-few-public-methods
class BackendListing(ctypes.Structure):
    """Encapsulation of backend listing"""

    _fields_ = [
        ("capacity", ctypes.c_uint32),
        ("count", ctypes.c_int32),
        ("item", ctypes.POINTER(BackendAttributes)),
    ]

# pylint: disable=too-few-public-methods
class Ident(ctypes.Structure):
    """Encapsulation of 'xnvme_ident'"""

    _fields_ = [
        ("nsid", ctypes.c_uint32),
        ("nst", ctypes.c_uint8),
        ("type", ctypes.c_uint8),
        ("_pad0", ctypes.c_uint8),
        ("be_attr", BackendAttributes),
        ("_pad", ctypes.c_uint8 * 38),
        ("uri", ctypes.c_char * 320),
        ("be_uri", ctypes.c_char * 320)
    ]

# pylint: disable=too-few-public-methods
class Enumeration(ctypes.Structure):
    """Encapsulation of an enumeration result"""

    _fields_ = [
        ("capacity", ctypes.c_uint32),
        ("nentries", ctypes.c_uint32),
        ("entries", ctypes.POINTER(Ident)),
    ]
