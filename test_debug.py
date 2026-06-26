"""Exact port of the C algorithm from dllmain.cpp lines 460-478.
Run on actual data from List.txt to find the bug."""

def parse_c_algorithm(buf):
    """Exact port of the C code."""
    # Find date DD.MM.YYYY
    date_pos = -1
    for i in range(len(buf) - 9):
        if (buf[i+2] == '.' and buf[i+5] == '.' and
            buf[i:i+2].isdigit() and buf[i+3:i+5].isdigit() and
            buf[i+6:i+10].isdigit()):
            date_pos = i
            break

    if date_pos < 0:
        return f"NO DATE FOUND, returning full: '{buf}'"

    # Everything left of date
    before_len = date_pos
    fn = list(buf[:before_len])

    # Trim trailing spaces
    while before_len > 0 and fn[before_len-1] == ' ':
        before_len -= 1
        fn[before_len] = '\0'

    print(f"  after trim: '{''.join(fn[:before_len])}' (len={before_len})")

    # Strip 3 size groups from right
    p = before_len - 1

    # Group 3
    while p > 0 and fn[p].isdigit():
        p -= 1
    g3_start = p + 1
    print(f"  group3: p={p} char='{fn[p]}'")
    if p > 0 and fn[p] == ' ':
        p -= 1
    else:
        print(f"  RESTORE from group3 (char='{fn[p]}' is not space)")
        p = before_len - 1

    # Group 2
    while p > 0 and fn[p].isdigit():
        p -= 1
    g2_start = p + 1
    print(f"  group2: p={p} char='{fn[p]}'")
    if p > 0 and fn[p] == ' ':
        p -= 1
    else:
        print(f"  RESTORE from group2")
        p = before_len - 1

    # Group 1
    while p > 0 and fn[p].isdigit():
        p -= 1
    g1_start = p + 1
    print(f"  group1: p={p} char='{fn[p]}'")
    if p > 0 and fn[p] == ' ':
        p -= 1
    else:
        print(f"  RESTORE from group1")
        p = before_len - 1

    print(f"  final p={p}, char='{fn[p]}'")

    fn[p + 1] = '\0'
    result = ''.join(fn[:p+1]).rstrip()
    return result


# Actual data from latest List.txt
tests = [
    "02 - Stone Wrote in Stone.mp3 10 198 564 24.12.2023 21:59 -a--",
    "03 - Dusk Century.mp3 8 404 459 24.12.2023 21:59 -a--",
    "04 - This Hate in Me Will Pass.mp3 8 873 644 24.12.2023 21:59 -a--",
]

for buf in tests:
    print(f"\nInput: '{buf}'")
    result = parse_c_algorithm(buf)
    print(f"Result: '{result}'")
    print()
