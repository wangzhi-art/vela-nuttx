/****************************************************************************
 * drivers/mmcsd/mmcsd_procfs.c
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <sys/stat.h>
#include <sys/mount.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <assert.h>
#include <debug.h>
#include <errno.h>

#include <sys/param.h>

#include <nuttx/nuttx.h>
#include <nuttx/fs/fs.h>
#include <nuttx/fs/procfs.h>
#include <nuttx/kmalloc.h>

#include "mmcsd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Determines the size of an intermediate buffer that must be large enough
 * to handle the longest line generated by this logic (plus a couple of
 * bytes).
 */

#define MMCSD_LINELEN 512

typedef CODE ssize_t (*mmcsd_read_t)(FAR struct file *filep,
                                     FAR char *buffer, size_t buflen,
                                     FAR struct mmcsd_state_s *priv);

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

/* File system methods */

static int     mmcsd_open(FAR struct file *filep, FAR const char *relpath,
                          int oflags, mode_t mode);
static int     mmcsd_close(FAR struct file *filep);
static ssize_t mmcsd_read_cid(FAR struct file *filep, FAR char *buffer,
                              size_t buflen, FAR struct mmcsd_state_s *priv);
static ssize_t mmcsd_read_csd(FAR struct file *filep, FAR char *buffer,
                              size_t buflen, FAR struct mmcsd_state_s *priv);
static ssize_t mmcsd_read_type(FAR struct file *filep, FAR char *buffer,
                              size_t buflen, FAR struct mmcsd_state_s *priv);
static ssize_t mmcsd_read(FAR struct file *filep, FAR char *buffer,
                          size_t buflen);
static int     mmcsd_dup(FAR const struct file *oldp,
                         FAR struct file *newp);

static int     mmcsd_opendir(FAR const char *relpath,
                             FAR struct fs_dirent_s **dir);
static int     mmcsd_closedir(FAR struct fs_dirent_s *dir);
static int     mmcsd_readdir(FAR struct fs_dirent_s *dir,
                             FAR struct dirent *entry);
static int     mmcsd_rewinddir(FAR struct fs_dirent_s *dir);

static int     mmcsd_stat(FAR const char *relpath, FAR struct stat *buf);

static int     mmcsd_get_file_index(FAR const char *relpath);

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* This structure describes one open "file" */

struct mmcsd_file_s
{
  struct procfs_file_s base;      /* Base open file structure */
  char line[MMCSD_LINELEN];       /* Pre-allocated buffer for formatted lines */
  int index;                      /* Device node index */
  mmcsd_read_t read;              /* Read function */
};

struct mmcsd_file_ops_s
{
  FAR const char *name;
  mmcsd_read_t read;
};

static const struct procfs_operations g_mmcsd_operations =
{
  mmcsd_open,       /* open */
  mmcsd_close,      /* close */
  mmcsd_read,       /* read */
  NULL,             /* write */
  NULL,             /* poll */

  mmcsd_dup,        /* dup */

  mmcsd_opendir,    /* opendir */
  mmcsd_closedir,   /* closedir */
  mmcsd_readdir,    /* readdir */
  mmcsd_rewinddir,  /* rewinddir */

  mmcsd_stat        /* stat */
};

static const struct procfs_entry_s g_mmcsd_procfs1 =
{
  "mmcsd", &g_mmcsd_operations, PROCFS_DIR_TYPE
};
static const struct procfs_entry_s g_mmcsd_procfs2 =
{
  "mmcsd/**", &g_mmcsd_operations, PROCFS_UNKOWN_TYPE
};

static const struct mmcsd_file_ops_s g_mmcsd_files[] =
{
  {"cid",  mmcsd_read_cid},
  {"csd",  mmcsd_read_csd},
  {"type", mmcsd_read_type},
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: mmcsd_get_file_index
 ****************************************************************************/

static int mmcsd_get_file_index(FAR const char *relpath)
{
  int i;

  for (i = 0; i < nitems(g_mmcsd_files); i++)
    {
      if (strncmp(relpath, g_mmcsd_files[i].name,
                  strlen(g_mmcsd_files[i].name)) == 0)
        {
          return i;
        }
    }

  return -1;
}

/****************************************************************************
 * Name: mmcsd_open
 ****************************************************************************/

static int mmcsd_open(FAR struct file *filep, FAR const char *relpath,
                      int oflags, mode_t mode)
{
  FAR struct mmcsd_file_s *mmcsdfile;
  int ret;

  /* This PROCFS file is read-only.  Any attempt to open with write access
   * is not permitted.
   */

  if ((oflags & O_WRONLY) != 0 || (oflags & O_RDONLY) == 0)
    {
      finfo("ERROR: Only O_RDONLY supported\n");
      return -EACCES;
    }

  relpath += strlen("mmcsd/");
  ret = mmcsd_get_file_index(relpath);
  if (ret < 0)
    {
      return -ENOENT;
    }

  /* Allocate a container to hold the file attributes */

  mmcsdfile = kmm_zalloc(sizeof(struct mmcsd_file_s));
  if (mmcsdfile == NULL)
    {
      finfo("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  mmcsdfile->read = g_mmcsd_files[ret].read;
  mmcsdfile->index = atoi(relpath + strlen(g_mmcsd_files[ret].name));

  /* Save the attributes as the open-specific state in filep->f_priv */

  filep->f_priv = mmcsdfile;
  return OK;
}

/****************************************************************************
 * Name: mmcsd_close
 ****************************************************************************/

static int mmcsd_close(FAR struct file *filep)
{
  DEBUGASSERT(filep->f_priv);

  /* Release the file attributes structure */

  kmm_free(filep->f_priv);
  filep->f_priv = NULL;
  return OK;
}

/****************************************************************************
 * Name: mmcsd_read_cid
 ****************************************************************************/

static ssize_t mmcsd_read_cid(FAR struct file *filep, FAR char *buffer,
                              size_t buflen, FAR struct mmcsd_state_s *priv)
{
  FAR struct mmcsd_file_s *mmcsdfile;
  size_t totalsize;
  size_t linesize;
  off_t offset;

  /* Recover our private data from the struct file instance */

  mmcsdfile = filep->f_priv;

  /* Save the file offset and the user buffer information */

  offset = filep->f_pos;

  linesize = snprintf(mmcsdfile->line, MMCSD_LINELEN, "%08lx%08lx%08lx%08lx",
                      priv->cid[0], priv->cid[1],
                      priv->cid[2], priv->cid[3]);
  totalsize = procfs_memcpy(mmcsdfile->line, linesize, buffer,
                            buflen, &offset);
  filep->f_pos += totalsize;
  return totalsize;
}

/****************************************************************************
 * Name: mmcsd_read_csd
 ****************************************************************************/

static ssize_t mmcsd_read_csd(FAR struct file *filep, FAR char *buffer,
                              size_t buflen, FAR struct mmcsd_state_s *priv)
{
  FAR struct mmcsd_file_s *mmcsdfile;
  size_t totalsize;
  size_t linesize;
  off_t offset;

  /* Recover our private data from the struct file instance */

  mmcsdfile = filep->f_priv;

  /* Save the file offset and the user buffer information */

  offset = filep->f_pos;

  linesize = snprintf(mmcsdfile->line, MMCSD_LINELEN, "%08lx%08lx%08lx%08lx",
                     priv->csd[0], priv->csd[1], priv->csd[2], priv->csd[3]);
  totalsize = procfs_memcpy(mmcsdfile->line, linesize, buffer,
                           buflen, &offset);
  filep->f_pos += totalsize;
  return totalsize;
}

/****************************************************************************
 * Name: mmcsd_read_type
 ****************************************************************************/

static ssize_t mmcsd_read_type(FAR struct file *filep, FAR char *buffer,
                               size_t buflen, FAR struct mmcsd_state_s *priv)
{
  FAR struct mmcsd_file_s *mmcsdfile;
  size_t totalsize;
  size_t linesize;
  off_t offset;

  mmcsdfile = filep->f_priv;

  /* Save the file offset and the user buffer information */

  offset = filep->f_pos;

  switch (priv->type)
    {
      case MMCSD_CARDTYPE_SDV1:
      case MMCSD_CARDTYPE_SDV2:
      case MMCSD_CARDTYPE_SDV2 | MMCSD_CARDTYPE_BLOCK:
        linesize = snprintf(mmcsdfile->line, MMCSD_LINELEN, "SD");
        break;
      case MMCSD_CARDTYPE_MMC:
      case MMCSD_CARDTYPE_MMC | MMCSD_CARDTYPE_BLOCK:
        linesize = snprintf(mmcsdfile->line, MMCSD_LINELEN, "MMC");
        break;

      /* Unknown card type */

      case MMCSD_CARDTYPE_UNKNOWN:
      default:
        ferr("ERROR: Invalid media type (%d)\n", priv->type);
        return -EINVAL;
    }

  totalsize = procfs_memcpy(mmcsdfile->line, linesize, buffer,
                            buflen, &offset);
  filep->f_pos += totalsize;
  return totalsize;
}

/****************************************************************************
 * Name: mmcsd_read
 ****************************************************************************/

static ssize_t mmcsd_read(FAR struct file *filep, FAR char *buffer,
                          size_t buflen)
{
  FAR struct mmcsd_file_s *mmcsdfile = filep->f_priv;
  FAR struct inode *inode;
  char path[32];
  ssize_t ret;

  snprintf(path, sizeof(path), "/dev/mmcsd%d", mmcsdfile->index);
  ret = open_blockdriver(path, MS_RDONLY, &inode);
  if (ret < 0)
    {
      return ret;
    }

  DEBUGASSERT(filep->f_priv);
  ret = mmcsdfile->read(filep, buffer, buflen, inode->i_private);
  close_blockdriver(inode);

  return ret;
}

/****************************************************************************
 * Name: mmcsd_dup
 *
 * Description:
 *   Duplicate open file data in the new file structure.
 *
 ****************************************************************************/

static int mmcsd_dup(FAR const struct file *oldp, FAR struct file *newp)
{
  FAR struct mmcsd_file_s *oldattr;
  FAR struct mmcsd_file_s *newattr;

  /* Recover our private data from the old struct file instance */

  oldattr = oldp->f_priv;
  DEBUGASSERT(oldattr);

  /* Allocate a new container to hold the task and attribute selection */

  newattr = kmm_malloc(sizeof(struct mmcsd_file_s));
  if (newattr == NULL)
    {
      ferr("ERROR: Failed to allocate file attributes\n");
      return -ENOMEM;
    }

  /* The copy the file attributes from the old attributes to the new */

  memcpy(newattr, oldattr, sizeof(struct mmcsd_file_s));

  /* Save the new attributes in the new file structure */

  newp->f_priv = newattr;
  return OK;
}

/****************************************************************************
 * Name: mmcsd_opendir
 *
 * Description:
 *   Open a directory for read access
 *
 ****************************************************************************/

static int mmcsd_opendir(FAR const char *relpath,
                         FAR struct fs_dirent_s **dir)
{
  FAR struct procfs_dir_priv_s *level1;

  DEBUGASSERT(relpath);

  /* Assume that path refers to the 1st level subdirectory.  Allocate the
   * level1 the dirent structure before checking.
   */

  level1 = kmm_zalloc(sizeof(struct procfs_dir_priv_s));
  if (level1 == NULL)
    {
      ferr("ERROR: Failed to allocate the level1 directory structure\n");
      return -ENOMEM;
    }

  /* Initialize base structure components */

  level1->level    = 1;
  level1->nentries = nitems(g_mmcsd_files);

  *dir = (FAR struct fs_dirent_s *)level1;
  return OK;
}

/****************************************************************************
 * Name: mmcsd_closedir
 *
 * Description: Close the directory listing
 *
 ****************************************************************************/

static int mmcsd_closedir(FAR struct fs_dirent_s *dir)
{
  DEBUGASSERT(dir);
  kmm_free(dir);
  return OK;
}

/****************************************************************************
 * Name: mmcsd_readdir
 *
 * Description: Read the next directory entry
 *
 ****************************************************************************/

static int mmcsd_readdir(FAR struct fs_dirent_s *dir,
                         FAR struct dirent *entry)
{
  FAR struct procfs_dir_priv_s *level1;
  int index;
  int fpos;

  DEBUGASSERT(dir);
  level1 = (FAR struct procfs_dir_priv_s *)dir;

  index = level1->index;
  if (index >= level1->nentries)
    {
      /* We signal the end of the directory by returning the special
       * error -ENOENT
       */

      finfo("Entry %d: End of directory\n", index);
      return -ENOENT;
    }

  fpos  = index % nitems(g_mmcsd_files);
  index = index / nitems(g_mmcsd_files);

  entry->d_type = DTYPE_FILE;
  snprintf(entry->d_name, NAME_MAX + 1, "%s%d",
           g_mmcsd_files[fpos].name, index);

  level1->index++;
  return OK;
}

/****************************************************************************
 * Name: mmcsd_rewindir
 *
 * Description: Reset directory read to the first entry
 *
 ****************************************************************************/

static int mmcsd_rewinddir(FAR struct fs_dirent_s *dir)
{
  FAR struct procfs_dir_priv_s *level1;

  DEBUGASSERT(dir);
  level1 = (FAR struct procfs_dir_priv_s *)dir;

  level1->index = 0;
  return OK;
}

/****************************************************************************
 * Name: mmcsd_stat
 *
 * Description: Return information about a file or directory
 *
 ****************************************************************************/

static int mmcsd_stat(FAR const char *relpath, FAR struct stat *buf)
{
  memset(buf, 0, sizeof(struct stat));

  if (strcmp(relpath, "mmcsd") == 0 || strcmp(relpath, "mmcsd/") == 0)
    {
      buf->st_mode = S_IFDIR | S_IROTH | S_IRGRP | S_IRUSR;
    }
  else
    {
      relpath += strlen("mmcsd/");
      if (mmcsd_get_file_index(relpath) < 0)
        {
          return -ENOENT;
        }

      buf->st_mode = S_IFREG | S_IROTH | S_IRGRP | S_IRUSR;
    }

  return OK;
}

/****************************************************************************
 * Name: mmcsd_initialize_procfs
 *
 * Description:
 *   Initialize mmcsd procfs
 *
 ****************************************************************************/

void mmcsd_initialize_procfs(void)
{
  int ret;
  ret = procfs_register(&g_mmcsd_procfs1);
  if (ret == OK)
    {
      ret = procfs_register(&g_mmcsd_procfs2);
    }

  DEBUGASSERT(ret == OK);
}
