/*
  Copyright 2012-2016 Jyri J. Virkki <jyri@virkki.com>

  This file is part of dupd.

  dupd is free software: you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  dupd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with dupd.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "filecompare.h"
#include "hashlist.h"
#include "main.h"
#include "md5.h"
#include "paths.h"
#include "stats.h"

static struct size_list * size_list_head;
static struct size_list * size_list_tail;

struct size_list {
  long size;
  char * path_list;
  struct size_list * next;
};


/** ***************************************************************************
 * Create a new size list node. A node contains one file size and points
 * to the head of the path list of files which are of this size.
 *
 * Parameters:
 *    size      - Size
 *    path_list - Head of path list of files of this size
 *
 * Return: An intialized/allocated size list node.
 *
 */
static struct size_list * new_size_list_entry(long size, char * path_list)
{
  struct size_list * e = (struct size_list *)malloc(sizeof(struct size_list));
  e->size = size;
  e->path_list = path_list;
  e->next = NULL;
  return e;
}


/** ***************************************************************************
 * Public function, see header file.
 *
 */
void add_to_size_list(long size, char * path_list)
{
  stats_size_list_avg = stats_size_list_avg +
    ( (size - stats_size_list_avg) / (stats_size_list_count + 1) );

  if (size_list_head == NULL) {
    size_list_head = new_size_list_entry(size, path_list);
    size_list_tail = size_list_head;
    stats_size_list_count = 1;
    return;
  }

  size_list_tail->next = new_size_list_entry(size, path_list);
  size_list_tail = size_list_tail->next;
  stats_size_list_count++;
}


/** ***************************************************************************
 * Public function, see header file.
 *
 */
void process_size_list(sqlite3 * dbh)
{
  if (size_list_head == NULL) {
    return;
  }

  char * line;
  char * node;
  char * path_list_head;
  int count = 0;
  int round_one_hash_blocks = 1;

  struct size_list * size_node = size_list_head;

  while (size_node != NULL) {
    count++;
    path_list_head = size_node->path_list;

    uint32_t path_count = pl_get_path_count(path_list_head);

    node = pl_get_first_entry(path_list_head);

    if (verbosity >= 3) {
      printf("Processing %d/%d (%d files of size %ld)\n",
             count, stats_size_list_count, path_count, size_node->size);
    }

    // If we only have two files of this size, compare them directly
    if (opt_compare_two && path_count == 2) {
      char * path1 = pl_entry_get_path(node);
      char * path2 = pl_entry_get_path(pl_entry_get_next(node));

      compare_two_files(dbh, path1, path2, size_node->size);
      stats_two_file_compare++;
      goto CONTINUE;
    }

    // If we only have three files of this size, compare them directly
    if (opt_compare_three && path_count == 3) {
      char * path1 = pl_entry_get_path(node);
      char * node2 = pl_entry_get_next(node);
      char * path2 = pl_entry_get_path(node2);
      char * path3 = pl_entry_get_path(pl_entry_get_next(node2));

      compare_three_files(dbh, path1, path2, path3, size_node->size);
      stats_three_file_compare++;
      goto CONTINUE;
    }

    // If we have files of smallish size, do a full hash up front
    if (size_node->size <= (hash_one_block_size * hash_one_max_blocks)) {
      stats_full_hash_first++;
      round_one_hash_blocks = 0;
      if (verbosity >= 4) {
        printf("Computing full hash up front, file size is small enough.\n");
      }
    } else {
      stats_one_block_hash_first++;
      round_one_hash_blocks = 1;
    }

    // Build initial hash list for these files
    stats_set_round_one++;
    struct hash_list * hl_one = get_hash_list(HASH_LIST_ONE);
    do {
      line = pl_entry_get_path(node);
      add_hash_list(hl_one, line, round_one_hash_blocks,
                    hash_one_block_size, 0);
      node = pl_entry_get_next(node);
    } while (node != NULL);

    if (verbosity >= 4) { printf("Done building first hash list.\n"); }
    if (verbosity >= 5) {
      printf("Contents of hash list hl_one:\n");
      print_hash_list(hl_one);
    }

    if (save_uniques) {
      record_uniques(dbh, hl_one);
    }

    // If no potential dups after this round, we're done!
    if (HASH_LIST_NO_DUPS(hl_one)) {
      if (verbosity >= 4) { printf("No potential dups left, done!\n"); }
      stats_set_no_dups_round_one++;
      goto CONTINUE;
    }

    // If by now we already have a full hash, publish and we're done!
    if (round_one_hash_blocks == 0) {
      stats_set_dups_done_round_one++;
      if (verbosity >= 4) { printf("Some dups confirmed, here they are:\n"); }
      publish_duplicate_hash_list(dbh, hl_one, size_node->size);
      goto CONTINUE;
    }

    struct hash_list * hl_previous = hl_one;

    // Do filtering pass with intermediate number of blocks, if configured
    if (intermediate_blocks > 1) {
      if (verbosity >= 4) {
        printf("Don't know yet. Filtering to second hash list\n");
      }
      stats_set_round_two++;
      struct hash_list * hl_partial = get_hash_list(HASH_LIST_PARTIAL);
      hl_previous = hl_partial;
      filter_hash_list(hl_one, intermediate_blocks,
                       hash_block_size, hl_partial, 0);

      if (verbosity >= 5) {
        printf("Contents of hash list hl_partial:\n");
        print_hash_list(hl_partial);
      }

      if (save_uniques) {
        record_uniques(dbh, hl_partial);
      }

      // If no potential dups after this round, we're done!
      if (HASH_LIST_NO_DUPS(hl_partial)) {
        if (verbosity >= 4) { printf("No potential dups left, done!\n"); }
        stats_set_no_dups_round_two++;
        goto CONTINUE;
      }

      // If this size < hashed so far, we're done so publish to db
      if (size_node->size < hash_block_size * intermediate_blocks) {
        stats_set_dups_done_round_two++;
        if (verbosity >= 4) {
          printf("Some dups confirmed, here they are:\n");
        }
        publish_duplicate_hash_list(dbh, hl_partial, size_node->size);
        goto CONTINUE;
      }
    }

    // for anything left, do full file hash
    if (verbosity >= 4) {
      printf("Don't know yet. Filtering to full hash list\n");
    }
    stats_set_full_round++;
    struct hash_list * hl_full = get_hash_list(HASH_LIST_FULL);
    filter_hash_list(hl_previous, 0, hash_block_size,
                     hl_full, intermediate_blocks);
    if (verbosity >= 5) {
      printf("Contents of hash list hl_full:\n");
      print_hash_list(hl_full);
    }

    if (save_uniques) {
      record_uniques(dbh, hl_full);
    }

    // If no potential dups after this round, we're done!
    if (HASH_LIST_NO_DUPS(hl_full)) {
      if (verbosity >= 4) { printf("No potential dups left, done!\n"); }
      stats_set_no_dups_full_round++;
      goto CONTINUE;
    }

    // Still something left, go publish them to db
    if (verbosity >= 4) {
      printf("Finally some dups confirmed, here they are:\n");
    }
    stats_set_dups_done_full_round++;
    publish_duplicate_hash_list(dbh, hl_full, size_node->size);

    CONTINUE:
    size_node = size_node->next;
  }
}


/** ***************************************************************************
 * Public function, see header file.
 *
 */
void init_size_list()
{
  size_list_head = NULL;
  size_list_tail = NULL;
  stats_size_list_count = 0;
  stats_size_list_avg = 0;
}


/** ***************************************************************************
 * Public function, see header file.
 *
 */
void free_size_list()
{
  if (size_list_head != NULL) {
    struct size_list * p = size_list_head;
    struct size_list * me = size_list_head;

    while (p != NULL) {
      p = p->next;
      free(me);
      me = p;
    }
  }
}
