import re

# Actual LB_GETTEXT output from DebugView
lines = [
    "03 - Regression.mp3 10 865 989 11.12.2023 19:14 -a--",
    "04 - Relativity Undone.mp3 11 782 378 11.12.2023 19:14 -a--",
    "05 - Shame as a Weapon.mp3 13 747 831 11.12.2023 19:14 -a--",
    "01 - Ashurbanipal's Request.mp3 16 578 063 10.10.2024 10:44 -a--",
]

# Pattern: filename is everything before " DD.MM.YYYY"
# The size (digits+spaces) comes right before the date
# So find the space before the date, then skip back past the size

for line in lines:
    # Find date DD.MM.YYYY
    m = re.search(r' (\d{2}\.\d{2}\.\d{4}) ', line)
    if m:
        date_start = m.start() + 1  # +1 to skip the leading space
        # Everything before date_start is "filename size"
        before_date = line[:date_start].rstrip()
        # Size is the last token before date (digits with spaces)
        # Find where size starts: walk backwards looking for a space followed by non-digit
        # The size pattern is: \d+ \d+ \d+ (3 groups of digits with spaces)
        size_match = re.search(r' (\d[\d ]+\d) $', before_date + ' ')
        if size_match:
            filename = before_date[:size_match.start()]
        else:
            filename = before_date
    else:
        filename = line
    
    print(f"Input:  {line}")
    print(f"Output: {filename.strip()}")
    print()
