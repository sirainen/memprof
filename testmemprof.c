#include <stdlib.h>
#include <unistd.h>

int main()
{
  char *block;
  
  block = malloc(1000);
  block = malloc(1000);
  free(block);

  sleep (1000);
  
  return 0;
}
