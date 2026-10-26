#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char *argv[]) {
    char *greeting = "My student ID is 2023076599\n";
    fprintf(1, "%s", greeting);
    int pid, ppid;

    pid = getpid();
    ppid = getppid();

    fprintf(1, "My pid is %d\n", pid);
    fprintf(1, "My ppid is %d\n", ppid);
    exit(0);
}