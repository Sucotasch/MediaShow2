import re

lines = [
    "03 - Regression.mp3 10 865 989 11.12.2023 19:14 -a--",
    "04 - Relativity Undone.mp3 11 782 378 11.12.2023 19:14 -a--",
    "05 - Shame as a Weapon.mp3 13 747 831 11.12.2023 19:14 -a--",
    "01 - Ashurbanipal's Request.mp3 16 578 063 10.10.2024 10:44 -a--",
]

# Find date DD.MM.YYYY, take everything before it as "filename size"
# Then from "filename size", the size is the last token (digits with spaces)
# Size always has exactly 2 spaces (3 groups of digits)
# So find the LAST space that precedes exactly "N N N" pattern

for line in lines:
    # Find the date and everything before it
    m = re.search(r'(\d{2}\.\d{2}\.\d{4})', line)
    if m:
        before_date = line[:m.start()].rstrip()
        # Size is at the end: digits, space, digits, space, digits
        # Find where size starts: look for " NNN NNN" at end
        size_match = re.search(r' (\d+) (\d+) (\d+)$', before_date)
        if size_match:
            filename = before_date[:size_match.start()]
        else:
            filename = before_date
    else:
        filename = line
    
    print(f"Input:  {line}")
    print(f"Result: {filename}")
    print()
