#include "memprof.h"
#include "process.h"

typedef struct {
  Symbol *symbol;
  guint self;
  guint total;
  GList *inherited;
  GList *children;
} ProfileFunc;

typedef struct {
  ProfileFunc *function;
  guint bytes;
} ProfileFuncRef;

typedef struct {
  gint n_functions;
  ProfileFunc **functions;

  /* Private */
  MPProcess *process;
  GHashTable *function_hash;
  GHashTable *skip_hash;
} Profile;

Profile *profile_create (MPProcess *process, char **skip_funcs, gint n_skip_funcs);
void profile_write (Profile *profile, gchar *outfile);
void profile_free (Profile *profile);

