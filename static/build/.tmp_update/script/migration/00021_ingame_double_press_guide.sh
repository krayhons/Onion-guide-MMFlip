#!/bin/sh

# Point the in-game double press of the menu button at the new guide reader.
#
# Fresh installs pick this up from the built-in defaults. Existing installs
# only have a keymap.json if the user visited Tweaks, so migrate the value
# there - but only when it still holds the previous default (3 = Quick
# switch), so a deliberate choice is never overwritten.

KEYMAP=/mnt/SDCARD/.tmp_update/config/keymap.json
OLD_DEFAULT=3
GUIDE_ACTION=5

if [ ! -f "$KEYMAP" ]; then
    exit 0
fi

if grep -q "\"ingame_double_press\": $OLD_DEFAULT" "$KEYMAP"; then
    sed -i "s/\"ingame_double_press\": $OLD_DEFAULT/\"ingame_double_press\": $GUIDE_ACTION/" "$KEYMAP"
fi
