#include <stdio.h>

#include "hello.h"
#include "world.h"

int main(void) {
  printf("%s %s\n", hello(), world());
  return 0;
}
