#include <stdlib.h>
#include <unistd.h>

void *a()
{
  return malloc(1000);
}

void *b()
{
  return a();
}

int main()
{
  char *block;

  block = b();
  block = malloc(1000);
  free(block);

  return 0;
}
