import re
with open('/mnt/f/NEO-OS/kernel/font_misans.h','r',encoding='utf-8') as f:
    content = f.read()
m = re.search(r'font_cn_codes\[FONT_CN_COUNT\] = \{([^}]+)\}', content)
if m:
    nums = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]+)', m.group(1))]
    print(f'count={len(nums)}')
    sorted_ok = all(nums[i] <= nums[i+1] for i in range(len(nums)-1))
    print(f'sorted={sorted_ok}')
    if not sorted_ok:
        for i in range(len(nums)-1):
            if nums[i] > nums[i+1]:
                print(f'unsorted at {i}: 0x{nums[i]:04X} > 0x{nums[i+1]:04X}')
                break
