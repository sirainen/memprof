#ifndef __LEAKDETECT_H__
#define __LEAKDETECT_H__

#include "process.h"

GSList *leaks_find (MPProcess *process);
void leaks_print (MPProcess *process, GSList *blocks, gchar *outfile);

#endif /* __LEAKDETECT_H__ */

