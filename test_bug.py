"""Test: is beforeLen update the bug?

NOTE (2026-08-15): analysis shows the "buggy" and "fixed" variants are
behaviourally identical on ALL inputs — the stale-before_len final trim
never executes, because after the initial trailing-space trim the char at
fn[before_len-1] is never a space. So this test documents parser
invariants and guards against the IndexError class (writing fn[before_len]
past the buffer end), not against the theoretical stale-len bug. It
cannot serve as a regression discriminator for that bug; do not extend it
with "distinguishing" cases that don't exist.
"""

def parse_c_algorithm_buggy(buf):
    """C code WITHOUT beforeLen update after truncation."""
    date_pos = -1
    for i in range(len(buf) - 9):
        if (buf[i+2] == '.' and buf[i+5] == '.' and
            buf[i:i+2].isdigit() and buf[i+3:i+5].isdigit() and
            buf[i+6:i+10].isdigit()):
            date_pos = i
            break

    if date_pos < 0:
        return buf[:]

    before_len = date_pos
    # C code uses a buffer of size before_len+1; Python list must match
    fn = list(buf[:before_len]) + ['\0']
    fn[before_len] = '\0'

    # Trim
    while before_len > 0 and fn[before_len-1] == ' ':
        before_len -= 1
        fn[before_len] = '\0'

    p = before_len - 1

    # Group 3
    while p > 0 and fn[p].isdigit(): p -= 1
    if p > 0 and fn[p] == ' ': p -= 1
    else: p = before_len - 1

    # Group 2
    while p > 0 and fn[p].isdigit(): p -= 1
    if p > 0 and fn[p] == ' ': p -= 1
    else: p = before_len - 1

    # Group 1
    while p > 0 and fn[p].isdigit(): p -= 1
    if p > 0 and fn[p] == ' ': p -= 1
    else: p = before_len - 1

    fn[p + 1] = '\0'

    # BUG: before_len NOT updated! Still points to original length.
    # Trim checks fn[before_len-1] which is past the new null terminator.
    while before_len > 0 and fn[before_len-1] == ' ':
        before_len -= 1
        fn[before_len] = '\0'

    result = ''.join(fn[:p+1]).rstrip()
    return result


def parse_c_algorithm_fixed(buf):
    """C code WITH beforeLen update."""
    date_pos = -1
    for i in range(len(buf) - 9):
        if (buf[i+2] == '.' and buf[i+5] == '.' and
            buf[i:i+2].isdigit() and buf[i+3:i+5].isdigit() and
            buf[i+6:i+10].isdigit()):
            date_pos = i
            break

    if date_pos < 0:
        return buf[:]

    before_len = date_pos
    # C code uses a buffer of size before_len+1; Python list must match
    fn = list(buf[:before_len]) + ['\0']
    fn[before_len] = '\0'

    while before_len > 0 and fn[before_len-1] == ' ':
        before_len -= 1
        fn[before_len] = '\0'

    p = before_len - 1

    while p > 0 and fn[p].isdigit(): p -= 1
    if p > 0 and fn[p] == ' ': p -= 1
    else: p = before_len - 1

    while p > 0 and fn[p].isdigit(): p -= 1
    if p > 0 and fn[p] == ' ': p -= 1
    else: p = before_len - 1

    while p > 0 and fn[p].isdigit(): p -= 1
    if p > 0 and fn[p] == ' ': p -= 1
    else: p = before_len - 1

    fn[p + 1] = '\0'
    # FIX: update before_len
    before_len = p + 1

    while before_len > 0 and fn[before_len-1] == ' ':
        before_len -= 1
        fn[before_len] = '\0'

    result = ''.join(fn[:p+1]).rstrip()
    return result


tests = [
    ("02 - Stone Wrote in Stone.mp3 10 198 564 24.12.2023 21:59 -a--",
     "02 - Stone Wrote in Stone.mp3"),
    ("03 - Dusk Century.mp3 8 404 459 24.12.2023 21:59 -a--",
     "03 - Dusk Century.mp3"),
    ("04 - This Hate in Me Will Pass.mp3 8 873 644 24.12.2023 21:59 -a--",
     "04 - This Hate in Me Will Pass.mp3"),
]

all_passed = True
for buf, expected in tests:
    print(f"Input: '{buf}'")
    buggy = parse_c_algorithm_buggy(buf)
    fixed = parse_c_algorithm_fixed(buf)
    ok = (buggy == expected and fixed == expected)
    print(f"  Buggy: '{buggy}'")
    print(f"  Fixed: '{fixed}'")
    print(f"  Expected: '{expected}' -> {'OK' if ok else 'FAIL'}")
    print()
    if not ok:
        all_passed = False

print("ALL TESTS PASSED" if all_passed else "SOME TESTS FAILED")
