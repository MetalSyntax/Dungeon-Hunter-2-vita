import re

with open('decompiled/apk_jadx/sources/com/gameloft/android/GAND/GloftD2SS/Sounddefs.java', 'r') as f:
    content = f.read()

match = re.search(r'\{([^}]+)\}', content)
if match:
    items = [x.strip() for x in match.group(1).split(',')]
    with open('source/sounddefs.h', 'w') as out:
        out.write('#ifndef SOUNDDEFS_H\n#define SOUNDDEFS_H\n\n')
        out.write('static const char * const Sounddefs[] = {\n')
        for item in items:
            out.write(f'    {item},\n')
        out.write('};\n\n')
        out.write(f'#define SOUNDDEFS_COUNT {len(items)}\n\n')
        out.write('#endif // SOUNDDEFS_H\n')
