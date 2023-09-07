#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int line;

/**
 * offset이 0이면 더이상 뒤로 밀리지 않고 0을 리턴
 * c에 offset 앞의 값을 저장해줍니다.
 * rewind and return now offset
 * return -1 when occured error
*/
off_xvt
rewindchar(int fd, char *c)
{
    off_xvt now_off;

    if((now_off = lseek(fd, 0, SEEK_CUR)) < 0) {
        printf(2, "rewindchar: lseek error\n");
        return -1;
    }
    if(now_off <= 0)
        return now_off;

    if(lseek(fd, -1, SEEK_CUR) < 0) {
        printf(2, "rewindchar: lseek error\n");
        return -1;
    }
    if(read(fd, c, 1) < 0) {
        printf(2, "rewindchar: read error\n");
        return -1;
    }
    return lseek(fd, -1, SEEK_CUR);
}

/**
 * 한 라인을 되돌립니다.
 * 오류시 -1 리턴
 * offset이 0이라면 라인을 되돌리지 않고 0 리턴
 * 성공시 현재 offset 리턴
*/
off_xvt
rewindline(int fd)
{
    off_xvt now_off;
    char c;

    if((now_off = rewindchar(fd, &c)) < 0) {
        printf(2, "rewindline: rewindchar error\n");
        return now_off;
    }
    if(now_off <= 0) {
        return now_off;
    }

    while ((now_off = rewindchar(fd, &c)) > 0) {
        if (c == '\n') {
            read(fd, &c, 1);
            break;
        }
    }
    return lseek(fd, 0, SEEK_CUR);
}

void
printline(int fd)
{
    int n;
    char c;

    while((n = read(fd, &c, 1)) > 0 && c != '\n') {
        if (write(1, &c, 1) < 1) {
            printf(1, "printline: write error\n");
            exit();
        }
    }
    if(n < 0){
        printf(2, "printline: read error\n");
        exit();
    }
    write(1, "\n", 1);
}

void
htac(int fd)
{
    off_xvt rewind_off;

    if(lseek(fd, 0, SEEK_END) < 0) {
        printf(2, "htac: lseek error\n");
        exit();
    }

    for(; line>0; line--) {
        if ((rewind_off = rewindline(fd)) < 0) {
            printf(2, "htac: rewindline error\n");
            exit();
        }
        printline(fd);

        if(rewind_off == 0)
            return;

        if (lseek(fd, rewind_off, SEEK_SET)) {
            printf(2, "htac: rewindline error\n");
            exit();
        }
    }
}

int
main(int argc, char *argv[])
{
    int fd;

    if (argc != 3) {
        printf(2, "Usage: %s <num> <filename>\n", argv[0]);
        exit();
    }

    if ((fd = open(argv[2], O_RDONLY)) < 0) {
        printf(2, "%s open failed\n", argv[2]);
        exit();
    }
    line = atoi(argv[1]);
    
    htac(fd);
    exit();
}