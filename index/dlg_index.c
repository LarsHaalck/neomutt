/**
 * @file
 * Index Dialog
 *
 * @authors
 * Copyright (C) 1996-2000,2002,2010,2012-2013 Michael R. Elkins <me@mutt.org>
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
 * @page index_dialog Index Dialog
 *
 * ## Overview
 *
 * The Index Dialog is the main screen within NeoMutt.  It contains @ref
 * index_index (a list of emails), @ref pager_dialog (a view of an email) and
 * @ref sidebar_window (a list of mailboxes).
 *
 * ## Windows
 *
 * | Name         | Type         | See Also          |
 * | :----------- | :----------- | :---------------- |
 * | Index Dialog | WT_DLG_INDEX | mutt_index_menu() |
 *
 * **Parent**
 * - @ref gui_dialog
 *
 * **Children**
 * - See: @ref index_ipanel
 * - See: @ref pager_ppanel
 * - See: @ref sidebar_window
 *
 * ## Data
 * - #IndexSharedData
 *
 * ## Events
 *
 * None.
 *
 * Some other events are handled by the dialog's children.
 */

#include "config.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include "private.h"
#include "mutt/lib.h"
#include "config/lib.h"
#include "email/lib.h"
#include "core/lib.h"
#include "conn/lib.h"
#include "gui/lib.h"
#include "lib.h"
#include "color/lib.h"
#include "menu/lib.h"
#include "pager/lib.h"
#include "pattern/lib.h"
#include "context.h"
#include "format_flags.h"
#include "functions.h"
#include "hdrline.h"
#include "hook.h"
#include "keymap.h"
#include "mutt_globals.h"
#include "mutt_logging.h"
#include "mutt_mailbox.h"
#include "mutt_thread.h"
#include "mx.h"
#include "opcodes.h"
#include "options.h"
#include "private_data.h"
#include "protos.h"
#include "shared_data.h"
#include "sort.h"
#include "status.h"
#ifdef USE_NOTMUCH
#include "notmuch/lib.h"
#endif
#ifdef USE_NNTP
#include "nntp/lib.h"
#include "nntp/adata.h" // IWYU pragma: keep
#include "nntp/mdata.h" // IWYU pragma: keep
#endif
#ifdef USE_INOTIFY
#include "monitor.h"
#endif

/// Help Bar for the Index dialog
static const struct Mapping IndexHelp[] = {
  // clang-format off
  { N_("Quit"),  OP_QUIT },
  { N_("Del"),   OP_DELETE },
  { N_("Undel"), OP_UNDELETE },
  { N_("Save"),  OP_SAVE },
  { N_("Mail"),  OP_MAIL },
  { N_("Reply"), OP_REPLY },
  { N_("Group"), OP_GROUP_REPLY },
  { N_("Help"),  OP_HELP },
  { NULL, 0 },
  // clang-format on
};

#ifdef USE_NNTP
/// Help Bar for the News Index dialog
const struct Mapping IndexNewsHelp[] = {
  // clang-format off
  { N_("Quit"),     OP_QUIT },
  { N_("Del"),      OP_DELETE },
  { N_("Undel"),    OP_UNDELETE },
  { N_("Save"),     OP_SAVE },
  { N_("Post"),     OP_POST },
  { N_("Followup"), OP_FOLLOWUP },
  { N_("Catchup"),  OP_CATCHUP },
  { N_("Help"),     OP_HELP },
  { NULL, 0 },
  // clang-format on
};
#endif

/**
 * check_acl - Check the ACLs for a function
 * @param m   Mailbox
 * @param acl ACL, see #AclFlags
 * @param msg Error message for failure
 * @retval true The function is permitted
 */
bool check_acl(struct Mailbox *m, AclFlags acl, const char *msg)
{
  if (!m)
    return false;

  if (!(m->rights & acl))
  {
    /* L10N: %s is one of the CHECK_ACL entries below. */
    mutt_error(_("%s: Operation not permitted by ACL"), msg);
    return false;
  }

  return true;
}

/**
 * collapse_all - Collapse/uncollapse all threads
 * @param ctx    Context
 * @param menu   current menu
 * @param toggle toggle collapsed state
 *
 * This function is called by the OP_MAIN_COLLAPSE_ALL command and on folder
 * enter if the `$collapse_all` option is set. In the first case, the @a toggle
 * parameter is 1 to actually toggle collapsed/uncollapsed state on all
 * threads. In the second case, the @a toggle parameter is 0, actually turning
 * this function into a one-way collapse.
 */
void collapse_all(struct Context *ctx, struct Menu *menu, int toggle)
{
  if (!ctx || !ctx->mailbox || (ctx->mailbox->msg_count == 0) || !menu)
    return;

  struct Email *e_cur = mutt_get_virt_email(ctx->mailbox, menu_get_index(menu));
  if (!e_cur)
    return;

  int final;

  /* Figure out what the current message would be after folding / unfolding,
   * so that we can restore the cursor in a sane way afterwards. */
  if (e_cur->collapsed && toggle)
    final = mutt_uncollapse_thread(e_cur);
  else if (mutt_thread_can_collapse(e_cur))
    final = mutt_collapse_thread(e_cur);
  else
    final = e_cur->vnum;

  if (final == -1)
    return;

  struct Email *base = mutt_get_virt_email(ctx->mailbox, final);
  if (!base)
    return;

  /* Iterate all threads, perform collapse/uncollapse as needed */
  ctx->collapsed = toggle ? !ctx->collapsed : true;
  mutt_thread_collapse(ctx->threads, ctx->collapsed);

  /* Restore the cursor */
  mutt_set_vnum(ctx->mailbox);
  menu->max = ctx->mailbox->vcount;
  for (int i = 0; i < ctx->mailbox->vcount; i++)
  {
    struct Email *e = mutt_get_virt_email(ctx->mailbox, i);
    if (!e)
      break;
    if (e->index == base->index)
    {
      menu_set_index(menu, i);
      break;
    }
  }

  menu_queue_redraw(menu, MENU_REDRAW_INDEX);
}

/**
 * ci_next_undeleted - Find the next undeleted email
 * @param m     Mailbox
 * @param msgno Message number to start at
 * @retval >=0 Message number of next undeleted email
 * @retval  -1 No more undeleted messages
 */
int ci_next_undeleted(struct Mailbox *m, int msgno)
{
  if (!m)
    return -1;

  for (int i = msgno + 1; i < m->vcount; i++)
  {
    struct Email *e = mutt_get_virt_email(m, i);
    if (!e)
      continue;
    if (!e->deleted)
      return i;
  }
  return -1;
}

/**
 * ci_previous_undeleted - Find the previous undeleted email
 * @param m     Mailbox
 * @param msgno Message number to start at
 * @retval >=0 Message number of next undeleted email
 * @retval  -1 No more undeleted messages
 */
int ci_previous_undeleted(struct Mailbox *m, int msgno)
{
  if (!m)
    return -1;

  for (int i = msgno - 1; i >= 0; i--)
  {
    struct Email *e = mutt_get_virt_email(m, i);
    if (!e)
      continue;
    if (!e->deleted)
      return i;
  }
  return -1;
}

/**
 * ci_first_message - Get index of first new message
 * @param m Mailbox
 * @retval num Index of first new message
 *
 * Return the index of the first new message, or failing that, the first
 * unread message.
 */
int ci_first_message(struct Mailbox *m)
{
  if (!m || (m->msg_count == 0))
    return 0;

  int old = -1;
  for (int i = 0; i < m->vcount; i++)
  {
    struct Email *e = mutt_get_virt_email(m, i);
    if (!e)
      continue;
    if (!e->read && !e->deleted)
    {
      if (!e->old)
        return i;
      if (old == -1)
        old = i;
    }
  }
  if (old != -1)
    return old;

  /* If `$use_threads` is not threaded and `$sort` is reverse, the latest
   * message is first.  Otherwise, the latest message is first if exactly
   * one of `$use_threads` and `$sort` are reverse.
   */
  short c_sort = cs_subset_sort(m->sub, "sort");
  if ((c_sort & SORT_MASK) == SORT_THREADS)
    c_sort = cs_subset_sort(m->sub, "sort_aux");
  bool reverse = false;
  switch (mutt_thread_style())
  {
    case UT_FLAT:
      reverse = c_sort & SORT_REVERSE;
      break;
    case UT_THREADS:
      reverse = c_sort & SORT_REVERSE;
      break;
    case UT_REVERSE:
      reverse = !(c_sort & SORT_REVERSE);
      break;
    default:
      assert(false);
  }

  if (reverse || (m->vcount == 0))
    return 0;

  return m->vcount - 1;
}

/**
 * resort_index - Resort the index
 * @param ctx  Context
 * @param menu Current Menu
 */
void resort_index(struct Context *ctx, struct Menu *menu)
{
  if (!ctx || !ctx->mailbox || !menu)
    return;

  struct Mailbox *m = ctx->mailbox;
  const int old_index = menu_get_index(menu);
  struct Email *e_cur = mutt_get_virt_email(m, old_index);

  int new_index = -1;
  mutt_sort_headers(m, ctx->threads, false, &ctx->vsize);
  /* Restore the current message */

  for (int i = 0; i < m->vcount; i++)
  {
    struct Email *e = mutt_get_virt_email(m, i);
    if (!e)
      continue;
    if (e == e_cur)
    {
      new_index = i;
      break;
    }
  }

  if (mutt_using_threads() && (old_index < 0))
    new_index = mutt_parent_message(e_cur, false);

  if (old_index < 0)
    new_index = ci_first_message(m);

  menu_set_index(menu, new_index);
  menu_queue_redraw(menu, MENU_REDRAW_INDEX);
}

/**
 * update_index_threaded - Update the index (if threaded)
 * @param ctx      Mailbox
 * @param check    Flags, e.g. #MX_STATUS_REOPENED
 * @param oldcount How many items are currently in the index
 */
static void update_index_threaded(struct Context *ctx, enum MxStatus check, int oldcount)
{
  struct Email **save_new = NULL;
  const bool lmt = ctx_has_limit(ctx);

  struct Mailbox *m = ctx->mailbox;
  int num_new = MAX(0, m->msg_count - oldcount);

  const bool c_uncollapse_new = cs_subset_bool(m->sub, "uncollapse_new");
  /* save the list of new messages */
  if ((check != MX_STATUS_REOPENED) && (oldcount > 0) &&
      (lmt || c_uncollapse_new) && (num_new > 0))
  {
    save_new = mutt_mem_malloc(num_new * sizeof(struct Email *));
    for (int i = oldcount; i < m->msg_count; i++)
      save_new[i - oldcount] = m->emails[i];
  }

  /* Sort first to thread the new messages, because some patterns
   * require the threading information.
   *
   * If the mailbox was reopened, need to rethread from scratch. */
  mutt_sort_headers(m, ctx->threads, (check == MX_STATUS_REOPENED), &ctx->vsize);

  if (lmt)
  {
    /* Because threading changes the order in m->emails, we don't
     * know which emails are new. Hence, we need to re-apply the limit to the
     * whole set.
     */
    for (int i = 0; i < m->msg_count; i++)
    {
      struct Email *e = m->emails[i];
      if ((e->vnum != -1) || mutt_pattern_exec(SLIST_FIRST(ctx->limit_pattern),
                                               MUTT_MATCH_FULL_ADDRESS, m, e, NULL))
      {
        /* vnum will get properly set by mutt_set_vnum(), which
         * is called by mutt_sort_headers() just below. */
        e->vnum = 1;
        e->visible = true;
      }
      else
      {
        e->vnum = -1;
        e->visible = false;
      }
    }
    /* Need a second sort to set virtual numbers and redraw the tree */
    mutt_sort_headers(m, ctx->threads, false, &ctx->vsize);
  }

  /* uncollapse threads with new mail */
  if (c_uncollapse_new)
  {
    if (check == MX_STATUS_REOPENED)
    {
      ctx->collapsed = false;
      mutt_thread_collapse(ctx->threads, ctx->collapsed);
      mutt_set_vnum(m);
    }
    else if (oldcount > 0)
    {
      for (int j = 0; j < num_new; j++)
      {
        if (save_new[j]->visible)
        {
          mutt_uncollapse_thread(save_new[j]);
        }
      }
      mutt_set_vnum(m);
    }
  }

  FREE(&save_new);
}

/**
 * update_index_unthreaded - Update the index (if unthreaded)
 * @param ctx      Mailbox
 * @param check    Flags, e.g. #MX_STATUS_REOPENED
 */
static void update_index_unthreaded(struct Context *ctx, enum MxStatus check)
{
  /* We are in a limited view. Check if the new message(s) satisfy
   * the limit criteria. If they do, set their virtual msgno so that
   * they will be visible in the limited view */
  if (ctx_has_limit(ctx))
  {
    int padding = mx_msg_padding_size(ctx->mailbox);
    ctx->mailbox->vcount = ctx->vsize = 0;
    for (int i = 0; i < ctx->mailbox->msg_count; i++)
    {
      struct Email *e = ctx->mailbox->emails[i];
      if (!e)
        break;
      if (mutt_pattern_exec(SLIST_FIRST(ctx->limit_pattern),
                            MUTT_MATCH_FULL_ADDRESS, ctx->mailbox, e, NULL))
      {
        assert(ctx->mailbox->vcount < ctx->mailbox->msg_count);
        e->vnum = ctx->mailbox->vcount;
        ctx->mailbox->v2r[ctx->mailbox->vcount] = i;
        e->visible = true;
        ctx->mailbox->vcount++;
        struct Body *b = e->body;
        ctx->vsize += b->length + b->offset - b->hdr_offset + padding;
      }
      else
      {
        e->visible = false;
      }
    }
  }

  /* if the mailbox was reopened, need to rethread from scratch */
  mutt_sort_headers(ctx->mailbox, ctx->threads, (check == MX_STATUS_REOPENED), &ctx->vsize);
}

/**
 * update_index - Update the index
 * @param menu     Current Menu
 * @param ctx      Mailbox
 * @param check    Flags, e.g. #MX_STATUS_REOPENED
 * @param oldcount How many items are currently in the index
 * @param shared   Shared Index data
 */
void update_index(struct Menu *menu, struct Context *ctx, enum MxStatus check,
                  int oldcount, const struct IndexSharedData *shared)
{
  if (!menu || !ctx)
    return;

  struct Mailbox *m = ctx->mailbox;
  if (mutt_using_threads())
    update_index_threaded(ctx, check, oldcount);
  else
    update_index_unthreaded(ctx, check);

  const int old_index = menu_get_index(menu);
  int index = -1;
  if (oldcount)
  {
    /* restore the current message to the message it was pointing to */
    for (int i = 0; i < m->vcount; i++)
    {
      struct Email *e = mutt_get_virt_email(m, i);
      if (!e)
        continue;
      if (index_shared_data_is_cur_email(shared, e))
      {
        index = i;
        break;
      }
    }
  }

  if (index < 0)
  {
    index = (old_index < m->vcount) ? old_index : ci_first_message(m);
  }
  menu_set_index(menu, index);
}

/**
 * mutt_update_index - Update the index
 * @param menu      Current Menu
 * @param ctx       Mailbox
 * @param check     Flags, e.g. #MX_STATUS_REOPENED
 * @param oldcount  How many items are currently in the index
 * @param shared    Shared Index data
 */
void mutt_update_index(struct Menu *menu, struct Context *ctx, enum MxStatus check,
                       int oldcount, struct IndexSharedData *shared)
{
  update_index(menu, ctx, check, oldcount, shared);
}

/**
 * index_mailbox_observer - Notification that a Mailbox has changed - Implements ::observer_t
 *
 * If a Mailbox is closed, then set a pointer to NULL.
 */
static int index_mailbox_observer(struct NotifyCallback *nc)
{
  if ((nc->event_type != NT_MAILBOX) || !nc->global_data)
    return -1;

  if (nc->event_subtype != NT_MAILBOX_DELETE)
    return 0;

  struct Mailbox **ptr = nc->global_data;
  if (!ptr || !*ptr)
    return 0;

  *ptr = NULL;
  mutt_debug(LL_DEBUG5, "mailbox done\n");
  return 0;
}

/**
 * change_folder_mailbox - Change to a different Mailbox by pointer
 * @param menu      Current Menu
 * @param m         Mailbox
 * @param oldcount  How many items are currently in the index
 * @param shared    Shared Index data
 * @param read_only Open Mailbox in read-only mode
 */
void change_folder_mailbox(struct Menu *menu, struct Mailbox *m, int *oldcount,
                           struct IndexSharedData *shared, bool read_only)
{
  if (!m)
    return;

  /* keepalive failure in mutt_enter_fname may kill connection. */
  if (shared->mailbox && (mutt_buffer_is_empty(&shared->mailbox->pathbuf)))
  {
    ctx_free(&shared->ctx);
    if (shared->mailbox->flags == MB_HIDDEN)
      mailbox_free(&shared->mailbox);
  }

  if (shared->mailbox)
  {
    char *new_last_folder = NULL;
#ifdef USE_INOTIFY
    int monitor_remove_rc = mutt_monitor_remove(NULL);
#endif
#ifdef USE_COMP_MBOX
    if (shared->mailbox->compress_info && (shared->mailbox->realpath[0] != '\0'))
      new_last_folder = mutt_str_dup(shared->mailbox->realpath);
    else
#endif
      new_last_folder = mutt_str_dup(mailbox_path(shared->mailbox));
    *oldcount = shared->mailbox->msg_count;

    const enum MxStatus check = mx_mbox_close(shared->mailbox);
    if (check == MX_STATUS_OK)
    {
      ctx_free(&shared->ctx);
      if ((shared->mailbox != m) && (shared->mailbox->flags == MB_HIDDEN))
        mailbox_free(&shared->mailbox);
    }
    else
    {
#ifdef USE_INOTIFY
      if (monitor_remove_rc == 0)
        mutt_monitor_add(NULL);
#endif
      if ((check == MX_STATUS_NEW_MAIL) || (check == MX_STATUS_REOPENED))
        update_index(menu, shared->ctx, check, *oldcount, shared);

      FREE(&new_last_folder);
      OptSearchInvalid = true;
      menu_queue_redraw(menu, MENU_REDRAW_INDEX);
      return;
    }
    FREE(&LastFolder);
    LastFolder = new_last_folder;
  }
  mutt_str_replace(&CurrentFolder, mailbox_path(m));

  /* If the `folder-hook` were to call `unmailboxes`, then the Mailbox (`m`)
   * could be deleted, leaving `m` dangling. */
  // TODO: Refactor this function to avoid the need for an observer
  notify_observer_add(m->notify, NT_MAILBOX, index_mailbox_observer, &m);
  char *dup_path = mutt_str_dup(mailbox_path(m));
  char *dup_name = mutt_str_dup(m->name);

  mutt_folder_hook(dup_path, dup_name);
  if (m)
  {
    /* `m` is still valid, but we won't need the observer again before the end
     * of the function. */
    notify_observer_remove(m->notify, index_mailbox_observer, &m);
  }
  else
  {
    // Recreate the Mailbox as the folder-hook might have invoked `mailboxes`
    // and/or `unmailboxes`.
    m = mx_path_resolve(dup_path);
  }

  FREE(&dup_path);
  FREE(&dup_name);

  if (!m)
    return;

  const OpenMailboxFlags flags = read_only ? MUTT_READONLY : MUTT_OPEN_NO_FLAGS;
  if (mx_mbox_open(m, flags))
  {
    struct Context *ctx = ctx_new(m);
    index_shared_data_set_context(shared, ctx);

    menu->max = m->msg_count;
    menu_set_index(menu, ci_first_message(shared->mailbox));
#ifdef USE_INOTIFY
    mutt_monitor_add(NULL);
#endif
  }
  else
  {
    index_shared_data_set_context(shared, NULL);
    menu_set_index(menu, 0);
  }

  const bool c_collapse_all = cs_subset_bool(shared->sub, "collapse_all");
  if (mutt_using_threads() && c_collapse_all)
    collapse_all(shared->ctx, menu, 0);

  struct MuttWindow *dlg = dialog_find(menu->win);
  struct EventMailbox ev_m = { shared->mailbox };
  mutt_debug(LL_NOTIFY, "NT_MAILBOX_SWITCH: %p\n", shared->mailbox);
  notify_send(dlg->notify, NT_MAILBOX, NT_MAILBOX_SWITCH, &ev_m);

  mutt_clear_error();
  /* force the mailbox check after we have changed the folder */
  mutt_mailbox_check(ev_m.mailbox, MUTT_MAILBOX_CHECK_FORCE);
  menu_queue_redraw(menu, MENU_REDRAW_FULL);
  OptSearchInvalid = true;
}

#ifdef USE_NOTMUCH
/**
 * change_folder_notmuch - Change to a different Notmuch Mailbox by string
 * @param menu      Current Menu
 * @param buf       Folder to change to
 * @param buflen    Length of buffer
 * @param oldcount  How many items are currently in the index
 * @param shared    Shared Index data
 * @param read_only Open Mailbox in read-only mode
 */
struct Mailbox *change_folder_notmuch(struct Menu *menu, char *buf, int buflen, int *oldcount,
                                      struct IndexSharedData *shared, bool read_only)
{
  if (!nm_url_from_query(NULL, buf, buflen))
  {
    mutt_message(_("Failed to create query, aborting"));
    return NULL;
  }

  struct Mailbox *m_query = mx_path_resolve(buf);
  change_folder_mailbox(menu, m_query, oldcount, shared, read_only);
  return m_query;
}
#endif

/**
 * change_folder_string - Change to a different Mailbox by string
 * @param menu         Current Menu
 * @param buf          Folder to change to
 * @param buflen       Length of buffer
 * @param oldcount     How many items are currently in the index
 * @param shared       Shared Index data
 * @param pager_return Return to the pager afterwards
 * @param read_only    Open Mailbox in read-only mode
 */
void change_folder_string(struct Menu *menu, char *buf, size_t buflen, int *oldcount,
                          struct IndexSharedData *shared, bool *pager_return, bool read_only)
{
#ifdef USE_NNTP
  if (OptNews)
  {
    OptNews = false;
    nntp_expand_path(buf, buflen, &CurrentNewsSrv->conn->account);
  }
  else
#endif
  {
    const char *const c_folder = cs_subset_string(shared->sub, "folder");
    mx_path_canon(buf, buflen, c_folder, NULL);
  }

  enum MailboxType type = mx_path_probe(buf);
  if ((type == MUTT_MAILBOX_ERROR) || (type == MUTT_UNKNOWN))
  {
    // Look for a Mailbox by its description, before failing
    struct Mailbox *m = mailbox_find_name(buf);
    if (m)
    {
      change_folder_mailbox(menu, m, oldcount, shared, read_only);
      *pager_return = false;
    }
    else
      mutt_error(_("%s is not a mailbox"), buf);
    return;
  }

  /* past this point, we don't return to the pager on error */
  *pager_return = false;

  struct Mailbox *m = mx_path_resolve(buf);
  change_folder_mailbox(menu, m, oldcount, shared, read_only);
}

/**
 * index_make_entry - Format a menu item for the index list - Implements Menu::make_entry() - @ingroup menu_make_entry
 */
void index_make_entry(struct Menu *menu, char *buf, size_t buflen, int line)
{
  buf[0] = '\0';

  if (!menu || !menu->mdata)
    return;

  struct IndexPrivateData *priv = menu->mdata;
  struct IndexSharedData *shared = priv->shared;
  struct Mailbox *m = shared->mailbox;

  if (!m || (line < 0) || (line >= m->email_max))
    return;

  struct Email *e = mutt_get_virt_email(m, line);
  if (!e)
    return;

  MuttFormatFlags flags = MUTT_FORMAT_ARROWCURSOR | MUTT_FORMAT_INDEX;
  struct MuttThread *tmp = NULL;

  const enum UseThreads c_threads = mutt_thread_style();
  if ((c_threads > UT_FLAT) && e->tree)
  {
    flags |= MUTT_FORMAT_TREE; /* display the thread tree */
    if (e->display_subject)
      flags |= MUTT_FORMAT_FORCESUBJ;
    else
    {
      const bool reverse = c_threads == UT_REVERSE;
      int edgemsgno;
      if (reverse)
      {
        if (menu->top + menu->pagelen > menu->max)
          edgemsgno = m->v2r[menu->max - 1];
        else
          edgemsgno = m->v2r[menu->top + menu->pagelen - 1];
      }
      else
        edgemsgno = m->v2r[menu->top];

      for (tmp = e->thread->parent; tmp; tmp = tmp->parent)
      {
        if (!tmp->message)
          continue;

        /* if no ancestor is visible on current screen, provisionally force
         * subject... */
        if (reverse ? (tmp->message->msgno > edgemsgno) : (tmp->message->msgno < edgemsgno))
        {
          flags |= MUTT_FORMAT_FORCESUBJ;
          break;
        }
        else if (tmp->message->vnum >= 0)
          break;
      }
      if (flags & MUTT_FORMAT_FORCESUBJ)
      {
        for (tmp = e->thread->prev; tmp; tmp = tmp->prev)
        {
          if (!tmp->message)
            continue;

          /* ...but if a previous sibling is available, don't force it */
          if (reverse ? (tmp->message->msgno > edgemsgno) : (tmp->message->msgno < edgemsgno))
            break;
          else if (tmp->message->vnum >= 0)
          {
            flags &= ~MUTT_FORMAT_FORCESUBJ;
            break;
          }
        }
      }
    }
  }

  const char *const c_index_format =
      cs_subset_string(shared->sub, "index_format");
  mutt_make_string(buf, buflen, menu->win->state.cols, NONULL(c_index_format),
                   m, shared->ctx->msg_in_pager, e, flags, NULL);
}

/**
 * index_color - Calculate the colour for a line of the index - Implements Menu::color() - @ingroup menu_color
 */
int index_color(struct Menu *menu, int line)
{
  struct IndexPrivateData *priv = menu->mdata;
  struct IndexSharedData *shared = priv->shared;
  struct Mailbox *m = shared->mailbox;
  if (!m || (line < 0))
    return 0;

  struct Email *e = mutt_get_virt_email(m, line);
  if (!e)
    return 0;

  if (e->pair)
    return e->pair;

  mutt_set_header_color(m, e);
  return e->pair;
}

/**
 * mutt_draw_statusline - Draw a highlighted status bar
 * @param win    Window
 * @param cols   Maximum number of screen columns
 * @param buf    Message to be displayed
 * @param buflen Length of the buffer
 *
 * Users configure the highlighting of the status bar, e.g.
 *     color status red default "[0-9][0-9]:[0-9][0-9]"
 *
 * Where regexes overlap, the one nearest the start will be used.
 * If two regexes start at the same place, the longer match will be used.
 */
void mutt_draw_statusline(struct MuttWindow *win, int cols, const char *buf, size_t buflen)
{
  if (!buf || !stdscr)
    return;

  size_t i = 0;
  size_t offset = 0;
  bool found = false;
  size_t chunks = 0;
  size_t len = 0;

  /**
   * struct StatusSyntax - Colours of the status bar
   */
  struct StatusSyntax
  {
    int color; ///< Colour pair
    int first; ///< First character of that colour
    int last;  ///< Last character of that colour
  } *syntax = NULL;

  do
  {
    struct RegexColor *cl = NULL;
    found = false;

    if (!buf[offset])
      break;

    /* loop through each "color status regex" */
    STAILQ_FOREACH(cl, regex_colors_get_list(MT_COLOR_STATUS), entries)
    {
      regmatch_t pmatch[cl->match + 1];

      if (regexec(&cl->regex, buf + offset, cl->match + 1, pmatch, 0) != 0)
        continue; /* regex doesn't match the status bar */

      int first = pmatch[cl->match].rm_so + offset;
      int last = pmatch[cl->match].rm_eo + offset;

      if (first == last)
        continue; /* ignore an empty regex */

      if (!found)
      {
        chunks++;
        mutt_mem_realloc(&syntax, chunks * sizeof(struct StatusSyntax));
      }

      i = chunks - 1;
      if (!found || (first < syntax[i].first) ||
          ((first == syntax[i].first) && (last > syntax[i].last)))
      {
        syntax[i].color = cl->pair;
        syntax[i].first = first;
        syntax[i].last = last;
      }
      found = true;
    }

    if (syntax)
    {
      offset = syntax[i].last;
    }
  } while (found);

  /* Only 'len' bytes will fit into 'cols' screen columns */
  len = mutt_wstr_trunc(buf, buflen, cols, NULL);

  offset = 0;

  if ((chunks > 0) && (syntax[0].first > 0))
  {
    /* Text before the first highlight */
    mutt_window_addnstr(win, buf, MIN(len, syntax[0].first));
    mutt_curses_set_color_by_id(MT_COLOR_STATUS);
    if (len <= syntax[0].first)
      goto dsl_finish; /* no more room */

    offset = syntax[0].first;
  }

  for (i = 0; i < chunks; i++)
  {
    /* Highlighted text */
    mutt_curses_set_attr(syntax[i].color);
    mutt_window_addnstr(win, buf + offset, MIN(len, syntax[i].last) - offset);
    if (len <= syntax[i].last)
      goto dsl_finish; /* no more room */

    size_t next;
    if ((i + 1) == chunks)
    {
      next = len;
    }
    else
    {
      next = MIN(len, syntax[i + 1].first);
    }

    mutt_curses_set_color_by_id(MT_COLOR_STATUS);
    offset = syntax[i].last;
    mutt_window_addnstr(win, buf + offset, next - offset);

    offset = next;
    if (offset >= len)
      goto dsl_finish; /* no more room */
  }

  mutt_curses_set_color_by_id(MT_COLOR_STATUS);
  if (offset < len)
  {
    /* Text after the last highlight */
    mutt_window_addnstr(win, buf + offset, len - offset);
  }

  int width = mutt_strwidth(buf);
  if (width < cols)
  {
    /* Pad the rest of the line with whitespace */
    mutt_paddstr(win, cols - width, "");
  }
dsl_finish:
  FREE(&syntax);
}

/**
 * index_custom_redraw - Redraw the index - Implements Menu::custom_redraw() - @ingroup menu_custom_redraw
 */
static void index_custom_redraw(struct Menu *menu)
{
  if (menu->redraw & MENU_REDRAW_FULL)
    menu_redraw_full(menu);

  struct IndexPrivateData *priv = menu->mdata;
  struct IndexSharedData *shared = priv->shared;
  struct Mailbox *m = shared->mailbox;
  const int index = menu_get_index(menu);
  if (m && m->emails && (index < m->vcount))
  {
    if (menu->redraw & MENU_REDRAW_INDEX)
    {
      menu_redraw_index(menu);
    }
    else if (menu->redraw & MENU_REDRAW_MOTION)
      menu_redraw_motion(menu);
    else if (menu->redraw & MENU_REDRAW_CURRENT)
      menu_redraw_current(menu);
  }

  menu->redraw = MENU_REDRAW_NO_FLAGS;
  mutt_debug(LL_DEBUG5, "repaint done\n");
}

/**
 * mutt_index_menu - Display a list of emails
 * @param dlg Dialog containing Windows to draw on
 * @param m_init Initial mailbox
 * @retval Mailbox open in the index
 *
 * This function handles the message index window as well as commands returned
 * from the pager (MENU_PAGER).
 */
struct Mailbox *mutt_index_menu(struct MuttWindow *dlg, struct Mailbox *m_init)
{
  struct Context *ctx_old = Context;
  struct IndexSharedData *shared = dlg->wdata;
  index_shared_data_set_context(shared, ctx_new(m_init));

  struct MuttWindow *panel_index = window_find_child(dlg, WT_INDEX);
  struct MuttWindow *panel_pager = window_find_child(dlg, WT_PAGER);

  struct IndexPrivateData *priv = panel_index->wdata;
  priv->attach_msg = OptAttachMsg;
  priv->win_index = window_find_child(panel_index, WT_MENU);
  priv->win_ibar = window_find_child(panel_index, WT_STATUS_BAR);
  priv->win_pager = window_find_child(panel_pager, WT_CUSTOM);
  priv->win_pbar = window_find_child(panel_pager, WT_STATUS_BAR);

  int op = OP_NULL;

#ifdef USE_NNTP
  if (shared->mailbox && (shared->mailbox->type == MUTT_NNTP))
    dlg->help_data = IndexNewsHelp;
  else
#endif
    dlg->help_data = IndexHelp;
  dlg->help_menu = MENU_MAIN;

  priv->menu = priv->win_index->wdata;
  priv->menu->make_entry = index_make_entry;
  priv->menu->color = index_color;
  priv->menu->custom_redraw = index_custom_redraw;
  priv->menu->max = shared->mailbox ? shared->mailbox->vcount : 0;
  menu_set_index(priv->menu, ci_first_message(shared->mailbox));
  mutt_window_reflow(NULL);

  if (!priv->attach_msg)
  {
    /* force the mailbox check after we enter the folder */
    mutt_mailbox_check(shared->mailbox, MUTT_MAILBOX_CHECK_FORCE);
  }
#ifdef USE_INOTIFY
  mutt_monitor_add(NULL);
#endif

  {
    const bool c_collapse_all = cs_subset_bool(shared->sub, "collapse_all");
    if (mutt_using_threads() && c_collapse_all)
    {
      collapse_all(shared->ctx, priv->menu, 0);
      menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
    }
  }

  while (true)
  {
    /* Clear the tag prefix unless we just started it.  Don't clear
     * the prefix on a timeout (op==-2), but do clear on an abort (op==-1) */
    if (priv->tag && (op != OP_TAG_PREFIX) && (op != OP_TAG_PREFIX_COND) && (op != -2))
      priv->tag = false;

    /* check if we need to resort the index because just about
     * any 'op' below could do mutt_enter_command(), either here or
     * from any new priv->menu launched, and change $sort/$sort_aux */
    if (OptNeedResort && shared->mailbox && (shared->mailbox->msg_count != 0) &&
        (menu_get_index(priv->menu) >= 0))
    {
      resort_index(shared->ctx, priv->menu);
    }

    priv->menu->max = shared->mailbox ? shared->mailbox->vcount : 0;
    priv->oldcount = shared->mailbox ? shared->mailbox->msg_count : 0;

    {
      if (OptRedrawTree && shared->mailbox &&
          (shared->mailbox->msg_count != 0) && mutt_using_threads())
      {
        mutt_draw_tree(shared->ctx->threads);
        OptRedrawTree = false;
      }
    }

    if (shared->mailbox)
    {
      mailbox_gc_run();

      shared->ctx->menu = priv->menu;
      /* check for new mail in the mailbox.  If nonzero, then something has
       * changed about the file (either we got new mail or the file was
       * modified underneath us.) */
      enum MxStatus check = mx_mbox_check(shared->mailbox);

      if (check == MX_STATUS_ERROR)
      {
        if (mutt_buffer_is_empty(&shared->mailbox->pathbuf))
        {
          /* fatal error occurred */
          ctx_free(&shared->ctx);
          menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
        }

        OptSearchInvalid = true;
      }
      else if ((check == MX_STATUS_NEW_MAIL) || (check == MX_STATUS_REOPENED) ||
               (check == MX_STATUS_FLAGS))
      {
        /* notify the user of new mail */
        if (check == MX_STATUS_REOPENED)
        {
          mutt_error(
              _("Mailbox was externally modified.  Flags may be wrong."));
        }
        else if (check == MX_STATUS_NEW_MAIL)
        {
          for (size_t i = 0; i < shared->mailbox->msg_count; i++)
          {
            const struct Email *e = shared->mailbox->emails[i];
            if (e && !e->read && !e->old)
            {
              mutt_message(_("New mail in this mailbox"));
              const bool c_beep_new = cs_subset_bool(shared->sub, "beep_new");
              if (c_beep_new)
                mutt_beep(true);
              const char *const c_new_mail_command =
                  cs_subset_string(shared->sub, "new_mail_command");
              if (c_new_mail_command)
              {
                char cmd[1024];
                menu_status_line(cmd, sizeof(cmd), shared, priv->menu,
                                 sizeof(cmd), NONULL(c_new_mail_command));
                if (mutt_system(cmd) != 0)
                  mutt_error(_("Error running \"%s\""), cmd);
              }
              break;
            }
          }
        }
        else if (check == MX_STATUS_FLAGS)
        {
          mutt_message(_("Mailbox was externally modified"));
        }

        /* avoid the message being overwritten by mailbox */
        priv->do_mailbox_notify = false;

        bool verbose = shared->mailbox->verbose;
        shared->mailbox->verbose = false;
        update_index(priv->menu, shared->ctx, check, priv->oldcount, shared);
        shared->mailbox->verbose = verbose;
        priv->menu->max = shared->mailbox->vcount;
        menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
        OptSearchInvalid = true;
      }

      index_shared_data_set_email(
          shared, mutt_get_virt_email(shared->mailbox, menu_get_index(priv->menu)));
    }

    if (!priv->attach_msg)
    {
      /* check for new mail in the incoming folders */
      priv->oldcount = priv->newcount;
      priv->newcount = mutt_mailbox_check(shared->mailbox, 0);
      if (priv->do_mailbox_notify)
      {
        if (mutt_mailbox_notify(shared->mailbox))
        {
          const bool c_beep_new = cs_subset_bool(shared->sub, "beep_new");
          if (c_beep_new)
            mutt_beep(true);
          const char *const c_new_mail_command =
              cs_subset_string(shared->sub, "new_mail_command");
          if (c_new_mail_command)
          {
            char cmd[1024];
            menu_status_line(cmd, sizeof(cmd), shared, priv->menu, sizeof(cmd),
                             NONULL(c_new_mail_command));
            if (mutt_system(cmd) != 0)
              mutt_error(_("Error running \"%s\""), cmd);
          }
        }
      }
      else
        priv->do_mailbox_notify = true;
    }

    if (op >= 0)
      mutt_curses_set_cursor(MUTT_CURSOR_INVISIBLE);

    if (priv->in_pager)
    {
      mutt_curses_set_cursor(MUTT_CURSOR_VISIBLE); /* fallback from the pager */
    }
    else
    {
      index_custom_redraw(priv->menu);
      window_redraw(NULL);

      /* give visual indication that the next command is a tag- command */
      if (priv->tag)
        msgwin_set_text(MT_COLOR_NORMAL, "tag-");

      const bool c_arrow_cursor = cs_subset_bool(shared->sub, "arrow_cursor");
      const bool c_braille_friendly =
          cs_subset_bool(shared->sub, "braille_friendly");
      const int index = menu_get_index(priv->menu);
      if (c_arrow_cursor)
      {
        mutt_window_move(priv->menu->win, 2, index - priv->menu->top);
      }
      else if (c_braille_friendly)
      {
        mutt_window_move(priv->menu->win, 0, index - priv->menu->top);
      }
      else
      {
        mutt_window_move(priv->menu->win, priv->menu->win->state.cols - 1,
                         index - priv->menu->top);
      }
      mutt_refresh();

      if (SigWinch)
      {
        SigWinch = false;
        mutt_resize_screen();
        priv->menu->top = 0; /* so we scroll the right amount */
        /* force a real complete redraw.  clrtobot() doesn't seem to be able
         * to handle every case without this.  */
        clearok(stdscr, true);
        msgwin_clear_text();
        continue;
      }

      window_redraw(NULL);
      op = km_dokey(MENU_MAIN);

      /* either user abort or timeout */
      if (op < 0)
      {
        mutt_timeout_hook();
        if (priv->tag)
          msgwin_clear_text();
        continue;
      }

      mutt_debug(LL_DEBUG1, "Got op %s (%d)\n", OpStrings[op][0], op);

      mutt_curses_set_cursor(MUTT_CURSOR_VISIBLE);

      /* special handling for the priv->tag-prefix function */
      const bool c_auto_tag = cs_subset_bool(shared->sub, "auto_tag");
      if ((op == OP_TAG_PREFIX) || (op == OP_TAG_PREFIX_COND))
      {
        /* A second priv->tag-prefix command aborts */
        if (priv->tag)
        {
          priv->tag = false;
          msgwin_clear_text();
          continue;
        }

        if (!shared->mailbox)
        {
          mutt_error(_("No mailbox is open"));
          continue;
        }

        if (shared->mailbox->msg_tagged == 0)
        {
          if (op == OP_TAG_PREFIX)
            mutt_error(_("No tagged messages"));
          else if (op == OP_TAG_PREFIX_COND)
          {
            mutt_flush_macro_to_endcond();
            mutt_message(_("Nothing to do"));
          }
          continue;
        }

        /* get the real command */
        priv->tag = true;
        continue;
      }
      else if (c_auto_tag && shared->mailbox && (shared->mailbox->msg_tagged != 0))
      {
        priv->tag = true;
      }

      mutt_clear_error();
    }

#ifdef USE_NNTP
    OptNews = false; /* for any case */
#endif

#ifdef USE_NOTMUCH
    nm_db_debug_check(shared->mailbox);
#endif

    int rc = index_function_dispatcher(priv->win_index, op);

    if (rc == IR_CONTINUE)
    {
      op = OP_DISPLAY_MESSAGE;
      continue;
    }

    if (rc > 0)
    {
      op = rc;
      continue;
    }

    if ((rc == IR_UNKNOWN) && !priv->in_pager)
      km_error_key(MENU_MAIN);

#ifdef USE_NOTMUCH
    nm_db_debug_check(shared->mailbox);
#endif

    if (priv->in_pager)
    {
      mutt_clear_pager_position();
      priv->in_pager = false;
      menu_queue_redraw(priv->menu, MENU_REDRAW_FULL);
    }

    if (rc == IR_DONE)
      break;
  }

  ctx_free(&shared->ctx);
  Context = ctx_old;

  return shared->mailbox;
}

/**
 * mutt_set_header_color - Select a colour for a message
 * @param m Mailbox
 * @param e Current Email
 */
void mutt_set_header_color(struct Mailbox *m, struct Email *e)
{
  if (!e)
    return;

  struct RegexColor *color = NULL;
  struct PatternCache cache = { 0 };

  STAILQ_FOREACH(color, regex_colors_get_list(MT_COLOR_INDEX), entries)
  {
    if (mutt_pattern_exec(SLIST_FIRST(color->color_pattern),
                          MUTT_MATCH_FULL_ADDRESS, m, e, &cache))
    {
      e->pair = color->pair;
      return;
    }
  }
  e->pair = simple_colors_get(MT_COLOR_NORMAL);
}

/**
 * index_pager_init - Allocate the Windows for the Index/Pager
 * @retval ptr Dialog containing nested Windows
 */
struct MuttWindow *index_pager_init(void)
{
  struct MuttWindow *dlg =
      mutt_window_new(WT_DLG_INDEX, MUTT_WIN_ORIENT_HORIZONTAL, MUTT_WIN_SIZE_MAXIMISE,
                      MUTT_WIN_SIZE_UNLIMITED, MUTT_WIN_SIZE_UNLIMITED);

  struct IndexSharedData *shared = index_shared_data_new();
  notify_set_parent(shared->notify, dlg->notify);

  dlg->wdata = shared;
  dlg->wdata_free = index_shared_data_free;

  const bool c_status_on_top = cs_subset_bool(NeoMutt->sub, "status_on_top");

  struct MuttWindow *panel_index = ipanel_new(c_status_on_top, shared);
  struct MuttWindow *panel_pager = ppanel_new(c_status_on_top, shared);

  mutt_window_add_child(dlg, panel_index);
  mutt_window_add_child(dlg, panel_pager);

  dlg->focus = panel_index;

  return dlg;
}
