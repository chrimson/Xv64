typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned int  uint32;
typedef unsigned long uint64;

// Chris Limson 2022-05-12
#if x64
typedef unsigned long uintp;
#else
typedef unsigned int  uintp;
#endif

typedef uintp pde_t;
