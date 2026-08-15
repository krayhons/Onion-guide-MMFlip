#ifndef INGAME_ACTION_H__
#define INGAME_ACTION_H__

/**
 * In-game menu button actions, as stored in config/keymap.json.
 *
 *   0  Off
 *   1  GameSwitcher
 *   2  Exit to menu
 *   3  Quick switch
 *   4  Quick menu
 *   5  Guide          <- added by the guide reader
 *
 * The number lives here so it is defined once. If upstream Onion ever adds
 * its own fifth action, this becomes 6 (or whatever is free) and three other
 * places follow automatically:
 *
 *   src/keymon/menuButtonAction.h   case INGAME_ACTION_GUIDE
 *   src/tweaks/menus.h              .value_max on the Double press item
 *   the add-on installer            via guide-action.txt, written at build time
 *
 * The remaining one is the label list, BUTTON_INGAME_LABELS in
 * src/tweaks/formatters.h, which is positional - "Guide" has to sit at this
 * index. A patch conflict there is the tripwire that tells you upstream has
 * taken the number.
 */
#define INGAME_ACTION_GUIDE 5

#endif // INGAME_ACTION_H__
