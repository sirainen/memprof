#include "memprof.h"
#include <stdlib.h>
#include <string.h>

static const char *program_name = "memprof";

void
bfd_nonfatal (const char *string)
{
  const char *errmsg = bfd_errmsg (bfd_get_error ());

  if (string)
    fprintf (stderr, "%s: %s: %s\n", program_name, string, errmsg);
  else
    fprintf (stderr, "%s: %s\n", program_name, errmsg);
}

void
bfd_fatal (const char *string)
{
  bfd_nonfatal (string);
  exit (1);
}

static asymbol **
slurp_symtab (bfd *abfd, long *symcount)
{
  asymbol **sy = (asymbol **) NULL;
  long storage;

  if (!(bfd_get_file_flags (abfd) & HAS_SYMS))
    {
      printf ("No symbols in \"%s\".\n", bfd_get_filename (abfd));
      *symcount = 0;
      return NULL;
    }

  storage = bfd_get_symtab_upper_bound (abfd);
  if (storage < 0)
    bfd_fatal (bfd_get_filename (abfd));

  if (storage)
    {
      sy = (asymbol **) malloc (storage);
    }
  *symcount = bfd_canonicalize_symtab (abfd, sy);
  if (*symcount < 0)
    bfd_fatal (bfd_get_filename (abfd));
  if (*symcount == 0)
    fprintf (stderr, "%s: %s: No symbols\n",
             program_name, bfd_get_filename (abfd));
  return sy;
}

gboolean
read_bfd (Map *map)
{
  asection *section;

  map->abfd = bfd_openr (map->name, NULL);
  if (map->abfd == NULL)
    {
      bfd_nonfatal (map->name);
      return FALSE;
    }

  if (!bfd_check_format (map->abfd, bfd_object))
    {
      bfd_nonfatal (bfd_get_filename (map->abfd));
      return FALSE;
    }

  map->syms = slurp_symtab (map->abfd, &map->symcount);
  if (!map->syms)
    return FALSE;

  for (section = map->abfd->sections; section; section = section->next)
    {
      if (strcmp (section->name, ".text") == 0)
	break;
    }

  if (!section)
    {
      fprintf (stderr, "%s: %s: %s\n", program_name, map->name,
	       ".text section not found");
      return FALSE;
    }

  map->section = section;

  return TRUE;
}

extern char *cplus_demangle (const char *mangled, int options);

#define DMGL_PARAMS     (1 << 0)        /* Include function args */
#define DMGL_ANSI       (1 << 1)        /* Include const, volatile, etc */

gboolean 
find_line (Map *map, bfd_vma addr,
	   const char **filename, char **functionname,
	   unsigned int *line)
{
  const char *name;
  
  if (!map->abfd || !map->syms)
    return FALSE;                  

  if (bfd_find_nearest_line (map->abfd, map->section, map->syms,
			     addr - map->section->vma, 
			     filename, &name, line))
    {
      if (bfd_get_symbol_leading_char (map->abfd) == *name)
	++name;

      *functionname = cplus_demangle (name, DMGL_ANSI | DMGL_PARAMS);
      if (!*functionname)
	*functionname = strdup (name);

      return TRUE;
    }
  else
    return FALSE;

}

void 
process_map_sections (Map *map, SectionFunc func, gpointer user_data)
{
  asection *section;

  if (map->abfd)
    for (section = map->abfd->sections; section; section = section->next)
      {
	if (strcmp (section->name, ".bss") == 0 ||
	    strcmp (section->name, ".data") == 0)
	  {
	    void *addr = (void *)section->vma;
	    if (map->do_offset)
	      addr += map->addr;
	    
	    (*func) (addr, bfd_section_size (map->abfd, section), user_data);
	  }
      }
}
