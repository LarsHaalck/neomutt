/**
 * @file
 * Sidebar Window
 *
 * @authors
 * Copyright (C) 2004 Justin Hibbits <jrh29@po.cwru.edu>
 * Copyright (C) 2004 Thomer M. Gil <mutt@thomer.com>
 * Copyright (C) 2015-2020 Richard Russon <rich@flatcap.org>
 * Copyright (C) 2016-2017 Kevin J. McCarthy <kevin@8t8.us>
 * Copyright (C) 2020 R Primus <rprimus@gmail.com>
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
 * @page sidebar_window Sidebar Window
 *
 * ## Overview
 *
 * The Sidebar Window is an interactive window that displays a list of
 * mailboxes to the user.
 *
 * ## Windows
 *
 * | Name           | Type       | See Also          |
 * | :------------- | :--------- | :---------------- |
 * | Sidebar Window | WT_SIDEBAR | mutt_window_new() |
 *
 * **Parent**
 * - @ref index_dialog
 *
 * **Children**
 *
 * None.
 *
 * ## Data
 * - #SidebarWindowData
 *
 * The Sidebar Window stores its data (#SidebarWindowData) in MuttWindow::wdata.
 *
 * ## Events
 *
 * Once constructed, it is controlled by the following events:
 *
 * | Event Type            | Handler               |
 * | :-------------------- | :-------------------- |
 * | #NT_ACCOUNT           | sb_account_observer() |
 * | #NT_COLOR             | sb_color_observer()   |
 * | #NT_COMMAND           | sb_command_observer() |
 * | #NT_CONFIG            | sb_config_observer()  |
 * | #NT_INDEX             | sb_index_observer()   |
 * | #NT_MAILBOX           | sb_mailbox_observer() |
 * | #NT_WINDOW            | sb_window_observer()  |
 * | MuttWindow::recalc()  | sb_recalc()           |
 * | MuttWindow::repaint() | sb_repaint()          |
 */

#include "config.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "private.h"
#include "mutt/lib.h"
#include "config/lib.h"
#include "email/lib.h"
#include "core/lib.h"
#include "gui/lib.h"
#include "color/lib.h"
#include "index/lib.h"
#include "format_flags.h"
#include "muttlib.h"

/**
 * struct SidebarFormatData - Data passed to sidebar_format_str()
 */
struct SidebarFormatData
{
  struct SbEntry *entry;          ///< Info about a folder
  struct IndexSharedData *shared; ///< Shared Index Data
};

/**
 * imap_is_prefix - Check if folder matches the beginning of mbox
 * @param folder Folder
 * @param mbox   Mailbox path
 * @retval num Length of the prefix
 */
static int imap_is_prefix(const char *folder, const char *mbox)
{
  int plen = 0;

  struct Url *url_m = url_parse(mbox);
  struct Url *url_f = url_parse(folder);
  if (!url_m || !url_f)
    goto done;

  if (!mutt_istr_equal(url_m->host, url_f->host))
    goto done;

  if (url_m->user && url_f->user && !mutt_istr_equal(url_m->user, url_f->user))
    goto done;

  size_t mlen = mutt_str_len(url_m->path);
  size_t flen = mutt_str_len(url_f->path);
  if (flen > mlen)
    goto done;

  if (!mutt_strn_equal(url_m->path, url_f->path, flen))
    goto done;

  plen = strlen(mbox) - mlen + flen;

done:
  url_free(&url_m);
  url_free(&url_f);

  return plen;
}

/**
 * abbrev_folder - Abbreviate a Mailbox path using a folder
 * @param mbox   Mailbox path to shorten
 * @param folder Folder path to use
 * @param type   Mailbox type
 * @retval ptr Pointer into the mbox param
 */
static const char *abbrev_folder(const char *mbox, const char *folder, enum MailboxType type)
{
  if (!mbox || !folder)
    return NULL;

  if (type == MUTT_IMAP)
  {
    int prefix = imap_is_prefix(folder, mbox);
    if (prefix == 0)
      return NULL;
    return mbox + prefix;
  }

  const char *const c_sidebar_delim_chars =
      cs_subset_string(NeoMutt->sub, "sidebar_delim_chars");
  if (!c_sidebar_delim_chars)
    return NULL;

  size_t flen = mutt_str_len(folder);
  if (flen == 0)
    return NULL;
  if (strchr(c_sidebar_delim_chars, folder[flen - 1])) // folder ends with a delimiter
    flen--;

  size_t mlen = mutt_str_len(mbox);
  if (mlen < flen)
    return NULL;

  if (!mutt_strn_equal(folder, mbox, flen))
    return NULL;

  // After the match, check that mbox has a delimiter
  if (!strchr(c_sidebar_delim_chars, mbox[flen]))
    return NULL;

  if (mlen > flen)
  {
    return mbox + flen + 1;
  }

  // mbox and folder are equal, use the chunk after the last delimiter
  while (mlen--)
  {
    if (strchr(c_sidebar_delim_chars, mbox[mlen]))
    {
      return mbox + mlen + 1;
    }
  }

  return NULL;
}

/**
 * abbrev_url - Abbreviate a url-style Mailbox path
 * @param mbox Mailbox path to shorten
 * @param type Mailbox type
 *
 * Use heuristics to shorten a non-local Mailbox path.
 * Strip the host part (or database part for Notmuch).
 *
 * e.g.
 * - `imap://user@host.com/apple/banana` becomes `apple/banana`
 * - `notmuch:///home/user/db?query=hello` becomes `query=hello`
 */
static const char *abbrev_url(const char *mbox, enum MailboxType type)
{
  /* This is large enough to skip `notmuch://`,
   * but not so large that it will go past the host part. */
  const int scheme_len = 10;

  size_t len = mutt_str_len(mbox);
  if ((len < scheme_len) || ((type != MUTT_NNTP) && (type != MUTT_IMAP) &&
                             (type != MUTT_NOTMUCH) && (type != MUTT_POP)))
  {
    return mbox;
  }

  const char split = (type == MUTT_NOTMUCH) ? '?' : '/';

  // Skip over the scheme, e.g. `imaps://`, `notmuch://`
  const char *last = strchr(mbox + scheme_len, split);
  if (last)
    mbox = last + 1;
  return mbox;
}

/**
 * add_indent - Generate the needed indentation
 * @param buf    Output buffer
 * @param buflen Size of output buffer
 * @param sbe    Sidebar entry
 * @retval Number of bytes written
 */
static size_t add_indent(char *buf, size_t buflen, const struct SbEntry *sbe)
{
  size_t res = 0;
  const char *const c_sidebar_indent_string =
      cs_subset_string(NeoMutt->sub, "sidebar_indent_string");
  for (int i = 0; i < sbe->depth; i++)
  {
    res += mutt_str_copy(buf + res, c_sidebar_indent_string, buflen - res);
  }
  return res;
}

/**
 * calc_color - Calculate the colour of a Sidebar row
 * @param m         Mailbox
 * @param current   true, if this is the current Mailbox
 * @param highlight true, if this Mailbox has the highlight on it
 * @retval num ColorId, e.g. #MT_COLOR_SIDEBAR_NEW
 */
static enum ColorId calc_color(const struct Mailbox *m, bool current, bool highlight)
{
  if (current)
  {
    if (simple_color_is_set(MT_COLOR_SIDEBAR_INDICATOR))
      return MT_COLOR_SIDEBAR_INDICATOR;
    return MT_COLOR_INDICATOR;
  }

  if (highlight)
    return MT_COLOR_SIDEBAR_HIGHLIGHT;

  if (m->has_new)
    return MT_COLOR_SIDEBAR_NEW;
  if (m->msg_unread > 0)
    return MT_COLOR_SIDEBAR_UNREAD;
  if (m->msg_flagged > 0)
    return MT_COLOR_SIDEBAR_FLAGGED;

  const char *const c_spool_file = cs_subset_string(NeoMutt->sub, "spool_file");
  if (simple_color_is_set(MT_COLOR_SIDEBAR_SPOOLFILE) &&
      mutt_str_equal(mailbox_path(m), c_spool_file))
  {
    return MT_COLOR_SIDEBAR_SPOOLFILE;
  }

  if (simple_color_is_set(MT_COLOR_SIDEBAR_ORDINARY))
    return MT_COLOR_SIDEBAR_ORDINARY;

  return MT_COLOR_NORMAL;
}

/**
 * calc_path_depth - Calculate the depth of a Mailbox path
 * @param[in]  mbox      Mailbox path to examine
 * @param[in]  delims    Delimiter characters
 * @param[out] last_part Last path component
 */
static int calc_path_depth(const char *mbox, const char *delims, const char **last_part)
{
  if (!mbox || !delims || !last_part)
    return 0;

  int depth = 0;
  const char *match = NULL;
  while ((match = strpbrk(mbox, delims)))
  {
    depth++;
    mbox = match + 1;
  }

  *last_part = mbox;
  return depth;
}

/**
 * sidebar_format_str - Format a string for the sidebar - Implements ::format_t - @ingroup expando_api
 *
 * | Expando | Description
 * |:--------|:--------------------------------------------------------
 * | \%!     | 'n!' Flagged messages
 * | \%B     | Name of the mailbox
 * | \%D     | Description of the mailbox
 * | \%d     | Number of deleted messages
 * | \%F     | Number of Flagged messages in the mailbox
 * | \%L     | Number of messages after limiting
 * | \%n     | 'N' if mailbox has new mail, ' ' (space) otherwise
 * | \%N     | Number of unread messages in the mailbox
 * | \%o     | Number of old unread messages in the mailbox
 * | \%r     | Number of read messages in the mailbox
 * | \%S     | Size of mailbox (total number of messages)
 * | \%t     | Number of tagged messages
 * | \%Z     | Number of new unseen messages in the mailbox
 */
static const char *sidebar_format_str(char *buf, size_t buflen, size_t col, int cols,
                                      char op, const char *src, const char *prec,
                                      const char *if_str, const char *else_str,
                                      intptr_t data, MuttFormatFlags flags)
{
  struct SidebarFormatData *sfdata = (struct SidebarFormatData *) data;
  struct SbEntry *sbe = sfdata->entry;
  struct IndexSharedData *shared = sfdata->shared;
  char fmt[256];

  if (!sbe || !shared || !buf)
    return src;

  buf[0] = '\0'; /* Just in case there's nothing to do */

  struct Mailbox *m = sbe->mailbox;
  if (!m)
    return src;

  struct Mailbox *m_cur = shared->mailbox;

  bool c = m_cur && mutt_str_equal(m_cur->realpath, m->realpath);

  bool optional = (flags & MUTT_FORMAT_OPTIONAL);

  switch (op)
  {
    case 'B':
    case 'D':
    {
      char indented[256] = { 0 };
      size_t ilen = sizeof(indented);
      size_t off = add_indent(indented, ilen, sbe);
      snprintf(indented + off, ilen - off, "%s",
               ((op == 'D') && sbe->mailbox->name) ? sbe->mailbox->name : sbe->box);
      mutt_format_s(buf, buflen, prec, indented);
      break;
    }

    case 'd':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, c ? m_cur->msg_deleted : 0);
      }
      else if ((c && (m_cur->msg_deleted == 0)) || !c)
        optional = false;
      break;

    case 'F':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, m->msg_flagged);
      }
      else if (m->msg_flagged == 0)
        optional = false;
      break;

    case 'L':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, c ? m_cur->vcount : m->msg_count);
      }
      else if ((c && (m_cur->vcount == m->msg_count)) || !c)
        optional = false;
      break;

    case 'N':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, m->msg_unread);
      }
      else if (m->msg_unread == 0)
        optional = false;
      break;

    case 'n':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sc", prec);
        snprintf(buf, buflen, fmt, m->has_new ? 'N' : ' ');
      }
      else if (m->has_new == false)
        optional = false;
      break;

    case 'o':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, m->msg_unread - m->msg_new);
      }
      else if ((c && (m_cur->msg_unread - m_cur->msg_new) == 0) || !c)
        optional = false;
      break;

    case 'r':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, m->msg_count - m->msg_unread);
      }
      else if ((c && (m_cur->msg_count - m_cur->msg_unread) == 0) || !c)
        optional = false;
      break;

    case 'S':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, m->msg_count);
      }
      else if (m->msg_count == 0)
        optional = false;
      break;

    case 't':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, c ? m_cur->msg_tagged : 0);
      }
      else if ((c && (m_cur->msg_tagged == 0)) || !c)
        optional = false;
      break;

    case 'Z':
      if (!optional)
      {
        snprintf(fmt, sizeof(fmt), "%%%sd", prec);
        snprintf(buf, buflen, fmt, m->msg_new);
      }
      else if ((c && (m_cur->msg_new) == 0) || !c)
        optional = false;
      break;

    case '!':
      if (m->msg_flagged == 0)
        mutt_format_s(buf, buflen, prec, "");
      else if (m->msg_flagged == 1)
        mutt_format_s(buf, buflen, prec, "!");
      else if (m->msg_flagged == 2)
        mutt_format_s(buf, buflen, prec, "!!");
      else
      {
        snprintf(fmt, sizeof(fmt), "%d!", m->msg_flagged);
        mutt_format_s(buf, buflen, prec, fmt);
      }
      break;
  }

  if (optional)
  {
    mutt_expando_format(buf, buflen, col, cols, if_str, sidebar_format_str, data, flags);
  }
  else if (flags & MUTT_FORMAT_OPTIONAL)
  {
    mutt_expando_format(buf, buflen, col, cols, else_str, sidebar_format_str, data, flags);
  }

  /* We return the format string, unchanged */
  return src;
}

/**
 * make_sidebar_entry - Turn mailbox data into a sidebar string
 * @param[out] buf     Buffer in which to save string
 * @param[in]  buflen  Buffer length
 * @param[in]  width   Desired width in screen cells
 * @param[in]  sbe     Mailbox object
 * @param[in]  shared  Shared Index Data
 *
 * Take all the relevant mailbox data and the desired screen width and then get
 * mutt_expando_format to do the actual work. mutt_expando_format will callback to
 * us using sidebar_format_str() for the sidebar specific formatting characters.
 */
static void make_sidebar_entry(char *buf, size_t buflen, int width,
                               struct SbEntry *sbe, struct IndexSharedData *shared)
{
  struct SidebarFormatData data = { sbe, shared };

  const char *const c_sidebar_format =
      cs_subset_string(NeoMutt->sub, "sidebar_format");
  mutt_expando_format(buf, buflen, 0, width, NONULL(c_sidebar_format),
                      sidebar_format_str, (intptr_t) &data, MUTT_FORMAT_NO_FLAGS);

  /* Force string to be exactly the right width */
  int w = mutt_strwidth(buf);
  int s = mutt_str_len(buf);
  width = MIN(buflen, width);
  if (w < width)
  {
    /* Pad with spaces */
    memset(buf + s, ' ', width - w);
    buf[s + width - w] = '\0';
  }
  else if (w > width)
  {
    /* Truncate to fit */
    size_t len = mutt_wstr_trunc(buf, buflen, width, NULL);
    buf[len] = '\0';
  }
}

/**
 * update_entries_visibility - Should a SbEntry be displayed in the sidebar?
 *
 * For each SbEntry in the entries array, check whether we should display it.
 * This is determined by several criteria.  If the Mailbox:
 * * is the currently open mailbox
 * * is the currently highlighted mailbox
 * * has unread messages
 * * has flagged messages
 * * is whitelisted
 */
static void update_entries_visibility(struct SidebarWindowData *wdata)
{
  /* Aliases for readability */
  const bool c_sidebar_new_mail_only =
      cs_subset_bool(NeoMutt->sub, "sidebar_new_mail_only");
  const bool c_sidebar_non_empty_mailbox_only =
      cs_subset_bool(NeoMutt->sub, "sidebar_non_empty_mailbox_only");
  struct SbEntry *sbe = NULL;

  struct IndexSharedData *shared = wdata->shared;
  struct SbEntry **sbep = NULL;
  ARRAY_FOREACH(sbep, &wdata->entries)
  {
    int i = ARRAY_FOREACH_IDX;
    sbe = *sbep;

    sbe->is_hidden = false;

    if (sbe->mailbox->flags & MB_HIDDEN)
    {
      sbe->is_hidden = true;
      continue;
    }

    if (shared->mailbox &&
        mutt_str_equal(sbe->mailbox->realpath, shared->mailbox->realpath))
    {
      /* Spool directories are always visible */
      continue;
    }

    if (mutt_list_find(&SidebarWhitelist, mailbox_path(sbe->mailbox)) ||
        mutt_list_find(&SidebarWhitelist, sbe->mailbox->name))
    {
      /* Explicitly asked to be visible */
      continue;
    }

    if (c_sidebar_non_empty_mailbox_only && (i != wdata->opn_index) &&
        (sbe->mailbox->msg_count == 0))
    {
      sbe->is_hidden = true;
    }

    if (c_sidebar_new_mail_only && (i != wdata->opn_index) &&
        (sbe->mailbox->msg_unread == 0) && (sbe->mailbox->msg_flagged == 0) &&
        !sbe->mailbox->has_new)
    {
      sbe->is_hidden = true;
    }
  }
}

/**
 * prepare_sidebar - Prepare the list of SbEntry's for the sidebar display
 * @param wdata     Sidebar data
 * @param page_size Number of lines on a page
 * @retval false No, don't draw the sidebar
 * @retval true  Yes, draw the sidebar
 *
 * Before painting the sidebar, we determine which are visible, sort
 * them and set up our page pointers.
 *
 * This is a lot of work to do each refresh, but there are many things that
 * can change outside of the sidebar that we don't hear about.
 */
static bool prepare_sidebar(struct SidebarWindowData *wdata, int page_size)
{
  if (ARRAY_EMPTY(&wdata->entries) || (page_size <= 0))
    return false;

  struct SbEntry **sbep = NULL;
  const bool c_sidebar_new_mail_only =
      cs_subset_bool(NeoMutt->sub, "sidebar_new_mail_only");
  const bool c_sidebar_non_empty_mailbox_only =
      cs_subset_bool(NeoMutt->sub, "sidebar_non_empty_mailbox_only");

  sbep = (wdata->opn_index >= 0) ? ARRAY_GET(&wdata->entries, wdata->opn_index) : NULL;
  const struct SbEntry *opn_entry = sbep ? *sbep : NULL;
  sbep = (wdata->hil_index >= 0) ? ARRAY_GET(&wdata->entries, wdata->hil_index) : NULL;
  const struct SbEntry *hil_entry = sbep ? *sbep : NULL;

  update_entries_visibility(wdata);
  const short c_sidebar_sort_method =
      cs_subset_sort(NeoMutt->sub, "sidebar_sort_method");
  sb_sort_entries(wdata, c_sidebar_sort_method);

  if (opn_entry || hil_entry)
  {
    ARRAY_FOREACH(sbep, &wdata->entries)
    {
      if ((opn_entry == *sbep) && ((*sbep)->mailbox->flags != MB_HIDDEN))
        wdata->opn_index = ARRAY_FOREACH_IDX;
      if ((hil_entry == *sbep) && ((*sbep)->mailbox->flags != MB_HIDDEN))
        wdata->hil_index = ARRAY_FOREACH_IDX;
    }
  }

  if ((wdata->hil_index < 0) || (hil_entry && hil_entry->is_hidden) ||
      (c_sidebar_sort_method != wdata->previous_sort))
  {
    if (wdata->opn_index >= 0)
      wdata->hil_index = wdata->opn_index;
    else
    {
      wdata->hil_index = 0;
      /* Note is_hidden will only be set when `$sidebar_new_mail_only` */
      if ((*ARRAY_GET(&wdata->entries, 0))->is_hidden && !select_next(wdata))
        wdata->hil_index = -1;
    }
  }

  /* Set the Top and Bottom to frame the wdata->hil_index in groups of page_size */

  /* If `$sidebar_new_mail_only` or `$sidebar_non_empty_mailbox_only` is set,
   * some entries may be hidden so we need to scan for the framing interval */
  if (c_sidebar_new_mail_only || c_sidebar_non_empty_mailbox_only)
  {
    wdata->top_index = -1;
    wdata->bot_index = -1;
    while (wdata->bot_index < wdata->hil_index)
    {
      wdata->top_index = wdata->bot_index + 1;
      int page_entries = 0;
      while (page_entries < page_size)
      {
        wdata->bot_index++;
        if (wdata->bot_index >= ARRAY_SIZE(&wdata->entries))
          break;
        if (!(*ARRAY_GET(&wdata->entries, wdata->bot_index))->is_hidden)
          page_entries++;
      }
    }
  }
  /* Otherwise we can just calculate the interval */
  else
  {
    wdata->top_index = (wdata->hil_index / page_size) * page_size;
    wdata->bot_index = wdata->top_index + page_size - 1;
  }

  if (wdata->bot_index > (ARRAY_SIZE(&wdata->entries) - 1))
    wdata->bot_index = ARRAY_SIZE(&wdata->entries) - 1;

  wdata->previous_sort = c_sidebar_sort_method;

  return (wdata->hil_index >= 0);
}

/**
 * sb_recalc - Recalculate the Sidebar display - Implements MuttWindow::recalc() - @ingroup window_recalc
 */
int sb_recalc(struct MuttWindow *win)
{
  if (!mutt_window_is_visible(win))
    return 0;

  struct SidebarWindowData *wdata = sb_wdata_get(win);
  struct IndexSharedData *shared = wdata->shared;

  if (ARRAY_EMPTY(&wdata->entries))
  {
    struct MailboxList ml = STAILQ_HEAD_INITIALIZER(ml);
    neomutt_mailboxlist_get_all(&ml, NeoMutt, MUTT_MAILBOX_ANY);
    struct MailboxNode *np = NULL;
    STAILQ_FOREACH(np, &ml, entries)
    {
      if (!(np->mailbox->flags & MB_HIDDEN))
        sb_add_mailbox(wdata, np->mailbox);
    }
    neomutt_mailboxlist_clear(&ml);
  }

  if (!prepare_sidebar(wdata, win->state.rows))
  {
    win->actions |= WA_REPAINT;
    return 0;
  }

  int num_rows = win->state.rows;
  int num_cols = win->state.cols;

  if (ARRAY_EMPTY(&wdata->entries) || (num_rows <= 0))
    return 0;

  if (wdata->top_index < 0)
    return 0;

  int width = num_cols - wdata->divider_width;
  int row = 0;
  struct Mailbox *m_cur = shared->mailbox;
  struct SbEntry **sbep = NULL;
  ARRAY_FOREACH_FROM(sbep, &wdata->entries, wdata->top_index)
  {
    if (row >= num_rows)
      break;

    if ((*sbep)->is_hidden)
      continue;

    struct SbEntry *entry = (*sbep);
    struct Mailbox *m = entry->mailbox;

    const int entryidx = ARRAY_FOREACH_IDX;
    entry->color =
        calc_color(m, (entryidx == wdata->opn_index), (entryidx == wdata->hil_index));

    if (m_cur && (m_cur->realpath[0] != '\0') &&
        mutt_str_equal(m->realpath, m_cur->realpath))
    {
      m->msg_unread = m_cur->msg_unread;
      m->msg_count = m_cur->msg_count;
      m->msg_flagged = m_cur->msg_flagged;
    }

    const char *path = mailbox_path(m);

    const char *const c_folder = cs_subset_string(NeoMutt->sub, "folder");
    // Try to abbreviate the full path
    const char *abbr = abbrev_folder(path, c_folder, m->type);
    if (!abbr)
      abbr = abbrev_url(path, m->type);
    const char *short_path = abbr ? abbr : path;

    /* Compute the depth */
    const char *last_part = abbr;
    const char *const c_sidebar_delim_chars =
        cs_subset_string(NeoMutt->sub, "sidebar_delim_chars");
    entry->depth = calc_path_depth(abbr, c_sidebar_delim_chars, &last_part);

    const bool short_path_is_abbr = (short_path == abbr);
    const bool c_sidebar_short_path =
        cs_subset_bool(NeoMutt->sub, "sidebar_short_path");
    if (c_sidebar_short_path)
    {
      short_path = last_part;
    }

    // Don't indent if we were unable to create an abbreviation.
    // Otherwise, the full path will be indent, and it looks unusual.
    const bool c_sidebar_folder_indent =
        cs_subset_bool(NeoMutt->sub, "sidebar_folder_indent");
    if (c_sidebar_folder_indent && short_path_is_abbr)
    {
      const short c_sidebar_component_depth =
          cs_subset_number(NeoMutt->sub, "sidebar_component_depth");
      if (c_sidebar_component_depth > 0)
        entry->depth -= c_sidebar_component_depth;
    }
    else if (!c_sidebar_folder_indent)
      entry->depth = 0;

    mutt_str_copy(entry->box, short_path, sizeof(entry->box));
    make_sidebar_entry(entry->display, sizeof(entry->display), width, entry, shared);
    row++;
  }

  win->actions |= WA_REPAINT;
  mutt_debug(LL_DEBUG5, "recalc done, request WA_REPAINT\n");
  return 0;
}

/**
 * draw_divider - Draw a line between the sidebar and the rest of neomutt
 * @param wdata    Sidebar data
 * @param win      Window to draw on
 * @param num_rows Height of the Sidebar
 * @param num_cols Width of the Sidebar
 * @retval 0   Empty string
 * @retval num Character occupies n screen columns
 *
 * Draw a divider using characters from the config option "sidebar_divider_char".
 * This can be an ASCII or Unicode character.
 * We calculate these characters' width in screen columns.
 *
 * If the user hasn't set $sidebar_divider_char we pick a character for them,
 * respecting the value of $ascii_chars.
 */
static int draw_divider(struct SidebarWindowData *wdata, struct MuttWindow *win,
                        int num_rows, int num_cols)
{
  if ((num_rows < 1) || (num_cols < 1) || (wdata->divider_width > num_cols))
    return 0;

  const int width = wdata->divider_width;
  const char *const c_sidebar_divider_char =
      cs_subset_string(NeoMutt->sub, "sidebar_divider_char");

  mutt_curses_set_color_by_id(MT_COLOR_SIDEBAR_DIVIDER);

  const bool c_sidebar_on_right =
      cs_subset_bool(NeoMutt->sub, "sidebar_on_right");
  const int col = c_sidebar_on_right ? 0 : (num_cols - width);

  for (int i = 0; i < num_rows; i++)
  {
    mutt_window_move(win, col, i);

    switch (wdata->divider_type)
    {
      case SB_DIV_USER:
        mutt_window_addstr(win, NONULL(c_sidebar_divider_char));
        break;
      case SB_DIV_ASCII:
        mutt_window_addch(win, '|');
        break;
      case SB_DIV_UTF8:
        mutt_window_addch(win, ACS_VLINE);
        break;
    }
  }

  mutt_curses_set_color_by_id(MT_COLOR_NORMAL);
  return width;
}

/**
 * fill_empty_space - Wipe the remaining Sidebar space
 * @param win        Window to draw on
 * @param first_row  Window line to start (0-based)
 * @param num_rows   Number of rows to fill
 * @param div_width  Width in screen characters taken by the divider
 * @param num_cols   Number of columns to fill
 *
 * Write spaces over the area the sidebar isn't using.
 */
static void fill_empty_space(struct MuttWindow *win, int first_row,
                             int num_rows, int div_width, int num_cols)
{
  /* Fill the remaining rows with blank space */
  mutt_curses_set_color_by_id(MT_COLOR_NORMAL);

  const bool c_sidebar_on_right =
      cs_subset_bool(NeoMutt->sub, "sidebar_on_right");
  if (!c_sidebar_on_right)
    div_width = 0;
  for (int r = 0; r < num_rows; r++)
  {
    mutt_window_move(win, div_width, first_row + r);

    for (int i = 0; i < num_cols; i++)
      mutt_window_addch(win, ' ');
  }
}

/**
 * sb_repaint - Repaint the Sidebar display - Implements MuttWindow::repaint() - @ingroup window_repaint
 */
int sb_repaint(struct MuttWindow *win)
{
  if (!mutt_window_is_visible(win))
    return 0;

  struct SidebarWindowData *wdata = sb_wdata_get(win);
  const bool c_sidebar_on_right =
      cs_subset_bool(NeoMutt->sub, "sidebar_on_right");

  int row = 0;
  int num_rows = win->state.rows;
  int num_cols = win->state.cols;

  if (wdata->top_index >= 0)
  {
    int col = 0;
    if (c_sidebar_on_right)
      col = wdata->divider_width;

    struct SbEntry **sbep = NULL;
    ARRAY_FOREACH_FROM(sbep, &wdata->entries, wdata->top_index)
    {
      if (row >= num_rows)
        break;

      if ((*sbep)->is_hidden)
        continue;

      struct SbEntry *entry = (*sbep);
      mutt_window_move(win, col, row);
      mutt_curses_set_color_by_id(entry->color);
      mutt_window_printf(win, "%s", entry->display);
      mutt_refresh();
      row++;
    }
  }

  fill_empty_space(win, row, num_rows - row, wdata->divider_width,
                   num_cols - wdata->divider_width);
  draw_divider(wdata, win, num_rows, num_cols);

  mutt_debug(LL_DEBUG5, "repaint done\n");
  return 0;
}
