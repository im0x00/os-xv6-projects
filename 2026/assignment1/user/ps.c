#include "kernel/types.h"
#include "kernel/fcntl.h"
#include "user/user.h"


void 
print_ps_header()
{
    printf("pid,state,name\n");
}


int
main(int argc, char *argv[])
{
    print_ps_header();
    if (argc == 1) {
        ps(0);
        exit(0);
    }
    for (int i = 1; i < argc; i++) {
        ps(atoi(argv[i]));
    }
    exit(0);
}
