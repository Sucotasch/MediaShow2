"""Test: is beforeLen update the bug?"""

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
    fn = list(buf[:before_len])
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
    fn = list(buf[:before_len])
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
    "02 - Stone Wrote in Stone.mp3 10 198 564 24.12.2023 21:59 -a--",
    "03 - Dusk Century.mp3 8 404 459 24.12.2023 21:59 -a--",
    "04 - This Hate in Me Will Pass.mp3 8 873 644 24.12.2023 21:59 -a--",
]

for buf in tests:
    print(f"Input: '{buf}'")
    buggy = parse_c_algorithm_buggy(buf)
    fixed = parse_c_algorithm_fixed(buf)
    print(f"  Buggy: '{buggy}'")
    print(f"  Fixed: '{fixed}'")
    print()
