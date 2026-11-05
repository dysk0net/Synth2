#pragma once

// Moves a file to the Trash (Finder's on mac, the Recycle Bin on Windows).
// Returns false where there is none (iOS app containers): the caller deletes
// instead. Define OXO_TRASH_IMPL before including this in exactly one
// translation unit per binary (a .mm on Apple, a .cpp on Windows).

#include <filesystem>

namespace oxo {
bool MoveToTrash(const std::filesystem::path &path);
}

#if defined(OXO_TRASH_IMPL) && defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <string>

bool oxo::MoveToTrash(const std::filesystem::path &path) {
    // SHFileOperation wants a double-NUL-terminated list of absolute paths.
    std::error_code ec;
    std::wstring from = std::filesystem::absolute(path, ec).wstring();
    if (ec || from.empty()) return false;
    from.push_back(L'\0');
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}
#elif defined(OXO_TRASH_IMPL)
#import <Foundation/Foundation.h>
#include <TargetConditionals.h>

bool oxo::MoveToTrash(const std::filesystem::path &path) {
#if TARGET_OS_IPHONE
    (void)path;
    return false; // a container Trash nobody can empty: the caller deletes
#else
    @autoreleasepool {
        NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        return [NSFileManager.defaultManager trashItemAtURL:url resultingItemURL:nil error:nil];
    }
#endif
}
#endif
