#!/usr/bin/env python3
import ctypes
import time


DISPLAY = b":0"
SCREEN_W = 1024
SCREEN_H = 600
FACE_W = 512
FACE_H = SCREEN_H
CHAT_X = 512
CHAT_W = SCREEN_W - CHAT_X
CHAT_H = 360
KEYBOARD_X = CHAT_X
KEYBOARD_Y = CHAT_H
KEYBOARD_W = CHAT_W
KEYBOARD_H = SCREEN_H - KEYBOARD_Y
KEYBOARD_CLIENT_H = 240


class XWindowAttributes(ctypes.Structure):
    _fields_ = [
        ("x", ctypes.c_int),
        ("y", ctypes.c_int),
        ("width", ctypes.c_int),
        ("height", ctypes.c_int),
        ("border_width", ctypes.c_int),
        ("depth", ctypes.c_int),
        ("visual", ctypes.c_void_p),
        ("root", ctypes.c_ulong),
        ("class", ctypes.c_int),
        ("bit_gravity", ctypes.c_int),
        ("win_gravity", ctypes.c_int),
        ("backing_store", ctypes.c_int),
        ("backing_planes", ctypes.c_ulong),
        ("backing_pixel", ctypes.c_ulong),
        ("save_under", ctypes.c_int),
        ("colormap", ctypes.c_ulong),
        ("map_installed", ctypes.c_int),
        ("map_state", ctypes.c_int),
        ("all_event_masks", ctypes.c_long),
        ("your_event_mask", ctypes.c_long),
        ("do_not_propagate_mask", ctypes.c_long),
        ("override_redirect", ctypes.c_int),
        ("screen", ctypes.c_void_p),
    ]


libx11 = ctypes.cdll.LoadLibrary("libX11.so.6")
libx11.XOpenDisplay.argtypes = [ctypes.c_char_p]
libx11.XOpenDisplay.restype = ctypes.c_void_p
libx11.XDefaultRootWindow.argtypes = [ctypes.c_void_p]
libx11.XDefaultRootWindow.restype = ctypes.c_ulong
libx11.XQueryTree.argtypes = [
    ctypes.c_void_p,
    ctypes.c_ulong,
    ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.c_ulong),
    ctypes.POINTER(ctypes.POINTER(ctypes.c_ulong)),
    ctypes.POINTER(ctypes.c_uint),
]
libx11.XQueryTree.restype = ctypes.c_int
libx11.XFetchName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.POINTER(ctypes.c_char_p)]
libx11.XFetchName.restype = ctypes.c_int
libx11.XFree.argtypes = [ctypes.c_void_p]
libx11.XMoveResizeWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_int, ctypes.c_int, ctypes.c_uint, ctypes.c_uint]
libx11.XRaiseWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
libx11.XLowerWindow.argtypes = [ctypes.c_void_p, ctypes.c_ulong]
libx11.XFlush.argtypes = [ctypes.c_void_p]


def fetch_name(dpy, win):
    name = ctypes.c_char_p()
    if libx11.XFetchName(dpy, win, ctypes.byref(name)) and name.value:
        value = name.value.decode("utf-8", "replace")
        libx11.XFree(name)
        return value
    return ""


def children(dpy, win):
    root = ctypes.c_ulong()
    parent = ctypes.c_ulong()
    kids = ctypes.POINTER(ctypes.c_ulong)()
    n = ctypes.c_uint()
    if not libx11.XQueryTree(dpy, win, ctypes.byref(root), ctypes.byref(parent), ctypes.byref(kids), ctypes.byref(n)):
        return []
    result = [kids[i] for i in range(n.value)]
    if kids:
        libx11.XFree(kids)
    return result


def find_named_with_root_parent(dpy, root, wanted):
    stack = [(root, root)]
    while stack:
        win, top = stack.pop()
        for child in children(dpy, win):
            child_top = child if win == root else top
            name = fetch_name(dpy, child)
            if wanted in name:
                return child, child_top
            stack.append((child, child_top))
    return None, None


def main():
    dpy = libx11.XOpenDisplay(DISPLAY)
    if not dpy:
        raise SystemExit("cannot open display")
    root = libx11.XDefaultRootWindow(dpy)
    for _ in range(20):
        face, face_top = find_named_with_root_parent(dpy, root, "Face Recognition")
        chat, chat_top = find_named_with_root_parent(dpy, root, "本地大模型")
        keyboard, keyboard_top = find_named_with_root_parent(dpy, root, "Keyboard")
        if face:
            libx11.XMoveResizeWindow(dpy, face, 0, 0, FACE_W, FACE_H)
        if face_top:
            libx11.XRaiseWindow(dpy, face_top)
        if chat_top:
            libx11.XMoveResizeWindow(dpy, chat_top, CHAT_X, 0, CHAT_W, CHAT_H)
            libx11.XRaiseWindow(dpy, chat_top)
        if keyboard:
            libx11.XMoveResizeWindow(dpy, keyboard, 0, 0, KEYBOARD_W, KEYBOARD_CLIENT_H)
        if keyboard_top:
            libx11.XMoveResizeWindow(dpy, keyboard_top, KEYBOARD_X, KEYBOARD_Y, KEYBOARD_W, KEYBOARD_CLIENT_H)
            libx11.XRaiseWindow(dpy, keyboard_top)
        libx11.XFlush(dpy)
        if face_top and chat_top and keyboard_top:
            return
        time.sleep(0.5)
    return


if __name__ == "__main__":
    main()
