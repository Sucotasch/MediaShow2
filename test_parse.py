"""
Test parsing of TC LB_GETTEXT strings.
Right-to-left approach: find DD.MM.YYYY, strip date+time+attrs,
then find size (3 digit groups), remaining = filename.
"""

def parse_tc_line(line):
    """
    Parse TC format: 'filename.ext NNN NNN NNN DD.MM.YYYY HH:MM -a--'
    Returns (filename, success)
    """
    # Step 1: Find DD.MM.YYYY pattern
    date_start = -1
    for i in range(len(line) - 9):
        if (line[i:i+2].isdigit() and line[i+2] == '.' and
            line[i+3:i+5].isdigit() and line[i+5] == '.' and
            line[i+6:i+10].isdigit()):
            date_start = i
            break

    if date_start < 0:
        return line.strip(), False

    # Step 2: Everything left of date = filename + size
    before_date = line[:date_start].rstrip()

    # Step 3: Find size (3 digit groups from right)
    # Walk backwards: digits, space, digits, space, digits, space
    pos = len(before_date) - 1

    # Group 3 (rightmost digits)
    while pos >= 0 and before_date[pos].isdigit():
        pos -= 1
    if pos < 0 or before_date[pos] != ' ':
        return before_date, False
    pos -= 1  # skip space

    # Group 2
    while pos >= 0 and before_date[pos].isdigit():
        pos -= 1
    if pos < 0 or before_date[pos] != ' ':
        return before_date, False
    pos -= 1  # skip space

    # Group 1 (leftmost digits)
    while pos >= 0 and before_date[pos].isdigit():
        pos -= 1
    if pos < 0 or before_date[pos] != ' ':
        return before_date, False
    pos -= 1  # skip space

    # Everything left of pos = filename
    filename = before_date[:pos + 1].rstrip()
    return filename, True


# Test data from List.txt
test_lines = [
    "02 - Arpadhazi Margit balladaja.mp3 12 681 905 02.10.2021 18:57 -a--",
    "03 - Galamb.mp3 13 881 408 02.10.2021 18:57 -a--",
    "04 - Vedj meg Lang! - 1. resz.mp3 11 339 207 02.10.2021 18:57 -a--",
]

# Edge cases
edge_cases = [
    "short.mp3 1234 01.01.2024 12:00 -a--",           # 1 group size
    "file.mp3 12 345 01.01.2024 12:00 -a--",           # 2 groups size
    "name.mp3 999 999 999 01.01.2024 12:00 -a--",      # max size
    "a.mp3 1 01.01.2024 12:00 -a--",                   # minimal
    "noext 12 345 678 01.01.2024 12:00 -a--",          # no extension
    "12345.mp3 12 345 678 01.01.2024 12:00 -a--",      # numeric filename
]

print("=== TC Line Parsing Test ===\n")

all_passed = True
for i, line in enumerate(test_lines):
    filename, ok = parse_tc_line(line)
    status = "OK" if ok else "FAIL"
    print(f"Test {i+1}: [{status}]")
    print(f"  Input:    '{line}'")
    print(f"  Filename: '{filename}'")
    print()
    if not ok:
        all_passed = False

print("=== Edge Cases ===\n")
for i, line in enumerate(edge_cases):
    filename, ok = parse_tc_line(line)
    status = "OK" if ok else "FAIL"
    print(f"Edge {i+1}: [{status}]")
    print(f"  Input:    '{line}'")
    print(f"  Filename: '{filename}'")
    print()
    if not ok:
        all_passed = False

print(f"{'ALL TESTS PASSED' if all_passed else 'SOME TESTS FAILED'}")
