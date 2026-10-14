#include <cstddef>
#include <epicsExit.h>
#include <iocsh.h>

int main(int argc, char* argv[])
{
    if (argc > 1) iocsh(argv[1]);
    iocsh(NULL); 
    epicsExit(0); 
    return 0;
}
