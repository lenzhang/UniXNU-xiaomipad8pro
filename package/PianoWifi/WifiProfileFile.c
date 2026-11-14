// SPDX-License-Identifier: BSD-2-Clause-Patent
// PianoWifi level 7: the join profile read from its root-only file by the kext itself (docs/drivers/wifi.md §13.2, §13.5), so
// that a RAM probe whose Data has no piano-wifi-join helper still joins. The same rules as the helper: the directory
// /private/var/db/piano-wifi must be root's with no group or other bits, the file a regular one of root's with none either
// (like an ssh key), at most 4 KiB, not a symbolic link; the text is parsed by WifiProfile.h's piano_wifi_profile_parse, the
// helper's own parser. Nothing of the file is ever logged or returned but the structure the caller asked for; every copy here
// is wiped. The vnode calls are PianoPower's (PianoAdspFirmware.c), in the kernel's own context. New code for this repository.
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/kauth.h>
#include <sys/errno.h>
#include <IOKit/IOLib.h>
#include "WifiProfile.h"

#define PROFILE_DIR "/private/var/db/piano-wifi"
#define PROFILE_MAX 4096

static void wipe(volatile uint8_t *p, size_t n)
{
    while (n--) *p++ = 0;
}

// The owner and mode of a vnode: 0, or the reason it is not one the profile may live in or be.
static const char *checkOwner(vnode_t vp, vfs_context_t ctx, int wantDir, uint64_t *size)
{
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_uid);
    VATTR_WANTED(&va, va_mode);
    VATTR_WANTED(&va, va_data_size);
    if (vnode_getattr(vp, &va, ctx) || !VATTR_IS_SUPPORTED(&va, va_uid) || !VATTR_IS_SUPPORTED(&va, va_mode))
        return "its owner and mode could not be read";
    if (wantDir ? !vnode_isdir(vp) : !vnode_isreg(vp)) return wantDir ? "the directory is not one" : "the profile is not a file";
    if (va.va_uid != 0) return wantDir ? "the directory is not root's" : "the file is not root's";
    if (va.va_mode & 077) return wantDir ? "the directory has group or other permissions" : "the file has group or other permissions";
    if (size) *size = VATTR_IS_SUPPORTED(&va, va_data_size) ? va.va_data_size : 0;
    return 0;
}

// 0 and *out filled: a usable profile. ENOENT: no file (nothing to say). Any other error: *problem says why (never a byte of
// the file).
int piano_wifi_profile_read_file(struct piano_wifi_profile *out, const char **problem)
{
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t dir = NULLVP, vp = NULLVP;
    static uint8_t text[PROFILE_MAX];
    uint64_t size = 0;
    int error;
    *problem = 0;
    error = vnode_lookup(PROFILE_DIR, VNODE_LOOKUP_NOFOLLOW, &dir, ctx);
    if (error) return ENOENT;
    *problem = checkOwner(dir, ctx, 1, 0);
    vnode_put(dir);
    if (*problem) return EPERM;
    error = vnode_open(PIANO_WIFI_PROFILE_PATH, FREAD, 0, VNODE_LOOKUP_NOFOLLOW, &vp, ctx);
    if (error) return ENOENT;
    *problem = checkOwner(vp, ctx, 0, &size);
    if (!*problem && (!size || size > PROFILE_MAX)) *problem = "the file is empty or larger than 4 KiB";
    if (*problem) {
        vnode_close(vp, FREAD, ctx);
        return EPERM;
    }
    int resid = (int)size;
    error = vn_rdwr(UIO_READ, vp, (caddr_t)text, (int)size, 0, UIO_SYSSPACE, IO_NOAUTH, vfs_context_ucred(ctx), &resid,
                    vfs_context_proc(ctx));
    vnode_close(vp, FREAD, ctx);
    if (error || resid) {
        wipe(text, sizeof text);
        *problem = "the file could not be read whole";
        return EIO;
    }
    *problem = piano_wifi_profile_parse(text, (uint32_t)size, out);
    wipe(text, sizeof text);
    if (*problem) {
        piano_wifi_profile_wipe(out);
        return EINVAL;
    }
    return 0;
}
