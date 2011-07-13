/**************************************************************************
 *
 * Copyright 2007-2009 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 **************************************************************************/


#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <list>

#include <zlib.h>

#include "os.hpp"
#include "range.hpp"
#include "trace_writer.hpp"
#include "trace_format.hpp"


namespace Trace {


Writer::Writer() :
    g_gzFile(NULL),
    call_no(0)
{
    close();
};

Writer::~Writer() {
    close();
};

void
Writer::close(void) {
    if (g_gzFile != NULL) {
        gzclose(g_gzFile);
        g_gzFile = NULL;
    }
}

bool
Writer::open(const char *filename) {
    close();

    g_gzFile = gzopen(filename, "wb");
    if (!g_gzFile) {
        return false;
    }

    call_no = 0;
    functions.clear();
    structs.clear();
    enums.clear();
    bitmasks.clear();

    _writeUInt(TRACE_VERSION);

    return true;
}

void
Writer::open(void) {

    static unsigned dwCounter = 0;

    const char *szExtension = "trace";
    char szFileName[PATH_MAX];
    const char *lpFileName;

    lpFileName = getenv("TRACE_FILE");
    if (lpFileName) {
        strncpy(szFileName, lpFileName, PATH_MAX);
    }
    else {
        char szProcessName[PATH_MAX];
        char szCurrentDir[PATH_MAX];
        OS::GetProcessName(szProcessName, PATH_MAX);
        OS::GetCurrentDir(szCurrentDir, PATH_MAX);

        for (;;) {
            FILE *file;

            if (dwCounter)
                snprintf(szFileName, PATH_MAX, "%s%c%s.%u.%s", szCurrentDir, PATH_SEP, szProcessName, dwCounter, szExtension);
            else
                snprintf(szFileName, PATH_MAX, "%s%c%s.%s", szCurrentDir, PATH_SEP, szProcessName, szExtension);

            file = fopen(szFileName, "rb");
            if (file == NULL)
                break;

            fclose(file);

            ++dwCounter;
        }
    }

    OS::DebugMessage("apitrace: tracing to %s\n", szFileName);

    open(szFileName);
}

void inline
Writer::_write(const void *sBuffer, size_t dwBytesToWrite) {
    if (g_gzFile == NULL)
        return;

    gzwrite(g_gzFile, sBuffer, dwBytesToWrite);
}

void inline
Writer::_writeByte(char c) {
    _write(&c, 1);
}

void inline
Writer::_writeUInt(unsigned long long value) {
    char buf[2 * sizeof value];
    unsigned len;

    len = 0;
    do {
        assert(len < sizeof buf);
        buf[len] = 0x80 | (value & 0x7f);
        value >>= 7;
        ++len;
    } while (value);

    assert(len);
    buf[len - 1] &= 0x7f;

    _write(buf, len);
}

void inline
Writer::_writeFloat(float value) {
    assert(sizeof value == 4);
    _write((const char *)&value, sizeof value);
}

void inline
Writer::_writeDouble(double value) {
    assert(sizeof value == 8);
    _write((const char *)&value, sizeof value);
}

void inline
Writer::_writeString(const char *str) {
    size_t len = strlen(str);
    _writeUInt(len);
    _write(str, len);
}

inline bool lookup(std::vector<bool> &map, size_t index) {
    if (index >= map.size()) {
        map.resize(index + 1);
        return false;
    } else {
        return map[index];
    }
}

unsigned Writer::beginEnter(const FunctionSig *sig) {
    OS::AcquireMutex();

    if (!g_gzFile) {
        open();
    }

    _writeByte(Trace::EVENT_ENTER);
    _writeUInt(sig->id);
    if (!lookup(functions, sig->id)) {
        _writeString(sig->name);
        _writeUInt(sig->num_args);
        for (unsigned i = 0; i < sig->num_args; ++i) {
            _writeString(sig->arg_names[i]);
        }
        functions[sig->id] = true;
    }

    return call_no++;
}

void Writer::endEnter(void) {
    _writeByte(Trace::CALL_END);
    gzflush(g_gzFile, Z_SYNC_FLUSH);
    OS::ReleaseMutex();
}

void Writer::beginLeave(unsigned call) {
    OS::AcquireMutex();
    _writeByte(Trace::EVENT_LEAVE);
    _writeUInt(call);
}

void Writer::endLeave(void) {
    _writeByte(Trace::CALL_END);
    gzflush(g_gzFile, Z_SYNC_FLUSH);
    OS::ReleaseMutex();
}

void Writer::beginArg(unsigned index) {
    _writeByte(Trace::CALL_ARG);
    _writeUInt(index);
}

void Writer::beginReturn(void) {
    _writeByte(Trace::CALL_RET);
}

void Writer::beginArray(size_t length) {
    _writeByte(Trace::TYPE_ARRAY);
    _writeUInt(length);
}

void Writer::beginStruct(const StructSig *sig) {
    _writeByte(Trace::TYPE_STRUCT);
    _writeUInt(sig->id);
    if (!lookup(structs, sig->id)) {
        _writeString(sig->name);
        _writeUInt(sig->num_members);
        for (unsigned i = 0; i < sig->num_members; ++i) {
            _writeString(sig->member_names[i]);
        }
        structs[sig->id] = true;
    }
}

void Writer::writeBool(bool value) {
    _writeByte(value ? Trace::TYPE_TRUE : Trace::TYPE_FALSE);
}

void Writer::writeSInt(signed long long value) {
    if (value < 0) {
        _writeByte(Trace::TYPE_SINT);
        _writeUInt(-value);
    } else {
        _writeByte(Trace::TYPE_UINT);
        _writeUInt(value);
    }
}

void Writer::writeUInt(unsigned long long value) {
    _writeByte(Trace::TYPE_UINT);
    _writeUInt(value);
}

void Writer::writeFloat(float value) {
    _writeByte(Trace::TYPE_FLOAT);
    _writeFloat(value);
}

void Writer::writeDouble(double value) {
    _writeByte(Trace::TYPE_DOUBLE);
    _writeDouble(value);
}

void Writer::writeString(const char *str) {
    if (!str) {
        Writer::writeNull();
        return;
    }
    _writeByte(Trace::TYPE_STRING);
    _writeString(str);
}

void Writer::writeString(const char *str, size_t len) {
    if (!str) {
        Writer::writeNull();
        return;
    }
    _writeByte(Trace::TYPE_STRING);
    _writeUInt(len);
    _write(str, len);
}

void Writer::writeWString(const wchar_t *str) {
    if (!str) {
        Writer::writeNull();
        return;
    }
    _writeByte(Trace::TYPE_STRING);
    _writeString("<wide-string>");
}

void Writer::writeBlob(const void *data, size_t size) {
    if (!data) {
        Writer::writeNull();
        return;
    }
    _writeByte(Trace::TYPE_BLOB);
    _writeUInt(size);
    if (size) {
        _write(data, size);
    }
}

void Writer::writeEnum(const EnumSig *sig) {
    _writeByte(Trace::TYPE_ENUM);
    _writeUInt(sig->id);
    if (!lookup(enums, sig->id)) {
        _writeString(sig->name);
        Writer::writeSInt(sig->value);
        enums[sig->id] = true;
    }
}

void Writer::writeBitmask(const BitmaskSig *sig, unsigned long long value) {
    _writeByte(Trace::TYPE_BITMASK);
    _writeUInt(sig->id);
    if (!lookup(bitmasks, sig->id)) {
        _writeUInt(sig->num_flags);
        for (unsigned i = 0; i < sig->num_flags; ++i) {
            if (i != 0 && sig->flags[i].value == 0) {
                OS::DebugMessage("apitrace: warning: sig %s is zero but is not first flag\n", sig->flags[i].name);
            }
            _writeString(sig->flags[i].name);
            _writeUInt(sig->flags[i].value);
        }
        bitmasks[sig->id] = true;
    }
    _writeUInt(value);
}

void Writer::writeNull(void) {
    _writeByte(Trace::TYPE_NULL);
}

void Writer::writeOpaque(const void *addr) {
    if (!addr) {
        Writer::writeNull();
        return;
    }
    _writeByte(Trace::TYPE_OPAQUE);
    _writeUInt((size_t)addr);
}

struct RangeInfo : public range::range<size_t>
{
    unsigned long crc;
};

typedef std::list<RangeInfo> RangeInfoList;

struct RegionInfo
{
    size_t size;
    const char *start;
    RangeInfoList ranges;
};

typedef std::list<RegionInfo> RegionInfoList;

static RegionInfoList regionInfoList;


static RegionInfo * lookupRegionInfo(Writer &writer, const void *ptr) {
    OS::MemoryInfo info;

    if (!OS::queryVirtualAddress(ptr, &info)) {
        OS::DebugMessage("apitrace: warning: failed to query virtual address %p\n", ptr);
        return NULL;
    }

    if (0) {
        OS::DebugMessage("apitrace: %p => %p..%p\n", ptr, info.start, info.stop);
    }

    for (RegionInfoList::iterator it = regionInfoList.begin(); it != regionInfoList.end(); ) {
        const char *start = it->start;
        const char *stop  = start + it->size;
        if (info.start < stop && info.stop > start) {
            // Intersect
            if (info.start == start && info.stop == stop) {
                return &*it;
            } else {
                OS::DebugMessage("apitrace: warning: range %p-%p changed to %p-%p\n", start, stop, info.start, info.stop);
                // XXX: Emit a realloc
                it = regionInfoList.erase(it);
            }
        } else {
            ++it;
        }
    }

    RegionInfo regionInfo;
    regionInfo.size = (const char*)info.stop - (const char*)info.start;
    regionInfo.start = (const char *)info.start;

    unsigned __call = writer.beginEnter(&malloc_sig);
    writer.beginArg(0);
    writer.writeUInt(regionInfo.size);
    writer.endArg();
    writer.endEnter();
    writer.beginLeave(__call);
    writer.beginReturn();
    writer.writeOpaque(regionInfo.start);
    writer.endReturn();
    writer.endLeave();

    return &*regionInfoList.insert(regionInfoList.end(), regionInfo);
}


static const char *memcpy_args[3] = {"dest", "src", "n"};
const FunctionSig memcpy_sig = {0, "memcpy", 3, memcpy_args};

static const char *malloc_args[1] = {"size"};
const FunctionSig malloc_sig = {1, "malloc", 1, malloc_args};

static const char *free_args[1] = {"ptr"};
const FunctionSig free_sig = {2, "free", 1, free_args};

static const char *realloc_args[2] = {"ptr", "size"};
const FunctionSig realloc_sig = {3, "realloc", 2, realloc_args};


/**
 * Ensure this region is updated, emitting the necessary memcpy calls.
 */
void Writer::updateRegion(const void *ptr, size_t size) {
    if (!ptr) {
        return;
    }

    OS::AcquireMutex();

    RegionInfo * regionInfo = lookupRegionInfo(*this, ptr);

    if (!regionInfo || !size) {
        OS::ReleaseMutex();
        return;
    }

    if ((char *)ptr + size > regionInfo->start + regionInfo->size) {
        OS::DebugMessage("apitrace: warning: range %p-%p exceeds region %p-%p\n", 
                         ptr, (char *)ptr + size,
                         regionInfo->start, regionInfo->start + regionInfo->size);

    }

    size_t start = (const char *)ptr - regionInfo->start;

#if 0

    // Simply emit one memcpy for the whole range
    unsigned __call = beginEnter(&memcpy_sig);
    beginArg(0);
    writeOpaque(ptr);
    endArg();
    beginArg(1);
    writeBlob(ptr, size);
    endArg();
    beginArg(2);
    writeUInt(size);
    endArg();
    endEnter();
    beginLeave(__call);
    endLeave();

#else

    size_t stop = start + size;

    // The range for what needs to be updated
    range::range<size_t> update(start, stop);

    // The set of ranges which need to be copied, i.e., the update range minus
    // everything that did not change since last time.
    range::range_set<size_t> copy(update);

    // Go through the all ranges that intersect the update range, checking for redundancy,
    for (RangeInfoList::iterator it = regionInfo->ranges.begin(); it != regionInfo->ranges.end(); ) {
        if (it->intersects(update)) {
            const Bytef *p = (const Bytef *)regionInfo->start + it->start;
            size_t length = it->stop - it->start;

            unsigned long crc = crc32(0L, Z_NULL, 0);
            crc = crc32(crc, p, length);

            if (it->crc == crc) {
                // This range did not change, so no need to copy it.
                copy.sub(*it);
                ++it;
            } else {
                // This range did change, so remove all info about it.
                it = regionInfo->ranges.erase(it);
            }
        } else {
            ++it;
        }
    }

    // Emit fake memcpy calls for each range that needs to be copied
    for (range::range_set<size_t>::const_iterator it = copy.begin(); it != copy.end(); ++it) {
        const Bytef *p = (const Bytef *)regionInfo->start + it->start;
        size_t length = it->stop - it->start;

        unsigned __call = beginEnter(&memcpy_sig);
        beginArg(0);
        writeOpaque(p);
        endArg();
        beginArg(1);
        writeBlob(p, length);
        endArg();
        beginArg(2);
        writeUInt(length);
        endArg();
        endEnter();
        beginLeave(__call);
        endLeave();

        // Note down this range's CRC for future redundancy checks.
        RangeInfo r;
        r.start = it->start;
        r.stop = it->stop;
        r.crc = crc32(0L, Z_NULL, 0);
        r.crc = crc32(r.crc, p, length);
        regionInfo->ranges.push_front(r);
    }

#endif

    OS::ReleaseMutex();
}

} /* namespace Trace */

