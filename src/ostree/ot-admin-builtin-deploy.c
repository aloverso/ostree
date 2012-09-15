/* -*- mode: C; c-file-style: "gnu"; indent-tabs-mode: nil; -*-
 *
 * Copyright (C) 2012 Colin Walters <walters@verbum.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 *
 * Author: Colin Walters <walters@verbum.org>
 */

#include "config.h"

#include "ot-admin-builtins.h"
#include "ot-admin-functions.h"
#include "ostree.h"

#include <glib/gi18n.h>

typedef struct {
  OstreeRepo  *repo;
  GFile *ostree_dir;
} OtAdminDeploy;

static gboolean opt_no_kernel;
static gboolean opt_force;

static GOptionEntry options[] = {
  { "no-kernel", 0, 0, G_OPTION_ARG_NONE, &opt_no_kernel, "Don't update kernel related config (initramfs, bootloader)", NULL },
  { "force", 0, 0, G_OPTION_ARG_NONE, &opt_force, "Overwrite any existing deployment", NULL },
  { NULL }
};

/**
 * update_current:
 *
 * Atomically swap the /ostree/current symbolic link to point to a new
 * path.  If successful, the old current will be saved as
 * /ostree/previous.
 *
 * Unless the new-current equals current, in which case, do nothing.
 */
static gboolean
update_current (OtAdminDeploy      *self,
                GFile              *current_deployment,
                GFile              *deploy_target,
                GCancellable       *cancellable,
                GError            **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *current_path = NULL;
  ot_lobj GFile *previous_path = NULL;
  ot_lobj GFile *tmp_current_path = NULL;
  ot_lobj GFile *tmp_previous_path = NULL;
  ot_lobj GFileInfo *previous_info = NULL;
  ot_lfree char *relative_current = NULL;
  ot_lfree char *relative_previous = NULL;

  current_path = g_file_get_child (self->ostree_dir, "current");
  previous_path = g_file_get_child (self->ostree_dir, "previous");

  relative_current = g_file_get_relative_path (self->ostree_dir, deploy_target);
  g_assert (relative_current);

  if (current_deployment)
    {
      ot_lfree char *relative_previous = NULL;

      if (g_file_equal (current_deployment, deploy_target))
        {
          g_print ("ostadmin: %s already points to %s\n", ot_gfile_get_path_cached (current_path),
                   relative_current);
          return TRUE;
        }

      tmp_previous_path = g_file_get_child (self->ostree_dir, "tmp-previous");
      (void) ot_gfile_unlink (tmp_previous_path, NULL, NULL);

      relative_previous = g_file_get_relative_path (self->ostree_dir, current_deployment);
      g_assert (relative_previous);
      if (symlink (relative_previous, ot_gfile_get_path_cached (tmp_previous_path)) < 0)
        {
          ot_util_set_error_from_errno (error, errno);
          goto out;
        }
    }

  tmp_current_path = g_file_get_child (self->ostree_dir, "tmp-current");
  (void) ot_gfile_unlink (tmp_current_path, NULL, NULL);

  if (symlink (relative_current, ot_gfile_get_path_cached (tmp_current_path)) < 0)
    {
      ot_util_set_error_from_errno (error, errno);
      goto out;
    }

  if (!ot_gfile_rename (tmp_current_path, current_path,
                        cancellable, error))
    goto out;

  if (tmp_previous_path)
    {
      if (!ot_gfile_rename (tmp_previous_path, previous_path,
                            cancellable, error))
        goto out;
    }

  g_print ("ostadmin: %s set to %s\n", ot_gfile_get_path_cached (current_path),
           relative_current);

  ret = TRUE;
 out:
  return ret;
}

typedef struct {
  GError **error;
  gboolean caught_error;

  GMainLoop *loop;
} ProcessOneCheckoutData;

static void
on_checkout_complete (GObject         *object,
                      GAsyncResult    *result,
                      gpointer         user_data)
{
  ProcessOneCheckoutData *data = user_data;
  GError *local_error = NULL;

  if (!ostree_repo_checkout_tree_finish ((OstreeRepo*)object, result,
                                         &local_error))
    goto out;

 out:
  if (local_error)
    {
      data->caught_error = TRUE;
      g_propagate_error (data->error, local_error);
    }
  g_main_loop_quit (data->loop);
}


/**
 * ensure_unlinked:
 *
 * Like ot_gfile_unlink(), but return successfully if the file doesn't
 * exist.
 */
static gboolean
ensure_unlinked (GFile         *path,
                 GCancellable  *cancellable,
                 GError       **error)
{
  gboolean ret = FALSE;
  GError *temp_error = NULL;

  if (!ot_gfile_unlink (path, cancellable, &temp_error))
    {
      if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
        {
          g_clear_error (&temp_error);
        }
      else
        {
          g_propagate_error (error, temp_error);
          goto out;
        }
    }
  
  ret = TRUE;
 out:
  return ret;
}

/**
 * copy_one_config_file:
 *
 * Copy @file from @modified_etc to @new_etc, overwriting any existing
 * file there.
 */
static gboolean
copy_one_config_file (OtAdminDeploy      *self,
                      GFile              *orig_etc,
                      GFile              *modified_etc,
                      GFile              *new_etc,
                      GFile              *src,
                      GCancellable       *cancellable,
                      GError            **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *dest = NULL;
  ot_lobj GFile *parent = NULL;
  ot_lfree char *relative_path = NULL;
  
  relative_path = g_file_get_relative_path (modified_etc, src);
  g_assert (relative_path);
  dest = g_file_resolve_relative_path (new_etc, relative_path);

  parent = g_file_get_parent (dest);

  /* FIXME actually we need to copy permissions and xattrs */
  if (!ot_gfile_ensure_directory (parent, TRUE, error))
    goto out;

  if (!g_file_copy (src, dest, G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA,
                    cancellable, NULL, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}

/**
 * merge_etc_changes:
 *
 * Compute the difference between @orig_etc and @modified_etc,
 * and apply that to @new_etc.
 *
 * The algorithm for computing the difference is pretty simple; it's
 * approximately equivalent to "diff -unR orig_etc modified_etc",
 * except that rather than attempting a 3-way merge if a file is also
 * changed in @new_etc, the modified version always wins.
 */
static gboolean
merge_etc_changes (OtAdminDeploy  *self,
                   GFile          *orig_etc,
                   GFile          *modified_etc,
                   GFile          *new_etc,
                   GCancellable   *cancellable,
                   GError        **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *ostree_etc = NULL;
  ot_lobj GFile *tmp_etc = NULL;
  ot_lptrarray GPtrArray *modified = NULL;
  ot_lptrarray GPtrArray *removed = NULL;
  ot_lptrarray GPtrArray *added = NULL;
  guint i;

  modified = g_ptr_array_new_with_free_func ((GDestroyNotify) ostree_diff_item_unref);
  removed = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);
  added = g_ptr_array_new_with_free_func ((GDestroyNotify) g_object_unref);

  if (!ostree_diff_dirs (orig_etc, modified_etc, modified, removed, added,
                         cancellable, error))
    {
      g_prefix_error (error, "While computing configuration diff: ");
      goto out;
    }

  if (modified->len > 0 || removed->len > 0 || added->len > 0)
    g_print ("ostadmin: Processing config: %u modified, %u removed, %u added\n", 
             modified->len,
             removed->len,
             added->len);
  else
    g_print ("ostadmin: No modified configuration\n");

  for (i = 0; i < removed->len; i++)
    {
      GFile *file = removed->pdata[i];
      ot_lobj GFile *target_file = NULL;
      ot_lfree char *path = NULL;

      path = g_file_get_relative_path (orig_etc, file);
      g_assert (path);
      target_file = g_file_resolve_relative_path (new_etc, path);

      if (!ensure_unlinked (target_file, cancellable, error))
        goto out;
    }

  for (i = 0; i < modified->len; i++)
    {
      OstreeDiffItem *diff = modified->pdata[i];

      if (!copy_one_config_file (self, orig_etc, modified_etc, new_etc, diff->target,
                                 cancellable, error))
        goto out;
    }
  for (i = 0; i < added->len; i++)
    {
      GFile *file = added->pdata[i];

      if (!copy_one_config_file (self, orig_etc, modified_etc, new_etc, file,
                                 cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  return ret;
}

/**
 * deploy_tree:
 *
 * Look up @revision in the repository, and check it out in
 * OSTREE_DIR/deploy/DEPLOY_TARGET.
 *
 * Merge configuration changes from the old deployment, if any.
 *
 * Update the OSTREE_DIR/current and OSTREE_DIR/previous symbolic
 * links.
 */
static gboolean
deploy_tree (OtAdminDeploy     *self,
             const char        *deploy_target,
             const char        *revision,
             GFile            **out_deploy_dir,           
             GCancellable      *cancellable,
             GError           **error)
{
  gboolean ret = FALSE;
  ot_lobj GFile *deploy_dir = NULL;
  ot_lfree char *deploy_target_fullname = NULL;
  ot_lfree char *deploy_target_fullname_tmp = NULL;
  ot_lobj GFile *deploy_target_path = NULL;
  ot_lobj GFile *deploy_target_path_tmp = NULL;
  ot_lfree char *deploy_target_etc_name = NULL;
  ot_lobj GFile *deploy_target_etc_path = NULL;
  ot_lobj GFile *deploy_target_default_etc_path = NULL;
  ot_lobj GFile *deploy_parent = NULL;
  ot_lobj GFile *previous_deployment = NULL;
  ot_lobj GFile *previous_deployment_etc = NULL;
  ot_lobj GFile *previous_deployment_etc_default = NULL;
  ot_lobj OstreeRepoFile *root = NULL;
  ot_lobj GFileInfo *file_info = NULL;
  ot_lobj GFileInfo *existing_checkout_info = NULL;
  ot_lfree char *checkout_target_name = NULL;
  ot_lfree char *checkout_target_tmp_name = NULL;
  ot_lfree char *resolved_commit = NULL;
  GError *temp_error = NULL;
  gboolean skip_checkout;

  if (!revision)
    revision = deploy_target;

  deploy_dir = g_file_get_child (self->ostree_dir, "deploy");

  if (!ostree_repo_resolve_rev (self->repo, revision, FALSE, &resolved_commit, error))
    goto out;

  root = (OstreeRepoFile*)ostree_repo_file_new_root (self->repo, resolved_commit);
  if (!ostree_repo_file_ensure_resolved (root, error))
    goto out;

  file_info = g_file_query_info ((GFile*)root, OSTREE_GIO_FAST_QUERYINFO,
                                 G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                 cancellable, error);
  if (!file_info)
    goto out;

  deploy_target_fullname = g_strconcat (deploy_target, "-", resolved_commit, NULL);
  deploy_target_path = g_file_resolve_relative_path (deploy_dir, deploy_target_fullname);

  deploy_target_fullname_tmp = g_strconcat (deploy_target_fullname, ".tmp", NULL);
  deploy_target_path_tmp = g_file_resolve_relative_path (deploy_dir, deploy_target_fullname_tmp);

  deploy_parent = g_file_get_parent (deploy_target_path);
  if (!ot_gfile_ensure_directory (deploy_parent, TRUE, error))
    goto out;

  deploy_target_etc_name = g_strconcat (deploy_target, "-", resolved_commit, "-etc", NULL);
  deploy_target_etc_path = g_file_resolve_relative_path (deploy_dir, deploy_target_etc_name);

  /* Delete any previous temporary data */
  if (!ot_gio_shutil_rm_rf (deploy_target_path_tmp, cancellable, error))
    goto out;

  existing_checkout_info = g_file_query_info (deploy_target_path, OSTREE_GIO_FAST_QUERYINFO,
                                              G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                              cancellable, &temp_error);
  if (existing_checkout_info)
    {
      if (opt_force)
        {
          if (!ot_gio_shutil_rm_rf (deploy_target_path, cancellable, error))
            goto out;
          if (!ot_gio_shutil_rm_rf (deploy_target_etc_path, cancellable, error))
            goto out;
          
          skip_checkout = FALSE;
        }
      else
        skip_checkout = TRUE;
    }
  else if (g_error_matches (temp_error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND))
    {
      g_clear_error (&temp_error);
      skip_checkout = FALSE;
    }
  else
    {
      g_propagate_error (error, temp_error);
      goto out;
    }

  if (!ot_admin_get_current_deployment (self->ostree_dir, &previous_deployment,
                                        cancellable, error))
    goto out;
  if (previous_deployment)
    {
      ot_lfree char *etc_name;
      ot_lobj GFile *parent;

      etc_name = g_strconcat (ot_gfile_get_basename_cached (previous_deployment), "-etc", NULL);
      parent = g_file_get_parent (previous_deployment);

      previous_deployment_etc = g_file_get_child (parent, etc_name);

      if (!g_file_query_exists (previous_deployment_etc, cancellable)
          || g_file_equal (previous_deployment, deploy_target_path))
        g_clear_object (&previous_deployment_etc);
      else
        previous_deployment_etc_default = g_file_get_child (previous_deployment, "etc");
    }


  if (!skip_checkout)
    {
      ProcessOneCheckoutData checkout_data;

      g_print ("ostadmin: Creating deployment %s\n",
               ot_gfile_get_path_cached (deploy_target_path));

      memset (&checkout_data, 0, sizeof (checkout_data));
      checkout_data.loop = g_main_loop_new (NULL, TRUE);
      checkout_data.error = error;

      ostree_repo_checkout_tree_async (self->repo, 0, 0, deploy_target_path_tmp, root,
                                       file_info, cancellable,
                                       on_checkout_complete, &checkout_data);

      g_main_loop_run (checkout_data.loop);

      g_main_loop_unref (checkout_data.loop);

      if (checkout_data.caught_error)
        goto out;

      if (!ostree_run_triggers_in_root (deploy_target_path_tmp, cancellable, error))
        goto out;

      deploy_target_default_etc_path = ot_gfile_get_child_strconcat (deploy_target_path_tmp, "etc", NULL);

      if (!ot_gio_shutil_rm_rf (deploy_target_etc_path, cancellable, error))
        goto out;

      if (!ot_gio_shutil_cp_a (deploy_target_default_etc_path, deploy_target_etc_path,
                               cancellable, error))
        goto out;

      g_print ("ostadmin: Created %s\n", ot_gfile_get_path_cached (deploy_target_etc_path));

      if (previous_deployment_etc)
        {
          if (!merge_etc_changes (self, previous_deployment_etc_default,
                                  previous_deployment_etc, deploy_target_etc_path, 
                                  cancellable, error))
            goto out;
        }
      else
        g_print ("ostadmin: No previous deployment; therefore, no configuration changes to merge\n");

      if (!ot_gfile_rename (deploy_target_path_tmp, deploy_target_path,
                            cancellable, error))
        goto out;
    }

  if (!update_current (self, previous_deployment, deploy_target_path,
                       cancellable, error))
    goto out;

  ret = TRUE;
  ot_transfer_out_value (out_deploy_dir, &deploy_target_path);
 out:
  return ret;
}

/**
 * do_update_kernel:
 *
 * Ensure we have a GRUB entry, initramfs set up, etc.
 */
static gboolean
do_update_kernel (OtAdminDeploy     *self,
                  GFile             *deploy_path,
                  GCancellable      *cancellable,
                  GError           **error)
{
  gboolean ret = FALSE;
  ot_lptrarray GPtrArray *args = NULL;

  args = g_ptr_array_new ();
  ot_ptrarray_add_many (args, "ostree", "admin",
                        "--ostree-dir", ot_gfile_get_path_cached (self->ostree_dir),
                        "update-kernel",
                        ot_gfile_get_path_cached (deploy_path), NULL);
  g_ptr_array_add (args, NULL);

  if (!ot_spawn_sync_checked (ot_gfile_get_path_cached (self->ostree_dir),
                              (char**)args->pdata, NULL, G_SPAWN_SEARCH_PATH,
                              NULL, NULL, NULL, NULL, error))
    goto out;

  ret = TRUE;
 out:
  return ret;
}


gboolean
ot_admin_builtin_deploy (int argc, char **argv, GFile *ostree_dir, GError **error)
{
  GOptionContext *context;
  OtAdminDeploy self_data;
  OtAdminDeploy *self = &self_data;
  gboolean ret = FALSE;
  ot_lobj GFile *repo_path = NULL;
  ot_lobj GFile *deploy_path = NULL;
  const char *deploy_target = NULL;
  const char *revision = NULL;
  __attribute__((unused)) GCancellable *cancellable = NULL;

  memset (self, 0, sizeof (*self));

  context = g_option_context_new ("NAME [REVISION] - Check out revision NAME (or REVISION as NAME)");

  g_option_context_add_main_entries (context, options, NULL);

  if (!g_option_context_parse (context, &argc, &argv, error))
    goto out;

  if (argc < 2)
    {
      ot_util_usage_error (context, "NAME must be specified", error);
      goto out;
    }

  self->ostree_dir = g_object_ref (ostree_dir);

  if (!ot_admin_ensure_initialized (self->ostree_dir, cancellable, error))
    goto out;

  repo_path = g_file_get_child (self->ostree_dir, "repo");
  self->repo = ostree_repo_new (repo_path);
  if (!ostree_repo_check (self->repo, error))
    goto out;

  deploy_target = argv[1];
  if (argc > 2)
    revision = argv[2];

  if (!deploy_tree (self, deploy_target, revision, &deploy_path,
                    cancellable, error))
    goto out;

  if (!opt_no_kernel)
    {
      if (!do_update_kernel (self, deploy_path, cancellable, error))
        goto out;
    }

  ret = TRUE;
 out:
  g_clear_object (&self->repo);
  g_clear_object (&self->ostree_dir);
  if (context)
    g_option_context_free (context);
  return ret;
}