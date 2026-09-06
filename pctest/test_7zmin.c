#include <stdio.h>
#include <string.h>
#include "../source/7zsdk/7z.h"
#include "../source/7zsdk/7zAlloc.h"
#include "../source/7zsdk/7zCrc.h"
#include "../source/7zsdk/7zFile.h"

static void* myAlloc(ISzAllocPtr p, size_t size) { return SzAlloc(p, size); }
static void myFree(ISzAllocPtr p, void* addr) { SzFree(p, addr); }
static ISzAlloc g_alloc = { myAlloc, myFree };
static ISzAlloc g_allocTemp = { SzAllocTemp, SzFreeTemp };

int main(int argc, char** argv) {
    if (argc < 2) { printf("no args\n"); return 2; }
    CFileInStream ais;
    CLookToRead2 look;
    CSzArEx db;
    File_Construct(&ais.file);
    ais.wres = 0;
    WRes w = InFile_Open(&ais.file, argv[1]);
    printf("InFile_Open wres=%d\n", (int)w);
    FileInStream_CreateVTable(&ais);
    ais.wres = 0;
    LookToRead2_CreateVTable(&look, False);
    look.buf = NULL; look.bufSize = 0; look.realStream = NULL;
    CrcGenerateTable();
    SzArEx_Init(&db);
    SRes res = SZ_OK;
    look.buf = (Byte*)SzAlloc(NULL, (size_t)1 << 20);
    if (!look.buf) res = SZ_ERROR_MEM;
    else { look.bufSize = (size_t)1 << 20; look.realStream = &ais.vt; LookToRead2_INIT(&look); }
    if (res == SZ_OK) res = SzArEx_Open(&db, &look.vt, &g_alloc, &g_allocTemp);
    printf("Open res=%d\n", (int)res);
    if (res != SZ_OK) return 1;
    printf("NumFiles=%u NumFolders=%u\n", db.NumFiles, db.db.NumFolders);
    if (argc > 2 && strcmp(argv[2], "dump") == 0) {
        UInt32 fi;
        for (fi = 0; fi < db.db.NumFolders; fi++) {
            CSzFolder f;
            CSzData sd;
            SRes r;
            UInt32 k;
            sd.Data = db.db.CodersData + db.db.FoCodersOffsets[fi];
            sd.Size = db.db.FoCodersOffsets[fi + 1] - db.db.FoCodersOffsets[fi];
            r = SzGetNextFolderItem(&f, &sd);
            printf("folder %u: res=%d coders=%u bonds=%u packStreams=%u unpackStream=%u\n",
                   fi, (int)r, f.NumCoders, f.NumBonds, f.NumPackStreams, f.UnpackStream);
            for (k = 0; k < f.NumCoders; k++)
                printf("  coder[%u] id=%08X streams=%u propsSize=%u\n",
                       k, f.Coders[k].MethodID, f.Coders[k].NumStreams, f.Coders[k].PropsSize);
            for (k = 0; k < f.NumBonds; k++)
                printf("  bond[%u] in=%u out=%u\n", k, f.Bonds[k].InIndex, f.Bonds[k].OutIndex);
            for (k = 0; k < f.NumPackStreams; k++)
                printf("  packStream[%u]=%u\n", k, f.PackStreams[k]);
        }
        return 0;
    }
    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = NULL;
    size_t outBufferSize = 0;
    for (UInt32 i = 0; i < db.NumFiles; i++) {
        size_t len = SzArEx_GetFileNameUtf16(&db, i, NULL);
        printf("entry %u: nameLen=%zu isDir=%d fileToFolder=%u\n", i, len,
               (int)SzArEx_IsDir(&db, i), db.FileToFolder[i]);
        UInt16* nb = (UInt16*)SzAlloc(NULL, (len + 1) * 2);
        SzArEx_GetFileNameUtf16(&db, i, nb);
        for (size_t k = 0; k + 1 < len; k++) printf("%c", (char)nb[k]);
        printf("\n");
        SzFree(NULL, nb);
        if (SzArEx_IsDir(&db, i)) continue;
        size_t offset = 0, outSizeProcessed = 0;
        res = SzArEx_Extract(&db, &look.vt, i, &blockIndex, &outBuffer, &outBufferSize,
                             &offset, &outSizeProcessed, &g_alloc, &g_allocTemp);
        printf("  Extract res=%d size=%zu\n", (int)res, outSizeProcessed);
        if (res != SZ_OK) break;
    }
    SzFree(NULL, outBuffer);
    SzFree(NULL, look.buf);
    SzArEx_Free(&db, &g_alloc);
    File_Close(&ais.file);
    printf("done\n");
    return 0;
}
