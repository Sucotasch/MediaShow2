"""Exact port of the C algorithm from dllmain.cpp lines 460-478."""

def parse_c_algorithm(buf):
    """Port of the C code. Returns extracted filename."""
    # Find date DD.MM.YYYY
    date_pos = -1
    for i in range(len(buf) - 9):
        if (buf[i+2] == '.' and buf[i+5] == '.' and
            buf[i:i+2].isdigit() and buf[i+3:i+5].isdigit() and
            buf[i+6:i+10].isdigit()):
            date_pos = i
            break

    if date_pos < 0:
        return buf[:]

    # Copy everything before date
    before_len = date_pos
    file_name = list(buf[:before_len])

    # Trim trailing spaces
    while before_len > 0 and file_name[before_len-1] == ' ':
        before_len -= 1
        file_name[before_len] = '\0'

    # Strip 3 size groups from right
    p = before_len - 1  # index into file_name

    # Group 3
    while p > 0 and file_name[p].isdigit():
        p -= 1
    if p > 0 and file_name[p] == ' ':
        p -= 1
    else:
        p = before_len - 1  # RESTORE

    # Group 2
    while p > 0 and file_name[p].isdigit():
        p -= 1
    if p > 0 and file_name[p] == ' ':
        p -= 1
    else:
        p = before_len - 1  # RESTORE

    # Group 1
    while p > 0 and file_name[p].isdigit():
        p -= 1
    if p > 0 and file_name[p] == ' ':
        p -= 1
    else:
        p = before_len - 1  # RESTORE

    # Terminate at p+1
    file_name[p + 1] = '\0'

    # Trim trailing spaces
    result = ''.join(file_name[:p+1]).rstrip()
    return result


# Real data from List.txt
tests = [
    ("02 - Arpadhazi Margit balladaja.mp3 12 681 905 02.10.2021 18:57 -a--",
     "02 - Arpadhazi Margit balladaja.mp3"),
    ("03 - Galamb.mp3 13 881 408 02.10.2021 18:57 -a--",
     "03 - Galamb.mp3"),
    ("04 - Vedj meg Lang! - 1. resz.mp3 11 339 207 02.10.2021 18:57 -a--",
     "04 - Vedj meg Lang! - 1. resz.mp3"),
]

# Problematic case: filename ends with digits
tests.append(
    ("track123.mp3 12 681 905 02.10.2021 18:57 -a--",
     "track123.mp3")
)

print("=== C Algorithm Port Test ===\n")
all_ok = True
for buf, expected in tests:
    result = parse_c_algorithm(buf)
    ok = result == expected
    all_ok = all_ok and ok
    print(f"[{'OK' if ok else 'FAIL'}]")
    print(f"  Input:    '{buf}'")
    print(f"  Expected: '{expected}'")
    print(f"  Got:      '{result}'")
    print()

print("ALL PASSED" if all_ok else "FAILED")
