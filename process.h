#ifndef __PROCESS_H__
#define __PROCESS_H__

#include <stdio.h>
#include <glib.h>
#include <unistd.h>

#include "memprof.h"

typedef struct _MPProcess MPProcess;

struct _MPProcess
{
  pid_t pid;
  gchar *program_name;

  guint bytes_used;
  guint n_allocations;

  gint input_tag;
  GIOChannel *input_channel;

  GList *map_list;
  GList *bad_pages;
  GHashTable *block_table;
};

void process_init (void);

void process_sections (MPProcess *process, SectionFunc func, gpointer user_data);
MPProcess *process_run (char *exec_string);
     
void process_start_input (MPProcess *process);
void process_stop_input (MPProcess *process);

gboolean 
process_find_line (MPProcess *process, void *address,
		   const char **filename, char **functionname,
		   unsigned int *line);
void process_dump_stack (MPProcess *process, FILE *out, gint stack_size, void **stack);
Symbol *process_locate_symbol (MPProcess *process, guint addr);

#endif /* __PROCESS_H__ */
