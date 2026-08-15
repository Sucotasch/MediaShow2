"""
Test parsing of TC LB_GETTEXT strings.
Right-to-left approach: find DD.MM.YYYY, strip date+time+attrs,
then find size (1..3 digit groups), remaining = filename.

Real TC format (see PROJECT_CONTEXT.md):
    filename.ext[SPC]NNN[NBSP]NNN[NBSP]NNN[TAB]DD.MM.YYYY[SPC]HH:MM[SPC]-a--
Separators between size groups are NON-BREAKING SPACES (U+00A0), and a
TAB (U+0009) precedes the date — not plain spaces. parse_tc_line mirrors
the C ParseTCFileName (dllmain.cpp), which treats space/NBSP/TAB alike.
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

    # Step 3: Find size (1..3 digit groups from right, like ParseTCFileName)
    # Walk backwards: up to 3 groups of [digits][separator]
    separators = (' ', '\u00A0', '\t')
    pos = len(before_date) - 1
    for _ in range(3):
        digit_start = pos
        while pos > 0 and before_date[pos].isdigit():
            pos -= 1
        if pos == digit_start:
            break  # no digits — not a size group
        if pos > 0 and before_date[pos] in separators:
            pos -= 1
        elif pos > 0:
            pos = digit_start  # group glued to name — keep digits
            break

    # Everything left of pos = filename
    filename = before_date[:pos + 1].rstrip()
    return filename, True


# Real data (as TC emits it)
test_lines = [
    ("02 - Arpadhazi Margit balladaja.mp3 12 681 905 02.10.2021 18:57 -a--",
     "02 - Arpadhazi Margit balladaja.mp3"),
    ("03 - Galamb.mp3 13 881 408 02.10.2021 18:57 -a--",
     "03 - Galamb.mp3"),
    ("04 - Vedj meg Lang! - 1. resz.mp3 11 339 207 02.10.2021 18:57 -a--",
     "04 - Vedj meg Lang! - 1. resz.mp3"),
]

# Edge cases: size groups 1..3, numeric names, no extension
edge_cases = [
    ("short.mp3 1234 01.01.2024 12:00 -a--", "short.mp3"),            # 1 group
    ("file.mp3 12 345 01.01.2024 12:00 -a--", "file.mp3"),            # 2 groups
    ("name.mp3 999 999 999 01.01.2024 12:00 -a--", "name.mp3"),       # 3 groups
    ("a.mp3 1 01.01.2024 12:00 -a--", "a.mp3"),                       # minimal
    ("noext 12 345 678 01.01.2024 12:00 -a--", "noext"),              # no extension
    ("12345.mp3 12 345 678 01.01.2024 12:00 -a--", "12345.mp3"),      # numeric name
    ("track123.mp3 12 681 905 02.10.2021 18:57 -a--", "track123.mp3"),  # name ends in digits
]

# Real TC format: NBSP (U+00A0) between size groups, TAB (U+0009) before date
tc_format_cases = [
    ("02 - Arpadhazi Margit balladaja.mp3 12\u00A0681\u00A0905\u000902.10.2021 18:57 -a--",
     "02 - Arpadhazi Margit balladaja.mp3"),
    ("short.mp3 1234\u000901.01.2024 12:00 -a--", "short.mp3"),
    ("a.mp3 1\u000901.01.2024 12:00 -a--", "a.mp3"),
]

print("=== TC Line Parsing Test ===\n")

all_passed = True

def run(label, cases):
    global all_passed
    for i, (line, expected) in enumerate(cases):
        filename, ok = parse_tc_line(line)
        good = ok and filename == expected
        all_passed = all_passed and good
        status = "OK" if good else "FAIL"
        print(f"{label} {i+1}: [{status}]")
        print(f"  Input:    {line!r}")
        print(f"  Filename: {filename!r}")
        if not good:
            print(f"  Expected: {expected!r}")
        print()

run("Test", test_lines)
run("Edge", edge_cases)
run("TCfmt", tc_format_cases)

print(f"{'ALL TESTS PASSED' if all_passed else 'SOME TESTS FAILED'}")
raise SystemExit(0 if all_passed else 1)
