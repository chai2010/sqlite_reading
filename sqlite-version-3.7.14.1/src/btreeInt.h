/*
** 2004 April 6
**
** The author disclaims copyright to this source code.  In place of
** a legal notice, here is a blessing:
**
**    May you do good and not evil.
**    May you find forgiveness for yourself and forgive others.
**    May you share freely, never taking more than you give.
**
*************************************************************************
** This file implements a external (disk-based) database using BTrees.
** For a detailed discussion of BTrees, refer to
**
**     Donald E. Knuth, THE ART OF COMPUTER PROGRAMMING, Volume 3:
**     "Sorting And Searching", pages 473-480. Addison-Wesley
**     Publishing Company, Reading, Massachusetts.
**
** The basic idea is that each page of the file contains N database
** entries and N+1 pointers to subpages.
**
**   ----------------------------------------------------------------
**   |  Ptr(0) | Key(0) | Ptr(1) | Key(1) | ... | Key(N-1) | Ptr(N) |
**   ----------------------------------------------------------------
**
** All of the keys on the page that Ptr(0) points to have values less
** than Key(0).  All of the keys on page Ptr(1) and its subpages have
** values greater than Key(0) and less than Key(1).  All of the keys
** on Ptr(N) and its subpages have values greater than Key(N-1).  And
** so forth.
**
** Finding a particular key requires reading O(log(M)) pages from the
** disk where M is the number of entries in the tree.
**
** In this implementation, a single file can hold one or more separate
** BTrees.  Each BTree is identified by the index of its root page.  The
** key and data for any entry are combined to form the "payload".  A
** fixed amount of payload can be carried directly on the database
** page.  If the payload is larger than the preset amount then surplus
** bytes are stored on overflow pages.  The payload for an entry
** and the preceding pointer are combined to form a "Cell".  Each
** page has a small header which contains the Ptr(N) pointer and other
** information such as the size of key and data.
**
** FORMAT DETAILS
**
** The file is divided into pages.  The first page is called page 1,
** the second is page 2, and so forth.  A page number of zero indicates
** "no such page".  The page size can be any power of 2 between 512 and 65536.
** Each page can be either a btree page, a freelist page, an overflow
** page, or a pointer-map page.
**
** The first page is always a btree page.  The first 100 bytes of the first
** page contain a special header (the "file header") that describes the file.
** The format of the file header is as follows:
**
**   OFFSET   SIZE    DESCRIPTION
**      0      16     Header string: "SQLite format 3\000"
**     16       2     Page size in bytes.
**     18       1     File format write version
**     19       1     File format read version
**     20       1     Bytes of unused space at the end of each page
**     21       1     Max embedded payload fraction
**     22       1     Min embedded payload fraction
**     23       1     Min leaf payload fraction
**     24       4     File change counter
**     28       4     Reserved for future use
**     32       4     First freelist page
**     36       4     Number of freelist pages in the file
**     40      60     15 4-byte meta values passed to higher layers
**
**     40       4     Schema cookie
**     44       4     File format of schema layer
**     48       4     Size of page cache
**     52       4     Largest root-page (auto/incr_vacuum)
**     56       4     1=UTF-8 2=UTF16le 3=UTF16be
**     60       4     User version
**     64       4     Incremental vacuum mode
**     68       4     unused
**     72       4     unused
**     76       4     unused
**
** All of the integer values are big-endian (most significant byte first).
**
** The file change counter is incremented when the database is changed
** This counter allows other processes to know when the file has changed
** and thus when they need to flush their cache.
**
** The max embedded payload fraction is the amount of the total usable
** space in a page that can be consumed by a single cell for standard
** B-tree (non-LEAFDATA) tables.  A value of 255 means 100%.  The default
** is to limit the maximum cell size so that at least 4 cells will fit
** on one page.  Thus the default max embedded payload fraction is 64.
**
** If the payload for a cell is larger than the max payload, then extra
** payload is spilled to overflow pages.  Once an overflow page is allocated,
** as many bytes as possible are moved into the overflow pages without letting
** the cell size drop below the min embedded payload fraction.
**
** The min leaf payload fraction is like the min embedded payload fraction
** except that it applies to leaf nodes in a LEAFDATA tree.  The maximum
** payload fraction for a LEAFDATA tree is always 100% (or 255) and it
** not specified in the header.
**
** Each btree pages is divided into three sections:  The header, the
** cell pointer array, and the cell content area.  Page 1 also has a 100-byte
** file header that occurs before the page header.
**
**      |----------------|
**      | file header    |   100 bytes.  Page 1 only.
**      |----------------|
**      | page header    |   8 bytes for leaves.  12 bytes for interior nodes
**      |----------------|
**      | cell pointer   |   |  2 bytes per cell.  Sorted order.
**      | array          |   |  Grows downward
**      |                |   v
**      |----------------|
**      | unallocated    |
**      | space          |
**      |----------------|   ^  Grows upwards
**      | cell content   |   |  Arbitrary order interspersed with freeblocks.
**      | area           |   |  and free space fragments.
**      |----------------|
**
** The page headers looks like this:
**
**   OFFSET   SIZE     DESCRIPTION
**      0       1      Flags. 1: intkey, 2: zerodata, 4: leafdata, 8: leaf
**      1       2      byte offset to the first freeblock
**      3       2      number of cells on this page
**      5       2      first byte of the cell content area
**      7       1      number of fragmented free bytes
**      8       4      Right child (the Ptr(N) value).  Omitted on leaves.
**
** The flags define the format of this btree page.  The leaf flag means that
** this page has no children.  The zerodata flag means that this page carries
** only keys and no data.  The intkey flag means that the key is a integer
** which is stored in the key size entry of the cell header rather than in
** the payload area.
**
** The cell pointer array begins on the first byte after the page header.
** The cell pointer array contains zero or more 2-byte numbers which are
** offsets from the beginning of the page to the cell content in the cell
** content area.  The cell pointers occur in sorted order.  The system strives
** to keep free space after the last cell pointer so that new cells can
** be easily added without having to defragment the page.
**
** Cell content is stored at the very end of the page and grows toward the
** beginning of the page.
**
** Unused space within the cell content area is collected into a linked list of
** freeblocks.  Each freeblock is at least 4 bytes in size.  The byte offset
** to the first freeblock is given in the header.  Freeblocks occur in
** increasing order.  Because a freeblock must be at least 4 bytes in size,
** any group of 3 or fewer unused bytes in the cell content area cannot
** exist on the freeblock chain.  A group of 3 or fewer free bytes is called
** a fragment.  The total number of bytes in all fragments is recorded.
** in the page header at offset 7.
**
**    SIZE    DESCRIPTION
**      2     Byte offset of the next freeblock
**      2     Bytes in this freeblock
**
** Cells are of variable length.  Cells are stored in the cell content area at
** the end of the page.  Pointers to the cells are in the cell pointer array
** that immediately follows the page header.  Cells is not necessarily
** contiguous or in order, but cell pointers are contiguous and in order.
**
** Cell content makes use of variable length integers.  A variable
** length integer is 1 to 9 bytes where the lower 7 bits of each
** byte are used.  The integer consists of all bytes that have bit 8 set and
** the first byte with bit 8 clear.  The most significant byte of the integer
** appears first.  A variable-length integer may not be more than 9 bytes long.
** As a special case, all 8 bytes of the 9th byte are used as data.  This
** allows a 64-bit integer to be encoded in 9 bytes.
**
**    0x00                      becomes  0x00000000
**    0x7f                      becomes  0x0000007f
**    0x81 0x00                 becomes  0x00000080
**    0x82 0x00                 becomes  0x00000100
**    0x80 0x7f                 becomes  0x0000007f
**    0x8a 0x91 0xd1 0xac 0x78  becomes  0x12345678
**    0x81 0x81 0x81 0x81 0x01  becomes  0x10204081
**
** Variable length integers are used for rowids and to hold the number of
** bytes of key and data in a btree cell.
**
** The content of a cell looks like this:
**
**    SIZE    DESCRIPTION
**      4     Page number of the left child. Omitted if leaf flag is set.
**     var    Number of bytes of data. Omitted if the zerodata flag is set.
**     var    Number of bytes of key. Or the key itself if intkey flag is set.
**      *     Payload
**      4     First page of the overflow chain.  Omitted if no overflow
**
** Overflow pages form a linked list.  Each page except the last is completely
** filled with data (pagesize - 4 bytes).  The last page can have as little
** as 1 byte of data.
**
**    SIZE    DESCRIPTION
**      4     Page number of next overflow page
**      *     Data
**
** Freelist pages come in two subtypes: trunk pages and leaf pages.  The
** file header points to the first in a linked list of trunk page.  Each trunk
** page points to multiple leaf pages.  The content of a leaf page is
** unspecified.  A trunk page looks like this:
**
**    SIZE    DESCRIPTION
**      4     Page number of next trunk page
**      4     Number of leaf pointers on this page
**      *     zero or more pages numbers of leaves
*/
#include "sqliteInt.h"


/* The following value is the maximum cell size assuming a maximum page
** size give above.
*/
#define MX_CELL_SIZE(pBt)  ((int)(pBt->pageSize-8))

/* The maximum number of cells on a single page of the database.  This
** assumes a minimum cell size of 6 bytes  (4 bytes for the cell itself
** plus 2 bytes for the index to the cell in the page header).  Such
** small cells will be rare, but they are possible.
*/
#define MX_CELL(pBt) ((pBt->pageSize-8)/6)

/* Forward declarations */
typedef struct MemPage MemPage;
typedef struct BtLock BtLock;

/*
** This is a magic string that appears at the beginning of every
** SQLite database in order to identify the file as a real database.
**
** You can change this value at compile-time by specifying a
** -DSQLITE_FILE_HEADER="..." on the compiler command-line.  The
** header must be exactly 16 bytes including the zero-terminator so
** the string itself should be 15 characters long.  If you change
** the header, then your custom library will not be able to read
** databases generated by the standard tools and the standard tools
** will not be able to read databases created by your custom library.
*/
#ifndef SQLITE_FILE_HEADER /* 123456789 123456 */
#  define SQLITE_FILE_HEADER "SQLite format 3"
#endif

/*
** Page type flags.  An ORed combination of these flags appear as the
** first byte of on-disk image of every BTree page.
*/
#define PTF_INTKEY    0x01
#define PTF_ZERODATA  0x02
#define PTF_LEAFDATA  0x04
#define PTF_LEAF      0x08

/*
** As each page of the file is loaded into memory, an instance of the following
** structure is appended and initialized to zero.  This structure stores
** information about the page that is decoded from the raw file page.
** 每加载文件一个页至内存,以下结构体的一个实例便会被创建,而且初始化为0.此结构体保存着页的相关信息.
**
** The pParent field points back to the parent page.  This allows us to
** walk up the BTree from any leaf to the root.  Care must be taken to
** unref() the parent page pointer when this page is no longer referenced.
** The pageDestructor() routine handles that chore.
** pParent字段指向父页(parent page),这个允许我们从btree的任意叶子节点遍历到根.
**
** Access to all fields of this structure is controlled by the mutex
** stored in MemPage.pBt->mutex.
** 访问此结构体的任意字段必须要加锁.
*/
struct MemPage
{
    u8 isInit;           /* True if previously initialized. MUST BE FIRST! */
    /* 存储在aCell[]数组中, overflow cell的个数 */
    u8 nOverflow;        /* Number of overflow cell bodies in aCell[] */
    u8 intKey;           /* True if intkey flag is set */
    /* 是否为叶子节点 */
    u8 leaf;             /* True if leaf flag is set */
    /* 此页是否存储数据 */
    u8 hasData;          /* True if this page stores data */
    /* 头部偏移,page1有一个100字节的文件头,其他page没有 */
    u8 hdrOffset;        /* 100 for page 1.  0 otherwise */
    u8 childPtrSize;     /* 0 if leaf==1.  4 if leaf==0 */
    u8 max1bytePayload;  /* min(maxLocal,127) */
    u16 maxLocal;        /* Copy of BtShared.maxLocal or BtShared.maxLeaf */
    u16 minLocal;        /* Copy of BtShared.minLocal or BtShared.minLeaf */
    /* cellOffset记录第一个cell pinter的偏移位置 */
    u16 cellOffset;      /* Index in aData of first cell pointer */
    /* page中可用空间大小 */
    u16 nFree;           /* Number of free bytes on the page */
    /* 本页中cell的个数 */
    u16 nCell;           /* Number of cells on this page, local and ovfl */
    u16 maskPage;        /* Mask for page offset */
    /* aiOvfl[i]个cell是overflow cell */
    u16 aiOvfl[5];       /* Insert the i-th overflow cell before the aiOvfl-th
                       ** non-overflow cell */
    /* apOvfl指向overflow cell的首部 */
    u8 *apOvfl[5];       /* Pointers to the body of overflow cells */
    BtShared *pBt;       /* Pointer to BtShared that this page is part of */
    /* 磁盘中的page数据 */
    u8 *aData;           /* Pointer to disk image of the page data */
    u8 *aDataEnd;        /* One byte past the end of usable data */
    u8 *aCellIdx;        /* The cell index area */
    DbPage *pDbPage;     /* Pager page handle */
    /* 页号 */
    Pgno pgno;           /* Page number for this page */
};

/*
** The in-memory image of a disk page has the auxiliary information appended
** to the end.  EXTRA_SIZE is the number of bytes of space needed to hold
** that extra information.
*/
#define EXTRA_SIZE sizeof(MemPage)

/*
** A linked list of the following structures is stored at BtShared.pLock.
** Locks are added (or upgraded from READ_LOCK to WRITE_LOCK) when a cursor
** is opened on the table with root page BtShared.iTable. Locks are removed
** from this list when a transaction is committed or rolled back, or when
** a btree handle is closed.
*/
struct BtLock
{
    Btree *pBtree;        /* Btree handle holding this lock */
    /* 表的root page */
    Pgno iTable;          /* Root page of table */
    u8 eLock;             /* READ_LOCK or WRITE_LOCK */
    BtLock *pNext;        /* Next in BtShared.pLock list */
};

/* Candidate values for BtLock.eLock */
#define READ_LOCK     1
#define WRITE_LOCK    2

/* A Btree handle
**
** A database connection contains a pointer to an instance of
** this object for every database file that it has open.  This structure
** is opaque to the database connection.  The database connection cannot
** see the internals of this structure and only deals with pointers to
** this structure.
**
** For some database files, the same underlying database cache might be
** shared between multiple connections.  In that case, each connection
** has it own instance of this object.  But each instance of this object
** points to the same BtShared object.  The database cache and the
** schema associated with the database file are all contained within
** the BtShared object.
**
** All fields in this structure are accessed under sqlite3.mutex.
** The pBt pointer itself may not be changed while there exists cursors
** in the referenced BtShared that point back to this Btree since those
** cursors have to go through this Btree to find their BtShared and
** they often do so without holding sqlite3.mutex.
*/
struct Btree
{
    sqlite3 *db;       /* The database connection holding this btree */
    BtShared *pBt;     /* Sharable content of this btree */
    /* inTrans表示事务的类型 */
    u8 inTrans;        /* TRANS_NONE, TRANS_READ or TRANS_WRITE */
    u8 sharable;       /* True if we can share pBt with another db */
    u8 locked;         /* True if db currently has pBt locked */
    int wantToLock;    /* Number of nested calls to sqlite3BtreeEnter() */
    int nBackup;       /* Number of backup operations reading this btree */
    Btree *pNext;      /* List of other sharable Btrees from the same db */
    Btree *pPrev;      /* Back pointer of the same list */
#ifndef SQLITE_OMIT_SHARED_CACHE
    BtLock lock;       /* Object used to lock page 1 */
#endif
};

/*
** Btree.inTrans may take one of the following values.
**
** If the shared-data extension is enabled, there may be multiple users
** of the Btree structure. At most one of these may open a write transaction,
** but any number may have active read transactions.
*/
#define TRANS_NONE  0
#define TRANS_READ  1
#define TRANS_WRITE 2

/*
** An instance of this object represents a single database file.
**  此结构的一个实例代表一个单个数据库文件.
** A single database file can be in use at the same time by two
** or more database connections.  When two or more connections are
** sharing the same database file, each connection has it own
** private Btree object for the file and each of those Btrees points
** to this one BtShared object.  BtShared.nRef is the number of
** connections currently sharing this database file.
** 单个数据库文件可以在同时被2个或者多个连接使用,每个连接都有自己私有的Btree实例,
** 这些Btree实例共享一个BtShared实例. BtShared.nRef代表共享连接个数.
**
** Fields in this structure are accessed under the BtShared.mutex
** mutex, except for nRef and pNext which are accessed under the
** global SQLITE_MUTEX_STATIC_MASTER mutex.  The pPager field
** may not be modified once it is initially set as long as nRef>0.
** The pSchema field may be set once under BtShared.mutex and
** thereafter is unchanged as long as nRef>0.
**
** isPending:
**
**   If a BtShared client fails to obtain a write-lock on a database
**   table (because there exists one or more read-locks on the table),
**   the shared-cache enters 'pending-lock' state and isPending is
**   set to true.
**
**   The shared-cache leaves the 'pending lock' state when either of
**   the following occur:
**
**     1) The current writer (BtShared.pWriter) concludes its transaction, OR
**     2) The number of locks held by other connections drops to zero.
**
**   while in the 'pending-lock' state, no connection may start a new
**   transaction.
**
**   This feature is included to help prevent writer-starvation.
*/
struct BtShared /* BtShared代表Btree中可以共享的部分 */
{
    Pager *pPager;        /* The page cache */
    sqlite3 *db;          /* Database connection currently using this Btree */
    BtCursor *pCursor;    /* A list of all open cursors */
    MemPage *pPage1;      /* First page of the database */
    u8 openFlags;         /* Flags to sqlite3BtreeOpen() */
#ifndef SQLITE_OMIT_AUTOVACUUM
    /* 自动释放空间 */
    u8 autoVacuum;        /* True if auto-vacuum is enabled */
    u8 incrVacuum;        /* True if incr-vacuum is enabled */
#endif
    u8 inTransaction;     /* Transaction state */
    u8 max1bytePayload;   /* Maximum first byte of cell for a 1-byte payload */
    u16 btsFlags;         /* Boolean parameters.  See BTS_* macros below */
    /* 非叶数据节点的最大payload */
    u16 maxLocal;         /* Maximum local payload in non-LEAFDATA tables */
    u16 minLocal;         /* Minimum local payload in non-LEAFDATA tables */
    u16 maxLeaf;          /* Maximum local payload in a LEAFDATA table */
    u16 minLeaf;          /* Minimum local payload in a LEAFDATA table */
    u32 pageSize;         /* Total number of bytes on a page */
    /* 每页中可用的字节数,和已经使用的字节数区别开来 */
    u32 usableSize;       /* Number of usable bytes on each page */
    int nTransaction;     /* Number of open transactions (read + write) */
    u32 nPage;            /* Number of pages in the database */
    void *pSchema;        /* Pointer to space allocated by sqlite3BtreeSchema() */
    void (*xFreeSchema)(void*);  /* Destructor for BtShared.pSchema */
    sqlite3_mutex *mutex; /* Non-recursive mutex required to access this object */
    Bitvec *pHasContent;  /* Set of pages moved to free-list this transaction */
#ifndef SQLITE_OMIT_SHARED_CACHE
    int nRef;             /* Number of references to this structure */
    BtShared *pNext;      /* Next on a list of sharable BtShared structs */
    BtLock *pLock;        /* List of locks held on this shared-btree struct */
    Btree *pWriter;       /* Btree with currently open write transaction */
#endif
    u8 *pTmpSpace;        /* BtShared.pageSize bytes of space for tmp use */
};

/*
** Allowed values for BtShared.btsFlags
*/
#define BTS_READ_ONLY        0x0001   /* Underlying file is readonly */
#define BTS_PAGESIZE_FIXED   0x0002   /* Page size can no longer be changed */
#define BTS_SECURE_DELETE    0x0004   /* PRAGMA secure_delete is enabled */
#define BTS_INITIALLY_EMPTY  0x0008   /* Database was empty at trans start */
#define BTS_NO_WAL           0x0010   /* Do not open write-ahead-log files */
#define BTS_EXCLUSIVE        0x0020   /* pWriter has an exclusive lock */
#define BTS_PENDING          0x0040   /* Waiting for read-locks to clear */

/*
** An instance of the following structure is used to hold information
** about a cell.  The parseCellPtr() function fills in this structure
** based on information extract from the raw disk page.
** 以下结构体的一个实例用于存储一个cell的信息.parseCellPtr()函数将基于从磁盘page中读出的数据
** 来填充这个结构体的字段.
*/
typedef struct CellInfo CellInfo;
struct CellInfo
{
    i64 nKey;      /* The key for INTKEY tables, or number of bytes in key */
    u8 *pCell;     /* Pointer to the start of cell content */
    u32 nData;     /* Number of bytes of data */
    u32 nPayload;  /* Total amount of payload */
    /* cell 头部所占用的字节数目 */
    u16 nHeader;   /* Size of the cell content header in bytes */
    /* cell在local page的数据量 */
    u16 nLocal;    /* Amount of payload held locally */
    /* cell在overflow page中的偏移量 */
    u16 iOverflow; /* Offset to overflow page number.  Zero if no overflow */
    /* cell在main b-tree page中占用的字节数 */
    u16 nSize;     /* Size of the cell content on the main b-tree page */
};

/*
** Maximum depth of an SQLite B-Tree structure. Any B-Tree deeper than
** this will be declared corrupt. This value is calculated based on a
** maximum database size of 2^31 pages a minimum fanout of 2 for a
** root-node and 3 for all other internal nodes.
**
** If a tree that appears to be taller than this is encountered, it is
** assumed that the database is corrupt.
*/
#define BTCURSOR_MAX_DEPTH 20

/*
** A cursor is a pointer to a particular entry within a particular
** b-tree within a database file.
** 游标(cursor)是数据库文件中一个b-tree中的指针,指向数据库文件中的一个特殊entry
**
** The entry is identified by its MemPage and the index in
** MemPage.aCell[] of the entry.
**
** A single database file can be shared by two more database connections,
** but cursors cannot be shared.  Each cursor is associated with a
** particular database connection identified BtCursor.pBtree.db.
** 游标不能在不同连接中共享
**
** Fields in this structure are accessed under the BtShared.mutex
** found at self->pBt->mutex.
*/
struct BtCursor
{
    /* 游标所属的Btree */
    Btree *pBtree;            /* The Btree to which this cursor belongs */
    BtShared *pBt;            /* The BtShared this cursor points to */
    BtCursor *pNext, *pPrev;  /* Forms a linked list of all cursors */
    struct KeyInfo *pKeyInfo; /* Argument passed to comparison function */
#ifndef SQLITE_OMIT_INCRBLOB
    Pgno *aOverflow;          /* Cache of overflow page locations */
#endif
    /* root页的页号 */
    Pgno pgnoRoot;            /* The root page of this tree */
    sqlite3_int64 cachedRowid; /* Next rowid cache.  0 means not valid */
    CellInfo info;            /* A parse of the cell we are pointing at */
    i64 nKey;        /* Size of pKey, or last integer key */
    void *pKey;      /* Saved key that was cursor's last known position */
    int skipNext;    /* Prev() is noop if negative. Next() is noop if positive */
    u8 wrFlag;                /* True if writable */
    /* 游标是否指向了表的最后一条记录(entry) */
    u8 atLast;                /* Cursor pointing to the last entry */
    /* 如果info.nKey有效,这个值为true */
    u8 validNKey;             /* True if info.nKey is valid */
    /* 游标状态 */
    u8 eState;                /* One of the CURSOR_XXX constants (see below) */
#ifndef SQLITE_OMIT_INCRBLOB
    u8 isIncrblobHandle;      /* True if this cursor is an incr. io handle */
#endif
    u8 hints;                             /* As configured by CursorSetHints() */
    /* apPage[iPage]记录了游标现在处在数据库文件中的页 */
    i16 iPage;                            /* Index of current page in apPage */
    /* 关于aiIdx以及apPage两个数组
    ** apPage[level]--游标在第level层,游标正处于apPage[level]所指示的page之中
    ** aiIdx[level]--在第level层,游标正指向第aiIdx[level]个cell
    */
    u16 aiIdx[BTCURSOR_MAX_DEPTH];        /* Current index in apPage[i] */
    /* apPage是一个数组,记录了游标从root page到current page所历经的页 */
    MemPage *apPage[BTCURSOR_MAX_DEPTH];  /* Pages from root to current page */
};

/*
** Potential values for BtCursor.eState.
**
** CURSOR_VALID:
**   Cursor points to a valid entry. getPayload() etc. may be called.
**   游标指向一个有效的entry.
**
** CURSOR_INVALID:
**   Cursor does not point to a valid entry. This can happen (for example)
**   because the table is empty or because BtreeCursorFirst() has not been
**   called.
**   游标指向一个无效的entry,这可能是因为表为空,又或者是因为BtreeCursorFirst没有被调用.
**
** CURSOR_REQUIRESEEK:
**   The table that this cursor was opened on still exists, but has been
**   modified since the cursor was last used. The cursor position is saved
**   in variables BtCursor.pKey and BtCursor.nKey. When a cursor is in
**   this state, restoreCursorPosition() can be called to attempt to
**   seek the cursor to the saved position.
**   游标指向的表依然存在,但是自从游标上次使用之后,表发生了更改.游标的位置被存储在变量BtCursor.pKey
**   以及BtCursor.nKey之中.如果游标处于此种状态,调用restoreCursorPostion()会尝试将游标移动到
**   保存的位置.
**
** CURSOR_FAULT:
**   A unrecoverable error (an I/O error or a malloc failure) has occurred
**   on a different connection that shares the BtShared cache with this
**   cursor.  The error has left the cache in an inconsistent state.
**   Do nothing else with this cursor.  Any attempt to use the cursor
**   should return the error code stored in BtCursor.skip
**   一个不可恢复的错误发生了.
*/
#define CURSOR_INVALID           0
#define CURSOR_VALID             1
#define CURSOR_REQUIRESEEK       2
#define CURSOR_FAULT             3

/*
** The database page the PENDING_BYTE occupies. This page is never used.
*/
# define PENDING_BYTE_PAGE(pBt) PAGER_MJ_PGNO(pBt)

/*
** These macros define the location of the pointer-map entry for a
** database page. The first argument to each is the number of usable
** bytes on each page of the database (often 1024). The second is the
** page number to look up in the pointer map.
**
** PTRMAP_PAGENO returns the database page number of the pointer-map
** page that stores the required pointer. PTRMAP_PTROFFSET returns
** the offset of the requested map entry.
**
** If the pgno argument passed to PTRMAP_PAGENO is a pointer-map page,
** then pgno is returned. So (pgno==PTRMAP_PAGENO(pgsz, pgno)) can be
** used to test if pgno is a pointer-map page. PTRMAP_ISPAGE implements
** this test.
*/
#define PTRMAP_PAGENO(pBt, pgno) ptrmapPageno(pBt, pgno)
#define PTRMAP_PTROFFSET(pgptrmap, pgno) (5*(pgno-pgptrmap-1))
#define PTRMAP_ISPAGE(pBt, pgno) (PTRMAP_PAGENO((pBt),(pgno))==(pgno))

/*
** The pointer map is a lookup table that identifies the parent page for
** each child page in the database file.  The parent page is the page that
** contains a pointer to the child.  Every page in the database contains
** 0 or 1 parent pages.  (In this context 'database page' refers
** to any page that is not part of the pointer map itself.)  Each pointer map
** entry consists of a single byte 'type' and a 4 byte parent page number.
** The PTRMAP_XXX identifiers below are the valid types.
**
** The purpose of the pointer map is to facility moving pages from one
** position in the file to another as part of autovacuum.  When a page
** is moved, the pointer in its parent must be updated to point to the
** new location.  The pointer map is used to locate the parent page quickly.
**
** PTRMAP_ROOTPAGE: The database page is a root-page. The page-number is not
**                  used in this case.
**
** PTRMAP_FREEPAGE: The database page is an unused (free) page. The page-number
**                  is not used in this case.
**
** PTRMAP_OVERFLOW1: The database page is the first page in a list of
**                   overflow pages. The page number identifies the page that
**                   contains the cell with a pointer to this overflow page.
**
** PTRMAP_OVERFLOW2: The database page is the second or later page in a list of
**                   overflow pages. The page-number identifies the previous
**                   page in the overflow page list.
**
** PTRMAP_BTREE: The database page is a non-root btree page. The page number
**               identifies the parent page in the btree.
*/
#define PTRMAP_ROOTPAGE 1
#define PTRMAP_FREEPAGE 2
#define PTRMAP_OVERFLOW1 3
#define PTRMAP_OVERFLOW2 4
#define PTRMAP_BTREE 5

/* A bunch of assert() statements to check the transaction state variables
** of handle p (type Btree*) are internally consistent.
*/
#define btreeIntegrity(p) \
  assert( p->pBt->inTransaction!=TRANS_NONE || p->pBt->nTransaction==0 ); \
  assert( p->pBt->inTransaction>=p->inTrans );


/*
** The ISAUTOVACUUM macro is used within balance_nonroot() to determine
** if the database supports auto-vacuum or not. Because it is used
** within an expression that is an argument to another macro
** (sqliteMallocRaw), it is not possible to use conditional compilation.
** So, this macro is defined instead.
*/
#ifndef SQLITE_OMIT_AUTOVACUUM
#define ISAUTOVACUUM (pBt->autoVacuum)
#else
#define ISAUTOVACUUM 0
#endif


/*
** This structure is passed around through all the sanity checking routines
** in order to keep track of some global state information.
**
** The aRef[] array is allocated so that there is 1 bit for each page in
** the database. As the integrity-check proceeds, for each page used in
** the database the corresponding bit is set. This allows integrity-check to
** detect pages that are used twice and orphaned pages (both of which
** indicate corruption).
*/
typedef struct IntegrityCk IntegrityCk;
struct IntegrityCk
{
    BtShared *pBt;    /* The tree being checked out */
    Pager *pPager;    /* The associated pager.  Also accessible by pBt->pPager */
    u8 *aPgRef;       /* 1 bit per page in the db (see above) */
    Pgno nPage;       /* Number of pages in the database */
    int mxErr;        /* Stop accumulating errors when this reaches zero */
    int nErr;         /* Number of messages written to zErrMsg so far */
    int mallocFailed; /* A memory allocation error has occurred */
    StrAccum errMsg;  /* Accumulate the error message text here */
};

/*
** Routines to read or write a two- and four-byte big-endian integer values.
*/
#define get2byte(x)   ((x)[0]<<8 | (x)[1])
#define put2byte(p,v) ((p)[0] = (u8)((v)>>8), (p)[1] = (u8)(v))
#define get4byte sqlite3Get4byte
#define put4byte sqlite3Put4byte
