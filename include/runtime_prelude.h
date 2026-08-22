#ifndef TIQ_RUNTIME_PRELUDE_H
#define TIQ_RUNTIME_PRELUDE_H

// Core Language Runtime Prelude
//
// ARCHITECTURAL BOUNDARY:
// Contains ONLY essential primitive runtime definitions, scalar types, allocation
// helpers, and slice headers required by the Tiq core language semantics.
// All auxiliary system, networking, and serialization stubs have been moved
// to `runtime_aux.h` and will be rewritten natively in Tiq (`std/*.tiq`) in M19.

static const char TIQ_CORE_RUNTIME_PRELUDE[] =
    "#if !defined(__APPLE__)\n"
    "#define _POSIX_C_SOURCE 200809L\n"
    "#endif\n"
    "#include <stdio.h>\n"
    "#include <stdlib.h>\n"
    "#include <stddef.h>\n"
    "#include <string.h>\n"
    "#include <strings.h>\n"
    "#include <stdint.h>\n"
    "#include <sys/stat.h>\n"
    "#include <sys/socket.h>\n"
    "#include <netdb.h>\n"
    "#include <netinet/in.h>\n"
    "#if defined(__APPLE__)\n"
    "#include <sys/event.h>\n"
    "#endif\n"
    "#if defined(__linux__)\n"
    "#include <sys/epoll.h>\n"
    "#endif\n"
    "#include <dirent.h>\n"
    "#include <unistd.h>\n"
    "#include <time.h>\n"
    "#include <dlfcn.h>\n"
    "#include <pthread.h>\n"
    "typedef struct { const void *ptr; int len; } TiqSlice;\n"
    "typedef struct { int64_t value; int has_value; } TiqOption;\n"
    "typedef struct { int64_t value; int64_t error; int is_ok; } TiqResult;\n"
    "typedef struct { int kind; unsigned char *buf,*used; size_t cap,pos,block,count; } TiqAllocator;\n\n"

    "static void *tiq_alloc(size_t n){void*p=malloc(n);if(!p){fprintf(stderr,\"tiq: out of memory\\n\");exit(1);}return p;}\n"
    "static int64_t tiq_aa(size_t a){return a&&!(a&(a-1));}\n"
    "uint64_t tiq_allocator_general(void){static TiqAllocator a={1,0,0,0,0,0,0};return(uint64_t)(uintptr_t)&a;}\n"
    "uint64_t tiq_arena_create(uint64_t c){if(!c||c>SIZE_MAX)return 0;TiqAllocator*a=calloc(1,sizeof*a);if(!a)return 0;a->buf=malloc((size_t)c);if(!a->buf){free(a);return 0;}a->kind=2;a->cap=(size_t)c;return(uint64_t)(uintptr_t)a;}\n"
    "uint64_t tiq_pool_create(uint64_t b,uint64_t c){size_t m=_Alignof(max_align_t);if(!b||!c||b>SIZE_MAX||c>SIZE_MAX||b%m||(size_t)c>SIZE_MAX/(size_t)b)return 0;TiqAllocator*a=calloc(1,sizeof*a);if(!a)return 0;a->buf=malloc((size_t)b*(size_t)c);a->used=calloc((size_t)c,1);if(!a->buf||!a->used){free(a->used);free(a->buf);free(a);return 0;}a->kind=3;a->block=(size_t)b;a->count=(size_t)c;return(uint64_t)(uintptr_t)a;}\n"
    "uint64_t tiq_allocator_alloc(uint64_t h,uint64_t n,uint64_t al){if(!h||!n||n>SIZE_MAX||al>SIZE_MAX)return 0;size_t z=(size_t)n,a=(size_t)al;if(!tiq_aa(a))return 0;TiqAllocator*x=(TiqAllocator*)(uintptr_t)h;if(x->kind==1)return a<=_Alignof(max_align_t)?(uint64_t)(uintptr_t)malloc(z):0;if(x->kind==2){if(x->pos>SIZE_MAX-(a-1))return 0;size_t p=(x->pos+a-1)&~(a-1);if(p>x->cap||z>x->cap-p)return 0;x->pos=p+z;return(uint64_t)(uintptr_t)(x->buf+p);}if(x->kind==3){if(z>x->block||a>_Alignof(max_align_t))return 0;for(size_t i=0;i<x->count;i++)if(!x->used[i]){x->used[i]=1;return(uint64_t)(uintptr_t)(x->buf+i*x->block);}}return 0;}\n"
    "int64_t tiq_allocator_dealloc(uint64_t h,uint64_t p,uint64_t n,uint64_t al){(void)n;(void)al;if(!h||!p)return-1;TiqAllocator*x=(TiqAllocator*)(uintptr_t)h;if(x->kind==1){free((void*)(uintptr_t)p);return 0;}if(x->kind==2)return 0;if(x->kind==3){uintptr_t b=(uintptr_t)x->buf,q=(uintptr_t)p;if(q<b||q>=b+x->block*x->count)return-1;size_t d=(size_t)(q-b);if(d%x->block)return-1;size_t i=d/x->block;if(!x->used[i])return-1;x->used[i]=0;return 0;}return-1;}\n"
    "int64_t tiq_allocator_reset(uint64_t h){if(!h)return-1;TiqAllocator*x=(TiqAllocator*)(uintptr_t)h;if(x->kind==1)return 0;if(x->kind==2){x->pos=0;return 0;}if(x->kind==3){memset(x->used,0,x->count);return 0;}return-1;}\n"
    "int64_t tiq_allocator_destroy(uint64_t h){if(!h)return-1;TiqAllocator*x=(TiqAllocator*)(uintptr_t)h;if(x->kind==1)return 0;if(x->kind!=2&&x->kind!=3)return-1;free(x->used);free(x->buf);free(x);return 0;}\n"
    "static const char *tiq_str_dup(const char*s){size_t n=strlen(s);char*b=(char*)tiq_alloc(n+1);memcpy(b,s,n+1);return b;}\n"
    "static int64_t tiq_argc=0;\n"
    "static char **tiq_argv=0;\n\n";

#endif
