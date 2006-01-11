/*
 * editorp.c :  Pipelined variation of the ra_svn editor
 *
 * ====================================================================
 * Copyright (c) 2000-2004 CollabNet.  All rights reserved.
 *
 * This software is licensed as described in the file COPYING, which
 * you should have received as part of this distribution.  The terms
 * are also available at http://subversion.tigris.org/license-1.html.
 * If newer versions of this license are posted there, you may use a
 * newer version instead, at your option.
 *
 * This software consists of voluntary contributions made by many
 * individuals.  For exact contribution history, see the revision
 * history and logs, available at http://subversion.tigris.org/.
 * ====================================================================
 */



#define APR_WANT_STRFUNC
#include <apr_want.h>
#include <apr_general.h>
#include <apr_strings.h>
#include <apr_md5.h>

#include <assert.h>

#include "svn_types.h"
#include "svn_string.h"
#include "svn_error.h"
#include "svn_path.h"
#include "svn_delta.h"
#include "svn_ra_svn.h"
#include "svn_pools.h"
#include "svn_private_config.h"
#include "svn_utf.h"
#include "svn_ebcdic.h"

#include "ra_svn.h"

/*
 * Both the client and server in the svn protocol need to drive and
 * consume editors.  For a commit, the client drives and the server
 * consumes; for an update/switch/status/diff, the server drives and
 * the client consumes.  This file provides a generic framework for
 * marshalling and unmarshalling editor operations over an svn
 * connection; both ends are useful for both server and client.
 */

typedef struct {
  svn_ra_svn_conn_t *conn;
  svn_ra_svn_edit_callback callback;    /* Called on successful completion. */
  void *callback_baton;
  int next_token;
  svn_boolean_t got_status;
} ra_svn_edit_baton_t;

/* Works for both directories and files. */
typedef struct {
  svn_ra_svn_conn_t *conn;
  apr_pool_t *pool;
  ra_svn_edit_baton_t *eb;
  const char *token;
} ra_svn_baton_t;

typedef struct {
  const svn_delta_editor_t *editor;
  void *edit_baton;
  apr_hash_t *tokens;
  svn_boolean_t *aborted;
  svn_boolean_t done;
  apr_pool_t *pool;
  apr_pool_t *file_pool;
  int file_refs;
} ra_svn_driver_state_t;

/* Works for both directories and files; however, the pool handling is
   different for files.  To save space during commits (where file
   batons generally last until the end of the commit), token entries
   for files are all created in a single reference-counted pool (the
   file_pool member of the driver state structure), which is cleared
   at close_file time when the reference count hits zero.  So the pool
   field in this structure is vestigial for files, and we use it for a
   different purpose instead: at apply-textdelta time, we set it to a
   subpool of the file pool, which is destroyed in textdelta-end. */
typedef struct {
  const char *token;
  void *baton;
  svn_boolean_t is_file;
  svn_stream_t *dstream;  /* svndiff stream for apply_textdelta */
  apr_pool_t *pool;
} ra_svn_token_entry_t;

#define ABORT_EDIT_STR \
       "\x61\x62\x6f\x72\x74\x2d\x65\x64\x69\x74"
       /* "abort-edit" */

#define ADD_DIR_STR \
        "\x61\x64\x64\x2d\x64\x69\x72"
        /* "add-dir" */

#define ADD_FILE_STR \
        "\x61\x64\x64\x2d\x66\x69\x6c\x65"
        /* "add-file" */

#define APPLY_TEXTDELTA_STR \
        "\x61\x70\x70\x6c\x79\x2d\x74\x65\x78\x74\x64\x65\x6c\x74\x61"
        /* "apply-textdelta" */

#define CHANGE_DIR_PROP_STR \
        "\x63\x68\x61\x6e\x67\x65\x2d\x64\x69\x72\x2d\x70\x72\x6f\x70"
        /* "change-dir-prop" */

#define CHANGE_FILE_PROP_STR \
        "\x63\x68\x61\x6e\x67\x65\x2d\x66\x69\x6c\x65\x2d\x70\x72\x6f\x70"
        /* "change-file-prop" */

#define CLOSE_DIR_STR \
        "\x63\x6c\x6f\x73\x65\x2d\x64\x69\x72"
        /* "close-dir" */

#define CLOSE_EDIT_STR \
        "\x63\x6c\x6f\x73\x65\x2d\x65\x64\x69\x74"
        /* "close-edit" */

#define CLOSE_FILE_STR \
        "\x63\x6c\x6f\x73\x65\x2d\x66\x69\x6c\x65"
        /* "close-file" */

#define DEL_ENTRY_STR \
        "\x64\x65\x6c\x65\x74\x65\x2d\x65\x6e\x74\x72\x79"
        /* "delete-entry" */

#define OPEN_DIR_STR \
        "\x6f\x70\x65\x6e\x2d\x64\x69\x72"
        /* "open-dir" */

#define OPEN_FILE_STR \
        "\x6f\x70\x65\x6e\x2d\x66\x69\x6c\x65"
        /* "open-file" */

#define OPEN_ROOT_STR \
        "\x6f\x70\x65\x6e\x2d\x72\x6f\x6f\x74"
        /* "open-root" */

#define TARGET_REV_STR \
        "\x74\x61\x72\x67\x65\x74\x2d\x72\x65\x76"
        /* "target-rev" */

#define TEXTDELTA_CNUNK_STR \
        "\x74\x65\x78\x74\x64\x65\x6c\x74\x61\x2d\x63\x68\x75\x6e\x6b"
        /* "textdelta-chunk" */

#define TEXTDELTA_END_STR \
        "\x74\x65\x78\x74\x64\x65\x6c\x74\x61\x2d\x65\x6e\x64"
        /* "textdelta-end" */

/* --- CONSUMING AN EDITOR BY PASSING EDIT OPERATIONS OVER THE NET --- */

static const char *make_token(char type, ra_svn_edit_baton_t *eb,
                              apr_pool_t *pool)
{
  return APR_PSPRINTF2(pool, "%c%d", type, eb->next_token++);
}

static ra_svn_baton_t *ra_svn_make_baton(svn_ra_svn_conn_t *conn,
                                         apr_pool_t *pool,
                                         ra_svn_edit_baton_t *eb,
                                         const char *token)
{
  ra_svn_baton_t *b;

  b = apr_palloc(pool, sizeof(*b));
  b->conn = conn;
  b->pool = pool;
  b->eb = eb;
  b->token = token;
  return b;
}

/* Check for an early error status report from the consumer.  If we
 * get one, abort the edit and return the error. */
static svn_error_t *check_for_error(ra_svn_edit_baton_t *eb, apr_pool_t *pool)
{
  assert(!eb->got_status);
  if (svn_ra_svn__input_waiting(eb->conn, pool))
    {
      eb->got_status = TRUE;
      SVN_ERR(svn_ra_svn_write_cmd(eb->conn, pool, ABORT_EDIT_STR, ""));
      SVN_ERR(svn_ra_svn_read_cmd_response(eb->conn, pool, ""));
      /* We shouldn't get here if the consumer is doing its job. */
      return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                              _("Successful edit status returned too soon"));
    }
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_target_rev(void *edit_baton, svn_revnum_t rev,
                                      apr_pool_t *pool)
{
  ra_svn_edit_baton_t *eb = edit_baton;

  SVN_ERR(check_for_error(eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(eb->conn, pool, TARGET_REV_STR, "r", rev));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_open_root(void *edit_baton, svn_revnum_t rev,
                                     apr_pool_t *pool, void **root_baton)
{
  ra_svn_edit_baton_t *eb = edit_baton;
  const char *token = make_token(SVN_UTF8_d, eb, pool);

  SVN_ERR(check_for_error(eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(eb->conn, pool, OPEN_ROOT_STR, "(?r)c", rev,
                               token));
  *root_baton = ra_svn_make_baton(eb->conn, pool, eb, token);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_delete_entry(const char *path, svn_revnum_t rev,
                                        void *parent_baton, apr_pool_t *pool)
{
  ra_svn_baton_t *b = parent_baton;

  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, DEL_ENTRY_STR, "c(?r)c",
                               path, rev, b->token));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_add_dir(const char *path, void *parent_baton,
                                   const char *copy_path,
                                   svn_revnum_t copy_rev,
                                   apr_pool_t *pool, void **child_baton)
{
  ra_svn_baton_t *b = parent_baton;
  const char *token = make_token(SVN_UTF8_d, b->eb, pool);

  assert((copy_path && SVN_IS_VALID_REVNUM(copy_rev))
         || (!copy_path && !SVN_IS_VALID_REVNUM(copy_rev)));
  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, ADD_DIR_STR, "ccc(?cr)", path,
                               b->token, token, copy_path, copy_rev));
  *child_baton = ra_svn_make_baton(b->conn, pool, b->eb, token);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_open_dir(const char *path, void *parent_baton,
                                    svn_revnum_t rev, apr_pool_t *pool,
                                    void **child_baton)
{
  ra_svn_baton_t *b = parent_baton;
  const char *token = make_token(SVN_UTF8_d, b->eb, pool);

  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, OPEN_DIR_STR, "ccc(?r)",
                               path, b->token, token, rev));
  *child_baton = ra_svn_make_baton(b->conn, pool, b->eb, token);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_change_dir_prop(void *dir_baton, const char *name,
                                           const svn_string_t *value,
                                           apr_pool_t *pool)
{
  ra_svn_baton_t *b = dir_baton;

  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, CHANGE_DIR_PROP_STR, "cc(?s)",
                               b->token, name, value));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_close_dir(void *dir_baton, apr_pool_t *pool)
{
  ra_svn_baton_t *b = dir_baton;

  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, CLOSE_DIR_STR, "c", b->token));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_add_file(const char *path,
                                    void *parent_baton,
                                    const char *copy_path,
                                    svn_revnum_t copy_rev,
                                    apr_pool_t *pool,
                                    void **file_baton)
{
  ra_svn_baton_t *b = parent_baton;
  const char *token = make_token(SVN_UTF8_c, b->eb, pool);

  assert((copy_path && SVN_IS_VALID_REVNUM(copy_rev))
         || (!copy_path && !SVN_IS_VALID_REVNUM(copy_rev)));
  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, ADD_FILE_STR, "ccc(?cr)", path,
                               b->token, token, copy_path, copy_rev));
  *file_baton = ra_svn_make_baton(b->conn, pool, b->eb, token);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_open_file(const char *path,
                                     void *parent_baton,
                                     svn_revnum_t rev,
                                     apr_pool_t *pool,
                                     void **file_baton)
{
  ra_svn_baton_t *b = parent_baton;
  const char *token = make_token(SVN_UTF8_c, b->eb, pool);

  SVN_ERR(check_for_error(b->eb, b->pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, OPEN_FILE_STR, "ccc(?r)",
                               path, b->token, token, rev));
  *file_baton = ra_svn_make_baton(b->conn, pool, b->eb, token);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_svndiff_handler(void *baton, const char *data,
                                           apr_size_t *len)
{
  ra_svn_baton_t *b = baton;
  svn_string_t str;

  SVN_ERR(check_for_error(b->eb, b->pool));
  str.data = data;
  str.len = *len;
  return svn_ra_svn_write_cmd(b->conn, b->pool, TEXTDELTA_CNUNK_STR, "cs",
                              b->token, &str);
}

static svn_error_t *ra_svn_svndiff_close_handler(void *baton)
{
  ra_svn_baton_t *b = baton;

  SVN_ERR(check_for_error(b->eb, b->pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, b->pool, TEXTDELTA_END_STR, "c",
                               b->token));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_apply_textdelta(void *file_baton,
                                           const char *base_checksum,
                                           apr_pool_t *pool,
                                           svn_txdelta_window_handler_t *wh,
                                           void **wh_baton)
{
  ra_svn_baton_t *b = file_baton;
  svn_stream_t *diff_stream;

  /* Tell the other side we're starting a text delta. */
  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, APPLY_TEXTDELTA_STR, "c(?c)",
                               b->token, base_checksum));

  /* Transform the window stream to an svndiff stream.  Reuse the
   * file baton for the stream handler, since it has all the
   * needed information. */
  diff_stream = svn_stream_create(b, pool);
  svn_stream_set_write(diff_stream, ra_svn_svndiff_handler);
  svn_stream_set_close(diff_stream, ra_svn_svndiff_close_handler);
  svn_txdelta_to_svndiff(diff_stream, pool, wh, wh_baton);
  return SVN_NO_ERROR;
}
  
static svn_error_t *ra_svn_change_file_prop(void *file_baton,
                                            const char *name,
                                            const svn_string_t *value,
                                            apr_pool_t *pool)
{
  ra_svn_baton_t *b = file_baton;

  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, CHANGE_FILE_PROP_STR, "cc(?s)",
                               b->token, name, value));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_close_file(void *file_baton,
                                      const char *text_checksum,
                                      apr_pool_t *pool)
{
  ra_svn_baton_t *b = file_baton;

  SVN_ERR(check_for_error(b->eb, pool));
  SVN_ERR(svn_ra_svn_write_cmd(b->conn, pool, CLOSE_FILE_STR, "c(?c)",
                               b->token, text_checksum));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_close_edit(void *edit_baton, apr_pool_t *pool)
{
  ra_svn_edit_baton_t *eb = edit_baton;
  svn_error_t *err;

  assert(!eb->got_status);
  eb->got_status = TRUE;
  SVN_ERR(svn_ra_svn_write_cmd(eb->conn, pool, CLOSE_EDIT_STR, ""));
  err = svn_ra_svn_read_cmd_response(eb->conn, pool, "");
  if (err)
    {
      svn_error_clear(svn_ra_svn_write_cmd(eb->conn, pool, ABORT_EDIT_STR, ""));
      return err;
    }
  if (eb->callback)
    SVN_ERR(eb->callback(eb->callback_baton));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_abort_edit(void *edit_baton, apr_pool_t *pool)
{
  ra_svn_edit_baton_t *eb = edit_baton;

  if (eb->got_status)
    return SVN_NO_ERROR;
  SVN_ERR(svn_ra_svn_write_cmd(eb->conn, pool, ABORT_EDIT_STR, ""));
  SVN_ERR(svn_ra_svn_read_cmd_response(eb->conn, pool, ""));
  return SVN_NO_ERROR;
}

void svn_ra_svn__get_editorp(const svn_delta_editor_t **editor,
                             void **edit_baton, svn_ra_svn_conn_t *conn,
                             apr_pool_t *pool,
                             svn_ra_svn_edit_callback callback,
                             void *callback_baton)
{
  svn_delta_editor_t *ra_svn_editor = svn_delta_default_editor(pool);
  ra_svn_edit_baton_t *eb;

  eb = apr_palloc(pool, sizeof(*eb));
  eb->conn = conn;
  eb->callback = callback;
  eb->callback_baton = callback_baton;
  eb->next_token = 0;
  eb->got_status = FALSE;

  ra_svn_editor->set_target_revision = ra_svn_target_rev;
  ra_svn_editor->open_root = ra_svn_open_root;
  ra_svn_editor->delete_entry = ra_svn_delete_entry;
  ra_svn_editor->add_directory = ra_svn_add_dir;
  ra_svn_editor->open_directory = ra_svn_open_dir;
  ra_svn_editor->change_dir_prop = ra_svn_change_dir_prop;
  ra_svn_editor->close_directory = ra_svn_close_dir;
  ra_svn_editor->add_file = ra_svn_add_file;
  ra_svn_editor->open_file = ra_svn_open_file;
  ra_svn_editor->apply_textdelta = ra_svn_apply_textdelta;
  ra_svn_editor->change_file_prop = ra_svn_change_file_prop;
  ra_svn_editor->close_file = ra_svn_close_file;
  ra_svn_editor->close_edit = ra_svn_close_edit;
  ra_svn_editor->abort_edit = ra_svn_abort_edit;

  *editor = ra_svn_editor;
  *edit_baton = eb;
}

/* --- DRIVING AN EDITOR --- */

/* Store a token entry.  The token string will be copied into pool. */
static ra_svn_token_entry_t *store_token(ra_svn_driver_state_t *ds,
                                         void *baton, const char *token,
                                         svn_boolean_t is_file,
                                         apr_pool_t *pool)
{
  ra_svn_token_entry_t *entry;

  entry = apr_palloc(pool, sizeof(*entry));
  entry->token = apr_pstrdup(pool, token);
  entry->baton = baton;
  entry->is_file = is_file;
  entry->dstream = NULL;
  entry->pool = pool;
  apr_hash_set(ds->tokens, entry->token, APR_HASH_KEY_STRING, entry);
  return entry;
}

static svn_error_t *lookup_token(ra_svn_driver_state_t *ds, const char *token,
                                 svn_boolean_t is_file,
                                 ra_svn_token_entry_t **entry)
{
  *entry = apr_hash_get(ds->tokens, token, APR_HASH_KEY_STRING);
  if (!*entry || (*entry)->is_file != is_file)
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            _("Invalid file or dir token during edit"));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_target_rev(svn_ra_svn_conn_t *conn,
                                             apr_pool_t *pool,
                                             apr_array_header_t *params,
                                             ra_svn_driver_state_t *ds)
{
  svn_revnum_t rev;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "r", &rev));
  SVN_CMD_ERR(ds->editor->set_target_revision(ds->edit_baton, rev, pool));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_open_root(svn_ra_svn_conn_t *conn,
                                            apr_pool_t *pool,
                                            apr_array_header_t *params,
                                            ra_svn_driver_state_t *ds)
{
  svn_revnum_t rev;
  apr_pool_t *subpool;
  const char *token;
  void *root_baton;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "(?r)c", &rev, &token));
  subpool = svn_pool_create(ds->pool);
  SVN_CMD_ERR(ds->editor->open_root(ds->edit_baton, rev, subpool,
                                    &root_baton));
  store_token(ds, root_baton, token, FALSE, subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_delete_entry(svn_ra_svn_conn_t *conn,
                                               apr_pool_t *pool,
                                               apr_array_header_t *params,
                                               ra_svn_driver_state_t *ds)
{
  const char *path, *token;
  svn_revnum_t rev;
  ra_svn_token_entry_t *entry;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c(?r)c", &path, &rev, &token));
  SVN_ERR(lookup_token(ds, token, FALSE, &entry));
  path = svn_path_canonicalize(path, pool);
  SVN_CMD_ERR(ds->editor->delete_entry(path, rev, entry->baton, pool));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_add_dir(svn_ra_svn_conn_t *conn,
                                          apr_pool_t *pool,
                                          apr_array_header_t *params,
                                          ra_svn_driver_state_t *ds)
{
  const char *path, *token, *child_token, *copy_path;
  svn_revnum_t copy_rev;
  ra_svn_token_entry_t *entry;
  apr_pool_t *subpool;
  void *child_baton;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "ccc(?cr)", &path, &token,
                                 &child_token, &copy_path, &copy_rev));
  SVN_ERR(lookup_token(ds, token, FALSE, &entry));
  subpool = svn_pool_create(entry->pool);
  path = svn_path_canonicalize(path, pool);
  if (copy_path)
    copy_path = svn_path_canonicalize(copy_path, pool);
  SVN_CMD_ERR(ds->editor->add_directory(path, entry->baton, copy_path,
                                        copy_rev, subpool, &child_baton));
  store_token(ds, child_baton, child_token, FALSE, subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_open_dir(svn_ra_svn_conn_t *conn,
                                           apr_pool_t *pool,
                                           apr_array_header_t *params,
                                           ra_svn_driver_state_t *ds)
{
  const char *path, *token, *child_token;
  svn_revnum_t rev;
  ra_svn_token_entry_t *entry;
  apr_pool_t *subpool;
  void *child_baton;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "ccc(?r)", &path, &token,
                                 &child_token, &rev));
  SVN_ERR(lookup_token(ds, token, FALSE, &entry));
  subpool = svn_pool_create(entry->pool);
  path = svn_path_canonicalize(path, pool);
  SVN_CMD_ERR(ds->editor->open_directory(path, entry->baton, rev, subpool,
                                         &child_baton));
  store_token(ds, child_baton, child_token, FALSE, subpool);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_change_dir_prop(svn_ra_svn_conn_t *conn,
                                                  apr_pool_t *pool,
                                                  apr_array_header_t *params,
                                                  ra_svn_driver_state_t *ds)
{
  const char *token, *name;
  svn_string_t *value;
  ra_svn_token_entry_t *entry;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "cc(?s)", &token, &name,
                                 &value));
  SVN_ERR(lookup_token(ds, token, FALSE, &entry));
  SVN_CMD_ERR(ds->editor->change_dir_prop(entry->baton, name, value,
                                          entry->pool));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_close_dir(svn_ra_svn_conn_t *conn,
                                            apr_pool_t *pool,
                                            apr_array_header_t *params,
                                            ra_svn_driver_state_t *ds)
{
  const char *token;
  ra_svn_token_entry_t *entry;

  /* Parse and look up the directory token. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c", &token));
  SVN_ERR(lookup_token(ds, token, FALSE, &entry));

  /* Close the directory and destroy the baton. */
  SVN_CMD_ERR(ds->editor->close_directory(entry->baton, pool));
  apr_hash_set(ds->tokens, token, APR_HASH_KEY_STRING, NULL);
  apr_pool_destroy(entry->pool);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_add_file(svn_ra_svn_conn_t *conn,
                                           apr_pool_t *pool,
                                           apr_array_header_t *params,
                                           ra_svn_driver_state_t *ds)
{
  const char *path, *token, *file_token, *copy_path;
  svn_revnum_t copy_rev;
  ra_svn_token_entry_t *entry, *file_entry;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "ccc(?cr)", &path, &token,
                                 &file_token, &copy_path, &copy_rev));
  SVN_ERR(lookup_token(ds, token, FALSE, &entry));
  ds->file_refs++;
  path = svn_path_canonicalize(path, pool);
  if (copy_path)
    copy_path = svn_path_canonicalize(copy_path, pool);
  file_entry = store_token(ds, NULL, file_token, TRUE, ds->file_pool);
  SVN_CMD_ERR(ds->editor->add_file(path, entry->baton, copy_path, copy_rev,
                                   ds->file_pool, &file_entry->baton));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_open_file(svn_ra_svn_conn_t *conn,
                                            apr_pool_t *pool,
                                            apr_array_header_t *params,
                                            ra_svn_driver_state_t *ds)
{
  const char *path, *token, *file_token;
  svn_revnum_t rev;
  ra_svn_token_entry_t *entry, *file_entry;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "ccc(?r)", &path, &token,
                                 &file_token, &rev));
  SVN_ERR(lookup_token(ds, token, FALSE, &entry));
  ds->file_refs++;
  path = svn_path_canonicalize(path, pool);
  file_entry = store_token(ds, NULL, file_token, TRUE, ds->file_pool);
  SVN_CMD_ERR(ds->editor->open_file(path, entry->baton, rev, ds->file_pool,
                                    &file_entry->baton));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_apply_textdelta(svn_ra_svn_conn_t *conn,
                                                  apr_pool_t *pool,
                                                  apr_array_header_t *params,
                                                  ra_svn_driver_state_t *ds)
{
  const char *token;
  ra_svn_token_entry_t *entry;
  svn_txdelta_window_handler_t wh;
  void *wh_baton;
  char *base_checksum;

  /* Parse arguments and look up the token. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c(?c)",
                                 &token, &base_checksum));
  SVN_ERR(lookup_token(ds, token, TRUE, &entry));
  if (entry->dstream)
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            _("Apply-textdelta already active"));
  entry->pool = svn_pool_create(ds->file_pool);
  SVN_CMD_ERR(ds->editor->apply_textdelta(entry->baton, base_checksum,
                                          entry->pool, &wh, &wh_baton));
  entry->dstream = svn_txdelta_parse_svndiff(wh, wh_baton, TRUE, entry->pool);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_textdelta_chunk(svn_ra_svn_conn_t *conn,
                                                  apr_pool_t *pool,
                                                  apr_array_header_t *params,
                                                  ra_svn_driver_state_t *ds)
{
  const char *token;
  ra_svn_token_entry_t *entry;
  svn_string_t *str;

  /* Parse arguments and look up the token. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "cs", &token, &str));
  SVN_ERR(lookup_token(ds, token, TRUE, &entry));
  if (!entry->dstream)
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            _("Apply-textdelta not active"));
  SVN_CMD_ERR(svn_stream_write(entry->dstream, str->data, &str->len));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_textdelta_end(svn_ra_svn_conn_t *conn,
                                                apr_pool_t *pool,
                                                apr_array_header_t *params,
                                                ra_svn_driver_state_t *ds)
{
  const char *token;
  ra_svn_token_entry_t *entry;

  /* Parse arguments and look up the token. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c", &token));
  SVN_ERR(lookup_token(ds, token, TRUE, &entry));
  if (!entry->dstream)
    return svn_error_create(SVN_ERR_RA_SVN_MALFORMED_DATA, NULL,
                            _("Apply-textdelta not active"));
  SVN_CMD_ERR(svn_stream_close(entry->dstream));
  entry->dstream = NULL;
  apr_pool_destroy(entry->pool);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_change_file_prop(svn_ra_svn_conn_t *conn,
                                                   apr_pool_t *pool,
                                                   apr_array_header_t *params,
                                                   ra_svn_driver_state_t *ds)
{
  const char *token, *name;
  svn_string_t *value;
  ra_svn_token_entry_t *entry;

  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "cc(?s)", &token, &name,
                                 &value));
  SVN_ERR(lookup_token(ds, token, TRUE, &entry));
  SVN_CMD_ERR(ds->editor->change_file_prop(entry->baton, name, value, pool));
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_close_file(svn_ra_svn_conn_t *conn,
                                             apr_pool_t *pool,
                                             apr_array_header_t *params,
                                             ra_svn_driver_state_t *ds)
{
  const char *token;
  ra_svn_token_entry_t *entry;
  const char *text_checksum;

  /* Parse arguments and look up the file token. */
  SVN_ERR(svn_ra_svn_parse_tuple(params, pool, "c(?c)",
                                 &token, &text_checksum));
  SVN_ERR(lookup_token(ds, token, TRUE, &entry));

  /* Close the file and destroy the baton. */
  SVN_CMD_ERR(ds->editor->close_file(entry->baton, text_checksum, pool));
  apr_hash_set(ds->tokens, token, APR_HASH_KEY_STRING, NULL);
  if (--ds->file_refs == 0)
    apr_pool_clear(ds->file_pool);
  return SVN_NO_ERROR;
}

static svn_error_t *ra_svn_handle_close_edit(svn_ra_svn_conn_t *conn,
                                             apr_pool_t *pool,
                                             apr_array_header_t *params,
                                             ra_svn_driver_state_t *ds)
{
  SVN_CMD_ERR(ds->editor->close_edit(ds->edit_baton, pool));
  ds->done = TRUE;
  if (ds->aborted)
    *ds->aborted = FALSE;
  return svn_ra_svn_write_cmd_response(conn, pool, "");
}

static svn_error_t *ra_svn_handle_abort_edit(svn_ra_svn_conn_t *conn,
                                             apr_pool_t *pool,
                                             apr_array_header_t *params,
                                             ra_svn_driver_state_t *ds)
{
  ds->done = TRUE;
  if (ds->aborted)
    *ds->aborted = TRUE;
  SVN_CMD_ERR(ds->editor->abort_edit(ds->edit_baton, pool));
  return svn_ra_svn_write_cmd_response(conn, pool, "");
}

static const struct {
  const char *cmd;
  svn_error_t *(*handler)(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                          apr_array_header_t *params,
                          ra_svn_driver_state_t *ds);
} ra_svn_edit_cmds[] = {
#if APR_CHARSET_EBCDIC
#pragma convert(1208)
#endif
  { "target-rev",       ra_svn_handle_target_rev },
  { "open-root",        ra_svn_handle_open_root },
  { "delete-entry",     ra_svn_handle_delete_entry },
  { "add-dir",          ra_svn_handle_add_dir },
  { "open-dir",         ra_svn_handle_open_dir },
  { "change-dir-prop",  ra_svn_handle_change_dir_prop },
  { "close-dir",        ra_svn_handle_close_dir },
  { "add-file",         ra_svn_handle_add_file },
  { "open-file",        ra_svn_handle_open_file },
  { "apply-textdelta",  ra_svn_handle_apply_textdelta },
  { "textdelta-chunk",  ra_svn_handle_textdelta_chunk },
  { "textdelta-end",    ra_svn_handle_textdelta_end },
  { "change-file-prop", ra_svn_handle_change_file_prop },
  { "close-file",       ra_svn_handle_close_file },
  { "close-edit",       ra_svn_handle_close_edit },
  { "abort-edit",       ra_svn_handle_abort_edit },
  { NULL }
#if APR_CHARSET_EBCDIC
#pragma convert(37)
#endif
};

static svn_error_t *blocked_write(svn_ra_svn_conn_t *conn, apr_pool_t *pool,
                                  void *baton)
{
  ra_svn_driver_state_t *ds = baton;
  const char *cmd;
  apr_array_header_t *params;

  /* We blocked trying to send an error.  Read and discard an editing
   * command in order to avoid deadlock. */
  SVN_ERR(svn_ra_svn_read_tuple(conn, pool, "wl", &cmd, &params));
  if (strcmp(cmd, ABORT_EDIT_STR) == 0)
    {
      ds->done = TRUE;
      svn_ra_svn__set_block_handler(conn, NULL, NULL);
    }
  return SVN_NO_ERROR;
}

svn_error_t *svn_ra_svn__drive_editorp(svn_ra_svn_conn_t *conn,
                                       apr_pool_t *pool,
                                       const svn_delta_editor_t *editor,
                                       void *edit_baton,
                                       svn_boolean_t *aborted)
{
  ra_svn_driver_state_t state;
  apr_pool_t *subpool = svn_pool_create(pool);
  const char *cmd;
  int i;
  svn_error_t *err, *write_err;
  apr_array_header_t *params;

  state.editor = editor;
  state.edit_baton = edit_baton;
  state.tokens = apr_hash_make(pool);
  state.aborted = aborted;
  state.done = FALSE;
  state.pool = pool;
  state.file_pool = svn_pool_create(pool);
  state.file_refs = 0;

  while (!state.done)
    {
      apr_pool_clear(subpool);
      SVN_ERR(svn_ra_svn_read_tuple(conn, subpool, "wl", &cmd, &params));
      for (i = 0; ra_svn_edit_cmds[i].cmd; i++)
        {
          if (strcmp(cmd, ra_svn_edit_cmds[i].cmd) == 0)
            break;
        }
      if (ra_svn_edit_cmds[i].cmd)
        err = (*ra_svn_edit_cmds[i].handler)(conn, subpool, params, &state);
      else
        {
          err = svn_error_createf(SVN_ERR_RA_SVN_UNKNOWN_CMD, NULL,
                                  _("Unknown command '%s'"), cmd);
          err = svn_error_create(SVN_ERR_RA_SVN_CMD_ERR, err, NULL);
        }

      if (err && err->apr_err == SVN_ERR_RA_SVN_CMD_ERR)
        {
          if (aborted)
            *aborted = TRUE;
          if (!state.done)
            {
              /* Abort the edit and use non-blocking I/O to write the error. */
              svn_error_clear (editor->abort_edit(edit_baton, subpool));
              svn_ra_svn__set_block_handler(conn, blocked_write, &state);
            }
          write_err = svn_ra_svn_write_cmd_failure(conn, subpool, err->child);
          if (!write_err)
            write_err = svn_ra_svn_flush(conn, subpool);
          svn_ra_svn__set_block_handler(conn, NULL, NULL);
          svn_error_clear(err);
          SVN_ERR(write_err);
          break;
        }
      SVN_ERR(err);
    }

  /* Read and discard editing commands until the edit is complete. */
  while (!state.done)
    {
      apr_pool_clear(subpool);
      SVN_ERR(svn_ra_svn_read_tuple(conn, subpool, "wl", &cmd, &params));
      state.done = (strcmp(cmd, ABORT_EDIT_STR) == 0);
    }

  apr_pool_destroy(subpool);
  return SVN_NO_ERROR;
}
