#include "types.h"
#include "stat.h"
#include "user.h"

#define PNUM 3

void scheduler_func(void)
{
    set_sche_info(1, 30);
}

int main(void)
{
    scheduler_func();
    exit();
}