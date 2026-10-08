// GvfsMount: a gvfsd MountTracker entry -> a dock location.
#include "../GvfsMount.hpp"

#include <cstdio>
#include <string>

using namespace ScrollOverview::Gvfs;

static int checks = 0, failed = 0;

static void expect(bool ok, const char* what) {
    ++checks;
    if (!ok) {
        ++failed;
        std::printf("FAIL %s\n", what);
    }
}

static void expectUri(const SMount& m, const std::string& want, const char* what) {
    const auto GOT = uriFor(m);
    ++checks;
    if (!GOT || *GOT != want) {
        ++failed;
        std::printf("FAIL %s: got [%s] want [%s]\n", what, GOT ? GOT->c_str() : "(none)", want.c_str());
    }
}

int main() {
    // The user's share, exactly as ListMounts2 returned it (2026-10-08).
    const SMount SMB{
        .display     = "office cloud on 100.122.4.49",
        .icon        = ". GThemedIcon folder-remote folder folder-remote-symbolic folder-symbolic",
        .fuse        = "/run/user/1000/gvfs/smb-share:server=100.122.4.49,share=office%20cloud",
        .prefix      = "/",
        .spec        = {{"server", "100.122.4.49"}, {"share", "office cloud"}, {"type", "smb-share"}},
        .userVisible = true,
    };
    expectUri(SMB, "smb://100.122.4.49/office%20cloud/", "smb share = what `gio mount -l` prints");
    expect(firstThemedIcon(SMB.icon) == "folder-remote", "themed icon's first name");
    expect(listed(SMB) && isNetwork("smb-share"), "an smb share is listed, and is network");

    expectUri({.prefix = "/home/me", .spec = {{"type", "sftp"}, {"host", "box.lan"}, {"user", "me"}, {"port", "2222"}}, .userVisible = true},
              "sftp://me@box.lan:2222/home/me", "sftp with user, port and path");
    expectUri({.prefix = "/remote.php/dav", .spec = {{"type", "dav"}, {"host", "cloud.example"}, {"ssl", "true"}}, .userVisible = true},
              "davs://cloud.example/remote.php/dav", "dav over ssl is davs");
    expectUri({.prefix = "/", .spec = {{"type", "smb-share"}, {"server", "fe80::1"}, {"share", "x"}}, .userVisible = true}, "smb://[fe80::1]/x/",
              "IPv6 host in brackets");
    expectUri({.fuse = "/run/user/1000/gvfs/mtp:host=Pixel_8", .spec = {{"type", "mtp"}, {"host", "Pixel_8"}}, .userVisible = true},
              "file:///run/user/1000/gvfs/mtp%3Ahost%3DPixel_8", "an unknown kind opens through FUSE");
    expect(!uriFor({.spec = {{"type", "mtp"}}, .userVisible = true}).has_value(), "no scheme and no FUSE: nothing to open");

    expect(!listed({.spec = {{"type", "trash"}}, .userVisible = true}), "gvfsd's trash is not a drive");
    expect(!listed({.spec = {{"type", "recent"}}, .userVisible = true}), "nor recent");
    expect(!listed({.spec = {{"type", "smb-share"}}, .userVisible = false}), "an invisible mount is not listed");
    expect(!isNetwork("mtp") && !isNetwork("gphoto2") && isNetwork("sftp"), "phones are local, sftp is network");
    expect(firstThemedIcon(". GEmblemedIcon x") == "" && firstThemedIcon("") == "", "a non-themed icon gives no name");
    expect(escape("a b/é", true) == "a%20b/%C3%A9" && escape("a/b", false) == "a%2Fb", "escaping");

    std::printf("gvfs_mount_test: %d checks, %d failed\n", checks, failed);
    return failed ? 1 : 0;
}
