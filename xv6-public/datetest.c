#include "types.h"
#include "user.h"
#include "date.h"

int main(int argc, char *argv[])
{
    struct rtcdate r;

    if (date(&r) < 0) {
        printf(2, "datetest failed!\n");
    }

    exit();
}