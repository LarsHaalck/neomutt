/**
 * @file
 * Define wrapper functions around Curses
 *
 * @authors
 * Copyright (C) 1996-2000,2012 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2004 g10 Code GmbH
 * Copyright (C) 2019 Richard Russon <rich@flatcap.org>
 *
 * @copyright
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * @page gui_curses Wrapper around Curses
 *
 * Wrapper functions around Curses
 */

#include "config.h"
#include "mutt_curses.h"
#include "color/lib.h"

/**
 * mutt_curses_set_attr - Set the attributes for text
 * @param attr Attributes to set, e.g. A_UNDERLINE
 */
void mutt_curses_set_attr(int attr)
{
  bkgdset(attr | ' ');
}

/**
 * mutt_curses_set_color_by_id - Set the current colour for text
 * @param color Colour to set, e.g. #MT_COLOR_HEADER
 *
 * If the system has bkgdset() use it rather than attrset() so that the clr*()
 * functions will properly set the background attributes all the way to the
 * right column.
 */
void mutt_curses_set_color_by_id(enum ColorId color)
{
  const int chosen = simple_colors_get(color);
  const int normal = simple_colors_get(MT_COLOR_NORMAL);
  bkgdset((chosen ? chosen : normal) | ' ');
}

/**
 * mutt_curses_set_cursor - Set the cursor state
 * @param state State to set, e.g. #MUTT_CURSOR_INVISIBLE
 */
void mutt_curses_set_cursor(enum MuttCursorState state)
{
  static int SavedCursor = MUTT_CURSOR_VISIBLE;

  if (state == MUTT_CURSOR_RESTORE_LAST)
    state = SavedCursor;
  else
    SavedCursor = state;

  if (curs_set(state) == ERR)
  {
    if (state == MUTT_CURSOR_VISIBLE)
      curs_set(MUTT_CURSOR_VERY_VISIBLE);
  }
}
