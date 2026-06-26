import re

# Actual data from List.txt - debug output
lines = [
    "03 - Regression.mp3 10 865 989 11.12.2023 19:14 -a--",
    "04 - Relativity Undone.mp3 11 782 378 11.12.2023 19:14 -a--",
    "05 - Shame as a Weapon.mp3 13 747 831 11.12.2023 19:14 -a--",
]

# Strategy: split from RIGHT side
# Last token = attributes (-a--)
# Second from right = time (HH:MM)
# Third from right = date (DD.MM.YYYY)
# Everything before that = filename + size
# Size is 3 groups of digits separated by spaces
# Filename ends where size begins

for line in lines:
    parts = line.rsplit(' ', 4)  # Split from right, max 4 splits
    # parts[0] = filename + size
    # parts[1] = date
    # parts[2] = time
    # parts[3] = empty (separator)
    # parts[4] = attributes

    combined = parts[0]  # "03 - Regression.mp3 10 865 989"
    
    # Size is always "NNN NNN NNN" or "NN NNN NNN" - 3 groups of digits
    # Filename ends where the last digit group starts
    # Find the LAST space before a digit sequence
    m = re.search(r'(\d[\d ]+\d) +$', combined)
    if m:
        filename = combined[:m.start()]
    else:
        filename = combined
    
    print(f"Input:    {line}")
    print(f"Combined: {combined}")
    print(f"Filename: {filename}")
    print()
