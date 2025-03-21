/**
 * @file
 * NeoMutt Logging
 *
 * @authors
 * Copyright (C) 2018 Richard Russon <rich@flatcap.org>
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
 * @page neo_mutt_logging NeoMutt Logging
 *
 * NeoMutt Logging
 */

#include "config.h"
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "mutt/lib.h"
#include "config/lib.h"
#include "core/lib.h"
#include "gui/lib.h"
#include "mutt_logging.h"
#include "color/lib.h"
#include "mutt_globals.h"
#include "muttlib.h"
#include "options.h"

uint64_t LastError = 0; ///< Time of the last error message (in milliseconds since the Unix epoch)

char *CurrentFile = NULL; ///< The previous log file name
const int NumOfLogs = 5;  ///< How many log files to rotate

#define S_TO_MS 1000L

/**
 * error_pause - Wait for an error message to be read
 *
 * If '$sleep_time' seconds hasn't elapsed since LastError, then wait
 */
static void error_pause(void)
{
  const short c_sleep_time = cs_subset_number(NeoMutt->sub, "sleep_time");
  const uint64_t elapsed = mutt_date_epoch_ms() - LastError;
  const uint64_t sleep = c_sleep_time * S_TO_MS;
  if ((LastError == 0) || (elapsed >= sleep))
    return;

  mutt_refresh();
  mutt_date_sleep_ms(sleep - elapsed);
}

/**
 * rotate_logs - Rotate a set of numbered files
 * @param file  Template filename
 * @param count Maximum number of files
 * @retval ptr Name of the 0'th file
 *
 * Given a template 'temp', rename files numbered 0 to (count-1).
 *
 * Rename:
 * - ...
 * - temp1 -> temp2
 * - temp0 -> temp1
 */
static const char *rotate_logs(const char *file, int count)
{
  if (!file)
    return NULL;

  struct Buffer *old_file = mutt_buffer_pool_get();
  struct Buffer *new_file = mutt_buffer_pool_get();

  /* rotate the old debug logs */
  for (count -= 2; count >= 0; count--)
  {
    mutt_buffer_printf(old_file, "%s%d", file, count);
    mutt_buffer_printf(new_file, "%s%d", file, count + 1);

    mutt_buffer_expand_path(old_file);
    mutt_buffer_expand_path(new_file);
    (void) rename(mutt_buffer_string(old_file), mutt_buffer_string(new_file));
  }

  file = mutt_buffer_strdup(old_file);
  mutt_buffer_pool_release(&old_file);
  mutt_buffer_pool_release(&new_file);

  return file;
}

/**
 * mutt_clear_error - Clear the message line (bottom line of screen)
 */
void mutt_clear_error(void)
{
  /* Make sure the error message has had time to be read */
  if (OptMsgErr)
    error_pause();

  ErrorBufMessage = false;
  if (!OptNoCurses)
    msgwin_clear_text();
}

/**
 * log_disp_curses - Display a log line in the message line - Implements ::log_dispatcher_t - @ingroup logging_api
 */
int log_disp_curses(time_t stamp, const char *file, int line,
                    const char *function, enum LogLevel level, ...)
{
  const short c_debug_level = cs_subset_number(NeoMutt->sub, "debug_level");
  if (level > c_debug_level)
    return 0;

  char buf[1024];

  va_list ap;
  va_start(ap, level);
  const char *fmt = va_arg(ap, const char *);
  int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (level == LL_PERROR)
  {
    char *buf2 = buf + ret;
    int len = sizeof(buf) - ret;
    const char *p = strerror(errno);
    if (!p)
      p = _("unknown error");

    ret += snprintf(buf2, len, ": %s (errno = %d)", p, errno);
  }

  const bool dupe = (strcmp(buf, ErrorBuf) == 0);
  if (!dupe)
  {
    /* Only log unique messages */
    log_disp_file(stamp, file, line, function, level, "%s", buf);
    if (stamp == 0)
      log_disp_queue(stamp, file, line, function, level, "%s", buf);
  }

  /* Don't display debugging message on screen */
  if (level > LL_MESSAGE)
    return 0;

  /* Only pause if this is a message following an error */
  if ((level > LL_ERROR) && OptMsgErr && !dupe)
    error_pause();

  size_t width = msgwin_get_width();
  mutt_simple_format(ErrorBuf, sizeof(ErrorBuf), 0, width ? width : sizeof(ErrorBuf),
                     JUSTIFY_LEFT, 0, buf, sizeof(buf), false);
  ErrorBufMessage = true;

  if (!OptKeepQuiet)
  {
    enum ColorId color = MT_COLOR_NORMAL;
    switch (level)
    {
      case LL_ERROR:
        mutt_beep(false);
        color = MT_COLOR_ERROR;
        break;
      case LL_WARNING:
        color = MT_COLOR_WARNING;
        break;
      default:
        color = MT_COLOR_MESSAGE;
        break;
    }

    msgwin_set_text(color, ErrorBuf);
  }

  if ((level <= LL_ERROR) && !dupe)
  {
    OptMsgErr = true;
    LastError = mutt_date_epoch_ms();
  }
  else
  {
    OptMsgErr = false;
    LastError = 0;
  }

  return ret;
}

/**
 * mutt_log_prep - Prepare to log
 */
void mutt_log_prep(void)
{
  char ver[64];
  snprintf(ver, sizeof(ver), "-%s%s", PACKAGE_VERSION, GitVer);
  log_file_set_version(ver);
}

/**
 * mutt_log_stop - Close the log file
 */
void mutt_log_stop(void)
{
  log_file_close(false);
  FREE(&CurrentFile);
}

/**
 * mutt_log_set_file - Change the logging file
 * @param file Name to use
 * @retval  0 Success, file opened
 * @retval -1 Error, see errno
 *
 * Close the old log, rotate the new logs and open the new log.
 */
int mutt_log_set_file(const char *file)
{
  const char *const c_debug_file = cs_subset_path(NeoMutt->sub, "debug_file");
  if (!mutt_str_equal(CurrentFile, c_debug_file))
  {
    const char *name = rotate_logs(c_debug_file, NumOfLogs);
    if (!name)
      return -1;

    log_file_set_filename(name, false);
    FREE(&name);
    mutt_str_replace(&CurrentFile, c_debug_file);
  }

  cs_subset_str_string_set(NeoMutt->sub, "debug_file", file, NULL);

  return 0;
}

/**
 * mutt_log_set_level - Change the logging level
 * @param level Logging level
 * @param verbose If true, then log the event
 * @retval  0 Success
 * @retval -1 Error, level is out of range
 */
int mutt_log_set_level(enum LogLevel level, bool verbose)
{
  if (!CurrentFile)
  {
    const char *const c_debug_file = cs_subset_path(NeoMutt->sub, "debug_file");
    mutt_log_set_file(c_debug_file);
  }

  if (log_file_set_level(level, verbose) != 0)
    return -1;

  cs_subset_str_native_set(NeoMutt->sub, "debug_level", level, NULL);
  return 0;
}

/**
 * mutt_log_start - Enable file logging
 * @retval  0 Success, or already running
 * @retval -1 Failed to start
 *
 * This also handles file rotation.
 */
int mutt_log_start(void)
{
  const short c_debug_level = cs_subset_number(NeoMutt->sub, "debug_level");
  if (c_debug_level < 1)
    return 0;

  if (log_file_running())
    return 0;

  const char *const c_debug_file = cs_subset_path(NeoMutt->sub, "debug_file");
  mutt_log_set_file(c_debug_file);

  /* This will trigger the file creation */
  if (log_file_set_level(c_debug_level, true) < 0)
    return -1;

  return 0;
}

/**
 * level_validator - Validate the "debug_level" config variable - Implements ConfigDef::validator() - @ingroup cfg_def_validator
 */
int level_validator(const struct ConfigSet *cs, const struct ConfigDef *cdef,
                    intptr_t value, struct Buffer *err)
{
  if ((value < 0) || (value >= LL_MAX))
  {
    mutt_buffer_printf(err, _("Invalid value for option %s: %ld"), cdef->name, value);
    return CSR_ERR_INVALID;
  }

  return CSR_SUCCESS;
}

/**
 * main_log_observer - Notification that a Config Variable has changed - Implements ::observer_t
 */
int main_log_observer(struct NotifyCallback *nc)
{
  if ((nc->event_type != NT_CONFIG) || !nc->event_data)
    return -1;

  struct EventConfig *ev_c = nc->event_data;

  if (mutt_str_equal(ev_c->name, "debug_file"))
  {
    const char *const c_debug_file = cs_subset_path(NeoMutt->sub, "debug_file");
    mutt_log_set_file(c_debug_file);
  }
  else if (mutt_str_equal(ev_c->name, "debug_level"))
  {
    const short c_debug_level = cs_subset_number(NeoMutt->sub, "debug_level");
    mutt_log_set_level(c_debug_level, true);
  }
  else
  {
    return 0;
  }

  mutt_debug(LL_DEBUG5, "log done\n");
  return 0;
}
