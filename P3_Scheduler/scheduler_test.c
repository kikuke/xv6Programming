#include "types.h"
#include "stat.h"
#include "user.h"

#define PNUM 3

// set_sche_info 실험 데이터
// (prior, timer)
int sched_test_map[][2] = {{1, 110}, {10, 60}, {11, 60}};

void scheduler_func(void)
{
    int childs[PNUM] = {};
    int isChild = 0;
    int test_map_len = sizeof(sched_test_map)/sizeof(sched_test_map[0]);

    printf(1, "start scheduler_test\n");

    for (int i=0; i<PNUM; i++) {
        if (!isChild && (childs[i] = fork()) == 0) { // 부모에서 생성된 자식 프로세스일 경우
            isChild = 1;
            if (i < test_map_len)
                set_sche_info(sched_test_map[i][0], sched_test_map[i][1]);
            
            exit();
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