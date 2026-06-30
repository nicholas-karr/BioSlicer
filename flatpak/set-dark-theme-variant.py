#!/usr/bin/env python3

import time
import subprocess
import os
import sys


def set_theme_variant_by_window_id(id, variant):
    try:
        s = subprocess.call(['xprop', '-f', '_GTK_THEME_VARIANT', '8u', '-set', '_GTK_THEME_VARIANT', variant, '-id', str(id)],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if s == 0:
            return True
        return False
    except Exception as ex:
        return False


def set_theme_variant_from_win_id_collection(win_id_collection, variant):
    for win_id in win_id_collection:
        set_theme_variant_by_window_id(win_id, variant)


def collection_win_id_from_wm_class_name(disp, root, net_client_list, win_class_name):
    import Xlib.X
    import Xlib.error

    collect = []

    for win_id in root.get_full_property(net_client_list, Xlib.X.AnyPropertyType).value:
        try:
            win = disp.create_resource_object('window', win_id)
            if not win.get_wm_transient_for():
                win_class = win.get_wm_class()
                if win_id and win_class_name in win_class:
                    collect.append(
                        win_id) if win_id not in collect else collect
        except Xlib.error.BadWindow:
            pass

    return collect


if __name__ == '__main__':

    if os.environ.get('PRUSA_SLICER_DARK_THEME', 'false') != 'true':
        sys.exit(0)

    try:
        import Xlib
        import Xlib.display
    except ImportError:
        sys.exit(0)

    disp = Xlib.display.Display()
    root = disp.screen().root

    NET_CLIENT_LIST = disp.intern_atom('_NET_CLIENT_LIST')

    root.change_attributes(event_mask=Xlib.X.PropertyChangeMask)
    win_class_name = 'prusa-slicer'
    variant = 'dark'

    start = time.time()

    while True:
        collect = collection_win_id_from_wm_class_name(disp, root, NET_CLIENT_LIST, win_class_name)
        if time.time() - start <= 2:
            disp.next_event()
            set_theme_variant_from_win_id_collection(collect, variant)
        else:
            break
