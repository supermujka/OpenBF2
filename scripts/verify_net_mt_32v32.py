#!/usr/bin/env python3
"""Static verification script for OpenBF2 feat/net-mt-32v32."""

import os
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent

CPP_EXTS = {".h", ".hpp", ".inl", ".cpp", ".c"}

STD_AND_POSIX_HEADERS = {
    # C++14 standard headers
    "algorithm", "array", "atomic", "bitset", "cassert", "cctype", "cerrno",
    "cfloat", "chrono", "cinttypes", "climits", "clocale", "cmath", "complex",
    "condition_variable", "csetjmp", "csignal", "cstdarg", "cstddef",
    "cstdint", "cstdio", "cstdlib", "cstring", "ctime", "cwchar", "cwctype",
    "deque", "exception", "forward_list", "fstream", "functional", "future",
    "initializer_list", "iomanip", "ios", "iosfwd", "iostream", "istream",
    "iterator", "limits", "list", "locale", "map", "memory", "mutex", "new",
    "numeric", "ostream", "queue", "random", "ratio", "regex", "scoped_allocator",
    "set", "shared_mutex", "sstream", "stack", "stdexcept", "streambuf",
    "string", "strstream", "system_error", "thread", "tuple", "type_traits",
    "typeindex", "typeinfo", "unordered_map", "unordered_set", "utility",
    "valarray", "vector",
    # C headers
    "assert.h", "ctype.h", "errno.h", "float.h", "inttypes.h", "limits.h",
    "locale.h", "math.h", "setjmp.h", "signal.h", "stdarg.h", "stddef.h",
    "stdint.h", "stdio.h", "stdlib.h", "string.h", "time.h", "wchar.h",
    # POSIX / Linux / Windows system headers
    "arpa/inet.h", "dirent.h", "dlfcn.h", "fcntl.h", "netdb.h",
    "netinet/in.h", "netinet/tcp.h", "poll.h", "pthread.h", "sched.h",
    "sys/epoll.h", "sys/ioctl.h", "sys/mman.h", "sys/select.h",
    "sys/socket.h", "sys/stat.h", "sys/time.h", "sys/types.h",
    "sys/utsname.h", "unistd.h", "windows.h", "winsock2.h", "ws2tcpip.h",
    "dbghelp.h", "mmsystem.h", "process.h", "shellapi.h", "shlobj.h",
    # External vendored headers on include path
    "Python.h", "zlib.h", "tinyxml.h",
}


def run_git(args):
    res = subprocess.run(
        ["git", "-C", str(REPO_ROOT)] + args,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    return res.returncode, res.stdout.strip(), res.stderr.strip()


def strip_comments_and_strings(code: str, keep_strings: bool = False) -> str:
    """Replace comments (and optionally string/char literals) with spaces, preserving newlines."""
    out = []
    i = 0
    n = len(code)
    while i < n:
        ch = code[i]
        # Line comment
        if ch == "/" and i + 1 < n and code[i + 1] == "/":
            while i < n and code[i] != "\n":
                out.append(" ")
                i += 1
            continue
        # Block comment
        if ch == "/" and i + 1 < n and code[i + 1] == "*":
            out.append("  ")
            i += 2
            while i < n and not (code[i] == "*" and i + 1 < n and code[i + 1] == "/"):
                out.append("\n" if code[i] == "\n" else " ")
                i += 1
            if i < n:
                out.append("  ")
                i += 2
            continue
        # String literal
        if ch == '"':
            if keep_strings:
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            while i < n:
                c = code[i]
                if keep_strings:
                    out.append(c)
                else:
                    out.append("\n" if c == "\n" else " ")
                if c == "\\" and i + 1 < n:
                    i += 1
                    if keep_strings:
                        out.append(code[i])
                    else:
                        out.append("\n" if code[i] == "\n" else " ")
                    i += 1
                    continue
                if c == '"':
                    i += 1
                    break
                i += 1
            continue
        # Char literal
        if ch == "'":
            if keep_strings:
                out.append(ch)
            else:
                out.append(" ")
            i += 1
            while i < n:
                c = code[i]
                if keep_strings:
                    out.append(c)
                else:
                    out.append("\n" if c == "\n" else " ")
                if c == "\\" and i + 1 < n:
                    i += 1
                    if keep_strings:
                        out.append(code[i])
                    else:
                        out.append("\n" if code[i] == "\n" else " ")
                    i += 1
                    continue
                if c == "'":
                    i += 1
                    break
                i += 1
            continue
        out.append(ch)
        i += 1
    return "".join(out)


def get_changed_and_untracked_cpp_files():
    rc1, out1, _ = run_git(["diff", "--name-only", "hitreg-v1"])
    rc2, out2, _ = run_git(["ls-files", "--others", "--exclude-standard"])
    files = set()
    for line in (out1 + "\n" + out2).splitlines():
        p = line.strip()
        if not p:
            continue
        if Path(p).suffix.lower() in CPP_EXTS and (REPO_ROOT / p).is_file():
            files.add(p)
    return sorted(files)


def check_git_branch_safety():
    issues = []
    rc, branch, err = run_git(["rev-parse", "--abbrev-ref", "HEAD"])
    if rc != 0 or branch != "feat/net-mt-32v32":
        issues.append(f"Current branch is '{branch}', expected 'feat/net-mt-32v32'")

    rc, sha, err = run_git(["rev-parse", "--short", "hitreg-v1"])
    if rc != 0 or not sha.startswith("9cd3527"):
        issues.append(f"hitreg-v1 resolves to '{sha}', expected '9cd3527'")

    return len(issues) == 0, issues, f"branch={branch}, hitreg-v1={sha}"


def check_hitreg_v1_invariants():
    issues = []
    lat_file = REPO_ROOT / "Code/BF2/Physics/LatencyCompensation.cpp"
    if not lat_file.is_file():
        issues.append("Missing Code/BF2/Physics/LatencyCompensation.cpp")
    else:
        lat_txt = lat_file.read_text(encoding="utf-8", errors="replace")
        if not re.search(
            r'Vars::getInt\s*\(\s*"phy-latencyComp-closestBone"\s*,\s*0\s*\)', lat_txt
        ):
            issues.append("phy-latencyComp-closestBone default is not 0")

    gs_file = REPO_ROOT / "Code/BF2/Game/GameServer/GameServer.cpp"
    if not gs_file.is_file():
        issues.append("Missing Code/BF2/Game/GameServer/GameServer.cpp")
    else:
        gs_txt = gs_file.read_text(encoding="utf-8", errors="replace")
        for sym in ("g_currentUpdateSimSteps", "forceFullRateSend", "HitregTelemetry::noteSnapshotSent", "HitregTelemetry::tickFlush"):
            if sym not in gs_txt:
                issues.append(f"GameServer.cpp missing hitreg-v1 symbol: {sym}")

    bf2_eng = REPO_ROOT / "Code/BF2/Game/Main/BF2Engine.cpp"
    if bf2_eng.is_file():
        eng_txt = bf2_eng.read_text(encoding="utf-8", errors="replace")
        if "HitregTelemetry::noteServerTickUs" not in eng_txt:
            issues.append("BF2Engine.cpp missing HitregTelemetry::noteServerTickUs")

    ht_file = REPO_ROOT / "Code/BF2/Physics/HitregTelemetry.cpp"
    if not ht_file.is_file():
        issues.append("Missing Code/BF2/Physics/HitregTelemetry.cpp")
    else:
        rc, ht_diff, _ = run_git(["diff", "hitreg-v1", "--", "Code/BF2/Physics/HitregTelemetry.cpp", "Code/BF2/Physics/HitregTelemetry.h"])
        if ht_diff:
            issues.append("HitregTelemetry.{h,cpp} was modified vs hitreg-v1 (verify 82-col CSV hooks manually)")

    return len(issues) == 0, issues, "closestBone=0, Opcija A, HitregTelemetry hooks intact"


def check_abi_layout_vtable():
    issues = []
    rc, diff_out, _ = run_git(["diff", "--diff-filter=M", "-U0", "hitreg-v1", "--", "*.h", "*.hpp"])
    if rc != 0:
        return False, [f"git diff failed: {diff_out}"], ""

    current_file = None
    new_struct_depth = 0
    pending_new_struct = False
    for raw_line in diff_out.splitlines():
        if raw_line.startswith("+++ b/"):
            current_file = raw_line[6:]
            new_struct_depth = 0
            pending_new_struct = False
            continue
        if raw_line.startswith("--- ") or raw_line.startswith("diff ") or raw_line.startswith("index ") or raw_line.startswith("@@"):
            new_struct_depth = 0
            pending_new_struct = False
            continue
        if not raw_line.startswith("+") and not raw_line.startswith("-"):
            continue

        sign = raw_line[0]
        line = raw_line[1:].strip()
        # Strip inline // comments
        line_no_cmt = re.sub(r"//.*", "", line).strip()
        if not line_no_cmt:
            continue
        if line_no_cmt.startswith("/*") and line_no_cmt.endswith("*/"):
            continue

        # Flag altered static_asserts
        if re.search(r"\bstatic_assert\b", line_no_cmt):
            issues.append(f"{current_file}: altered static_assert ({sign}): {line_no_cmt}")
            continue

        if sign != "+":
            continue

        # Flag virtual keywords in headers
        if re.search(r"\bvirtual\b", line_no_cmt):
            issues.append(f"{current_file}: added virtual keyword: {line_no_cmt}")

        # Flag added inheritance (ignore `enum class Foo : uint8_t`)
        if not re.search(r"\benum\s+class\b", line_no_cmt) and re.search(r"\b(class|struct)\s+\w+\s*:", line_no_cmt):
            issues.append(f"{current_file}: added/modified class inheritance: {line_no_cmt}")

        # Track brand-new standalone struct/class definitions added in the diff
        if new_struct_depth == 0 and re.match(r"^(struct|class)\s+\w+\s*(\{.*)?$", line_no_cmt) and not line_no_cmt.endswith(";"):
            if "{" in line_no_cmt:
                new_struct_depth += line_no_cmt.count("{") - line_no_cmt.count("}")
            else:
                pending_new_struct = True
            continue
        if pending_new_struct:
            if "{" in line_no_cmt:
                new_struct_depth += line_no_cmt.count("{") - line_no_cmt.count("}")
                pending_new_struct = False
                continue
            pending_new_struct = False
        if new_struct_depth > 0:
            new_struct_depth += line_no_cmt.count("{") - line_no_cmt.count("}")
            if new_struct_depth < 0:
                new_struct_depth = 0
            continue

        # Heuristic for non-static member variables in existing classes:
        if line_no_cmt.endswith(";") and line_no_cmt != "};":
            is_func = "(" in line_no_cmt and ")" in line_no_cmt
            is_non_field = re.match(
                r"^(friend|typedef|using|static|enum|namespace|class|struct|return|extern)\b",
                line_no_cmt,
            )
            if not is_func and not is_non_field and not line_no_cmt.startswith("#"):
                issues.append(f"{current_file}: potential added member/field: {line_no_cmt}")

    return len(issues) == 0, issues, "existing .h/.hpp diffs scanned"


def check_cpp14_compliance(files):
    issues = []
    forbidden = [
        ("std::string_view", re.compile(r"\bstd::string_view\b")),
        ("std::optional", re.compile(r"\bstd::optional\b")),
        ("std::variant", re.compile(r"\bstd::variant\b")),
        ("std::any", re.compile(r"\bstd::any\b")),
        ("if constexpr", re.compile(r"\bif\s+constexpr\b")),
        ("structured binding auto [", re.compile(r"\bauto\s*\[")),
        ("[[nodiscard]]", re.compile(r"\[\[\s*nodiscard\s*\]\]")),
        ("[[maybe_unused]]", re.compile(r"\[\[\s*maybe_unused\s*\]\]")),
        ("nested namespace a::b", re.compile(r"\bnamespace\s+[A-Za-z_]\w*\s*::")),
        ("std::clamp", re.compile(r"\bstd::clamp\b")),
        ("std::size", re.compile(r"\bstd::size\b")),
    ]

    for rel in files:
        txt = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        cleaned = strip_comments_and_strings(txt, keep_strings=False)
        for lineno, line in enumerate(cleaned.splitlines(), 1):
            for label, pat in forbidden:
                if pat.search(line):
                    issues.append(f"{rel}:{lineno}: forbidden C++17/20 construct ({label})")

    return len(issues) == 0, issues, f"{len(files)} files scanned"


def check_cvar_defaults(files):
    issues = []
    found_cvars = []
    # Find added Vars::get* in git diff hitreg-v1 + untracked files
    rc, diff_out, _ = run_git(["diff", "-U3", "hitreg-v1"])
    cvar_pat = re.compile(
        r'Vars::(getInt|getFloat|getString)\s*\(\s*"([^"]+)"\s*,\s*([^)]+)\)'
    )

    seen = set()
    current_file = None
    for raw_line in diff_out.splitlines():
        if raw_line.startswith("+++ b/"):
            current_file = raw_line[6:]
            continue
        if raw_line.startswith("+") and not raw_line.startswith("+++"):
            line = raw_line[1:]
            for m in cvar_pat.finditer(line):
                kind, name, default_val = m.group(1), m.group(2), m.group(3).strip()
                key = (current_file, name, default_val)
                if key not in seen:
                    seen.add(key)
                    found_cvars.append((current_file, kind, name, default_val))

    # Also check untracked files
    rc2, untracked_out, _ = run_git(["ls-files", "--others", "--exclude-standard"])
    for rel in untracked_out.splitlines():
        rel = rel.strip()
        if not rel or Path(rel).suffix.lower() not in CPP_EXTS:
            continue
        p = REPO_ROOT / rel
        if not p.is_file():
            continue
        txt = strip_comments_and_strings(p.read_text(encoding="utf-8", errors="replace"), keep_strings=True)
        for m in cvar_pat.finditer(txt):
            kind, name, default_val = m.group(1), m.group(2), " ".join(m.group(3).split())
            key = (rel, name, default_val)
            if key not in seen:
                seen.add(key)
                found_cvars.append((rel, kind, name, default_val))

    for fpath, kind, name, default_val in found_cvars:
        if name == "sv-perf-profile-interval":
            continue
        if kind == "getInt" and default_val != "0":
            issues.append(f"{fpath}: {name} defaults to {default_val} (expected 0)")
        elif kind == "getFloat" and default_val not in ("0.0f", "0.0", "0"):
            issues.append(f"{fpath}: {name} defaults to {default_val} (expected 0.0f)")
        elif kind == "getString" and default_val not in ('""', "0", "nullptr"):
            issues.append(f"{fpath}: {name} defaults to {default_val} (expected \"\" or 0)")

    summary = ", ".join(f"{n}={d}" for _, _, n, d in found_cvars) or "none"
    return len(issues) == 0, issues, summary


def check_cpp_hygiene(files):
    issues = []
    include_roots = [
        REPO_ROOT / "Code/BF2",
        REPO_ROOT / "Code/BF2/External/zlib-1.2.3",
        REPO_ROOT / "Code/BF2/External/zlib-1.2.3/contrib/minizip",
        REPO_ROOT / "Code/BF2/External/TinyXML",
        REPO_ROOT / "Code/BF2/External/python-embedded/linux",
        REPO_ROOT / "Code/BF2/External/python-embedded/py-src-2.3.4/Include",
        REPO_ROOT / "Code/BF2/External/python-embedded/py-src-2.3.4/PC",
    ]

    static_init_check_files = {
        "Code/BF2/IO/Network/NetworkManager.cpp",
        "Code/BF2/IO/TickCalculator.cpp",
        "Code/BF2/Game/Common/ClientConnection.cpp",
        "Code/BF2/Game/Common/GhostManager.cpp",
    }

    for rel in files:
        p = REPO_ROOT / rel
        raw = p.read_text(encoding="utf-8", errors="replace")
        cleaned = strip_comments_and_strings(raw, keep_strings=False)

        # 1. Balanced braces, parens, and #if/#endif
        braces = 0
        parens = 0
        if_depth = 0
        for lineno, line in enumerate(cleaned.splitlines(), 1):
            s = line.strip()
            if re.match(r"^#\s*(if|ifdef|ifndef)\b", s):
                if_depth += 1
            elif re.match(r"^#\s*endif\b", s):
                if_depth -= 1
                if if_depth < 0:
                    issues.append(f"{rel}:{lineno}: unmatched #endif")
            braces += line.count("{") - line.count("}")
            parens += line.count("(") - line.count(")")
            if braces < 0:
                issues.append(f"{rel}:{lineno}: unmatched '}}'")
                braces = 0
            if parens < 0:
                issues.append(f"{rel}:{lineno}: unmatched ')'")
                parens = 0

        if braces != 0:
            issues.append(f"{rel}: unbalanced braces (delta={braces})")
        if parens != 0:
            issues.append(f"{rel}: unbalanced parens (delta={parens})")
        if if_depth != 0:
            issues.append(f"{rel}: unbalanced #if/#endif (delta={if_depth})")

        # 2. Verify #include paths
        for lineno, line in enumerate(raw.splitlines(), 1):
            m = re.match(r'^\s*#\s*include\s*"([^"]+)"', line)
            if m:
                inc = m.group(1)
                candidates = [p.parent / inc] + [r / inc for r in include_roots]
                if not any(c.is_file() for c in candidates):
                    issues.append(f"{rel}:{lineno}: missing local include \"{inc}\"")
            else:
                m2 = re.match(r"^\s*#\s*include\s*<([^>]+)>", line)
                if m2:
                    inc = m2.group(1)
                    if inc not in STD_AND_POSIX_HEADERS and not any((r / inc).is_file() for r in include_roots):
                        issues.append(f"{rel}:{lineno}: unknown system include <{inc}>")

        # 3. Check for file-scope Vars::get* static initializers in guarded TUs
        if rel in static_init_check_files:
            depth = 0
            in_anon_ns = False
            anon_ns_depth = -1
            for lineno, line in enumerate(cleaned.splitlines(), 1):
                if re.search(r"\bnamespace\s*\{", line):
                    in_anon_ns = True
                    anon_ns_depth = depth + 1
                if "Vars::get" in line:
                    # Outside function bodies: brace depth 0, or depth 1 inside anonymous namespace
                    if depth == 0 or (in_anon_ns and depth == anon_ns_depth):
                        issues.append(f"{rel}:{lineno}: file-scope Vars::get* static initializer")
                depth += line.count("{") - line.count("}")
                if in_anon_ns and depth < anon_ns_depth:
                    in_anon_ns = False
                    anon_ns_depth = -1

    return len(issues) == 0, issues, f"{len(files)} files checked"


def main():
    files = get_changed_and_untracked_cpp_files()
    checks = [
        ("1. Git branch safety", check_git_branch_safety),
        ("2. hitreg-v1 invariants", check_hitreg_v1_invariants),
        ("3. Zero ABI/layout/vtable breakage", check_abi_layout_vtable),
        ("4. Strict C++14 compliance", lambda: check_cpp14_compliance(files)),
        ("5. CVar default audit", lambda: check_cvar_defaults(files)),
        ("6. Basic C++ hygiene", lambda: check_cpp_hygiene(files)),
    ]

    all_ok = True
    print(f"=== OpenBF2 feat/net-mt-32v32 Static Verification ({len(files)} changed/new C/C++ files) ===")
    for name, fn in checks:
        ok, issues, info = fn()
        status = "PASS" if ok else "FAIL"
        print(f"[{status}] {name} ({info})")
        if not ok:
            all_ok = False
            for iss in issues:
                print(f"       - {iss}")

    print("=== RESULT:", "ALL CHECKS PASSED ===" if all_ok else "FAILED ===")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
