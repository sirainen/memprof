#include <glib.h>
#include <stdio.h>
#include "bfd.h"

#ifndef __MEMPROF_H__
#define __MEMPROF_H__

typedef enum {
  BLOCK_MARKED  = 1 << 0,
  BLOCK_IS_ROOT = 1 << 1
} BlockFlags;

typedef struct {
  guint flags;
  void *addr;
  guint size;
  gint stack_size;
  void **stack;
} Block;

typedef struct {
  guint addr;
  guint size;
  gchar *name;
  bfd *abfd;
  GArray *symbols;
  long symcount;
  asymbol **syms;
  asection *section;
  gboolean do_offset : 1;
  gboolean prepared : 1;
} Map;

typedef struct {
  guint addr;
  guint size;
  gchar *name;
} Symbol;

typedef void (*SectionFunc) (void *addr, guint size, gpointer user_data);

typedef enum {
  ERROR_WARNING,
  ERROR_MODAL,
  ERROR_FATAL
} ErrorType;

void show_error	(ErrorType error,
		 const gchar *format,
		 ...) G_GNUC_PRINTF (2, 3);

void process_map_sections (Map *map, SectionFunc func, gpointer user_data);

gboolean read_bfd (Map *map);
gboolean find_line (Map *map, bfd_vma addr,
		    const char **filename, char **functionname,
		    unsigned int *line);

#endif /* __MEMPROF_H__ */

