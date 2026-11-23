#!/usr/bin/env python3
"""Static verification script for OpenBF2 feat/loadbot-64p."""

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
    rc1, out1, _ = run_git(["diff", "--name-only", "feat/net-mt-32v32"])
    rc2, out2, _ = run_git(["ls-files", "--others", "--exclude-standard"])
    files = set()
    for line in (out1 + "\n" + out2).splitlines():
        p = line.strip().replace("\\", "/")
        if not p:
            continue
        if Path(p).suffix.lower() in CPP_EXTS and (REPO_ROOT / p).is_file():
            files.add(p)
    return sorted(files)


def resolve_ref_short(ref_name: str):
    rc, sha, _ = run_git(["rev-parse", "--short", ref_name])
    if rc == 0 and sha:
        return rc, sha
    rc, sha, _ = run_git(["rev-parse", "--short", f"origin/{ref_name}"])
    return rc, sha


def check_git_branch_safety():
    issues = []
    rc, branch, _ = run_git(["rev-parse", "--abbrev-ref", "HEAD"])
    if rc != 0 or branch != "feat/loadbot-64p":
        issues.append(f"Current branch is '{branch}', expected 'feat/loadbot-64p'")

    rc, sha_hitreg = resolve_ref_short("hitreg-v1")
    if rc != 0 or not sha_hitreg.startswith("9cd3527"):
        issues.append(f"hitreg-v1 resolves to '{sha_hitreg}', expected '9cd3527'")

    rc, sha_mt = resolve_ref_short("feat/net-mt-32v32")
    if rc != 0 or not sha_mt.startswith("f579a3c"):
        issues.append(f"feat/net-mt-32v32 resolves to '{sha_mt}', expected 'f579a3c'")

    return len(issues) == 0, issues, f"branch={branch}, hitreg-v1={sha_hitreg}, feat/net-mt-32v32={sha_mt}"


def check_abi_layout_vtable():
    issues = []
    rc, diff_out, _ = run_git(["diff", "--diff-filter=M", "-U0", "feat/net-mt-32v32", "--", "*.h", "*.hpp"])
    if rc != 0:
        return False, [f"git diff failed: {diff_out}"], ""

    current_file = None
    new_struct_depth = 0
    pending_new_struct = False
    scanned_headers = set()
    for raw_line in diff_out.splitlines():
        if raw_line.startswith("+++ b/"):
            current_file = raw_line[6:].replace("\\", "/")
            new_struct_depth = 0
            pending_new_struct = False
            if current_file.startswith("Code/BF2/"):
                scanned_headers.add(current_file)
            continue
        if raw_line.startswith("--- ") or raw_line.startswith("diff ") or raw_line.startswith("index ") or raw_line.startswith("@@"):
            new_struct_depth = 0
            pending_new_struct = False
            continue
        if not current_file or not current_file.startswith("Code/BF2/"):
            continue
        if not raw_line.startswith("+") and not raw_line.startswith("-"):
            continue

        sign = raw_line[0]
        line = raw_line[1:].strip()
        line_no_cmt = re.sub(r"//.*", "", line).strip()
        if not line_no_cmt:
            continue
        if line_no_cmt.startswith("/*") and line_no_cmt.endswith("*/"):
            continue

        if re.search(r"\bstatic_assert\b", line_no_cmt):
            issues.append(f"{current_file}: altered static_assert ({sign}): {line_no_cmt}")
            continue

        if sign != "+":
            continue

        if re.search(r"\bvirtual\b", line_no_cmt):
            issues.append(f"{current_file}: added virtual keyword: {line_no_cmt}")

        if not re.search(r"\benum\s+class\b", line_no_cmt) and re.search(r"\b(class|struct)\s+\w+\s*:", line_no_cmt):
            issues.append(f"{current_file}: added/modified class inheritance: {line_no_cmt}")

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

        if line_no_cmt.endswith(";") and line_no_cmt != "};":
            is_func = "(" in line_no_cmt and ")" in line_no_cmt
            is_non_field = re.match(
                r"^(friend|typedef|using|static|enum|namespace|class|struct|return|extern)\b",
                line_no_cmt,
            )
            if not is_func and not is_non_field and not line_no_cmt.startswith("#"):
                issues.append(f"{current_file}: potential added member/field: {line_no_cmt}")

    info = f"{len(scanned_headers)} modified Code/BF2/ header(s) scanned"
    return len(issues) == 0, issues, info


def check_cvar_safety(files):
    issues = []
    rec_path = REPO_ROOT / "Code/BF2/Game/GameServer/RouteActionRecorder.cpp"
    if not rec_path.is_file():
        issues.append("Missing Code/BF2/Game/GameServer/RouteActionRecorder.cpp")
    else:
        rec_txt = strip_comments_and_strings(rec_path.read_text(encoding="utf-8", errors="replace"), keep_strings=True)
        if not re.search(r'Vars::getInt\s*\(\s*"sv-record-routes"\s*,\s*0\s*\)', rec_txt):
            issues.append("sv-record-routes does not default to 0 in RouteActionRecorder.cpp")

    # Identify added line numbers in tracked Code/BF2/ files vs feat/net-mt-32v32,
    # plus all lines in untracked Code/BF2/ files.
    rc_untracked, out_untracked, _ = run_git(["ls-files", "--others", "--exclude-standard", "--", "Code/BF2"])
    untracked_bf2 = {line.strip().replace("\\", "/") for line in out_untracked.splitlines() if line.strip()}

    added_lines_by_file = {}
    rc_diff, diff_out, _ = run_git(["diff", "-U0", "feat/net-mt-32v32", "--", "Code/BF2"])
    if rc_diff == 0:
        cur_f = None
        cur_ln = 0
        for raw_line in diff_out.splitlines():
            if raw_line.startswith("+++ b/"):
                cur_f = raw_line[6:].replace("\\", "/")
                added_lines_by_file.setdefault(cur_f, set())
            elif raw_line.startswith("@@"):
                hm = re.search(r"\+(\d+)", raw_line)
                if hm:
                    cur_ln = int(hm.group(1))
            elif raw_line.startswith("+") and not raw_line.startswith("+++") and cur_f:
                added_lines_by_file[cur_f].add(cur_ln)
                cur_ln += 1

    bf2_files = [f for f in files if f.startswith("Code/BF2/")]
    for rel in bf2_files:
        is_untracked = rel in untracked_bf2
        added_lines = added_lines_by_file.get(rel, set())
        raw = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        cleaned = strip_comments_and_strings(raw, keep_strings=False)
        depth = 0
        in_anon_ns = False
        anon_ns_depth = -1
        for lineno, line in enumerate(cleaned.splitlines(), 1):
            if re.search(r"\bnamespace\s*\{", line):
                in_anon_ns = True
                anon_ns_depth = depth + 1
            if "Vars::get" in line and (is_untracked or lineno in added_lines):
                if depth == 0 or (in_anon_ns and depth == anon_ns_depth):
                    issues.append(f"{rel}:{lineno}: file-scope Vars::get* static initializer")
            depth += line.count("{") - line.count("}")
            if in_anon_ns and depth < anon_ns_depth:
                in_anon_ns = False
                anon_ns_depth = -1

    return len(issues) == 0, issues, f"sv-record-routes=0, {len(bf2_files)} Code/BF2/ files checked"


def check_cpp14_compliance(files):
    issues = []
    forbidden = [
        ("std::string_view", re.compile(r"\bstd::string_view\b")),
        ("std::optional", re.compile(r"\bstd::optional\b")),
        ("std::variant", re.compile(r"\bstd::variant\b")),
        ("std::any", re.compile(r"\bstd::any\b")),
        ("std::filesystem", re.compile(r"\bstd::filesystem\b")),
        ("if constexpr", re.compile(r"\bif\s+constexpr\b")),
        ("structured binding auto [", re.compile(r"\bauto\s*\[")),
        ("[[nodiscard]]", re.compile(r"\[\[\s*nodiscard\s*\]\]")),
        ("[[maybe_unused]]", re.compile(r"\[\[\s*maybe_unused\s*\]\]")),
        ("nested namespace a::b", re.compile(r"\bnamespace\s+[A-Za-z_]\w*\s*::")),
        ("std::clamp", re.compile(r"\bstd::clamp\b")),
        ("std::size", re.compile(r"\bstd::size\b")),
        ("C++17/20 header", re.compile(r"#\s*include\s*<\s*(filesystem|optional|variant|any|string_view|charconv|span|concepts|ranges|bit)\s*>")),
    ]

    for rel in files:
        raw = (REPO_ROOT / rel).read_text(encoding="utf-8", errors="replace")
        cleaned = strip_comments_and_strings(raw, keep_strings=False)
        for lineno, line in enumerate(cleaned.splitlines(), 1):
            for label, pat in forbidden:
                if pat.search(line):
                    issues.append(f"{rel}:{lineno}: forbidden C++17/20 construct ({label})")
        # Also check raw lines for C++17/20 system headers (since keep_strings=False doesn't strip <...>)
        for lineno, line in enumerate(raw.splitlines(), 1):
            m = re.match(r"^\s*#\s*include\s*<\s*(filesystem|optional|variant|any|string_view|charconv|span|concepts|ranges|bit)\s*>", line)
            if m:
                issues.append(f"{rel}:{lineno}: forbidden C++17/20 header <{m.group(1)}>")

    return len(issues) == 0, issues, f"{len(files)} files scanned"


def extract_struct_fields(text: str, struct_name: str):
    cleaned = strip_comments_and_strings(text, keep_strings=False)
    m = re.search(rf"\bstruct\s+{re.escape(struct_name)}\s*\{{(.*?)\}};", cleaned, re.DOTALL)
    if not m:
        return None
    fields = []
    for raw_stmt in m.group(1).split(";"):
        stmt = " ".join(raw_stmt.split())
        if not stmt:
            continue
        # Normalize std::uint32_t -> uint32_t etc. for comparison
        stmt = re.sub(r"\bstd::(uint\d+_t|int\d+_t)\b", r"\1", stmt)
        # Split type and variable name (plus optional array)
        fm = re.match(r"^(.*?)\s+([A-Za-z_]\w*(?:\[\d+\])?)$", stmt)
        if fm:
            fields.append((fm.group(1).strip(), fm.group(2).strip()))
        else:
            fields.append((stmt, ""))
    return fields


def check_cpp_hygiene_and_parity(files):
    issues = []
    include_roots = [
        REPO_ROOT / "Code/BF2",
        REPO_ROOT / "Code/Tools/LoadBot",
        REPO_ROOT / "Code/BF2/External/zlib-1.2.3",
        REPO_ROOT / "Code/BF2/External/zlib-1.2.3/contrib/minizip",
        REPO_ROOT / "Code/BF2/External/TinyXML",
        REPO_ROOT / "Code/BF2/External/python-embedded/linux",
        REPO_ROOT / "Code/BF2/External/python-embedded/py-src-2.3.4/Include",
        REPO_ROOT / "Code/BF2/External/python-embedded/py-src-2.3.4/PC",
    ]
    loadbot_roots = [REPO_ROOT / "Code/Tools/LoadBot"]
    bf2_root = (REPO_ROOT / "Code/BF2").resolve()

    for rel in files:
        p = REPO_ROOT / rel
        raw = p.read_text(encoding="utf-8", errors="replace")
        cleaned = strip_comments_and_strings(raw, keep_strings=False)

        # 1. Balanced braces, parens, #if/#endif
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

        # 2. Verify #include resolution & LoadBot isolation
        is_loadbot = rel.startswith("Code/Tools/LoadBot/")
        roots = loadbot_roots if is_loadbot else include_roots
        for lineno, line in enumerate(raw.splitlines(), 1):
            m = re.match(r'^\s*#\s*include\s*"([^"]+)"', line)
            if m:
                inc = m.group(1)
                if is_loadbot and ("BF2" in inc or ".." in inc):
                    issues.append(f"{rel}:{lineno}: LoadBot must not include engine/parent header \"{inc}\"")
                candidates = [p.parent / inc] + [r / inc for r in roots]
                resolved = [c.resolve() for c in candidates if c.is_file()]
                if not resolved:
                    issues.append(f"{rel}:{lineno}: missing local include \"{inc}\"")
                elif is_loadbot:
                    for rc in resolved:
                        try:
                            rc.relative_to(bf2_root)
                            issues.append(f"{rel}:{lineno}: LoadBot resolved include into Code/BF2/: \"{inc}\"")
                        except ValueError:
                            pass
            else:
                m2 = re.match(r"^\s*#\s*include\s*<([^>]+)>", line)
                if m2:
                    inc = m2.group(1)
                    if inc not in STD_AND_POSIX_HEADERS and not any((r / inc).is_file() for r in roots):
                        issues.append(f"{rel}:{lineno}: unknown system include <{inc}>")

    # 3. RouteFormat.h vs RouteActionRecorder.h binary struct parity
    rf_path = REPO_ROOT / "Code/Tools/LoadBot/RouteFormat.h"
    ra_path = REPO_ROOT / "Code/BF2/Game/GameServer/RouteActionRecorder.h"
    if not rf_path.is_file() or not ra_path.is_file():
        issues.append("Missing RouteFormat.h or RouteActionRecorder.h for binary struct parity check")
    else:
        rf_txt = rf_path.read_text(encoding="utf-8", errors="replace")
        ra_txt = ra_path.read_text(encoding="utf-8", errors="replace")
        for sname, exp_sz in (("RouteActionHeader", 32), ("RouteActionSample", 36)):
            rf_fields = extract_struct_fields(rf_txt, sname)
            ra_fields = extract_struct_fields(ra_txt, sname)
            if rf_fields is None or ra_fields is None:
                issues.append(f"Could not extract struct {sname} from RouteFormat.h or RouteActionRecorder.h")
                continue
            if len(rf_fields) != len(ra_fields):
                issues.append(f"Struct {sname} field count mismatch: RouteFormat={len(rf_fields)} vs RouteActionRecorder={len(ra_fields)}")
            else:
                for idx, ((t1, n1), (t2, n2)) in enumerate(zip(rf_fields, ra_fields)):
                    if t1 != t2 or n1 != n2:
                        issues.append(f"Struct {sname} field[{idx}] mismatch: ({t1} {n1}) vs ({t2} {n2})")
            for label, txt in (("RouteFormat.h", rf_txt), ("RouteActionRecorder.h", ra_txt)):
                if not re.search(rf"static_assert\s*\(\s*sizeof\s*\(\s*{sname}\s*\)\s*==\s*{exp_sz}\b", txt):
                    issues.append(f"{label}: missing static_assert(sizeof({sname}) == {exp_sz})")

    # 4. CMakeLists.txt target sanity
    cmake_path = REPO_ROOT / "CMakeLists.txt"
    if not cmake_path.is_file():
        issues.append("Missing CMakeLists.txt")
    else:
        cmake_txt = cmake_path.read_text(encoding="utf-8", errors="replace")
        for tgt in ("bf2_loadbot", "bf2_loadbot_tests"):
            if not re.search(rf"add_executable\s*\(\s*{re.escape(tgt)}\b", cmake_txt):
                issues.append(f"CMakeLists.txt missing add_executable({tgt} ...)")

    return len(issues) == 0, issues, f"{len(files)} files checked + struct parity + CMakeLists"


def main():
    files = get_changed_and_untracked_cpp_files()
    checks = [
        ("1. Git branch safety", check_git_branch_safety),
        ("2. Zero ABI/layout/vtable breakage in Code/BF2/", check_abi_layout_vtable),
        ("3. CVar safety (sv-record-routes=0, no static Vars::get*)", lambda: check_cvar_safety(files)),
        ("4. Strict C++14 compliance", lambda: check_cpp14_compliance(files)),
        ("5. C++ hygiene, LoadBot isolation, struct parity, CMake targets", lambda: check_cpp_hygiene_and_parity(files)),
    ]

    all_ok = True
    print(f"=== OpenBF2 feat/loadbot-64p Static Verification ({len(files)} changed/new C/C++ files) ===")
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
