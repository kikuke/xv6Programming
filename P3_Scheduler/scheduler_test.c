#include "types.h"
#include "stat.h"
#include "user.h"

#define PNUM 3

// set_sche_info 실험 데이터
// (prior, timer)
int sched_test_map[][2] = {{1, 300}, {22, 600}, {34, 600}};

void scheduler_func(void)
{
    int childs[PNUM] = {};
    int isChild = 0;
    int test_map_len = sizeof(sched_test_map)/sizeof(sched_test_map[0]);

    for (int i=0; i<PNUM; i++) {
        if (!isChild && (childs[i] = fork()) == 0) { // 부모에서 생성된 자식 프로세스일 경우
            if (i == 0)
                printf(1, "start scheduler_test\n");
            isChild = 1;
            if (i < test_map_len)
                set_sche_info(sched_test_map[i][0], sched_test_map[i][1]);
            while(isChild) {};
        }
    }

    // 부모일 경우 자식을 기다려준다
    if (!isChild)
        for (int i=0; i<PNUM; i++)
            wait();
    
    printf(1, "end of scheduler_test\n");
}

int main(void)
{
    scheduler_func();
    exit();
}