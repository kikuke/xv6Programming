// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
static void itrunc(struct inode*);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// 각 레이어별 최대 addrs idx
int l_addrs_max[N_LAYER_LEN] = {
  NDIRECT,
  N_INDIRECT_L1,
  N_INDIRECT_L2,
  N_INDIRECT_L3
};

// Read the super block.
void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// 최초로 만나는 빈 블럭의 비트맵을 1로 설정후 빈 블럭을 리턴함
// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){ // 블럭당 비트맵 개수만큼 b를 증가
    bp = bread(dev, BBLOCK(b, sb)); // b가 위치한 비트맵 블럭을 읽어옴. 버퍼에 락도 걸어놓음. 결과적으로 비트맵 한블럭씩 읽어오게 됨
    for(bi = 0; bi < BPB && b + bi < sb.size; bi++){ // 비트단위로 순회
      m = 1 << (bi % 8);
      if((bp->data[bi/8] & m) == 0){  // Is block free? 해당 비트 블럭이 비어있나
        bp->data[bi/8] |= m;  // Mark block in use. 비어있다면 사용한다고 마킹
        log_write(bp);
        brelse(bp); // 읽어서 락해뒀던거 다시 원복
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp); // 읽어서 락해뒀던거 다시 원복
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void
iinit(int dev)
{
  int i = 0;
  
  initlock(&icache.lock, "icache");
  for(i = 0; i < NINODE; i++) {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n", sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

static struct inode* iget(uint dev, uint inum);

//PAGEBREAK!
// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

// inode의 데이터를 디스크의 dinode로 옮김
// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb)); // 해당 아이노드가 위치한 블럭을 읽어들임
  dip = (struct dinode*)bp->data + ip->inum%IPB; // 해당 블록의 inode를 읽어들임
  dip->type = ip->type; // 이하 데이터 갱신
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for(ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++){
    if(ip->ref > 0 && ip->dev == dev && ip->inum == inum){
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if(empty == 0 && ip->ref == 0)    // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if(empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if(ip->valid && ip->nlink == 0){
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if(r == 1){
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

//PAGEBREAK!
// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// 해당 위치의 블록에 대한 블록 인덱스를 리턴. 만약 할당받은 블럭이 없다면 할당해서 줌
// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;
  uint bs_p_b = 1; // addrs 블록 하나당 가리킬 수 있는 사이즈
  uint d_idx = 0; // addrs를 직접 가리키는 포인터
  uint s_idx; // 보조 인덱스

  if (bn >= MAXFILE)
    panic("bmap: out of range");
  
  // cprintf("bmap bn: %d\n", bn);
  for (int i=0; i<N_LAYER_LEN; i++) { // 레이어 단위로 체크. i는 레이어를 의미함
    if (bn < l_addrs_max[i] * bs_p_b) { // 해당 레이어에서 가리킬 수 있는 블록이라면
      s_idx = bn / bs_p_b;
      if ((addr = ip->addrs[d_idx + s_idx]) == 0) // addr은 addrs의 0단계를 가리키게됨
        ip->addrs[d_idx + s_idx] = addr = balloc(ip->dev);
      
      // cprintf("Total layer %d\n", i);
      // cprintf("layer 0 idx: %d\n", d_idx + s_idx);
      for (int j=0; j<i; j++) { // 단계만큼 파고듦
        bp = bread(ip->dev, addr); // 해당 블럭의 내용을 읽어들임. buf에 락걸림
        a = (uint*)bp->data; // uint 포인터배열로 바라보겠다는 의미. 포인터가 uint 크기. 블록 / uint 만큼 포인터 생김

        bs_p_b /= NINDIRECT; // 해당 레이어의 0단계 블록이 가리키는 크기
        d_idx = (bn / bs_p_b) % NINDIRECT; // 해당 레이어의 블록의 idx

        // cprintf("layer %d idx: %d\n", j+1, d_idx);
        if((addr = a[d_idx]) == 0){ // 해당 간접 포인터의 인덱스가 가리키는 블록이 없다면 할당
          a[d_idx] = addr = balloc(ip->dev); 
          log_write(bp);
        }
        brelse(bp);
        bn %= bs_p_b; // 레이어 축소
      }
      return addr;
    }

    d_idx += l_addrs_max[i];
    bn -= l_addrs_max[i] * bs_p_b; // 이전레이어와 관련된 블럭수는 없앰
    bs_p_b *= NINDIRECT; // 해당 레이어의 0단계 블록이 가리키는 크기
  }

  panic("bmap: out of range");
}

void
itrunc_layer(struct inode *ip, uint addr, int depth)
{
  struct buf *bp;
  int i;
  uint *a;
  
  if (depth <= 0)
    return;

  bp = bread(ip->dev, addr);
  a = (uint*)bp->data;
  for (i=0; i<NINDIRECT; i++) {
    if (a[i]) {
      itrunc_layer(ip, a[i], depth-1);
      bfree(ip->dev, a[i]);
    }
  }
  brelse(bp);
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
itrunc(struct inode *ip)
{
  uint addr;
  int i, depth = 0;
  uint addr_len = NDIRECT + N_INDIRECT_L1 + N_INDIRECT_L2 + N_INDIRECT_L3;
  int layer_cnt = 0;

  for (i=0; i < addr_len; i++) { // 모든 addr에 대해
    if ((addr=ip->addrs[i]) == 0) // 해당 블럭이 없는 경우 패스
      continue;
    if (layer_cnt == l_addrs_max[depth]) {
      layer_cnt = 0;
      depth++;
    }

    itrunc_layer(ip, ip->addrs[i], depth); // 해당 단계 블록 삭제

    bfree(ip->dev, ip->addrs[i]);
    ip->addrs[i] = 0;

    layer_cnt++;
  }

  ip->size = 0;
  iupdate(ip);
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

//PAGEBREAK!
// Read data from inode.
// Caller must hold ip->lock.
int
readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){ // 디바이스읽기인 경우
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n); // consoleread임
  }

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    bp = bread(ip->dev, bmap(ip, off/BSIZE)); // 해당 블록을 읽어들임. buf에 락걸림
    m = min(n - tot, BSIZE - off%BSIZE); // 블록이 꽉찼다면 0, 데이터 끝이라면 데이터 끝 블록에 담긴 데이터 크기
    memmove(dst, bp->data + off%BSIZE, m); // dst(메모리)에 블록단위로 해당 데이터를 쓰기함
    brelse(bp);
  }
  return n;
}

// Todo: 분석하기
// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
int
writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(ip->type == T_DEV){ // 디바이스인 경우
    if(ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write) // 해당 디바이스 값 잘 설정됏나
      return -1;
    return devsw[ip->major].write(ip, src, n); // consolewrite로 씀
  }

  if(off > ip->size || off + n < off) // 이상한 위치에 쓰려하는가
    return -1;
  if(off + n > MAXFILE*BSIZE) // 파일 최대 크기를 넘어서는가
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){ // n만큼 쓰기함
    // Todo: 에러지점
    bp = bread(ip->dev, bmap(ip, off/BSIZE)); // 해당 블록을 읽어들임. buf에 락걸림
    m = min(n - tot, BSIZE - off%BSIZE); // 블록이 꽉찼다면 0, 데이터 끝이라면 데이터 끝 블록에 담긴 데이터 크기
    memmove(bp->data + off%BSIZE, src, m); // 해당 블록에 (실제론 buf) 블록단위로 쓰기함
    log_write(bp);
    brelse(bp);
  }

  if(n > 0 && off > ip->size){
    ip->size = off; // size 업데이트
    iupdate(ip); // inmemory inode를 이용해 디스크의 inode를 업데이트
  }
  return n;
}

//PAGEBREAK!
// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, (char*)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

//PAGEBREAK!
// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
static struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
