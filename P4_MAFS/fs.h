// On-disk file system format.
// Both the kernel and user programs use this header file.


#define ROOTINO 1  // root i-number
#define BSIZE 512  // block size

// Disk layout:
// [ boot block | super block | log | inode blocks |
//                                          free bit map | data blocks]
//
// mkfs computes the super block and builds an initial file system. The
// super block describes the disk layout:
struct superblock {
  uint size;         // Size of file system image (blocks)
  uint nblocks;      // Number of data blocks
  uint ninodes;      // Number of inodes.
  uint nlog;         // Number of log blocks
  uint logstart;     // Block number of first log block
  uint inodestart;   // Block number of first inode block
  uint bmapstart;    // Block number of first free map block
};

#define N_LAYER_LEN 4 // 레이어 개수
#define NDIRECT 6 // 직접 포인터 개수 // Todo: 머해야될지 모르겠다면 이거 기준으로 조회해보기 이거쓰는 애들만 잘 고쳐보면 될듯
#define N_INDIRECT_L1 4 // 1간접
#define N_INDIRECT_L2 2 // 2간접
#define N_INDIRECT_L3 1 // 3간접
#define NINDIRECT (BSIZE / sizeof(uint)) // 간접 포인터 개수
#define MAXFILE (NDIRECT + (N_INDIRECT_L1 * NINDIRECT) + (N_INDIRECT_L2 * NINDIRECT * NINDIRECT) + (N_INDIRECT_L3 * NINDIRECT * NINDIRECT * NINDIRECT))

// On-disk inode structure
struct dinode {
  short type;           // File type
  short major;          // Major device number (T_DEV only)
  short minor;          // Minor device number (T_DEV only)
  short nlink;          // Number of links to inode in file system
  uint size;            // Size of file (bytes)
  uint addrs[NDIRECT + N_INDIRECT_L1 + N_INDIRECT_L2 + N_INDIRECT_L3];   // Data block addresses
};

// Inodes per block.
#define IPB           (BSIZE / sizeof(struct dinode))

// 해당 inode가 위치한 블럭
// Block containing inode i
#define IBLOCK(i, sb)     ((i) / IPB + sb.inodestart)

// Bitmap bits per block
#define BPB           (BSIZE*8)

// b가 bitmap 위치일때 몇번째 블럭에 위치했는지 알려줌
// Block of free map containing bit for block b
#define BBLOCK(b, sb) (b/BPB + sb.bmapstart)

// Directory is a file containing a sequence of dirent structures.
#define DIRSIZ 14

struct dirent {
  ushort inum;
  char name[DIRSIZ];
};

