#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ck/command.h>

int main() {
  // TODO: actually login lol
  // system("cat /cfg/motd");
  while (1) {
		system("/bin/sh");
  	printf("[login] Restarting shell...\n");
	}
  return 0;
}
