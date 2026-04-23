/*
 * Limceron Compiler — Cross-Compilation Target Support
 *
 * Target triple parsing, native detection, cross-compiler discovery.
 * Used by both the compiler (main.c) and test suite (test_runner.c).
 */

#include "lcn.h"
#include <sys/stat.h>
#include <unistd.h>

/* ============================================================
 * String Helpers
 * ============================================================ */

const char *lcn_arch_str(LcnArch arch) {
    switch (arch) {
    case LCN_ARCH_X86_64:  return "x86_64";
    case LCN_ARCH_AARCH64: return "aarch64";
    case LCN_ARCH_ARM:     return "arm";
    default:               return "unknown";
    }
}

const char *lcn_os_str(LcnOS os) {
    switch (os) {
    case LCN_OS_LINUX:   return "linux";
    case LCN_OS_DARWIN:  return "darwin";
    case LCN_OS_WINDOWS: return "windows";
    default:             return "unknown";
    }
}

const char *lcn_abi_str(LcnABI abi) {
    switch (abi) {
    case LCN_ABI_GNU:  return "gnu";
    case LCN_ABI_MUSL: return "musl";
    case LCN_ABI_MSVC: return "msvc";
    default:           return "";
    }
}

const char *lcn_target_triple_str(const LcnTarget *target) {
    return target->triple;
}

/* ============================================================
 * Target Triple Parsing
 * ============================================================ */

static LcnArch parse_arch(const char *s, size_t len) {
    if ((len == 6 && strncmp(s, "x86_64", 6) == 0) ||
        (len == 5 && strncmp(s, "amd64", 5) == 0))
        return LCN_ARCH_X86_64;
    if ((len == 7 && strncmp(s, "aarch64", 7) == 0) ||
        (len == 5 && strncmp(s, "arm64", 5) == 0))
        return LCN_ARCH_AARCH64;
    if (len == 3 && strncmp(s, "arm", 3) == 0)
        return LCN_ARCH_ARM;
    return LCN_ARCH_UNKNOWN;
}

static LcnOS parse_os(const char *s, size_t len) {
    if (len == 5 && strncmp(s, "linux", 5) == 0) return LCN_OS_LINUX;
    if (len == 6 && strncmp(s, "darwin", 6) == 0) return LCN_OS_DARWIN;
    if (len == 5 && strncmp(s, "macos", 5) == 0) return LCN_OS_DARWIN;
    if (len == 7 && strncmp(s, "windows", 7) == 0) return LCN_OS_WINDOWS;
    if (len == 5 && strncmp(s, "win32", 5) == 0) return LCN_OS_WINDOWS;
    return LCN_OS_UNKNOWN;
}

static LcnABI parse_abi(const char *s, size_t len) {
    if (len == 3 && strncmp(s, "gnu", 3) == 0) return LCN_ABI_GNU;
    if (len == 4 && strncmp(s, "musl", 4) == 0) return LCN_ABI_MUSL;
    if (len == 4 && strncmp(s, "msvc", 4) == 0) return LCN_ABI_MSVC;
    return LCN_ABI_NONE;
}

LcnTarget lcn_parse_target(const char *triple) {
    LcnTarget t;
    memset(&t, 0, sizeof(t));

    if (!triple || !triple[0]) return t;

    /* Copy the triple string */
    strncpy(t.triple, triple, sizeof(t.triple) - 1);
    t.triple[sizeof(t.triple) - 1] = '\0';

    /* Split on '-': ARCH-OS[-ABI] */
    char buf[128];
    strncpy(buf, triple, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *parts[4] = {0};
    int nparts = 0;
    char *p = buf;
    while (p && *p && nparts < 4) {
        parts[nparts++] = p;
        char *dash = strchr(p, '-');
        if (dash) { *dash = '\0'; p = dash + 1; }
        else break;
    }

    if (nparts < 2) return t; /* need at least ARCH-OS */

    t.arch = parse_arch(parts[0], strlen(parts[0]));
    t.os   = parse_os(parts[1], strlen(parts[1]));

    if (nparts >= 3) {
        t.abi = parse_abi(parts[2], strlen(parts[2]));
    }

    /* Default ABI based on OS if not specified */
    if (t.abi == LCN_ABI_NONE) {
        if (t.os == LCN_OS_LINUX)   t.abi = LCN_ABI_GNU;
        if (t.os == LCN_OS_WINDOWS) t.abi = LCN_ABI_MSVC;
    }

    /* Rebuild canonical triple string */
    if (t.abi != LCN_ABI_NONE) {
        snprintf(t.triple, sizeof(t.triple), "%s-%s-%s",
                 lcn_arch_str(t.arch), lcn_os_str(t.os), lcn_abi_str(t.abi));
    } else {
        snprintf(t.triple, sizeof(t.triple), "%s-%s",
                 lcn_arch_str(t.arch), lcn_os_str(t.os));
    }

    /* Set target-specific CFLAGS and LDFLAGS */
    t.cflags[0] = '\0';
    t.ldflags[0] = '\0';

    switch (t.os) {
    case LCN_OS_LINUX:
        if (t.static_link) {
            snprintf(t.ldflags, sizeof(t.ldflags), "-static -lpthread -lm -ldl");
        } else {
            snprintf(t.ldflags, sizeof(t.ldflags), "-lpthread -lm -ldl");
        }
        break;
    case LCN_OS_DARWIN:
        snprintf(t.ldflags, sizeof(t.ldflags), "-lm");
        break;
    case LCN_OS_WINDOWS:
        snprintf(t.ldflags, sizeof(t.ldflags), "-lws2_32");
        break;
    default:
        break;
    }

    return t;
}

/* ============================================================
 * Native Target Detection
 * ============================================================ */

LcnTarget lcn_native_target(void) {
    LcnTarget t;
    memset(&t, 0, sizeof(t));

    /* Detect host architecture */
#if defined(__x86_64__) || defined(_M_X64) || defined(__amd64__)
    t.arch = LCN_ARCH_X86_64;
#elif defined(__aarch64__) || defined(_M_ARM64) || defined(__arm64__)
    t.arch = LCN_ARCH_AARCH64;
#elif defined(__arm__)
    t.arch = LCN_ARCH_ARM;
#else
    t.arch = LCN_ARCH_UNKNOWN;
#endif

    /* Detect host OS */
#if defined(__linux__)
    t.os = LCN_OS_LINUX;
    t.abi = LCN_ABI_GNU;
    snprintf(t.ldflags, sizeof(t.ldflags), "-lpthread -lm -ldl");
#elif defined(__APPLE__) && defined(__MACH__)
    t.os = LCN_OS_DARWIN;
    t.abi = LCN_ABI_NONE;
    snprintf(t.ldflags, sizeof(t.ldflags), "-lm");
#elif defined(_WIN32) || defined(_WIN64)
    t.os = LCN_OS_WINDOWS;
    t.abi = LCN_ABI_MSVC;
    snprintf(t.ldflags, sizeof(t.ldflags), "-lws2_32");
#else
    t.os = LCN_OS_UNKNOWN;
    t.abi = LCN_ABI_NONE;
#endif

    t.is_native = true;
    snprintf(t.cc, sizeof(t.cc), "cc");

    /* Build triple */
    if (t.abi != LCN_ABI_NONE) {
        snprintf(t.triple, sizeof(t.triple), "%s-%s-%s",
                 lcn_arch_str(t.arch), lcn_os_str(t.os), lcn_abi_str(t.abi));
    } else {
        snprintf(t.triple, sizeof(t.triple), "%s-%s",
                 lcn_arch_str(t.arch), lcn_os_str(t.os));
    }

    return t;
}

/* ============================================================
 * Cross-Compiler Detection
 * ============================================================ */

/* Check if a command exists on PATH by running "which <cmd>" */
static bool command_exists(const char *cmd) {
    char check[1024];
    snprintf(check, sizeof(check), "which %s > /dev/null 2>&1", cmd);
    return system(check) == 0;
}

bool lcn_find_cross_cc(LcnTarget *target) {
    if (!target || target->arch == LCN_ARCH_UNKNOWN || target->os == LCN_OS_UNKNOWN)
        return false;

    /* Check if native — just use "cc" */
    LcnTarget native = lcn_native_target();
    if (target->arch == native.arch && target->os == native.os) {
        target->is_native = true;
        snprintf(target->cc, sizeof(target->cc), "cc");
        return true;
    }

    const char *arch_str = lcn_arch_str(target->arch);
    const char *os_str   = lcn_os_str(target->os);
    const char *abi_s    = lcn_abi_str(target->abi);

    /* 1. Check environment variable: LCN_CC_AARCH64_LINUX etc. */
    {
        char env_name[128];
        char arch_upper[32], os_upper[32];
        size_t i;
        for (i = 0; arch_str[i] && i < sizeof(arch_upper) - 1; i++)
            arch_upper[i] = (arch_str[i] >= 'a' && arch_str[i] <= 'z')
                ? (char)(arch_str[i] - 32) : arch_str[i];
        arch_upper[i] = '\0';
        for (i = 0; os_str[i] && i < sizeof(os_upper) - 1; i++)
            os_upper[i] = (os_str[i] >= 'a' && os_str[i] <= 'z')
                ? (char)(os_str[i] - 32) : os_str[i];
        os_upper[i] = '\0';

        snprintf(env_name, sizeof(env_name), "LCN_CC_%s_%s", arch_upper, os_upper);
        const char *env_cc = getenv(env_name);
        if (env_cc && env_cc[0]) {
            snprintf(target->cc, sizeof(target->cc), "%s", env_cc);
            return true;
        }
    }

    /* 2. Try standard GNU cross-compiler: aarch64-linux-gnu-gcc */
    if (target->os == LCN_OS_LINUX) {
        char try_cc[256];

        snprintf(try_cc, sizeof(try_cc), "%s-%s-%s-gcc",
                 arch_str, os_str, abi_s[0] ? abi_s : "gnu");
        if (command_exists(try_cc)) {
            snprintf(target->cc, sizeof(target->cc), "%s", try_cc);
            return true;
        }

        /* 3. Try with -unknown- infix: aarch64-unknown-linux-gnu-gcc */
        snprintf(try_cc, sizeof(try_cc), "%s-unknown-%s-%s-gcc",
                 arch_str, os_str, abi_s[0] ? abi_s : "gnu");
        if (command_exists(try_cc)) {
            snprintf(target->cc, sizeof(target->cc), "%s", try_cc);
            return true;
        }
    }

    /* 4. Try zig cc if available */
    if (command_exists("zig")) {
        char zig_target[128];
        if (target->os == LCN_OS_LINUX) {
            snprintf(zig_target, sizeof(zig_target), "%s-%s-%s",
                     arch_str, os_str, abi_s[0] ? abi_s : "gnu");
        } else if (target->os == LCN_OS_DARWIN) {
            snprintf(zig_target, sizeof(zig_target), "%s-macos", arch_str);
        } else {
            snprintf(zig_target, sizeof(zig_target), "%s-%s", arch_str, os_str);
        }
        /* zig cc needs to be invoked as "zig cc -target ..." but we store
         * the full command prefix since system() will use it */
        snprintf(target->cc, sizeof(target->cc), "zig cc -target %s", zig_target);
        return true;
    }

    /* 5. Try clang with --target */
    if (command_exists("clang")) {
        char clang_target[128];
        if (target->os == LCN_OS_LINUX) {
            snprintf(clang_target, sizeof(clang_target), "%s-%s-%s",
                     arch_str, os_str, abi_s[0] ? abi_s : "gnu");
        } else if (target->os == LCN_OS_DARWIN) {
            snprintf(clang_target, sizeof(clang_target), "%s-apple-darwin", arch_str);
        } else if (target->os == LCN_OS_WINDOWS) {
            snprintf(clang_target, sizeof(clang_target), "%s-pc-windows-msvc", arch_str);
        } else {
            snprintf(clang_target, sizeof(clang_target), "%s-%s", arch_str, os_str);
        }
        snprintf(target->cc, sizeof(target->cc), "clang --target=%s", clang_target);
        return true;
    }

    return false;
}
